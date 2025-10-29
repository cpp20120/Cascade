#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

#include "chunk.hpp"

namespace cascade::memory {

class QuiescentStateReclaimer {
 public:
  explicit QuiescentStateReclaimer(std::size_t max_threads = MAX_THREADS);
  ~QuiescentStateReclaimer();

  std::size_t register_thread();
  void unregister_thread(std::size_t id);

  void quiescent() noexcept;
  void retire(Chunk* chunk) noexcept;
  bool try_reclaim() noexcept;

  std::size_t current_thread_id() const noexcept;

  QuiescentStateReclaimer(const QuiescentStateReclaimer&) = delete;
  QuiescentStateReclaimer& operator=(const QuiescentStateReclaimer&) = delete;

 private:
  struct RetiredObject {
	Chunk* chunk;
	std::size_t epoch;
  };

  void reclaim_chunk(Chunk* chunk) noexcept;
  std::size_t compute_safe_epoch() noexcept;

  std::size_t max_threads_;
  std::atomic<std::size_t> next_id_{0};
  std::atomic<std::size_t>* observed_epochs_;
  std::vector<std::vector<RetiredObject>> retired_lists_;
  std::atomic<std::size_t> global_epoch_{1};

  static std::size_t& thread_local_id();
};

}  // namespace cascade::memory