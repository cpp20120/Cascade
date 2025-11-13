#pragma once
/**
 * @file small_function.hpp
 * @brief Small, move-only function wrapper with small-buffer optimization for
 * any function signature.
 *
 * @details
 * This is a compact function wrapper that supports any callable signature.
 * It is move-only by design, which allows a light-weight implementation with:
 *  - inline storage (Small Buffer Optimization) of size `StorageSize`;
 *  - three function pointers for call / move / destroy;
 *  - no dynamic allocation when the callable fits into the inline buffer.
 *  - type information without RTTI using type_index
 *
 * Limitations:
 *  - The stored callable type must be invocable with the specified signature.
 *  - The size of the callable must be <= StorageSize, otherwise a static_assert
 * fires.
 *
 * [] integrate it to arena pool without allocations
 * [X] perfect forwarding in call - template<typename... CallArgs>
 * [X] full type_info implementation without RTTI using type_index
 */

#include <cstddef>
#include <cstring>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace cascade::utility {

/**
 * @brief Compile-time type index for type identification without RTTI
 */
struct type_index {
  const char* name;
  std::size_t hash;

  constexpr bool operator==(const type_index& other) const noexcept {
	return hash == other.hash;
  }

  constexpr bool operator!=(const type_index& other) const noexcept {
	return hash != other.hash;
  }
};

namespace detail {
// Compile-time string hashing for type names
constexpr std::size_t hash_string(const char* str, std::size_t h = 0) {
  return (*str == '\0') ? h : hash_string(str + 1, (h * 131) + *str);
}

// Type name demangling 
template <typename T>
constexpr const char* get_type_name() {
#ifdef _MSC_VER
  return __FUNCSIG__;
#else
  return __PRETTY_FUNCTION__;
#endif
}
}  // namespace detail

/**
 * @brief Get type_index for type T at compile time
 */
template <typename T>
constexpr type_index get_type_index() noexcept {
  return type_index{detail::get_type_name<T>(),
					detail::hash_string(detail::get_type_name<T>())};
}
/**
 /**
 * @brief Small, move-only function wrapper with small-buffer optimization
 * 
 * @tparam R Return type of the function signature
 * @tparam Args Argument types of the function signature  
 * @tparam StorageSize Size of the inline storage in bytes (default: 64)
 *
 * @details
 * This class provides a lightweight, move-only function wrapper that stores
 * callable objects inline when possible, avoiding dynamic allocation for
 * small objects. It supports any callable type (functions, lambdas, function
 * objects) that matches the specified signature and fits within the storage.
 *
 * ## Key Features:
 * - **Move-only semantics**: Prevents slicing and enables efficient implementation
 * - **Small Buffer Optimization (SBO)**: No heap allocation for small callables
 * - **Type erasure**: Stores any callable with matching signature
 * - **Type information**: Runtime type queries without RTTI
 * - **Exception safety**: Strong exception guarantee for emplace operations
 *
 * ## Storage Requirements:
 * The callable object must satisfy:
 * - `sizeof(Callable) <= StorageSize`
 * - `alignof(Callable) <= alignof(std::max_align_t)`
 * - Must be invocable with the function signature
 *
 * ## Performance Characteristics:
 * - Construction: O(1) with SBO, no allocation for fitting types
 * - Move construction/assignment: O(1), just pointer swaps with SBO
 * - Invocation: One indirect function call overhead
 * - Destruction: O(1), trivial for trivially destructible types
 *
 * @note This class is not copyable. Use std::ref or lambda captures for
 *       shared callable semantics.
 *
 * @see small_function32
 * @see small_function64  
 * @see small_function128
 * @see swap(small_function&, small_function&)
 */
template <typename Sig, std::size_t StorageSize = 64>
class small_function;

template <typename R, typename... Args, std::size_t StorageSize>
class small_function<R(Args...), StorageSize> {
 public:
  using result_type = R;

  /// Default-construct an empty function.
  small_function() noexcept = default;

  /// Construct an empty function from nullptr for symmetry.
  small_function(std::nullptr_t) noexcept {}

  small_function(const small_function&) = delete;
  small_function& operator=(const small_function&) = delete;

  /// Move-construct from another small_function, stealing its callable.
  small_function(small_function&& other) noexcept {
	move_from(std::move(other));
  }

  /// Move-assign from another small_function, destroying current callable
  /// first.
  small_function& operator=(small_function&& other) noexcept {
	if (this != &other) {
	  reset();
	  move_from(std::move(other));
	}
	return *this;
  }

  /**
   * @brief Construct from any callable F that is not small_function itself.
   * @tparam F Callable type.
   * @param f  Callable instance, perfectly forwarded.
   */
  template <typename F, typename = std::enable_if_t<
							!std::is_same_v<std::decay_t<F>, small_function>>>
  small_function(F&& f) {
	emplace(std::forward<F>(f));
  }

  /// Destroy and release any stored callable.
  ~small_function() { reset(); }

  /// @return true if a callable is currently stored.
  explicit operator bool() const noexcept { return call_ != nullptr; }

  /// Invoke the stored callable. Behavior is undefined if empty.
  template <typename... CallArgs>
  R operator()(CallArgs&&... args) {
	return call_(storage_, std::forward<CallArgs>(args)...);
  }

  /// Destroy the stored callable and make this wrapper empty.
  void reset() noexcept {
	if (destroy_) {
	  destroy_(storage_, nullptr);
	  destroy_ = nullptr;
	  call_ = nullptr;
	  move_ = nullptr;
	  type_index_ = type_index{nullptr, 0};
	}
  }

  /**
   * @brief In-place construct a callable into the inline storage.
   * @tparam F Callable type.
   * @param f  Callable instance to store.
   *
   * Destroys any currently stored callable first. The callable must fit
   * into the inline storage and be invocable with the function signature.
   */
  template <typename F>
  void emplace(F&& f) {
	reset();
	using FnT = std::decay_t<F>;
	static_assert(std::is_invocable_r_v<R, FnT&, Args...>,
				  "Callable signature mismatch");
	static_assert(sizeof(FnT) <= StorageSize,
				  "Callable too large for small_function");
	static_assert(alignof(FnT) <= alignof(std::max_align_t),
				  "Callable alignment requirement too high");

	new (storage_) FnT(std::forward<F>(f));

	call_ = [](void* s, Args... args) -> R {
	  return (*reinterpret_cast<FnT*>(s))(std::forward<Args>(args)...);
	};

	// Optimize for trivially copyable types
	if constexpr (std::is_trivially_copyable_v<FnT> &&
				  std::is_trivially_destructible_v<FnT>) {
	  move_ = [](void* dst, void* src) { std::memcpy(dst, src, sizeof(FnT)); };
	} else if constexpr (std::is_trivially_copyable_v<FnT>) {
	  move_ = [](void* dst, void* src) {
		std::memcpy(dst, src, sizeof(FnT));
		// Source will be destroyed by destroy_ function
	  };
	} else {
	  move_ = [](void* dst, void* src) {
		new (dst) FnT(std::move(*reinterpret_cast<FnT*>(src)));
		reinterpret_cast<FnT*>(src)->~FnT();
	  };
	}

	// Optimize for trivially destructible types
	if constexpr (std::is_trivially_destructible_v<FnT>) {
	  destroy_ = [](void*, void*) {};  // No-op for trivial types
	} else {
	  destroy_ = [](void* s, void*) { reinterpret_cast<FnT*>(s)->~FnT(); };
	}

	// Store type information
	type_index_ = get_type_index<FnT>();
  }

  /// Swap with another small_function
  void swap(small_function& other) noexcept {
	small_function temp = std::move(*this);
	*this = std::move(other);
	other = std::move(temp);
  }

  /// Get the target type index (returns empty type_index if empty)
  type_index target_type() const noexcept { return type_index_; }

  /// Get the type name as C-string (returns nullptr if empty)
  const char* target_type_name() const noexcept { return type_index_.name; }

  /// Get pointer to the stored callable if it matches type T
  template <typename T>
  T* target() noexcept {
	if (!call_) return nullptr;
	if (type_index_ == get_type_index<T>()) {
	  return reinterpret_cast<T*>(storage_);
	}
	return nullptr;
  }

  /// Const version of target()
  template <typename T>
  const T* target() const noexcept {
	if (!call_) return nullptr;
	if (type_index_ == get_type_index<T>()) {
	  return reinterpret_cast<const T*>(storage_);
	}
	return nullptr;
  }

  /// Check if stored callable is of type T
  template <typename T>
  bool contains_type() const noexcept {
	return call_ && (type_index_ == get_type_index<T>());
  }

 private:
  /// Type of the call thunk.
  using CallFn = R (*)(void*, Args...);
  /// Type of the move/destroy thunk.
  using OpFn = void (*)(void*, void*);

  /// Helper to steal the callable from another instance.
  void move_from(small_function&& other) noexcept {
	if (other.call_) {
	  move_ = other.move_;
	  destroy_ = other.destroy_;
	  call_ = other.call_;
	  type_index_ = other.type_index_;
	  move_(storage_, other.storage_);

	  // Reset source
	  other.call_ = nullptr;
	  other.move_ = nullptr;
	  other.destroy_ = nullptr;
	  other.type_index_ = type_index{nullptr, 0};
	}
  }

  /// Inline storage buffer properly aligned for any type storable here.
  alignas(std::max_align_t) unsigned char storage_[StorageSize];
  /// Call thunk pointer or nullptr if empty.
  CallFn call_ = nullptr;
  /// Move thunk pointer or nullptr if empty.
  OpFn move_ = nullptr;
  /// Destroy thunk pointer or nullptr if empty.
  OpFn destroy_ = nullptr;
  /// Type information for the stored callable.
  type_index type_index_{nullptr, 0};
};

// Non-member swap
template <typename Sig, std::size_t StorageSize>
void swap(small_function<Sig, StorageSize>& a,
		  small_function<Sig, StorageSize>& b) noexcept {
  a.swap(b);
}

// Convenience aliases for common sizes
template <typename Sig>
using small_function32 = small_function<Sig, 32>;

template <typename Sig>
using small_function64 = small_function<Sig, 64>;

template <typename Sig>
using small_function128 = small_function<Sig, 128>;

// Comparison with nullptr
template <typename Sig, std::size_t StorageSize>
bool operator==(const small_function<Sig, StorageSize>& f,
				std::nullptr_t) noexcept {
  return !static_cast<bool>(f);
}

template <typename Sig, std::size_t StorageSize>
bool operator==(std::nullptr_t,
				const small_function<Sig, StorageSize>& f) noexcept {
  return !static_cast<bool>(f);
}

template <typename Sig, std::size_t StorageSize>
bool operator!=(const small_function<Sig, StorageSize>& f,
				std::nullptr_t) noexcept {
  return static_cast<bool>(f);
}

template <typename Sig, std::size_t StorageSize>
bool operator!=(std::nullptr_t,
				const small_function<Sig, StorageSize>& f) noexcept {
  return static_cast<bool>(f);
}

}  // namespace cascade::utility

// Fallback to std::move_only_function when available and desired
#ifdef SMALL_FUNCTION_USE_STD_FALLBACK
#ifdef __cpp_lib_move_only_function
#include <functional>

namespace cascade::utility {
template <typename Sig, std::size_t StorageSize = 64>
using small_function = std::move_only_function<Sig>;

// Keep the convenience aliases
template <typename Sig>
using small_function32 = small_function<Sig>;

template <typename Sig>
using small_function64 = small_function<Sig>;

template <typename Sig>
using small_function128 = small_function<Sig>;
}  // namespace cascade::utility
#endif
#endif