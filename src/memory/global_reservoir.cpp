#include <algorithm>
#include <atomic>
#include <new>
#include <vector>

#include <memory/config.hpp>
#include <memory/chunk.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace cascade::memory {

namespace {

void* os_alloc_pages(std::size_t size) {
#if defined(_WIN32)
  void* p =
	  VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!p) throw std::bad_alloc();
  return p;
#else
  static long page_size = []() {
	long ps = sysconf(_SC_PAGESIZE);
	return ps > 0 ? ps : 4096;
  }();

  std::size_t aligned_size = ((size + page_size - 1) / page_size) * page_size;
  void* p = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if (p == MAP_FAILED) throw std::bad_alloc();

  madvise(p, aligned_size, MADV_WILLNEED | MADV_SEQUENTIAL);
  return p;
#endif
}

void os_free_pages(void* p, std::size_t size) {
  if (!p) return;

#if defined(_WIN32)
  VirtualFree(p, 0, MEM_RELEASE);
#else
  long page_size = sysconf(_SC_PAGESIZE);
  std::size_t aligned_size = ((size + page_size - 1) / page_size) * page_size;
  munmap(p, aligned_size);
#endif
}
}  // namespace

class ChunkReservoir {
 private:
  alignas(CACHE_LINE_SIZE) std::atomic<Chunk*> head_;
  const std::size_t chunk_size_;
  std::atomic<std::size_t> allocated_count_{0};
  std::atomic<std::size_t> peak_usage_{0};
  std::atomic<std::size_t> available_count_{0};

  struct ThreadLocalCache {
	Chunk* chunks[CHUNK_COUNT];
	std::size_t count{0};
  };

  static constexpr std::size_t LOCAL_CACHE_SIZE = 4;

 public:
  explicit ChunkReservoir(std::size_t chunk_size)
	  : head_(nullptr), chunk_size_(chunk_size) {}

  ~ChunkReservoir() { clear(); }

  ChunkReservoir(const ChunkReservoir&) = delete;
  ChunkReservoir& operator=(const ChunkReservoir&) = delete;

  Chunk* obtain() {
	ThreadLocalCache* tlc = get_thread_local_cache();
	if (tlc && tlc->count > 0) {
	  Chunk* chunk = tlc->chunks[--tlc->count];
	  reset_chunk(chunk);
	  update_usage_stats();
	  return chunk;
	}

	Chunk* chunk = pop();
	if (!chunk) {
	  chunk = allocate_new_chunk();
	} else {
	  available_count_.fetch_sub(1, std::memory_order_relaxed);
	  reset_chunk(chunk);
	  update_usage_stats();
	}
	return chunk;
  }

  void release(Chunk* chunk) noexcept {
	if (!chunk) return;

	reset_chunk(chunk);

	ThreadLocalCache* tlc = get_thread_local_cache();
	if (tlc && tlc->count < LOCAL_CACHE_SIZE) {
	  tlc->chunks[tlc->count++] = chunk;
	  return;
	}

	push(chunk);
	available_count_.fetch_add(1, std::memory_order_relaxed);
	update_usage_stats();
  }

  void preallocate(std::size_t count) {
	std::vector<Chunk*> allocated_chunks;
	allocated_chunks.reserve(count);

	try {
	  for (std::size_t i = 0; i < count; ++i) {
		Chunk* chunk = allocate_new_chunk();
		allocated_chunks.push_back(chunk);
	  }
	} catch (const std::bad_alloc&) {
	  for (Chunk* chunk : allocated_chunks) {
		deallocate_chunk(chunk);
	  }
	  allocated_count_.fetch_sub(allocated_chunks.size(),
								 std::memory_order_relaxed);
	  throw;
	}

	for (Chunk* chunk : allocated_chunks) {
	  push(chunk);
	  available_count_.fetch_add(1, std::memory_order_relaxed);
	}
  }

  void clear() noexcept {
	Chunk* current = head_.exchange(nullptr, std::memory_order_acquire);
	std::size_t count = 0;

	while (current) {
	  Chunk* next = current->next.load(std::memory_order_acquire);
	  deallocate_chunk(current);
	  current = next;
	  ++count;
	}

	allocated_count_.fetch_sub(count, std::memory_order_relaxed);
	available_count_.store(0, std::memory_order_relaxed);

	clear_thread_local_caches();
  }

  std::size_t get_allocated_count() const noexcept {
	return allocated_count_.load(std::memory_order_relaxed);
  }

  std::size_t get_available_count() const noexcept {
	return available_count_.load(std::memory_order_relaxed);
  }

  std::size_t get_peak_usage() const noexcept {
	return peak_usage_.load(std::memory_order_relaxed);
  }

  void drain_thread_caches() {
	ThreadLocalCache* tlc = get_thread_local_cache();
	if (tlc && tlc->count > 0) {
	  for (std::size_t i = 0; i < tlc->count; ++i) {
		push(tlc->chunks[i]);
		available_count_.fetch_add(1, std::memory_order_relaxed);
	  }
	  tlc->count = 0;
	}

	// Note: To drain ALL thread caches, you would need a global registry
	// of thread-local cache pointers and iterate through them here
  }

 private:
  Chunk* pop() {
	Chunk* current = head_.load(std::memory_order_acquire);
	while (current) {
	  Chunk* next = current->next.load(std::memory_order_acquire);
	  if (head_.compare_exchange_weak(current, next, std::memory_order_acq_rel,
									  std::memory_order_acquire)) {
		return current;
	  }
	}
	return nullptr;
  }

  void push(Chunk* chunk) noexcept {
	chunk->next.store(nullptr, std::memory_order_relaxed);

	Chunk* expected = head_.load(std::memory_order_acquire);
	do {
	  chunk->next.store(expected, std::memory_order_release);
	} while (!head_.compare_exchange_weak(
		expected, chunk, std::memory_order_release, std::memory_order_acquire));
  }

  Chunk* allocate_new_chunk() {
	std::size_t total_size = sizeof(Chunk) + chunk_size_;
	void* memory = os_alloc_pages(total_size);

	Chunk* chunk = new (memory) Chunk();
	chunk->capacity_bytes = chunk_size_;
	chunk->next.store(nullptr, std::memory_order_relaxed);

	allocated_count_.fetch_add(1, std::memory_order_relaxed);
	return chunk;
  }

  void deallocate_chunk(Chunk* chunk) noexcept {
	if (!chunk) return;

	std::size_t total_size = sizeof(Chunk) + chunk->capacity_bytes;
	chunk->~Chunk();
	os_free_pages(chunk, total_size);
  }

  void reset_chunk(Chunk* chunk) noexcept {
	chunk->used.store(0, std::memory_order_relaxed);
	chunk->active_count.store(0, std::memory_order_relaxed);
	chunk->owner_id.store(INVALID_OWNER_ID, std::memory_order_relaxed);
  }

  void update_usage_stats() noexcept {
	std::size_t available = available_count_.load(std::memory_order_relaxed);
	std::size_t allocated = allocated_count_.load(std::memory_order_relaxed);

	std::size_t in_use = allocated - available;
	std::size_t current_peak = peak_usage_.load(std::memory_order_relaxed);

	while (in_use > current_peak) {
	  if (peak_usage_.compare_exchange_weak(current_peak, in_use,
											std::memory_order_relaxed,
											std::memory_order_relaxed)) {
		break;
	  }
	}
  }

  ThreadLocalCache* get_thread_local_cache() {
	thread_local ThreadLocalCache tlc;
	return &tlc;
  }

  void clear_thread_local_caches() {
	ThreadLocalCache* tlc = get_thread_local_cache();
	if (tlc && tlc->count > 0) {
	  for (std::size_t i = 0; i < tlc->count; ++i) {
		deallocate_chunk(tlc->chunks[i]);
	  }
	  tlc->count = 0;
	}

	// Note: To clear ALL thread-local caches, you would need:
	// 1. A global registry of all thread-local cache pointers
	// 2. Thread-safe registration/deregistration mechanism
	// 3. Iteration through all registered caches during cleanup
  }
};

}  // namespace cascade::memory