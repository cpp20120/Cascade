#include "../../include/memory/arena.hpp"

#include <cassert>
#include <cstring>

#include <memory/config.hpp>

namespace cascade::memory {

LockFreeArena::LockFreeArena(uint32_t owner_id, ChunkReservoir* reservoir,
							 QuiescentStateReclaimer* qsbr)
	: owner_id_(owner_id),
	  reservoir_(reservoir),
	  qsbr_(qsbr),
	  current_chunk_(nullptr),
	  local_free_list_(nullptr),
	  remote_list_(nullptr),
	  local_bump_ptr_(0),
	  local_bump_cache_(0) {}

LockFreeArena::~LockFreeArena() {
  drain_remote();

  if (Chunk* chunk = current_chunk_.load(std::memory_order_relaxed)) {
	reservoir_->release(chunk);
  }
}

void* LockFreeArena::allocate(std::size_t size,
							  std::size_t alignment) noexcept {
  if (void* obj = pop_local_free()) {
	return obj;
  }

  drain_remote();
  if (void* obj = pop_local_free()) {
	return obj;
  }

  void* obj = bump_allocate(size, alignment);
  if (!obj) {
	refill_chunk();
	obj = bump_allocate(size, alignment);
  }

  return obj;
}

void LockFreeArena::deallocate(void* ptr, std::size_t size) noexcept {
  if (!ptr) return;

  ObjectHeader* header = ObjectHeader::from_payload(ptr);
  Chunk* chunk = header->chunk_ptr;
  assert(chunk);

  if (is_current_thread_owner()) {
	push_local_free(ptr);
	if (chunk->active_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
	  retire_chunk_if_needed(chunk);
	}
  } else {
	push_remote(ptr);
  }
}

void LockFreeArena::push_remote(void* payload) noexcept {
  if (!payload) return;

  ObjectHeader* header = ObjectHeader::from_payload(payload);
  void* head = remote_list_.load(std::memory_order_acquire);

  do {
	header->next.store(head, std::memory_order_relaxed);
  } while (!remote_list_.compare_exchange_weak(
	  head, payload, std::memory_order_release, std::memory_order_acquire));
}

void LockFreeArena::drain_remote() noexcept {
  void* head = remote_list_.exchange(nullptr, std::memory_order_acq_rel);
  if (!head) return;

  void* reversed = nullptr;
  void* current = head;

  while (current) {
	ObjectHeader* header = ObjectHeader::from_payload(current);
	void* next = header->next.load(std::memory_order_relaxed);

	header->next.store(reversed, std::memory_order_relaxed);
	reversed = current;
	current = next;
  }

  current = reversed;
  while (current) {
	ObjectHeader* header = ObjectHeader::from_payload(current);
	void* next = header->next.load(std::memory_order_relaxed);

	push_local_free(current);
	current = next;
  }
}

void LockFreeArena::poll() noexcept {
  drain_remote();
  qsbr_->quiescent();
  qsbr_->try_reclaim();
  try_return_empty_chunk();
}

void* LockFreeArena::bump_allocate(std::size_t size,
								   std::size_t alignment) noexcept {
  Chunk* chunk = current_chunk_.load(std::memory_order_acquire);
  if (!chunk) return nullptr;

  const std::size_t total_needed = sizeof(ObjectHeader) + size + alignment - 1;
  std::size_t current_used = chunk->used.load(std::memory_order_relaxed);
  std::size_t new_used = current_used + total_needed;

  if (new_used > chunk->capacity_bytes) {
	return nullptr;
  }

  if (chunk->used.compare_exchange_weak(current_used, new_used,
										std::memory_order_acq_rel,
										std::memory_order_relaxed)) {
	std::byte* chunk_data = reinterpret_cast<std::byte*>(chunk + 1);
	std::byte* object_start = chunk_data + current_used + sizeof(ObjectHeader);

	std::size_t misalignment =
		reinterpret_cast<uintptr_t>(object_start) % alignment;
	if (misalignment != 0) {
	  object_start += alignment - misalignment;
	}

	ObjectHeader* header =
		reinterpret_cast<ObjectHeader*>(object_start - sizeof(ObjectHeader));
	header->chunk_ptr = chunk;
	header->alloc_size = size;
	header->is_task = false;
	header->next.store(nullptr, std::memory_order_relaxed);

	void* payload = ObjectHeader::to_payload(header);

	chunk->active_count.fetch_add(1, std::memory_order_acq_rel);
	chunk->owner_id.store(owner_id_, std::memory_order_release);

	return payload;
  }

  return nullptr;
}

void LockFreeArena::refill_chunk() noexcept {
  Chunk* new_chunk = reservoir_->obtain();
  new_chunk->owner_id.store(owner_id_, std::memory_order_release);
  current_chunk_.store(new_chunk, std::memory_order_release);
}

void LockFreeArena::retire_chunk_if_needed(Chunk* chunk) noexcept {
  if (chunk->active_count.load(std::memory_order_acquire) == 0) {
	Chunk* current = current_chunk_.load(std::memory_order_acquire);
	if (current == chunk) {
	  current_chunk_.compare_exchange_strong(current, nullptr,
											 std::memory_order_release,
											 std::memory_order_relaxed);
	}
	qsbr_->retire(chunk);
  }
}

void LockFreeArena::try_return_empty_chunk() noexcept {
  Chunk* chunk = current_chunk_.load(std::memory_order_acquire);
  if (!chunk) return;

  if (chunk->active_count.load(std::memory_order_acquire) == 0 &&
	  chunk->used.load(std::memory_order_acquire) == 0) {
	if (current_chunk_.compare_exchange_strong(chunk, nullptr,
											   std::memory_order_release,
											   std::memory_order_relaxed)) {
	  reservoir_->release(chunk);
	}
  }
}

void LockFreeArena::push_local_free(void* obj) noexcept {
  if (!obj) return;

  ObjectHeader* header = ObjectHeader::from_payload(obj);
  void* head = local_free_list_.load(std::memory_order_relaxed);

  do {
	header->next.store(head, std::memory_order_relaxed);
  } while (!local_free_list_.compare_exchange_weak(
	  head, obj, std::memory_order_release, std::memory_order_relaxed));
}

void* LockFreeArena::pop_local_free() noexcept {
  void* head = local_free_list_.load(std::memory_order_acquire);
  while (head) {
	ObjectHeader* header = ObjectHeader::from_payload(head);
	void* next = header->next.load(std::memory_order_relaxed);

	if (local_free_list_.compare_exchange_weak(
			head, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
	  Chunk* chunk = header->chunk_ptr;
	  chunk->active_count.fetch_add(1, std::memory_order_acq_rel);
	  return head;
	}
  }
  return nullptr;
}

bool LockFreeArena::is_current_thread_owner() const noexcept {
  return qsbr_->current_thread_id() == owner_id_;
}

}  // namespace cascade::memory