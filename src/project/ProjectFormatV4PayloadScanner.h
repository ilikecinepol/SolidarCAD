#pragma once

#include <QJsonValue>
#include <QString>

#include <optional>
#include <vector>

#include "model/ModelIds.h"
#include "model/TopologyReference.h"
#include "project/ProjectFormatV4EnvelopeDecoder.h"

namespace solidar::project {

enum class ProjectFormatV4PayloadScanStatus {
  Candidate,
  Invalid,
};

enum class ProjectFormatV4TopologySiteFamily {
  SketchSupport,
  ExtrudeFace,
  FilletEdge,
  ChamferEdge,
  MirrorPlaneFace,
  ShellRemovedFace,
  DraftNeutralPlaneFace,
  DraftRotationEdge,
  DraftedFace,
};

struct ProjectFormatV4TopologySite {
  ProjectFormatV4TopologySiteFamily family{
      ProjectFormatV4TopologySiteFamily::SketchSupport};
  QString path;
  TopologyKind expectedKind{TopologyKind::Face};
  QJsonValue authoredReference;
  std::optional<BodyId> containingBodyId;
  std::optional<FeatureId> containingFeatureId;
};

struct ProjectFormatV4FeatureOwner {
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};

  friend bool operator==(const ProjectFormatV4FeatureOwner&,
                         const ProjectFormatV4FeatureOwner&) = default;
};

struct ProjectFormatV4PayloadCandidate {
  ProjectFormatV4EnvelopeCandidate envelope;
  std::vector<ProjectFormatV4TopologySite> topologySites;
  // Authored structural inventory only; this is not an ownership verdict.
  std::vector<ProjectFormatV4FeatureOwner> featureOwners;
};

struct ProjectFormatV4PayloadScanResult {
  ProjectFormatV4PayloadScanStatus status{
      ProjectFormatV4PayloadScanStatus::Invalid};
  std::optional<ProjectFormatV4PayloadCandidate> candidate;
  QString error;

  [[nodiscard]] bool hasCandidate() const noexcept {
    return status == ProjectFormatV4PayloadScanStatus::Candidate &&
           candidate.has_value();
  }
};

// P2c validates only the bounded v3-compatible payload shells and enumerates
// authored topology-reference embedding sites. Reference objects themselves
// remain opaque for the P2a decoder and later ownership validation stages.
// ImportedShape B-Rep data is checked only as a bounded non-empty string;
// base64 decoding validity remains deferred to the payload decoder.
[[nodiscard]] ProjectFormatV4PayloadScanResult scanProjectFormatV4Payload(
    ProjectFormatV4EnvelopeCandidate envelope);

}  // namespace solidar::project
