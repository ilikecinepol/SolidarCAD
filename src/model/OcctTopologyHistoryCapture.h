#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>

#include "model/TopologyLineageBuilder.h"

class BRepBuilderAPI_Transform;
class BRepPrimAPI_MakePrism;
class TopoDS_Edge;
class TopoDS_Face;
class TopoDS_Shape;

namespace solidar {

struct PrismProfileEdgeSeed {
  sketch::GeometryId geometryId{sketch::kInvalidGeometryId};
  std::shared_ptr<const TopoDS_Edge> edge;
};

// A caller-authored name for exactly one face in a coherent source snapshot.
// The adapter validates this key against the actual OCCT relation; it never
// invents instance/branch identity from traversal order or from the source.
struct TransformFaceSeed {
  TopologyProvenanceOrigin sourceOrigin;
  TopologyProvenanceKey outputProvenance;
};

// Captures all prism face history synchronously while maker is alive. The
// returned builder owns copied shapes/facts and retains no maker/list handles.
[[nodiscard]] std::optional<TopologyLineageBuilder>
capturePrismFaceHistory(
    BRepPrimAPI_MakePrism& maker, FeatureId producerFeatureId,
    SketchId sketchId, std::shared_ptr<const TopoDS_Face> profileFace,
    std::span<const PrismProfileEdgeSeed> profileEdges,
    std::string* error = nullptr);

// Captures a one-to-one transform of a coherent CompleteFaces snapshot.
// Each source face must have one explicit caller-authored output key. The
// adapter validates that the key describes the actual Retained/Modified OCCT
// relation and that its sole origin is the exact source identity.
[[nodiscard]] std::optional<TopologyLineageBuilder>
captureTransformFaceHistory(
    BRepBuilderAPI_Transform& maker, FeatureId producerFeatureId,
    std::shared_ptr<const TopoDS_Shape> makerInputShape,
    TopologySourceSnapshot source,
    std::span<const TransformFaceSeed> faceSeeds,
    std::string* error = nullptr);

}  // namespace solidar
