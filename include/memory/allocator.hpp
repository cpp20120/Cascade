#pragma once

#include <memory>
#include <vector>

#include "arena.hpp"
#include "global_reservoir.hpp"
#include "qsbr.hpp"


namespace cascade::memory {

class AllocatorManager {
 public:
  explicit AllocatorManager(std::size_t max_threads = MAX_THREADS,
							std::size_t chunk_size = DEFAULT_CHUNK_SIZE);
  ~AllocatorManager();

  std::size_t register_thread();
  void unregister_thread(std::size_t id);

  LockFreeArena* get_arena(std::size_t thread_id) noexcept;
  LockFreeArena* create_arena_for_current_thread();

  void deallocate_remote(void* payload) noexcept;

  void poll_all() noexcept;

  AllocatorManager(const AllocatorManager&) = delete;
  AllocatorManager& operator=(const AllocatorManager&) = delete;

 private:
  QuiescentStateReclaimer qsbr_;
  ChunkReservoir reservoir_;
  std::size_t max_threads_;
  std::vector<std::atomic<LockFreeArena*>> arenas_;
};

class GlobalAllocator {
 public:
  static void initialize(std::size_t max_threads = MAX_THREADS,
						 std::size_t chunk_size = DEFAULT_CHUNK_SIZE);
  static void shutdown() noexcept;

  static LockFreeArena* get_arena() noexcept;
  static void* allocate(std::size_t size, std::size_t alignment) noexcept;
  static void deallocate(void* ptr, std::size_t size) noexcept;

  template <typename T, typename... Args>
  static T* create(Args&&... args);

  template <typename T>
  static void destroy(T* obj) noexcept;

  static void poll() noexcept;

 private:
  static AllocatorManager& instance();
};


template <typename T>
class Allocator {
 public:
  using value_type = T;

  Allocator() noexcept = default;

  template <typename U>
  Allocator(const Allocator<U>&) noexcept {}

  T* allocate(std::size_t n) {
	return static_cast<T*>(
		GlobalAllocator::allocate(n * sizeof(T), alignof(T)));
  }

  void deallocate(T* p, std::size_t n) noexcept {
	GlobalAllocator::deallocate(p, n * sizeof(T));
  }

  template <typename U>
  bool operator==(const Allocator<U>&) const noexcept {
	return true;
  }

  template <typename U>
  bool operator!=(const Allocator<U>&) const noexcept {
	return false;
  }
};

}  // namespace cascade::memory