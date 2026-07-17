#include "scalanative/support/Arena.h"

#include <algorithm>
#include <memory>
#include <new>

namespace scalanative::support {

Arena::Arena(std::size_t blockSize)
    : blockSize_(std::max<std::size_t>(blockSize, 4096)) {}

void* Arena::allocate(std::size_t bytes, std::size_t alignment) {
  bytes = std::max<std::size_t>(bytes, 1);
  alignment = std::max<std::size_t>(alignment, alignof(std::max_align_t));

  if (blocks_.empty()) {
    addBlock(bytes + alignment);
  }

  if (void* ptr = tryAllocate(blocks_.back(), bytes, alignment)) {
    return ptr;
  }

  addBlock(bytes + alignment);
  if (void* ptr = tryAllocate(blocks_.back(), bytes, alignment)) {
    return ptr;
  }

  throw std::bad_alloc();
}

void Arena::reset() { blocks_.clear(); }

void* Arena::tryAllocate(Block& block, std::size_t bytes,
                         std::size_t alignment) {
  void* current = block.data.get() + block.used;
  std::size_t space = block.size - block.used;
  void* aligned = std::align(alignment, bytes, current, space);
  if (aligned == nullptr) {
    return nullptr;
  }

  auto* alignedByte = static_cast<std::byte*>(aligned);
  block.used = static_cast<std::size_t>(alignedByte - block.data.get()) + bytes;
  return aligned;
}

void Arena::addBlock(std::size_t minSize) {
  const std::size_t size = std::max(blockSize_, minSize);
  blocks_.push_back(Block{std::make_unique<std::byte[]>(size), size, 0});
}

} // namespace scalanative::support

