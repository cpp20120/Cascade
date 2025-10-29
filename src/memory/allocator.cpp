#include "../../include/memory/allocator.hpp"

#include <mutex>
#include <stdexcept>

namespace cascade::memory {

namespace {
std::unique_ptr<AllocatorManager> g_allocator_manager = nullptr;
std::once_flag g_init_flag;
}  // namespace

AllocatorManager::AllocatorManager(std::size_t max_threads,
								   std::size_t chunk_size)
	: qsbr_(max_threads),
	  reservoir_(chunk_size),
	  max_threads_(max_threads),
	  arenas_(max_threads) {
  reservoir_.preallocate(max_threads * 2);
}

AllocatorManager::~AllocatorManager() {
  for (auto& arena : arenas_) {
	if (LockFreeArena* a = arena.load(std::memory_order_relaxed)) {
	  delete a;
	}
  }
}

std::size_t AllocatorManager::register_thread() {
  return qsbr_.register_thread();
}

void AllocatorManager::unregister_thread(std::size_t id) {
  qsbr_.unregister_thread(id);

  if (id < arenas_.size()) {
	if (LockFreeArena* arena = arenas_[id].load(std::memory_order_relaxed)) {
	  delete arena;
	  arenas_[id].store(nullptr, std::memory_order_relaxed);
	}
  }
}

LockFreeArena* AllocatorManager::get_arena(std::size_t thread_id) noexcept {
  if (thread_id >= arenas_.size()) return nullptr;
  return arenas_[thread_id].load(std::memory_order_acquire);
}

LockFreeArena* AllocatorManager::create_arena_for_current_thread() {
  std::size_t id = qsbr_.current_thread_id();
  if (id == SIZE_MAX) {
	id = register_thread();
  }

  if (id >= max_threads_) {
	throw std::runtime_error("Thread ID exceeds maximum allowed threads");
  }

  LockFreeArena* expected = arenas_[id].load(std::memory_order_acquire);
  if (!expected) {
	std::unique_ptr<LockFreeArena> new_arena = std::make_unique<LockFreeArena>(
		static_cast<uint32_t>(id), &reservoir_, &qsbr_);

	if (arenas_[id].compare_exchange_strong(expected, new_arena.get(),
											std::memory_order_release,
											std::memory_order_acquire)) {
	  return new_arena.release();
	} else {
	  return expected;
	}
  }
  return expected;
}

void AllocatorManager::deallocate_remote(void* payload) noexcept {
  if (!payload) return;

  ObjectHeader* header = ObjectHeader::from_payload(payload);
  Chunk* chunk = header->chunk_ptr;
  if (!chunk) return;

  uint32_t owner_id = chunk->owner_id.load(std::memory_order_acquire);
  if (owner_id == INVALID_OWNER_ID) {
	// Use current thread's arena
	if (LockFreeArena* arena = create_arena_for_current_thread()) {
	  arena->push_remote(payload);
	}
	return;
  }

  if (LockFreeArena* owner_arena = get_arena(owner_id)) {
	owner_arena->push_remote(payload);
  } else {
	// Fallback to current thread
	if (LockFreeArena* arena = create_arena_for_current_thread()) {
	  arena->push_remote(payload);
	}
  }
}

void AllocatorManager::poll_all() noexcept {
  for (auto& arena_atomic : arenas_) {
	if (LockFreeArena* arena = arena_atomic.load(std::memory_order_acquire)) {
	  arena->poll();
	}
  }
  qsbr_.try_reclaim();
}

// GlobalAllocator implementation
AllocatorManager& GlobalAllocator::instance() {
  if (!g_allocator_manager) {
	std::call_once(g_init_flag, [] {
	  g_allocator_manager = std::make_unique<AllocatorManager>();
	});
  }
  return *g_allocator_manager;
}

void GlobalAllocator::initialize(std::size_t max_threads,
								 std::size_t chunk_size) {
  std::call_once(g_init_flag, [max_threads, chunk_size] {
	g_allocator_manager =
		std::make_unique<AllocatorManager>(max_threads, chunk_size);
  });
}

void GlobalAllocator::shutdown() noexcept {
  if (g_allocator_manager) {
	g_allocator_manager->poll_all();
	g_allocator_manager.reset();
  }
}

LockFreeArena* GlobalAllocator::get_arena() noexcept {
  return instance().create_arena_for_current_thread();
}

void* GlobalAllocator::allocate(std::size_t size,
								std::size_t alignment) noexcept {
  if (LockFreeArena* arena = get_arena()) {
	return arena->allocate(size, alignment);
  }
  return nullptr;
}

void GlobalAllocator::deallocate(void* ptr, std::size_t size) noexcept {
  if (!ptr) return;
  instance().deallocate_remote(ptr);
}

void GlobalAllocator::poll() noexcept {
  if (g_allocator_manager) {
	g_allocator_manager->poll_all();
  }
}

}  // namespace cascade::memory