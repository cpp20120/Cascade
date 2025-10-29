#pragma once

#include <atomic>

#include "chunk.hpp"

namespace cascade::memory {

class ChunkReservoir {
 public:
  explicit ChunkReservoir(std::size_t chunk_size = DEFAULT_CHUNK_SIZE);
  ~ChunkReservoir();

  Chunk* obtain();
  void release(Chunk* chunk) noexcept;
  void preallocate(std::size_t count);

  std::size_t chunk_size() const noexcept { return chunk_size_; }

  ChunkReservoir(const ChunkReservoir&) = delete;
  ChunkReservoir& operator=(const ChunkReservoir&) = delete;

 private:
  Chunk* allocate_new_chunk();
  void deallocate_chunk(Chunk* chunk) noexcept;

  std::atomic<Chunk*> head_;
  std::size_t chunk_size_;
};

}  // namespace cascade::memory