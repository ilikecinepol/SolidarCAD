#pragma once

#include <QJsonValue>
#include <QString>

#include <cstddef>
#include <optional>
#include <variant>

#include "model/EdgeReference.h"
#include "model/FaceReference.h"

namespace solidar::project {

enum class ProjectFormatV4ReferenceDecodeStatus {
  Valid,
  Invalid,
  Unsupported,
};

using DecodedProjectFormatV4TopologyReference =
    std::variant<FaceReference, EdgeReference>;

struct ProjectFormatV4ReferenceDecodeResult {
  ProjectFormatV4ReferenceDecodeStatus status{
      ProjectFormatV4ReferenceDecodeStatus::Invalid};
  bool success{};
  std::optional<DecodedProjectFormatV4TopologyReference> reference;
  std::size_t provenanceRecordCount{};
  std::size_t localNameUtf8Bytes{};
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return success &&
           status == ProjectFormatV4ReferenceDecodeStatus::Valid &&
           reference.has_value();
  }
};

// Decoder-only P2a boundary for one tagged v4 topology reference. It does not
// validate document ownership, embedding wrappers, capabilities, or
// document-wide budgets and is intentionally disconnected from stageLoad and
// every writer/application path.
[[nodiscard]] ProjectFormatV4ReferenceDecodeResult
decodeProjectFormatV4TopologyReference(const QJsonValue& value,
                                       TopologyKind expectedKind);

}  // namespace solidar::project
