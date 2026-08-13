#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace scalanative::support {

class Arena {
public:
  explicit Arena(std::size_t blockSize = 64 * 1024);
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) noexcept = default;
  Arena& operator=(Arena&&) noexcept = default;

  [[nodiscard]] void* allocate(std::size_t bytes, std::size_t alignment);
  void reset();

  template <typename T, typename... Args> [[nodiscard]] T* make(Args&&... args) {
    void* storage = allocate(sizeof(T), alignof(T));
    return new (storage) T(std::forward<Args>(args)...);
  }

private:
  struct Block {
    std::unique_ptr<std::byte[]> data;
    std::size_t size = 0;
    std::size_t used = 0;
  };

  [[nodiscard]] void* tryAllocate(Block& block, std::size_t bytes,
                                  std::size_t alignment);
  void addBlock(std::size_t minSize);

  std::size_t blockSize_;
  std::vector<Block> blocks_;
};

} // namespace scalanative::support

