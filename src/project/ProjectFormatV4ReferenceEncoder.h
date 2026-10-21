#pragma once

#include <QJsonObject>
#include <QString>

#include <cstddef>
#include <optional>

#include "model/EdgeReference.h"
#include "model/FaceReference.h"

namespace solidar::project {

enum class ProjectFormatV4ReferenceEncodeStatus {
  Valid,
  Invalid,
};

struct ProjectFormatV4ReferenceEncodeResult {
  ProjectFormatV4ReferenceEncodeStatus status{
      ProjectFormatV4ReferenceEncodeStatus::Invalid};
  std::optional<QJsonObject> reference;
  std::size_t provenanceRecordCount{};
  std::size_t localNameUtf8Bytes{};
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProjectFormatV4ReferenceEncodeStatus::Valid &&
           reference.has_value();
  }
};

// Pure P3a boundary for one tagged v4 topology reference. It serializes only
// the authoritative carrier stored by a consumer and does not inspect runtime
// topology, resolve a reference, materialize a Document, or write a file.
[[nodiscard]] ProjectFormatV4ReferenceEncodeResult
encodeProjectFormatV4TopologyReference(const FaceReference& reference);

[[nodiscard]] ProjectFormatV4ReferenceEncodeResult
encodeProjectFormatV4TopologyReference(const EdgeReference& reference);

}  // namespace solidar::project
