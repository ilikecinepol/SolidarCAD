#pragma once

#include <QJsonObject>
#include <QString>

#include <cstddef>
#include <optional>

#include "model/Document.h"

namespace solidar::project::detail {

struct ProjectFormatV4DocumentMetadata {
  QString name;
  QString createdAt;
};

enum class ProjectFormatV4DocumentEncodeStatus {
  Valid,
  Invalid,
  PayloadTooLarge,
};

struct ProjectFormatV4DocumentEncodeResult {
  ProjectFormatV4DocumentEncodeStatus status{
      ProjectFormatV4DocumentEncodeStatus::Invalid};
  std::optional<QJsonObject> root;
  std::size_t topologyReferenceCount{};
  std::size_t provenanceRecordCount{};
  std::size_t localNameUtf8Bytes{};
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProjectFormatV4DocumentEncodeStatus::Valid &&
           root.has_value();
  }
};

// Disconnected P3b seam. It serializes the carriers already stored by the
// typed model into a complete v4 root and validates that root through the P2
// pipeline. It never resolves topology, recomputes/materializes a Document,
// writes a file, or changes the production ProjectFile dispatcher.
[[nodiscard]] ProjectFormatV4DocumentEncodeResult
encodeProjectFormatV4DocumentRoot(
    const Document& document,
    const ProjectFormatV4DocumentMetadata& metadata);

}  // namespace solidar::project::detail
