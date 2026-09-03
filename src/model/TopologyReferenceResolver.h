#pragma once

#include <optional>
#include <string>

#include "model/TopologyReference.h"

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

class TopoDS_Shape;

namespace solidar {

struct EdgeReference;
struct FaceReference;

inline constexpr double kTopologyPositionRelativeTolerance = 0.20;
inline constexpr double kTopologyMeasureRelativeTolerance = 0.35;
inline constexpr double kTopologyDirectionCosineTolerance = 0.996;
inline constexpr double kTopologyAmbiguityScoreTolerance = 0.01;

enum class TopologyMatchMethod { None, SemanticTag, GeometricSignature, LegacyIndex };

template <typename Subshape>
struct TopologyResolution {
  std::optional<Subshape> subshape;
  std::size_t index{};
  TopologyMatchMethod method{TopologyMatchMethod::None};
  std::string error;
  [[nodiscard]] explicit operator bool() const noexcept {
    return subshape.has_value();
  }
};

using FaceResolution = TopologyResolution<TopoDS_Face>;
using EdgeResolution = TopologyResolution<TopoDS_Edge>;

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

[[nodiscard]] std::optional<TopoDS_Edge> resolveEdge(
    const TopoDS_Shape& shape, std::size_t edgeIndex);
[[nodiscard]] std::optional<TopoDS_Edge> resolveEdge(
    const TopoDS_Shape& shape, const TopologyReference& reference);

}  // namespace solidar
