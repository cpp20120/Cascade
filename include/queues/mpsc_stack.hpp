#pragma once

#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <thread>

#include "../memory/allocator.hpp"
#include "../memory/config.hpp"

namespace cascade::memory {

template <typename T>
class MPSCStack {
 private:
  struct Node {
	Node* next;
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

  template <typename U>
  void push(U&& value) {
	static_assert(std::is_constructible_v<T, U&&>,
				  "T must be constructible from U&&");

	Node* new_node = allocate_node(std::forward<U>(value));
	push_node(new_node);
  }

  template <typename InputIt>
  void push_bulk(InputIt first, InputIt last) {
	if (first == last) return;

	Node* chain_first = nullptr;
	Node* chain_last = nullptr;

	try {
	  for (auto it = first; it != last; ++it) {
		Node* new_node = allocate_node(*it);

		if (!chain_first) {
		  chain_first = chain_last = new_node;
		} else {
		  new_node->next = chain_first;
		  chain_first = new_node;
		}
	  }
	} catch (...) {
	  while (chain_first) {
		Node* next = chain_first->next;
		chain_first->~Node();
		GlobalAllocator::deallocate(chain_first, sizeof(Node));
		chain_first = next;
	  }
	  throw;
	}

	if (chain_first) {
	  push_chain(chain_first, chain_last);
	}
  }

  bool pop(T& value) {
	verify_consumer();

	Node* node = pop_node();
	if (!node) {
	  return false;
	}

	value = std::move(node->data);
	deallocate_node(node);
	return true;
  }

  template <typename Container>
  std::size_t pop_all(Container& container) {
	verify_consumer();

	Node* chain = pop_all_nodes();
	if (!chain) {
	  return 0;
	}

	std::size_t count = 0;
	Node* current = chain;
	while (current) {
	  container.push_back(std::move(current->data));
	  Node* next = current->next;
	  deallocate_node(current);
	  current = next;
	  ++count;
	}

	return count;
  }

  bool empty() const noexcept {
	return head_.load(std::memory_order_acquire) == nullptr;
  }

  void clear() {
	verify_consumer();

	Node* current = head_.exchange(nullptr, std::memory_order_acquire);
	while (current) {
	  Node* next = current->next;
	  deallocate_node(current);
	  current = next;
	}
  }

 private:
  template <typename... Args>
  Node* allocate_node(Args&&... args) {
	void* memory = GlobalAllocator::allocate(sizeof(Node), alignof(Node));
	if (!memory) {
	  throw std::bad_alloc();
	}

	try {
	  return new (memory) Node(std::forward<Args>(args)...);
	} catch (...) {
	  GlobalAllocator::deallocate(memory, sizeof(Node));
	  throw;
	}
  }

  void deallocate_node(Node* node) noexcept {
	node->~Node();
	GlobalAllocator::deallocate(node, sizeof(Node));
  }

  void push_node(Node* new_node) noexcept {
	Node* old_head = head_.load(std::memory_order_relaxed);
	new_node->next = old_head;

	while (!head_.compare_exchange_weak(old_head, new_node,
										std::memory_order_release,
										std::memory_order_relaxed)) {
	  new_node->next = old_head;
	}
  }

  void push_chain(Node* first, Node* last) noexcept {
	Node* old_head = head_.load(std::memory_order_relaxed);
	last->next = old_head;

	while (!head_.compare_exchange_weak(old_head, first,
										std::memory_order_release,
										std::memory_order_relaxed)) {
	  last->next = old_head;
	}
  }

  Node* pop_node() noexcept {
	Node* old_head = head_.load(std::memory_order_acquire);
	while (old_head) {
	  if (head_.compare_exchange_weak(old_head, old_head->next,
									  std::memory_order_acquire,
									  std::memory_order_relaxed)) {
		return old_head;
	  }
	}
	return nullptr;
  }

  Node* pop_all_nodes() noexcept {
	return head_.exchange(nullptr, std::memory_order_acquire);
  }

  void verify_consumer() {
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