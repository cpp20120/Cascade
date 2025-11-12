#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>

#include "../../include/memory/allocator.hpp"
#include "../../include/memory/config.hpp"

namespace cascade::memory {

template <typename T>
class MPSCStack {
 private:
  struct Node {
	std::atomic<Node*> next;
	T data;

	template <typename... Args>
	explicit Node(Args&&... args)
		: next(nullptr), data(std::forward<Args>(args)...) {}
  };

  
  alignas(CACHE_LINE_SIZE) std::atomic<Node*> head_;

  std::atomic<std::thread::id> consumer_thread_id_;
  bool consumer_registered_;

 public:
  MPSCStack()
	  : head_(nullptr),
		consumer_thread_id_(std::thread::id{}),
		consumer_registered_(false) {}

  ~MPSCStack() { clear(); }

  MPSCStack(const MPSCStack&) = delete;
  MPSCStack& operator=(const MPSCStack&) = delete;

  template <typename... Args>
  void push(Args&&... args) {
	void* memory = GlobalAllocator::allocate(sizeof(Node), alignof(Node));
	if (!memory) {
	  throw std::bad_alloc();
	}

	Node* new_node = nullptr;
	try {
	  new_node = new (memory) Node(std::forward<Args>(args)...);
	} catch (...) {
	  GlobalAllocator::deallocate(memory, sizeof(Node));
	  throw;
	}

	push_node(new_node);
  }

  bool pop(T& value) {
	register_consumer();

	Node* node = pop_node();
	if (!node) {
	  return false;
	}

	value = std::move(node->data);

	node->~Node();
	GlobalAllocator::deallocate(node, sizeof(Node));

	return true;
  }

  bool try_pop(T& value) { return pop(value); }

  bool empty() const noexcept {
	return head_.load(std::memory_order_acquire) == nullptr;
  }

  void clear() {
	register_consumer();

	Node* current = head_.exchange(nullptr, std::memory_order_acquire);
	while (current) {
	  Node* next = current->next.load(std::memory_order_relaxed);
	  current->~Node();
	  GlobalAllocator::deallocate(current, sizeof(Node));
	  current = next;
	}
  }

  std::size_t unsafe_size() const noexcept {
	std::size_t count = 0;
	Node* current = head_.load(std::memory_order_acquire);
	while (current) {
	  ++count;
	  current = current->next.load(std::memory_order_acquire);
	}
	return count;
  }

 private:
  void push_node(Node* new_node) noexcept {
	Node* old_head = head_.load(std::memory_order_relaxed);
	new_node->next.store(old_head, std::memory_order_relaxed);

	while (!head_.compare_exchange_weak(old_head, new_node,
										std::memory_order_release,
										std::memory_order_relaxed)) {
	  new_node->next.store(old_head, std::memory_order_relaxed);
	}
  }

  Node* pop_node() noexcept {
	Node* old_head = head_.load(std::memory_order_acquire);
	while (old_head) {
	  Node* new_head = old_head->next.load(std::memory_order_relaxed);
	  if (head_.compare_exchange_weak(old_head, new_head,
									  std::memory_order_acquire,
									  std::memory_order_relaxed)) {
		return old_head;
	  }
	}
	return nullptr;
  }

  void register_consumer() {
	if (!consumer_registered_) {
	  std::thread::id this_id = std::this_thread::get_id();
	  std::thread::id expected{};

	  if (consumer_thread_id_.compare_exchange_strong(
			  expected, this_id, std::memory_order_acq_rel,
			  std::memory_order_acquire)) {
		consumer_registered_ = true;
	  } else if (expected != this_id) {
		throw std::runtime_error("MPSCStack: Multiple consumers detected!");
	  } else {
		consumer_registered_ = true;
	  }
	}
  }
};

}  // namespace cascade::memory