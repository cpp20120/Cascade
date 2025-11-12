#pragma once

#include <atomic>
#include <cstdint>

#include "config.hpp"

namespace cascade::memory {

constexpr int CHUNK_COUNT = 4;

struct Chunk {
  std::atomic<Chunk*> next;
  std::size_t capacity_bytes;
  std::atomic<std::size_t> used;
  std::atomic<std::size_t> active_count;
  std::atomic<uint32_t> owner_id;

  Chunk();
};

struct ObjectHeader {
  std::atomic<void*> next;
  Chunk* chunk_ptr;
  std::size_t alloc_size;
  bool is_task;

  static ObjectHeader* from_payload(void* payload) noexcept;
  static void* to_payload(ObjectHeader* header) noexcept;
};

}  // namespace cascade::memory