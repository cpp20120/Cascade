/**
 * @file small_function_with_arena_launder.hpp
 * @brief Small, type-erased, move-only function wrapper with SBO and arena
 * fallback.
 *
 * This header defines a fully modernized and allocation-aware `small_function`
 * designed as a high-performance alternative to `std::function`. It provides:
 *
 * - Small-buffer optimization (SBO) using an inline storage block of
 * `StorageSize` bytes.
 * - Automatic fallback to arena allocation using
 * `cascade::memory::GlobalAllocator` when the callable does not fit inline.
 * - Strict compliance with C++ object lifetime rules using `std::launder`.
 * - Correct move semantics with zero double-destruction risk.
 * - Type-safe querying of stored callable type without RTTI.
 * - `operator()` throws `std::bad_function_call()` when the wrapper is empty.
 *
 * @note This file assumes `GlobalAllocator::allocate()` and
 * `GlobalAllocator::deallocate()` already handle remote deallocation via QSBR
 * or local arena mechanisms.
 * 
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <atomic>

namespace cascade::memory {
/**
 * @brief Global allocator interface used by `small_function` for large object
 * fallback.
 *
 * Implementations must guarantee thread safety and remote deallocation support.
 */
struct GlobalAllocator {
  /**
   * @brief Allocate raw memory from the global arena.
   *
   * @param size Number of bytes requested.
   * @param alignment Required alignment.
   * @return Pointer to allocated memory or nullptr on failure.
   *
   * @note This function must be thread-safe.
   */
  static void* allocate(std::size_t size, std::size_t alignment) noexcept;

  /**
   * @brief Deallocate memory previously allocated by `allocate`.
   *
   * @param ptr Pointer to memory to deallocate.
   * @param size Size of the allocation (for tracking purposes).
   *
   * @note Implementations MUST route this to `deallocate_remote` internally.
   * @note This function must be thread-safe.
   */
  static void deallocate(void* ptr, std::size_t size) noexcept;
};
}  // namespace cascade::memory

namespace cascade::utility {

// ---- type_index ------------------------------------------------------------

/**
 * @brief Lightweight type token used to test type identity without RTTI.
 *
 * Provides fast type comparison using a stable per-type address token
 * rather than string comparison of type names.
 */
struct type_index {
  const char* name =
	  nullptr; /**< Compiler-provided type name (for debugging). */
  std::size_t hash =
	  0; /**< Stable, per-type address token for fast comparison. */

  /**
   * @brief Equality comparison for type indices.
   *
   * @param other The type_index to compare against.
   * @return true if both type_index objects represent the same type.
   */
  constexpr bool operator==(const type_index& other) const noexcept {
	return hash == other.hash;
  }

  /**
   * @brief Inequality comparison for type indices.
   *
   * @param other The type_index to compare against.
   * @return true if type_index objects represent different types.
   */
  constexpr bool operator!=(const type_index& other) const noexcept {
	return hash != other.hash;
  }
};

namespace detail {
/**
 * @brief Obtain a compiler-specific pretty name for a type T.
 *
 * @tparam T The type to get the name of.
 * @return constexpr const char* Compiler-specific string representing the type.
 */
template <typename T>
constexpr const char* get_type_name() {
#ifdef _MSC_VER
  return __FUNCSIG__;
#else
  return __PRETTY_FUNCTION__;
#endif
}

/**
 * @brief Create a stable per-type address token.
 *
 * Because the address is unique per type and stable across translation units
 * (due to static storage duration), it can serve as a fast RTTI surrogate.
 *
 * @tparam T The type to create an identity token for.
 * @return const void* Stable address unique to type T.
 */
template <typename T>
inline const void* type_identity_tag() noexcept {
  static std::atomic<int> tag = 0;
  return &tag;
}
}  // namespace detail

/**
 * @brief Get a `type_index` representing type T.
 *
 * @tparam T The type to get an index for.
 * @return type_index Object containing type name and identity hash.
 */
template <typename T>
inline type_index get_type_index() noexcept {
  return type_index{
	  detail::get_type_name<T>(),
	  reinterpret_cast<std::size_t>(detail::type_identity_tag<T>())};
}


/**
 * @brief High-performance, move-only function wrapper with SBO and arena
 * fallback.
 *
 * @tparam Sig Callable signature (e.g., `void(int)`).
 * @tparam StorageSize Size of inline SBO buffer in bytes.
 *
 * This class provides a type-erased callable wrapper that stores small
 * callables inline and falls back to arena allocation for larger objects. It is
 * move-only and provides type-safe access to the stored callable.
 */
template <typename Sig, std::size_t StorageSize>
class small_function;

/**
 * @brief Primary template specialization for small_function.
 *
 * @tparam R Return type of the callable.
 * @tparam Args Argument types of the callable.
 * @tparam StorageSize Size of inline SBO buffer in bytes.
 */
template <typename R, typename... Args, std::size_t StorageSize>
class small_function<R(Args...), StorageSize> {
 public:
  using result_type = R; /**< The return type of the stored callable. */

  /**
   * @brief Construct an empty small_function.
   */
  small_function() noexcept = default;

  /**
   * @brief Construct an empty small_function from nullptr.
   */
  small_function(std::nullptr_t) noexcept {}

  // Delete copy operations
  small_function(const small_function&) = delete;
  small_function& operator=(const small_function&) = delete;

  /**
   * @brief Move-construct from another small_function.
   *
   * @param other The small_function to move from.
   * @post `other` is left in an empty state.
   */
  small_function(small_function&& other) noexcept {
	move_from(std::move(other));
  }

  /**
   * @brief Move-assign from another small_function.
   *
   * @param other The small_function to move from.
   * @return small_function& Reference to this object.
   * @post `other` is left in an empty state.
   */
  small_function& operator=(small_function&& other) noexcept {
	if (this != &other) {
	  reset();
	  move_from(std::move(other));
	}
	return *this;
  }

  /**
   * @brief Construct from any compatible callable.
   *
   * @tparam F Type of the callable (deduced).
   * @param f Callable to store.
   *
   * @note This constructor is disabled if F is the same as small_function
   *       to avoid ambiguous constructor calls.
   */
  template <typename F, typename = std::enable_if_t<
							!std::is_same_v<std::decay_t<F>, small_function>>>
  small_function(F&& f) {
	emplace(std::forward<F>(f));
  }

  /**
   * @brief Destroy the small_function and any stored callable.
   */
  ~small_function() { reset(); }

  /**
   * @brief Check if the small_function contains a callable.
   *
   * @return true if a callable is stored, false otherwise.
   */
  explicit operator bool() const noexcept { return call_ != nullptr; }

  /**
   * @brief Invoke the stored callable.
   *
   * @tparam CallArgs Argument types (deduced).
   * @param args Arguments to pass to the callable.
   * @return R Result of the callable invocation.
   * @throw std::bad_function_call if the small_function is empty.
   */
  template <typename... CallArgs>
  R operator()(CallArgs&&... args) {
	if (!call_) [[unlikely]]
	  throw std::bad_function_call();
	return call_(storage_ptr(), std::forward<CallArgs>(args)...);
  }

  /**
	* @brief Invoke the stored callable (const version).
	* @tparam CallArgs Argument types (deduced).
	* @param args Arguments to pass to the callable.
	* @return R Result of the callable invocation.
	* @throw std::bad_function_call if the small_function is empty.
	*/
  template <typename ... CallArgs>
  R operator()(CallArgs&&... args) const {
	if (!call_) [[unlikely]]
	  throw std::bad_function_call();
	return call_(storage_ptr(), std::forward<CallArgs>(args)...);
  }
  /**
   * @brief Destroy stored callable and release arena memory if used.
   *
   * @post The small_function is empty and ready for reuse.
   */
  void reset() noexcept {
	if (!call_) return;

	if (destroy_) destroy_(storage_ptr(), nullptr);

	if (uses_heap_) {
	  cascade::memory::GlobalAllocator::deallocate(payload_ptr_,
												   allocated_size_);
	  payload_ptr_ = nullptr;
	  allocated_size_ = 0;
	  uses_heap_ = false;
	}

	call_ = nullptr;
	move_ = nullptr;
	destroy_ = nullptr;
	type_index_ = type_index{nullptr, 0};
	trivially_copyable_stored_ = false;
  }

  /**
   * @brief Replace stored callable with a new one.
   *
   * @tparam F Type of the callable to store.
   * @param f Callable to store.
   *
   * @throw std::bad_alloc if arena allocation fails and the callable doesn't
   * fit in SBO.
   *
   * @post Any previously stored callable is destroyed and replaced with the new
   * one.
   */
  template <typename F>
  void emplace(F&& f) {
	reset();
	using FnT = std::decay_t<F>;

	static_assert(std::is_invocable_r_v<R, FnT&, Args...>,
				  "Callable signature mismatch");

	if constexpr (std::is_empty_v<FnT> && sizeof(FnT) <= StorageSize) {
	  new (storage_inline_ptr()) FnT(std::forward<F>(f));
	  payload_ptr_ = nullptr;
	  allocated_size_ = 0;
	  uses_heap_ = false;
	}
	else if (sizeof(FnT) <= StorageSize &&
			 alignof(FnT) <= alignof(std::max_align_t)) {
	  new (storage_inline_ptr()) FnT(std::forward<F>(f));
	  payload_ptr_ = nullptr;
	  allocated_size_ = 0;
	  uses_heap_ = false;
	} else {
	  void* mem =
		  cascade::memory::GlobalAllocator::allocate(sizeof(FnT), alignof(FnT));
	  if (!mem) throw std::bad_alloc();
	  new (mem) FnT(std::forward<F>(f));
	  payload_ptr_ = mem;
	  allocated_size_ = sizeof(FnT);
	  uses_heap_ = true;
	}

	// Call thunk
	call_ = [](void* s, Args... args) -> R {
	  auto ptr = std::launder(reinterpret_cast<FnT*>(s));
	  if constexpr (std::is_void_v<R>) {
		(*ptr)(std::forward<Args>(args)...);
		return;
	  } else {
		return (*ptr)(std::forward<Args>(args)...);
	  }
	};

	// Move thunk
	if constexpr (std::is_trivially_copyable_v<FnT>) {
	  move_ = [](void* dst, void* src) { std::memcpy(dst, src, sizeof(FnT)); };
	  trivially_copyable_stored_ = true;
	} else {
	  move_ = [](void* dst, void* src) {
		new (dst) FnT(std::move(*std::launder(reinterpret_cast<FnT*>(src))));
	  };
	  trivially_copyable_stored_ = false;
	}

	// Destroy thunk
	if constexpr (std::is_trivially_destructible_v<FnT>) {
	  destroy_ = [](void*, void*) {};
	} else {
	  destroy_ = [](void* s, void*) {
		std::launder(reinterpret_cast<FnT*>(s))->~FnT();
	  };
	}

	type_index_ = get_type_index<FnT>();
  }

  /**
   * @brief Swap contents with another small_function.
   *
   * @param other The small_function to swap with.
   *
   * @note This operation is noexcept and provides strong exception guarantee.
   */
  void swap(small_function& other) noexcept {
	if (this == &other) return;

	if (!call_ && !other.call_) return;

	if (uses_heap_ && other.uses_heap_) {
	  std::swap(payload_ptr_, other.payload_ptr_);
	  std::swap(allocated_size_, other.allocated_size_);
	  std::swap(call_, other.call_);
	  std::swap(move_, other.move_);
	  std::swap(destroy_, other.destroy_);
	  std::swap(type_index_, other.type_index_);
	  std::swap(trivially_copyable_stored_, other.trivially_copyable_stored_);
	  std::swap(uses_heap_, other.uses_heap_);
	  return;
	}

	if (!uses_heap_ && !other.uses_heap_ && trivially_copyable_stored_ &&
		other.trivially_copyable_stored_) {
	  unsigned char tmp[StorageSize];
	  std::memcpy(tmp, storage_inline_ptr(), StorageSize);
	  std::memcpy(storage_inline_ptr(), other.storage_inline_ptr(),
				  StorageSize);
	  std::memcpy(other.storage_inline_ptr(), tmp, StorageSize);

	  std::swap(call_, other.call_);
	  std::swap(move_, other.move_);
	  std::swap(destroy_, other.destroy_);
	  std::swap(type_index_, other.type_index_);
	  std::swap(trivially_copyable_stored_, other.trivially_copyable_stored_);
	  return;
	}

	small_function tmp = std::move(*this);
	*this = std::move(other);
	other = std::move(tmp);
  }

  /**
   * @brief Get the type index of the stored callable.
   *
   * @return type_index Type information of the stored callable, or empty if
   * none.
   */
  type_index target_type() const noexcept { return type_index_; }

  /**
   * @brief Get the human-readable type name of the stored callable.
   *
   * @return const char* Compiler-specific type name string, or nullptr if
   * empty.
   */
  const char* target_type_name() const noexcept { return type_index_.name; }

  /**
   * @brief Obtain pointer to stored callable of type T.
   *
   * @tparam T The expected type of the stored callable.
   * @return T* Pointer to the stored callable if types match, otherwise
   * nullptr.
   */
  template <typename T>
  T* target() noexcept {
	if (!call_) return nullptr;
	if (type_index_ == get_type_index<T>())
	  return std::launder(reinterpret_cast<T*>(storage_ptr()));
	return nullptr;
  }

  /**
   * @brief Obtain const pointer to stored callable of type T.
   *
   * @tparam T The expected type of the stored callable.
   * @return const T* Const pointer to the stored callable if types match,
   * otherwise nullptr.
   */
  template <typename T>
  const T* target() const noexcept {
	if (!call_) return nullptr;
	if (type_index_ == get_type_index<T>())
	  return std::launder(reinterpret_cast<const T*>(storage_ptr()));
	return nullptr;
  }

  /**
   * @brief Check if the stored callable is of type T.
   *
   * @tparam T The type to check against.
   * @return true if a callable is stored and it is of type T.
   */
  template <typename T>
  bool contains_type() const noexcept {
	return call_ && (type_index_ == get_type_index<T>());
  }

 private:
  using CallFn = R (*)(void*,
					   Args...); /**< Function pointer type for invocation. */
  using OpFn = void (*)(void*,
						void*); /**< Function pointer type for operations. */

  /**
   * @brief Move internal state from another small_function.
   *
   * @param other The small_function to move from.
   *
   * @post `other` is left in an empty but valid state.
   */
  void move_from(small_function&& other) noexcept {
	if (!other.call_) return;

	if (other.uses_heap_) {
	  payload_ptr_ = other.payload_ptr_;
	  allocated_size_ = other.allocated_size_;
	  uses_heap_ = true;

	  call_ = other.call_;
	  move_ = other.move_;
	  destroy_ = other.destroy_;
	  type_index_ = other.type_index_;
	  trivially_copyable_stored_ = other.trivially_copyable_stored_;

	  other.payload_ptr_ = nullptr;
	  other.allocated_size_ = 0;
	  other.uses_heap_ = false;
	  other.call_ = nullptr;
	  other.move_ = nullptr;
	  other.destroy_ = nullptr;
	  other.type_index_ = type_index{nullptr, 0};
	  other.trivially_copyable_stored_ = false;
	  return;
	}

	move_ = other.move_;
	destroy_ = other.destroy_;
	call_ = other.call_;
	type_index_ = other.type_index_;
	trivially_copyable_stored_ = other.trivially_copyable_stored_;
	uses_heap_ = false;
	payload_ptr_ = nullptr;
	allocated_size_ = 0;

	move_(storage_inline_ptr(), other.storage_inline_ptr());

	if (other.destroy_) other.destroy_(other.storage_inline_ptr(), nullptr);

	other.call_ = nullptr;
	other.move_ = nullptr;
	other.destroy_ = nullptr;
	other.type_index_ = type_index{nullptr, 0};
	other.trivially_copyable_stored_ = false;
  }

  /**
   * @brief Get pointer to inline storage.
   *
   * @return void* Pointer to the inline storage buffer.
   */
  void* storage_inline_ptr() noexcept { return static_cast<void*>(&storage_); }

  /**
   * @brief Get const pointer to inline storage.
   *
   * @return const void* Const pointer to the inline storage buffer.
   */
  const void* storage_inline_ptr() const noexcept {
	return static_cast<const void*>(&storage_);
  }

  /**
   * @brief Get pointer to active storage (inline or heap).
   *
   * @return void* Pointer to the active storage location.
   */
  void* storage_ptr() noexcept {
	return uses_heap_ ? payload_ptr_ : storage_inline_ptr();
  }

  /**
   * @brief Get const pointer to active storage (inline or heap).
   *
   * @return const void* Const pointer to the active storage location.
   */
  const void* storage_ptr() const noexcept {
	return uses_heap_ ? payload_ptr_ : storage_inline_ptr();
  }

  alignas(std::max_align_t)
	  std::byte storage_[StorageSize]; /**< Inline SBO buffer. */

  void* payload_ptr_ = nullptr;	   /**< Pointer for arena-allocated callable. */
  std::size_t allocated_size_ = 0; /**< Size of arena allocation. */
  bool uses_heap_ = false;		   /**< True if using arena allocation. */

  CallFn call_ = nullptr;  /**< Pointer to invocation thunk. */
  OpFn move_ = nullptr;	   /**< Pointer to move-construction thunk. */
  OpFn destroy_ = nullptr; /**< Pointer to destructor thunk. */
  type_index type_index_{nullptr, 0};	   /**< Stored callable type info. */
  bool trivially_copyable_stored_ = false; /**< Optimized swap path. */
};


/**
 * @brief Swap two small_function objects.
 *
 * @tparam Sig The function signature.
 * @tparam StorageSize The storage size in bytes.
 * @param a First small_function to swap.
 * @param b Second small_function to swap.
 */
template <typename Sig, std::size_t StorageSize>
void swap(small_function<Sig, StorageSize>& a,
		  small_function<Sig, StorageSize>& b) noexcept {
  a.swap(b);
}


/**
 * @brief small_function with 32 bytes of inline storage.
 */
template <typename Sig>
using small_function32 = small_function<Sig, 32>;

/**
 * @brief small_function with 64 bytes of inline storage.
 */
template <typename Sig>
using small_function64 = small_function<Sig, 64>;

/**
 * @brief small_function with 128 bytes of inline storage.
 */
template <typename Sig>
using small_function128 = small_function<Sig, 128>;

/**
 * @brief Compare small_function with nullptr for equality.
 *
 * @tparam Sig The function signature.
 * @tparam StorageSize The storage size in bytes.
 * @param f The small_function to check.
 * @return true if the small_function is empty.
 */
template <typename Sig, std::size_t StorageSize>
bool operator==(const small_function<Sig, StorageSize>& f,
				std::nullptr_t) noexcept {
  return !static_cast<bool>(f);
}

/**
 * @brief Compare small_function with nullptr for inequality.
 *
 * @tparam Sig The function signature.
 * @tparam StorageSize The storage size in bytes.
 * @param f The small_function to check.
 * @return true if the small_function contains a callable.
 */
template <typename Sig, std::size_t StorageSize>
bool operator!=(const small_function<Sig, StorageSize>& f,
				std::nullptr_t) noexcept {
  return static_cast<bool>(f);
}

}  // namespace cascade::utility