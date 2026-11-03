#pragma once

#include <QString>

#include "model/ProductDocument.h"

namespace solidar::project {

enum class ProductEnvelopeV5RelocationStatus {
  Relocated,
  InvalidProduct,
  InvalidPath,
  SourceMissing,
  SourceNotRegular,
  AmbiguousPath,
  MultipleLinks,
  SameFile,
  DestinationExists,
  CrossVolume,
  SourceChanged,
  DestinationChanged,
  UnsupportedFilesystem,
  RenameFailed,
};

struct ProductEnvelopeV5RelocationResult {
  ProductEnvelopeV5RelocationStatus status{
      ProductEnvelopeV5RelocationStatus::InvalidProduct};
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProductEnvelopeV5RelocationStatus::Relocated;
  }
};

// Whether this build has a native primitive that can hold the validated
// source and destination namespace stable through an atomic no-replace move.
[[nodiscard]] bool productEnvelopeV5RelocationIsSupported() noexcept;

// Moves an existing canonical single-part v5 file without serializing a new
// file or changing ProductDocument identity/model state. The destination must
// be absent and on the same volume.
[[nodiscard]] ProductEnvelopeV5RelocationResult relocateProductEnvelopeV5(
    const QString& sourcePath, const QString& targetPath,
    const ProductDocument& productDocument) noexcept;

}  // namespace solidar::project
