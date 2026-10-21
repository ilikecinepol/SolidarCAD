#pragma once

#include <QJsonValue>
#include <QString>

#include <cstddef>
#include <optional>
#include <variant>
#include <vector>

#include "model/EdgeReference.h"
#include "model/FaceReference.h"

namespace solidar::project {

enum class ProjectFormatV4ReferenceDecodeStatus {
  Valid,
  Invalid,
  Unsupported,
};

enum class ProjectFormatV4ReferenceVariant {
  V1,
  V2,
};

// Structural metadata from a fully parsed, recognized reference variant.
// It is not an ownership or resolvability verdict.
struct ProjectFormatV4ReferenceInspection {
  ProjectFormatV4ReferenceVariant variant{ProjectFormatV4ReferenceVariant::V1};
  BodyId bodyId{kInvalidBodyId};
  FeatureId topLevelFeatureId{kInvalidFeatureId};
  std::vector<FeatureId> immediateOriginFeatureIds;

  friend bool operator==(const ProjectFormatV4ReferenceInspection&,
                         const ProjectFormatV4ReferenceInspection&) = default;
};

using DecodedProjectFormatV4TopologyReference =
    std::variant<FaceReference, EdgeReference>;

struct ProjectFormatV4ReferenceDecodeResult {
  ProjectFormatV4ReferenceDecodeStatus status{
      ProjectFormatV4ReferenceDecodeStatus::Invalid};
  bool success{};
  std::optional<DecodedProjectFormatV4TopologyReference> reference;
  std::optional<ProjectFormatV4ReferenceInspection> inspection;
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
