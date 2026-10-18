#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "model/TopologyLineage.h"
#include "sketch/Sketch.h"

class TopoDS_Shape;

namespace solidar {

// Immutable source evidence held across the transient lifetime of an OCCT
// maker/history object. No maker-owned list or raw subshape pointer escapes.
struct TopologySourceSnapshot {
  FeatureId featureId{kInvalidFeatureId};
  std::shared_ptr<const TopoDS_Shape> shape;
  std::shared_ptr<const TopologyLineage> lineage;
};

enum class ShapeHistoryFactKind : std::uint8_t {
  Seed,
  Generated,
  Modified,
  Retained,
  Deleted,
};

// Bounded, typed identity for an authored root. The token deliberately does
// not encode identity in a string and does not retain a Sketch object.
enum class AuthoredTopologySourceKind : std::uint8_t {
  ProfileStartCap,
  ProfileEndCap,
  GeometrySide,
};

struct AuthoredTopologyToken {
  AuthoredTopologySourceKind kind{AuthoredTopologySourceKind::GeometrySide};
  SketchId sketchId{kInvalidSketchId};
  sketch::GeometryId geometryId{sketch::kInvalidGeometryId};

  friend bool operator==(const AuthoredTopologyToken&,
                         const AuthoredTopologyToken&) = default;
};

// Canonical typed authored-root projection. Caps are identified by their
// SketchId alone; GeometrySide additionally requires a GeometryId.
[[nodiscard]] std::optional<TopologyProvenanceKey>
projectAuthoredTopologyToken(const AuthoredTopologyToken& token,
                             FeatureId producerFeatureId,
                             std::string* error = nullptr);

struct HistoryFact {
  ShapeHistoryFactKind kind{ShapeHistoryFactKind::Generated};
  // Seed uses authoredSource. All other relations use an exact, strongly
  // owned source identity/candidate pair copied out of transient history.
  std::optional<AuthoredTopologyToken> authoredSource;
  std::optional<TopologyProvenanceOrigin> source;
  std::shared_ptr<const TopoDS_Shape> sourceSubshape;
  // Deleted facts intentionally have neither output field.
  std::optional<TopologyProvenanceKey> outputProvenance;
  std::shared_ptr<const TopoDS_Shape> outputSubshape;
};

struct ShapeHistoryArtifact {
  FeatureId producerFeatureId{kInvalidFeatureId};
  std::shared_ptr<const TopoDS_Shape> outputShape;
  TopologyLineageCoverage coverage{TopologyLineageCoverage::None};
  std::vector<TopologySourceSnapshot> sources;
  std::vector<HistoryFact> facts;
};

[[nodiscard]] bool validateTopologySourceSnapshot(
    const TopologySourceSnapshot& snapshot, std::string* error = nullptr);
[[nodiscard]] bool validateHistoryFact(
    const HistoryFact& fact, FeatureId producerFeatureId,
    std::span<const TopologySourceSnapshot> sources,
    std::string* error = nullptr);
[[nodiscard]] bool validateShapeHistoryArtifact(
    const ShapeHistoryArtifact& artifact, std::string* error = nullptr);

// Neutral, Qt-free seam between future OCCT producer adapters and the
// immutable lineage carrier. It owns output/source snapshots and copied facts.
class TopologyLineageBuilder final {
 public:
  [[nodiscard]] static std::optional<TopologyLineageBuilder> create(
      ShapeHistoryArtifact artifact, std::string* error = nullptr);

  [[nodiscard]] const ShapeHistoryArtifact& artifact() const noexcept;
  [[nodiscard]] std::shared_ptr<const TopologyLineage> build(
      std::string* error = nullptr) const;

 private:
  explicit TopologyLineageBuilder(ShapeHistoryArtifact artifact);

  ShapeHistoryArtifact artifact_;
};

}  // namespace solidar
