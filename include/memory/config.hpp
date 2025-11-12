#pragma once

#include <cstddef>
#include <cstdint>
#include <new>

namespace cascade::memory {

#ifdef __cpp_lib_hardware_interference_size
inline constexpr std::size_t CACHE_LINE_SIZE =
	std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t CACHE_LINE_SIZE = 64;
#endif

inline constexpr std::size_t DEFAULT_CHUNK_SIZE = 4 * 1024 * 1024;	// 4MB
inline constexpr std::size_t MAX_THREADS = 256;
inline constexpr uint32_t INVALID_OWNER_ID = UINT32_MAX;

}  // namespace cascade::memory