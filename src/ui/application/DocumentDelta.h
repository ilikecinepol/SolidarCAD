#pragma once

#include <cstddef>
#include <memory>
#include <utility>

namespace solidar {

class Document;

// Cold, Qt-free difference between two valid Document states.  The opaque
// storage owns only authored model slices prepared for history; transient
// OCCT shapes are discarded by the builder.  A delta is immutable and may be
// shared by the standalone and assembly histories without sharing a live
// Document owner.
class DocumentDelta final {
 public:
  class Storage;

  DocumentDelta() noexcept = default;

  [[nodiscard]] static DocumentDelta create(const Document& before,
                                            const Document& after);

  void applyForward(Document& document) const;
  void applyBackward(Document& document) const;
  // Definition-edit transactions let ProductDocument own the sole validation
  // recompute immediately before the atomic authoritative swap.
  void applyForwardUnrecomputed(Document& document) const;
  void applyBackwardUnrecomputed(Document& document) const;

  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::size_t sliceCount() const noexcept;
  [[nodiscard]] std::size_t retainedBytes() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept {
    return storage_ != nullptr;
  }

 private:
  explicit DocumentDelta(std::shared_ptr<const Storage> storage) noexcept
      : storage_(std::move(storage)) {}

  std::shared_ptr<const Storage> storage_;
};

}  // namespace solidar
