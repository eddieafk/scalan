#pragma once

#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace scalanative::support {

template <typename T> class GcPtr {
public:
  constexpr GcPtr() = default;
  explicit constexpr GcPtr(T* ptr)
      : ptr_(ptr) {}

  [[nodiscard]] constexpr T* get() const { return ptr_; }
  [[nodiscard]] constexpr T& operator*() const { return *ptr_; }
  [[nodiscard]] constexpr T* operator->() const { return ptr_; }
  explicit constexpr operator bool() const { return ptr_ != nullptr; }

private:
  T* ptr_ = nullptr;
};

class GcHeap {
public:
  GcHeap() = default;
  GcHeap(const GcHeap&) = delete;
  GcHeap& operator=(const GcHeap&) = delete;

  template <typename T, typename... Args> [[nodiscard]] GcPtr<T> make(Args&&... args) {
    static_assert(!std::is_array_v<T>, "GcHeap stores objects, not arrays");
    auto object = std::make_shared<T>(std::forward<Args>(args)...);
    T* ptr = object.get();
    objects_.push_back(std::move(object));
    return GcPtr<T>(ptr);
  }

  [[nodiscard]] std::size_t objectCount() const { return objects_.size(); }
  void collectNow() {}

private:
  std::vector<std::shared_ptr<void>> objects_;
};

} // namespace scalanative::support

