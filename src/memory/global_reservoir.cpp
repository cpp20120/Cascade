#include "../../include/memory/global_reservoir.hpp"

#include <new>

#include "../../include/memory/config.hpp"


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
  long page_size = sysconf(_SC_PAGESIZE);
  std::size_t aligned_size = ((size + page_size - 1) / page_size) * page_size;
  void* p = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) throw std::bad_alloc();
  return p;
#endif
}

void os_free_pages(void* p, std::size_t size) {
#if defined(_WIN32)
  VirtualFree(p, 0, MEM_RELEASE);
#else
  long page_size = sysconf(_SC_PAGESIZE);
  std::size_t aligned_size = ((size + page_size - 1) / page_size) * page_size;
  munmap(p, aligned_size);
#endif
}
}  // namespace

ChunkReservoir::ChunkReservoir(std::size_t chunk_size)
	: head_(nullptr), chunk_size_(chunk_size) {}

ChunkReservoir::~ChunkReservoir() {
  Chunk* current = head_.load(std::memory_order_acquire);
  while (current) {
	Chunk* next = current->next.load(std::memory_order_relaxed);
	deallocate_chunk(current);
	current = next;
  }
}

Chunk* ChunkReservoir::obtain() {
  Chunk* current = head_.load(std::memory_order_acquire);
  while (current) {
	Chunk* next = current->next.load(std::memory_order_relaxed);
	if (head_.compare_exchange_weak(current, next, std::memory_order_acq_rel,
									std::memory_order_acquire)) {
	  // Reset chunk state
	  current->used.store(0, std::memory_order_relaxed);
	  current->active_count.store(0, std::memory_order_relaxed);
	  current->owner_id.store(INVALID_OWNER_ID, std::memory_order_relaxed);
	  return current;
	}
  }
  return allocate_new_chunk();
}

void ChunkReservoir::release(Chunk* chunk) noexcept {
  if (!chunk) return;

  chunk->next.store(head_.load(std::memory_order_acquire),
					std::memory_order_relaxed);
  while (!head_.compare_exchange_weak(chunk->next, chunk,
									  std::memory_order_release,
									  std::memory_order_acquire)) {
  }
}

void ChunkReservoir::preallocate(std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
	Chunk* chunk = allocate_new_chunk();
	release(chunk);
  }
}

Chunk* ChunkReservoir::allocate_new_chunk() {
  std::size_t total_size = sizeof(Chunk) + chunk_size_;
  void* memory = os_alloc_pages(total_size);

  Chunk* chunk = new (memory) Chunk();
  chunk->capacity_bytes = chunk_size_;
  chunk->used.store(0, std::memory_order_relaxed);
  chunk->active_count.store(0, std::memory_order_relaxed);
  chunk->owner_id.store(INVALID_OWNER_ID, std::memory_order_relaxed);

  return chunk;
}

void ChunkReservoir::deallocate_chunk(Chunk* chunk) noexcept {
  if (!chunk) return;

  std::size_t total_size = sizeof(Chunk) + chunk->capacity_bytes;
  chunk->~Chunk();
  os_free_pages(chunk, total_size);
}

}  // namespace cascade::memory