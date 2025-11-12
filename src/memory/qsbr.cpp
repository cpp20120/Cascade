
#include <algorithm>
#include <stdexcept>


#include <memory/qsbr.hpp>
#include <memory/global_reservoir.hpp>

namespace cascade::memory {

QuiescentStateReclaimer::QuiescentStateReclaimer(std::size_t max_threads)
	: max_threads_(max_threads),
	  observed_epochs_(nullptr),
	  retired_lists_(max_threads) {
  observed_epochs_ = static_cast<std::atomic<std::size_t>*>(
	  ::operator new[](sizeof(std::atomic<std::size_t>) * max_threads_));

  for (std::size_t i = 0; i < max_threads_; ++i) {
	new (&observed_epochs_[i]) std::atomic<std::size_t>(0);
  }
}

QuiescentStateReclaimer::~QuiescentStateReclaimer() {
  try_reclaim();

  for (std::size_t i = 0; i < max_threads_; ++i) {
	observed_epochs_[i].~atomic();
  }
  ::operator delete[](observed_epochs_);
}

std::size_t QuiescentStateReclaimer::register_thread() {
  std::size_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
  if (id >= max_threads_) {
	throw std::runtime_error("QSBR: maximum thread count exceeded");
  }

  thread_local_id() = id;
  observed_epochs_[id].store(global_epoch_.load(std::memory_order_relaxed),
							 std::memory_order_release);
  return id;
}

void QuiescentStateReclaimer::unregister_thread(std::size_t id) {
  if (id < max_threads_) {
	observed_epochs_[id].store(SIZE_MAX, std::memory_order_release);
  }
}

void QuiescentStateReclaimer::quiescent() noexcept {
  std::size_t id = current_thread_id();
  if (id == SIZE_MAX) return;

  std::size_t current_epoch = global_epoch_.load(std::memory_order_acquire);
  observed_epochs_[id].store(current_epoch, std::memory_order_release);
}

void QuiescentStateReclaimer::retire(Chunk* chunk) noexcept {
  std::size_t id = current_thread_id();
  if (id == SIZE_MAX || !chunk) return;

  RetiredObject obj{chunk, global_epoch_.load(std::memory_order_relaxed)};
  auto& list = retired_lists_[id];

  // Grow vector if needed (this is slow path anyway)
  if (list.size() >= list.capacity()) {
	list.reserve(list.capacity() * 2);
  }

  list.push_back(obj);
}

bool QuiescentStateReclaimer::try_reclaim() noexcept {
  std::size_t old_epoch = global_epoch_.load(std::memory_order_acquire);
  global_epoch_.fetch_add(1, std::memory_order_acq_rel);

  std::size_t safe_epoch = compute_safe_epoch();
  if (safe_epoch == SIZE_MAX) return false;

  bool reclaimed_any = false;
  for (auto& list : retired_lists_) {
	auto it = std::remove_if(list.begin(), list.end(),
							 [safe_epoch](const RetiredObject& obj) {
							   if (obj.epoch < safe_epoch) {
								 reclaim_chunk(obj.chunk);
								 return true;
							   }
							   return false;
							 });

	if (it != list.end()) {
	  list.erase(it, list.end());
	  reclaimed_any = true;
	}
  }

  return reclaimed_any;
}

std::size_t QuiescentStateReclaimer::current_thread_id() const noexcept {
  return thread_local_id();
}

std::size_t& QuiescentStateReclaimer::thread_local_id() {
  thread_local std::size_t id = SIZE_MAX;
  return id;
}

std::size_t QuiescentStateReclaimer::compute_safe_epoch() noexcept {
  std::size_t registered = next_id_.load(std::memory_order_acquire);
  if (registered == 0) return SIZE_MAX;

  std::size_t min_observed = SIZE_MAX;
  for (std::size_t i = 0; i < registered; ++i) {
	std::size_t epoch = observed_epochs_[i].load(std::memory_order_acquire);
	if (epoch < min_observed) {
	  min_observed = epoch;
	}
  }

  return min_observed;
}

void QuiescentStateReclaimer::reclaim_chunk(Chunk* chunk) noexcept {
  // Chunk will be properly deallocated by the reservoir
  // This just calls destructor for any objects in the chunk
  if (chunk) {
	chunk->~Chunk();
  }
}

}  // namespace cascade::memory