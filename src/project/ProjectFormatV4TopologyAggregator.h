#pragma once

#include <QString>

#include <cstddef>
#include <optional>
#include <vector>

#include "project/ProjectFormatV4PayloadScanner.h"
#include "project/ProjectFormatV4ReferenceDecoder.h"

namespace solidar::project {

inline constexpr std::size_t kProjectFormatV4MaximumProvenanceRecords = 65536;
inline constexpr std::size_t kProjectFormatV4MaximumLocalNameUtf8Bytes =
    8 * 1024 * 1024;

enum class ProjectFormatV4TopologyAggregateStatus {
  Candidate,
  UnsupportedCandidate,
  Invalid,
};

struct ProjectFormatV4DeferredTopologyReference {
  QString path;
  QString diagnostic;

  friend bool operator==(const ProjectFormatV4DeferredTopologyReference&,
                         const ProjectFormatV4DeferredTopologyReference&) =
      default;
};

struct ProjectFormatV4TopologyAggregateCandidate {
  ProjectFormatV4PayloadCandidate payload;
  std::vector<std::optional<DecodedProjectFormatV4TopologyReference>>
      topologyReferences;
  // One entry per payload.topologySites item. Recognized reference variants
  // retain structural owner metadata even when their payload is unsupported;
  // future opaque referenceVersion values remain std::nullopt.
  std::vector<std::optional<ProjectFormatV4ReferenceInspection>>
      topologyInspections;
  std::vector<ProjectFormatV4DeferredTopologyReference> deferredUnsupported;
  std::size_t provenanceRecordCount{};
  std::size_t localNameUtf8Bytes{};
};

struct ProjectFormatV4TopologyAggregateResult {
  ProjectFormatV4TopologyAggregateStatus status{
      ProjectFormatV4TopologyAggregateStatus::Invalid};
  std::optional<ProjectFormatV4TopologyAggregateCandidate> candidate;
  QString error;

  [[nodiscard]] bool hasCandidate() const noexcept {
    return (status == ProjectFormatV4TopologyAggregateStatus::Candidate ||
            status ==
                ProjectFormatV4TopologyAggregateStatus::UnsupportedCandidate) &&
           candidate.has_value();
  }

  [[nodiscard]] bool allTopologyReferencesDecoded() const noexcept;
};

// Bounded structural aggregation only. It revalidates the payload DTO, decodes
// each authored topology reference, accounts document-wide budgets and checks
// persisted owner IDs. It does not resolve topology or materialize a Document.
[[nodiscard]] ProjectFormatV4TopologyAggregateResult
aggregateProjectFormatV4Topology(ProjectFormatV4PayloadCandidate payload);

}  // namespace solidar::project
