#pragma once

#include <memory>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "model/SketchPlacement.h"
#include "model/OperationFailure.h"

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

class TopoDS_Shape;

namespace solidar {

// Ordinary parametric edits can translate a supported face or stretch an edge
// by a substantial fraction of the model diagonal.  Surface/curve kind,
// measure and direction still constrain the candidate set before scoring.
inline constexpr double kTopologyPositionRelativeTolerance = 0.35;
inline constexpr double kTopologyMeasureRelativeTolerance = 0.35;
inline constexpr double kTopologyDirectionCosineTolerance = 0.996;
inline constexpr double kTopologyAmbiguityScoreTolerance = 0.01;

enum class TopologyMatchMethod { None, SemanticTag, GeometricSignature, LegacyIndex };

enum class TopologyResolutionFailure {
  None,
  InvalidReference,
  Missing,
  Ambiguous,
  Unexpected,
};

[[nodiscard]] constexpr OperationFailureCode operationFailureCode(
    TopologyResolutionFailure failure) noexcept {
  switch (failure) {
    case TopologyResolutionFailure::None:
      return OperationFailureCode::None;
    case TopologyResolutionFailure::InvalidReference:
      return OperationFailureCode::TopologyReferenceInvalid;
    case TopologyResolutionFailure::Missing:
      return OperationFailureCode::TopologyReferenceMissing;
    case TopologyResolutionFailure::Ambiguous:
      return OperationFailureCode::TopologyReferenceAmbiguous;
    case TopologyResolutionFailure::Unexpected:
      return OperationFailureCode::TopologyResolutionUnexpected;
  }
  return OperationFailureCode::TopologyResolutionUnexpected;
}

template <typename Subshape>
struct TopologyResolution {
  std::optional<Subshape> subshape;
  std::size_t index{};
  TopologyMatchMethod method{TopologyMatchMethod::None};
  TopologyResolutionFailure failure{TopologyResolutionFailure::None};
  std::string error;
  [[nodiscard]] explicit operator bool() const noexcept {
    return subshape.has_value();
  }
};

using FaceResolution = TopologyResolution<TopoDS_Face>;
using EdgeResolution = TopologyResolution<TopoDS_Edge>;

struct FaceReferenceCreation {
  FaceReference reference;
  std::string error;
  [[nodiscard]] explicit operator bool() const noexcept {
    return reference.bodyId != kInvalidBodyId &&
           reference.featureId != kInvalidFeatureId &&
           reference.signature.has_value();
  }
};

struct EdgeReferenceCreation {
  EdgeReference reference;
  std::string error;
  [[nodiscard]] explicit operator bool() const noexcept {
    return reference.bodyId != kInvalidBodyId &&
           reference.featureId != kInvalidFeatureId &&
           reference.signature.has_value();
  }
};

// Immutable per-shape snapshot. It owns the TopoDS_Shape handle, builds each
// signature exactly once, and maps every raw traversal ordinal onto a unique
// IsSame canonical subshape.
class TopologyIndex final {
 public:
  using ShapePtr = std::shared_ptr<const TopoDS_Shape>;
  struct Data;

  [[nodiscard]] static std::shared_ptr<const TopologyIndex> build(
      ShapePtr shape, ShapeRevision revision = kInvalidShapeRevision,
      std::string* error = nullptr);
  [[nodiscard]] static std::shared_ptr<const TopologyIndex> build(
      const TopoDS_Shape& shape,
      ShapeRevision revision = kInvalidShapeRevision,
      std::string* error = nullptr);
  // Monotonic diagnostic counter. Tests compare snapshots; production never
  // resets it, so concurrent index creation cannot invalidate observations.
  [[nodiscard]] static std::uint64_t buildAttemptCount() noexcept;

  [[nodiscard]] const ShapePtr& shape() const noexcept;
  [[nodiscard]] ShapeRevision revision() const noexcept;
  [[nodiscard]] std::size_t faceCount() const noexcept;
  [[nodiscard]] std::size_t edgeCount() const noexcept;
  [[nodiscard]] std::size_t rawFaceCount() const noexcept;
  [[nodiscard]] std::size_t rawEdgeCount() const noexcept;

  [[nodiscard]] FaceResolution resolveFace(
      const TopologyReference& reference) const;
  [[nodiscard]] EdgeResolution resolveEdge(
      const TopologyReference& reference) const;
  [[nodiscard]] std::vector<FaceResolution> resolveFaces(
      std::span<const TopologyReference> references) const;
  [[nodiscard]] std::vector<EdgeResolution> resolveEdges(
      std::span<const TopologyReference> references) const;
  [[nodiscard]] FaceReferenceCreation createFaceReference(
      BodyId bodyId, FeatureId featureId, std::size_t rawFaceIndex,
      std::string semanticTag = {}) const;
  [[nodiscard]] EdgeReferenceCreation createEdgeReference(
      BodyId bodyId, FeatureId featureId, std::size_t rawEdgeIndex,
      std::string semanticTag = {}) const;

 private:
  explicit TopologyIndex(std::shared_ptr<const Data> data) noexcept;
  std::shared_ptr<const Data> data_;
};

[[nodiscard]] FaceReferenceCreation createFaceReference(
    const TopologyIndex& index, BodyId bodyId, FeatureId featureId,
    std::size_t faceIndex, std::string semanticTag = {});
[[nodiscard]] EdgeReferenceCreation createEdgeReference(
    const TopologyIndex& index, BodyId bodyId, FeatureId featureId,
    std::size_t edgeIndex, std::string semanticTag = {});

[[nodiscard]] FaceReference makeFaceReference(
    const TopoDS_Shape& shape, BodyId bodyId, FeatureId featureId,
    std::size_t faceIndex, std::string semanticTag = {});
[[nodiscard]] EdgeReference makeEdgeReference(
    const TopoDS_Shape& shape, BodyId bodyId, FeatureId featureId,
    std::size_t edgeIndex, std::string semanticTag = {});

[[nodiscard]] FaceResolution resolveFaceReference(
    const TopoDS_Shape& shape, const TopologyReference& reference);
[[nodiscard]] EdgeResolution resolveEdgeReference(
    const TopoDS_Shape& shape, const TopologyReference& reference);
[[nodiscard]] FaceResolution resolveFaceReference(
    const TopologyIndex& index, const TopologyReference& reference);
[[nodiscard]] EdgeResolution resolveEdgeReference(
    const TopologyIndex& index, const TopologyReference& reference);
[[nodiscard]] std::vector<FaceResolution> resolveFaceReferences(
    const TopologyIndex& index,
    std::span<const TopologyReference> references);
[[nodiscard]] std::vector<EdgeResolution> resolveEdgeReferences(
    const TopologyIndex& index,
    std::span<const TopologyReference> references);

[[nodiscard]] std::optional<TopoDS_Edge> resolveEdge(
    const TopoDS_Shape& shape, std::size_t edgeIndex);
[[nodiscard]] std::optional<TopoDS_Edge> resolveEdge(
    const TopoDS_Shape& shape, const TopologyReference& reference);

}  // namespace solidar
