#include <cstddef>

#include <memory/chunk.hpp>
#include <memory/config.hpp>


namespace cascade::memory {

Chunk::Chunk()
	: next(nullptr),
	  capacity_bytes(0),
	  used(0),
	  active_count(0),
	  owner_id(INVALID_OWNER_ID) {}

ObjectHeader* ObjectHeader::from_payload(void* payload) noexcept {
  return reinterpret_cast<ObjectHeader*>(static_cast<std::byte*>(payload) -
										 sizeof(ObjectHeader));
}

void* ObjectHeader::to_payload(ObjectHeader* header) noexcept {
  return static_cast<void*>(reinterpret_cast<std::byte*>(header) +
							sizeof(ObjectHeader));
}

}  // namespace cascade::memory