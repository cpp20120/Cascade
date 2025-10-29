#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "chunk.hpp"
#include "global_reservoir.hpp"
#include "qsbr.hpp"

namespace cascade::memory {

class LockFreeArena {
 public:
  LockFreeArena(uint32_t owner_id, ChunkReservoir* reservoir,
				QuiescentStateReclaimer* qsbr);
  ~LockFreeArena();

  template <typename F>
  void* allocate_task(std::size_t size, std::size_t alignment, F&& func);

  void deallocate_task(void* task) noexcept;

  void* allocate(std::size_t size, std::size_t alignment) noexcept;
  void deallocate(void* ptr, std::size_t size) noexcept;

  void push_remote(void* payload) noexcept;
  void drain_remote() noexcept;

  void poll() noexcept;

  uint32_t owner_id() const noexcept { return owner_id_; }

  LockFreeArena(const LockFreeArena&) = delete;
  LockFreeArena& operator=(const LockFreeArena&) = delete;

 private:
  void* bump_allocate(std::size_t size, std::size_t alignment) noexcept;
  void refill_chunk() noexcept;
  void retire_chunk_if_needed(Chunk* chunk) noexcept;
  void try_return_empty_chunk() noexcept;

  void push_local_free(void* obj) noexcept;
  void* pop_local_free() noexcept;

  bool is_current_thread_owner() const noexcept;

  uint32_t owner_id_;
  ChunkReservoir* reservoir_;
  QuiescentStateReclaimer* qsbr_;

  std::atomic<Chunk*> current_chunk_;
  std::atomic<void*> local_free_list_;
  std::atomic<void*> remote_list_;

  alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> local_bump_ptr_;
  std::size_t local_bump_cache_;
};

}  // namespace cascade::memory