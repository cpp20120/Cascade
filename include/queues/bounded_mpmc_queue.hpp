#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

#include "../memory/allocator.hpp"
#include "../memory/config.hpp"


namespace cascade::memory {

template <typename T>
class BoundedMPMCQueue {
 private:
  struct Node {
	std::atomic<std::size_t> sequence;
	T data;
  };

  alignas(CACHE_LINE_SIZE) Node* buffer_;
  alignas(CACHE_LINE_SIZE) const std::size_t capacity_;
  alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> head_;
  alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> tail_;

 public:
  explicit BoundedMPMCQueue(std::size_t capacity)
	  : capacity_(capacity), head_(0), tail_(0) {
	void* memory =
		GlobalAllocator::allocate(sizeof(Node) * capacity, alignof(Node));
	if (!memory) {
	  throw std::bad_alloc();
	}

	buffer_ = static_cast<Node*>(memory);

	for (std::size_t i = 0; i < capacity_; ++i) {
	  buffer_[i].sequence.store(i, std::memory_order_relaxed);
	}
  }

  ~BoundedMPMCQueue() {
	if (buffer_) {
	  std::size_t head = head_.load(std::memory_order_relaxed);
	  std::size_t tail = tail_.load(std::memory_order_relaxed);

	  for (std::size_t i = head; i != tail; i = (i + 1) % capacity_) {
		buffer_[i].data.~T();
	  }

	  GlobalAllocator::deallocate(buffer_, sizeof(Node) * capacity_);
	}
  }

  BoundedMPMCQueue(const BoundedMPMCQueue&) = delete;
  BoundedMPMCQueue& operator=(const BoundedMPMCQueue&) = delete;

  bool try_enqueue(T&& value) {
	std::size_t tail = tail_.load(std::memory_order_relaxed);
	Node* node = &buffer_[tail % capacity_];
	std::size_t seq = node->sequence.load(std::memory_order_acquire);

	std::size_t diff = seq - tail;
	if (diff == 0) {
	  if (tail_.compare_exchange_strong(tail, tail + 1,
										std::memory_order_relaxed)) {
		new (&node->data) T(std::move(value));
		node->sequence.store(tail + 1, std::memory_order_release);
		return true;
	  }
	} else if (static_cast<std::int64_t>(diff) < 0) {
	  return false;
	}

	return false;
  }

  bool try_enqueue(const T& value) {
	T temp = value;
	return try_enqueue(std::move(temp));
  }

  bool try_dequeue(T& value) {
	std::size_t head = head_.load(std::memory_order_relaxed);
	Node* node = &buffer_[head % capacity_];
	std::size_t seq = node->sequence.load(std::memory_order_acquire);

	std::size_t diff = seq - (head + 1);
	if (diff == 0) {
	  if (head_.compare_exchange_strong(head, head + 1,
										std::memory_order_relaxed)) {
		value = std::move(node->data);
		node->data.~T();
		node->sequence.store(head + capacity_, std::memory_order_release);
		return true;
	  }
	} else if (static_cast<std::int64_t>(diff) < 0) {
	  return false;
	}

	return false;
  }

  bool empty() const noexcept {
	std::size_t head = head_.load(std::memory_order_acquire);
	std::size_t tail = tail_.load(std::memory_order_acquire);
	return head >= tail;
  }

  bool full() const noexcept {
	std::size_t head = head_.load(std::memory_order_acquire);
	std::size_t tail = tail_.load(std::memory_order_acquire);
	return (tail - head) >= capacity_;
  }

  std::size_t size() const noexcept {
	std::size_t head = head_.load(std::memory_order_acquire);
	std::size_t tail = tail_.load(std::memory_order_acquire);
	return tail - head;
  }

  std::size_t capacity() const noexcept { return capacity_; }
};

}  // namespace cascade::memory