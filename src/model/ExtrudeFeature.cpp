#include "model/ExtrudeFeature.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <span>
#include <utility>
#include <variant>

#include "model/FaceBoundaryEdgeProvenance.h"
#include "model/FaceExtrudeBuilder.h"
#include "model/OcctBooleanTopologyHistoryCapture.h"
#include "model/OcctFacePrismTopologyHistoryCapture.h"
#include "model/SketchExtrudeBuilder.h"
#include "model/TopologyLineage.h"
#include "model/TopologyReferenceResolver.h"

namespace solidar {
namespace {

constexpr double kSelectionMatchTolerance = 1e-7;

bool sameValue(double first, double second) noexcept {
  return std::abs(first - second) <= kSelectionMatchTolerance;
}

bool samePoint(sketch::Point first, sketch::Point second) noexcept {
  return sameValue(first.xMm, second.xMm) &&
         sameValue(first.yMm, second.yMm);
}

bool sameLine(const sketch::Line& first, const sketch::Line& second) noexcept {
  return first.dashed == second.dashed &&
         ((samePoint(first.start, second.start) &&
           samePoint(first.end, second.end)) ||
          (samePoint(first.start, second.end) &&
           samePoint(first.end, second.start)));
}

bool sameCircle(const sketch::Circle& first,
                const sketch::Circle& second) noexcept {
  return first.dashed == second.dashed &&
         samePoint(first.center, second.center) &&
         sameValue(first.radiusMm, second.radiusMm);
}

bool sameCircleIdentity(const sketch::Circle& first,
                        const sketch::Circle& second) noexcept {
  return first.dashed == second.dashed &&
         samePoint(first.center, second.center);
}

bool sameArc(const sketch::Arc& first, const sketch::Arc& second) noexcept {
  return first.dashed == second.dashed &&
         samePoint(first.center, second.center) &&
         sameValue(first.radiusMm, second.radiusMm) &&
         sameValue(first.startAngleRad, second.startAngleRad) &&
         sameValue(first.sweepAngleRad, second.sweepAngleRad);
}

bool sameArcIdentity(const sketch::Arc& first,
                     const sketch::Arc& second) noexcept {
  return first.dashed == second.dashed &&
         samePoint(first.center, second.center) &&
         sameValue(first.startAngleRad, second.startAngleRad) &&
         sameValue(first.sweepAngleRad, second.sweepAngleRad);
}

bool sameBezier(const sketch::Bezier& first,
                const sketch::Bezier& second) noexcept {
  if (first.dashed != second.dashed) return false;
  for (std::size_t index = 0; index < first.points.size(); ++index)
    if (!samePoint(first.points[index], second.points[index])) return false;
  return true;
}

std::size_t solidCount(const TopoDS_Shape& shape) {
  std::size_t count = 0;
  for (TopExp_Explorer solids(shape, TopAbs_SOLID); solids.More(); solids.Next())
    ++count;
  return count;
}

bool containsFace(const TopoDS_Shape& shape, const TopoDS_Shape& face) {
  for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More(); faces.Next())
    if (faces.Current().IsSame(face)) return true;
  return false;
}

bool validateRawBooleanResult(const TopoDS_Shape& baseShape,
                              const TopoDS_Shape& result,
                              ExtrudeOperation operation,
                              std::string* error) {
  const std::size_t beforeSolidCount = solidCount(baseShape);
  const std::size_t resultSolidCount = solidCount(result);
  if (resultSolidCount == 0) {
    if (error) *error = "Extrude result does not contain a solid";
    return false;
  }
  if (operation == ExtrudeOperation::Join &&
      resultSolidCount > beforeSolidCount) {
    if (error) *error = "Extrude Join does not intersect the body";
    return false;
  }
  if (operation == ExtrudeOperation::Cut &&
      resultSolidCount > beforeSolidCount) {
    if (error) *error = "Extrude Cut would split the body";
    return false;
  }

  GProp_GProps beforeProperties;
  GProp_GProps afterProperties;
  BRepGProp::VolumeProperties(baseShape, beforeProperties);
  BRepGProp::VolumeProperties(result, afterProperties);
  const double before = beforeProperties.Mass();
  const double after = afterProperties.Mass();
  const double tolerance = std::max(1e-7, std::abs(before) * 1e-10);
  if (operation == ExtrudeOperation::Join && after <= before + tolerance) {
    if (error) *error = "Extrude Join does not intersect the body";
    return false;
  }
  if (operation == ExtrudeOperation::Cut && after >= before - tolerance) {
    if (error) *error = "Extrude Cut does not intersect the body";
    return false;
  }
  return true;
}

bool validateFinalBooleanResult(const TopoDS_Shape& baseShape,
                                const TopoDS_Shape& result,
                                ExtrudeOperation operation,
                                std::string* error) {
  if (!validateRawBooleanResult(baseShape, result, operation, error))
    return false;
  if (!BRepCheck_Analyzer(result).IsValid()) {
    if (error) *error = "Extrude result is invalid";
    return false;
  }
  return true;
}

bool validateImmediatePredecessor(const RebuildContext& context,
                                  const ShapeFeature& current,
                                  std::string* error) {
  const auto fail = [&] {
    if (error)
      *error = "Extrude base Feature is not the immediate predecessor";
    return false;
  };
  if (!context.body || !context.previousFeature || !context.previousShape ||
      context.previousFeature == &current)
    return fail();
  if (context.document.findBody(context.body->id()) != context.body)
    return fail();
  const auto currentIndex = context.body->featureIndex(current.id());
  if (!currentIndex || *currentIndex == 0 ||
      context.body->features()[*currentIndex].get() != &current)
    return fail();
  const auto* expected = context.body->features()[*currentIndex - 1].get();
  if (expected != context.previousFeature ||
      context.document.findFeature(context.body->id(), expected->id()) !=
          expected ||
      context.document.findBodyForFeature(expected->id()) != context.body)
    return fail();
  const auto& currentShape = expected->shape();
  if (!currentShape || currentShape.get() != context.previousShape ||
      !currentShape->IsSame(*context.previousShape))
    return fail();
  return true;
}

bool validateCapturedSnapshot(const BooleanTopologyCaptureResult& captured,
                              const TopologyLineage& lineage,
                              std::string* error) {
  if (!captured.outputShape || captured.outputShape->IsNull()) {
    if (error) *error = "Extrude face history output shape is invalid";
    return false;
  }
  if (lineage.coverage() != TopologyLineageCoverage::CompleteFaces) {
    if (error) *error = "Extrude face lineage is incomplete";
    return false;
  }
  for (const auto& binding : lineage.bindings()) {
    if (!binding.subshape || binding.subshape->IsNull() ||
        !containsFace(*captured.outputShape, *binding.subshape)) {
      if (error) *error = "Extrude captured face is outside the output shape";
      return false;
    }
  }
  return true;
}

struct StrictBooleanExtrusion {
  ShapeFeature::ShapePtr shape;
  std::shared_ptr<const TopologyLineage> lineage;
};

OperationFailureCode operationFailureCode(
    FaceBoundaryEdgeProvenanceFailureCode code) noexcept {
  switch (code) {
    case FaceBoundaryEdgeProvenanceFailureCode::MissingSelectedFace:
      return OperationFailureCode::TopologyReferenceMissing;
    case FaceBoundaryEdgeProvenanceFailureCode::AmbiguousSelectedFace:
      return OperationFailureCode::TopologyReferenceAmbiguous;
    case FaceBoundaryEdgeProvenanceFailureCode::InvalidSelectedFace:
    case FaceBoundaryEdgeProvenanceFailureCode::ForeignSelectedFace:
      return OperationFailureCode::TopologyReferenceInvalid;
    case FaceBoundaryEdgeProvenanceFailureCode::GeometryException:
      return OperationFailureCode::GeometryOperationFailed;
    default:
      return OperationFailureCode::TopologyIndexUnavailable;
  }
}

OperationFailureCode operationFailureCode(
    FacePrismTopologyHistoryFailureCode code) noexcept {
  switch (code) {
    case FacePrismTopologyHistoryFailureCode::MissingSelectedFace:
      return OperationFailureCode::TopologyReferenceMissing;
    case FacePrismTopologyHistoryFailureCode::AmbiguousSelectedFace:
      return OperationFailureCode::TopologyReferenceAmbiguous;
    case FacePrismTopologyHistoryFailureCode::InvalidSelectedFace:
    case FacePrismTopologyHistoryFailureCode::ForeignSelectedFace:
      return OperationFailureCode::TopologyReferenceInvalid;
    case FacePrismTopologyHistoryFailureCode::GeometryException:
      return OperationFailureCode::GeometryOperationFailed;
    default:
      return OperationFailureCode::TopologyIndexUnavailable;
  }
}

bool augmentStrictBooleanExtrusionEdges(FeatureId producerFeatureId,
                                        StrictBooleanExtrusion* result,
                                        std::string* error) {
  if (!result || !result->shape || !result->lineage) {
    if (error) *error = "Extrude edge lineage source is incomplete";
    return false;
  }
  FaceBoundaryEdgeProvenanceFailure failure;
  auto augmented = augmentCompleteFaceLineageWithUniqueManifoldEdges(
      {producerFeatureId, result->shape, result->lineage}, &failure);
  if (!augmented) {
    if (error)
      *error = failure.detail.empty()
                   ? "Extrude edge lineage augmentation failed"
                   : "Extrude edge lineage augmentation failed: " +
                         failure.detail;
    return false;
  }
  result->lineage = std::move(augmented->augmentedSource.lineage);
  return true;
}

template <typename Maker>
bool captureStrictBooleanExtrusion(
    Maker& maker, const ShapeFeature& baseFeature,
    const SketchExtrusionWithLineage& tool, FeatureId producerFeatureId,
    ExtrudeOperation operation, StrictBooleanExtrusion* result,
    std::string* error) {
  // Preserve the historical pre-unify solid/volume acceptance contract. In
  // particular, do not reject a raw Join B-Rep that OCCT's capture-time
  // SimplifyResult is expected to heal. B-Rep validity belongs to the final
  // captured/simplified snapshot below.
  if (!validateRawBooleanResult(*baseFeature.shape(), maker.Shape(), operation,
                                error))
    return false;
  const std::array<BooleanTopologySource, 2> sources{{
      {BooleanOperandRole::Object,
       {baseFeature.id(), baseFeature.shape(), baseFeature.topologyLineage()}},
      {BooleanOperandRole::Tool,
       {producerFeatureId, tool.shape, tool.lineage}},
  }};
  BooleanTopologyCaptureOptions options;
  options.simplifyResult = operation == ExtrudeOperation::Join;
  BooleanTopologyCaptureFailure failure;
  auto captured = captureBooleanFaceHistory(
      maker, producerFeatureId,
      std::span<const BooleanTopologySource>(sources), {}, options, &failure);
  if (!captured) {
    if (error)
      *error = failure.detail.empty()
                   ? "Extrude face history capture failed"
                   : std::move(failure.detail);
    return false;
  }
  if (!captured->outputShape || captured->outputShape->IsNull()) {
    if (error) *error = "Extrude face history output shape is invalid";
    return false;
  }
  if (!validateFinalBooleanResult(*baseFeature.shape(),
                                  *captured->outputShape, operation, error))
    return false;

  std::string lineageError;
  auto lineage = captured->builder.build(&lineageError);
  if (!lineage) {
    if (error)
      *error = lineageError.empty() ? "Extrude face lineage build failed"
                                    : std::move(lineageError);
    return false;
  }
  if (!validateCapturedSnapshot(*captured, *lineage, error)) return false;

  result->shape = std::move(captured->outputShape);
  result->lineage = std::move(lineage);
  return true;
}

bool buildStrictSketchBooleanExtrusion(
    const DocumentSketch& profile, const ShapeFeature& baseFeature,
    const TopoDS_Shape& baseShape, FeatureId producerFeatureId,
    double lengthMm, bool reversed, ExtrudeOperation operation,
    StrictBooleanExtrusion* result, std::string* error) {
  if (!result || !baseFeature.shape() ||
      !baseFeature.shape()->IsSame(baseShape)) {
    if (error) *error = "Extrude base snapshot is incoherent";
    return false;
  }

  SketchExtrusionWithLineage tool;
  std::string toolError;
  if (!buildSimpleNewBodyExtrusionWithLineage(
          profile, producerFeatureId, lengthMm, reversed, &tool,
          &toolError) ||
      !tool.shape || !tool.lineage ||
      tool.lineage->coverage() != TopologyLineageCoverage::CompleteFaces) {
    if (error)
      *error = toolError.empty() ? "Extrude tool face lineage is incomplete"
                                 : "Extrude " + toolError;
    return false;
  }

  if (operation == ExtrudeOperation::Join) {
    BRepAlgoAPI_Fuse fuse(baseShape, *tool.shape);
    fuse.Build();
    if (!fuse.IsDone() || fuse.Shape().IsNull()) {
      if (error) *error = "Extrude Join boolean fuse failed";
      return false;
    }
    return captureStrictBooleanExtrusion(fuse, baseFeature, tool,
                                         producerFeatureId, operation, result,
                                         error);
  }

  BRepAlgoAPI_Cut cut(baseShape, *tool.shape);
  cut.Build();
  if (!cut.IsDone() || cut.Shape().IsNull()) {
    if (error) *error = "Extrude Cut boolean cut failed";
    return false;
  }
  return captureStrictBooleanExtrusion(cut, baseFeature, tool,
                                       producerFeatureId, operation, result,
                                       error);
}

bool normalizeStrictFaceExtrusion(StrictBooleanExtrusion* result,
                                  std::string* error) {
  if (!result || !result->shape || result->shape->IsNull() ||
      !result->lineage ||
      result->lineage->coverage() !=
          TopologyLineageCoverage::CompleteFaces) {
    if (error) *error = "Extrude face result snapshot is incomplete";
    return false;
  }

  TopExp_Explorer solids(*result->shape, TopAbs_SOLID);
  if (!solids.More()) {
    if (error) *error = "Extrude face result does not contain a solid";
    return false;
  }
  const TopoDS_Shape singleSolid = solids.Current();
  solids.Next();
  if (solids.More()) {
    if (error) *error = "Extrude face result contains multiple solids";
    return false;
  }
  if (!BRepCheck_Analyzer(singleSolid).IsValid()) {
    if (error) *error = "Extrude face result is invalid";
    return false;
  }
  if (std::any_of(result->lineage->bindings().begin(),
                  result->lineage->bindings().end(),
                  [&](const auto& binding) {
                    return !binding.subshape || binding.subshape->IsNull() ||
                           !containsFace(singleSolid, *binding.subshape);
                  })) {
    if (error) *error = "Extrude face result changed captured faces";
    return false;
  }
  result->shape = std::make_shared<TopoDS_Shape>(singleSolid);
  return true;
}

bool buildStrictFaceBooleanExtrusion(
    const RebuildContext& context, const ShapeFeature& current,
    const FaceReference& selectedFace, double lengthMm, bool reversed,
    ExtrudeOperation operation, StrictBooleanExtrusion* result,
    std::string* error, OperationFailureCode* failureCode) {
  if (failureCode)
    *failureCode = OperationFailureCode::GeometryOperationFailed;
  if (!result || !selectedFace.isV2()) {
    if (failureCode)
      *failureCode = OperationFailureCode::TopologyReferenceInvalid;
    if (error) *error = "Extrude face reference is not v2";
    return false;
  }
  if (operation != ExtrudeOperation::Join &&
      operation != ExtrudeOperation::Cut) {
    if (failureCode) *failureCode = OperationFailureCode::InvalidInput;
    if (error) *error = "Extrude face operation is invalid";
    return false;
  }
  if (!validateImmediatePredecessor(context, current, error)) {
    if (failureCode)
      *failureCode = OperationFailureCode::TopologyReferenceMismatch;
    return false;
  }

  const ShapeFeature& baseFeature = *context.previousFeature;
  const auto baseLineage = baseFeature.topologyLineage();
  if (!baseLineage ||
      baseLineage->coverage() != TopologyLineageCoverage::CompleteFaces) {
    if (failureCode)
      *failureCode = OperationFailureCode::TopologyIndexUnavailable;
    if (error) *error = "Extrude base face lineage is incomplete";
    return false;
  }
  if (selectedFace.bodyId() != context.body->id()) {
    if (failureCode)
      *failureCode = OperationFailureCode::TopologyReferenceMismatch;
    if (error) *error = "Extrude face belongs to a different Body";
    return false;
  }
  if (selectedFace.featureId() != baseFeature.id()) {
    if (failureCode)
      *failureCode = OperationFailureCode::TopologyReferenceMismatch;
    if (error)
      *error = "Extrude face must belong to the immediate source Feature";
    return false;
  }

  // Classify the authored v2 provenance against the exact committed source
  // before the stricter boundary/history capture runs. Those later helpers
  // intentionally collapse several lineage-integrity failures, while the
  // application gate must retain Missing versus Ambiguous for this reference.
  std::string sourceTopologyError;
  const auto sourceTopology =
      baseFeature.topologyIndex(&sourceTopologyError);
  if (!sourceTopology) {
    if (failureCode)
      *failureCode = OperationFailureCode::TopologyIndexUnavailable;
    if (error)
      *error = sourceTopologyError.empty()
                   ? "Extrude source topology is unavailable"
                   : std::move(sourceTopologyError);
    return false;
  }
  const auto sourceResolution = sourceTopology->resolveFace(selectedFace);
  if (!sourceResolution) {
    if (failureCode)
      *failureCode = operationFailureCode(sourceResolution.failure);
    if (error)
      *error = "Extrude face could not be resolved: " +
               sourceResolution.error;
    return false;
  }

  const TopologySourceSnapshot baseSnapshot{
      baseFeature.id(), baseFeature.shape(), baseLineage};
  FaceBoundaryEdgeProvenanceFailure boundaryFailure;
  auto boundary = captureUniqueManifoldFaceBoundary(
      baseSnapshot, selectedFace, &boundaryFailure);
  if (!boundary) {
    if (failureCode)
      *failureCode = operationFailureCode(boundaryFailure.code);
    if (error)
      *error = boundaryFailure.detail.empty()
                   ? "Extrude face boundary provenance capture failed"
                   : std::move(boundaryFailure.detail);
    return false;
  }

  std::string topologyError;
  const auto topology = TopologyIndex::build(
      boundary->augmentedSource.shape, kInvalidShapeRevision,
      boundary->augmentedSource.lineage, &topologyError);
  if (!topology) {
    if (failureCode)
      *failureCode = OperationFailureCode::TopologyIndexUnavailable;
    if (error)
      *error = topologyError.empty()
                   ? "Extrude topology could not be indexed"
                   : "Extrude topology could not be indexed: " + topologyError;
    return false;
  }
  const auto resolved = topology->resolveFace(selectedFace);
  if (!resolved) {
    if (failureCode)
      *failureCode = operationFailureCode(resolved.failure);
    if (error)
      *error = "Extrude face could not be resolved: " + resolved.error;
    return false;
  }

  gp_Pln plane;
  gp_Dir normal;
  if (!resolveFacePlaneAndNormal(*resolved.subshape, &plane, &normal, error,
                                 failureCode))
    return false;
  const gp_Vec offset =
      gp_Vec(normal) * (reversed ? -lengthMm : lengthMm);
  BRepPrimAPI_MakePrism prism(*resolved.subshape, offset, false);
  prism.Build();
  if (!prism.IsDone() || prism.Shape().IsNull()) {
    if (error) *error = "Extrude face could not build a solid prism";
    return false;
  }

  FacePrismTopologyHistoryFailure prismFailure;
  auto prismBuilder = captureFacePrismTopologyHistory(
      prism, current.id(), selectedFace, *boundary, &prismFailure);
  if (!prismBuilder) {
    if (failureCode)
      *failureCode = operationFailureCode(prismFailure.code);
    if (error)
      *error = prismFailure.detail.empty()
                   ? "Extrude face prism history capture failed"
                   : std::move(prismFailure.detail);
    return false;
  }
  std::string lineageError;
  auto prismLineage = prismBuilder->build(&lineageError);
  const auto prismShape = prismBuilder->artifact().outputShape;
  if (!prismShape || prismShape->IsNull() || !prismLineage ||
      prismLineage->coverage() != TopologyLineageCoverage::CompleteFaces) {
    if (error)
      *error = lineageError.empty()
                   ? "Extrude face prism lineage is incomplete"
                   : std::move(lineageError);
    return false;
  }

  const std::array<BooleanTopologySource, 2> sources{{
      {BooleanOperandRole::Object, boundary->augmentedSource},
      {BooleanOperandRole::Tool, {current.id(), prismShape, prismLineage}},
  }};
  BooleanTopologyCaptureOptions options;
  options.simplifyResult = operation == ExtrudeOperation::Join;
  BooleanTopologyCaptureFailure booleanFailure;
  std::optional<BooleanTopologyCaptureResult> captured;
  if (operation == ExtrudeOperation::Join) {
    BRepAlgoAPI_Fuse fuse(*context.previousShape, *prismShape);
    fuse.Build();
    if (!fuse.IsDone() || fuse.Shape().IsNull()) {
      if (error) *error = "Extrude Join boolean fuse failed";
      return false;
    }
    if (!validateRawBooleanResult(*context.previousShape, fuse.Shape(),
                                  operation, error))
      return false;
    captured = captureBooleanFaceHistory(
        fuse, current.id(), std::span<const BooleanTopologySource>(sources),
        {}, options, &booleanFailure);
  } else {
    BRepAlgoAPI_Cut cut(*context.previousShape, *prismShape);
    cut.Build();
    if (!cut.IsDone() || cut.Shape().IsNull()) {
      if (error) *error = "Extrude Cut boolean cut failed";
      return false;
    }
    if (!validateRawBooleanResult(*context.previousShape, cut.Shape(),
                                  operation, error))
      return false;
    captured = captureBooleanFaceHistory(
        cut, current.id(), std::span<const BooleanTopologySource>(sources),
        {}, options, &booleanFailure);
  }
  if (!captured) {
    if (error)
      *error = booleanFailure.detail.empty()
                   ? "Extrude face history capture failed"
                   : std::move(booleanFailure.detail);
    return false;
  }
  if (!captured->outputShape || captured->outputShape->IsNull() ||
      !validateFinalBooleanResult(*context.previousShape,
                                  *captured->outputShape, operation, error))
    return false;

  auto finalLineage = captured->builder.build(&lineageError);
  if (!finalLineage) {
    if (error)
      *error = lineageError.empty() ? "Extrude face lineage build failed"
                                    : std::move(lineageError);
    return false;
  }
  if (!validateCapturedSnapshot(*captured, *finalLineage, error)) return false;

  result->shape = std::move(captured->outputShape);
  result->lineage = std::move(finalLineage);
  return normalizeStrictFaceExtrusion(result, error);
}

template <typename Primitive, typename Equal, typename SameIdentity,
          typename IdAt>
bool matchPrimitiveIds(const std::vector<Primitive>& source,
                       const std::vector<Primitive>& selection,
                       Equal equal, SameIdentity sameIdentity, IdAt idAt,
                       std::vector<sketch::GeometryId>* ids) {
  std::vector<bool> used(source.size(), false);
  ids->clear();
  ids->reserve(selection.size());
  for (const auto& selected : selection) {
    std::optional<std::size_t> matched;
    for (std::size_t index = 0; index < source.size(); ++index) {
      if (used[index] || !equal(source[index], selected)) continue;
      matched = index;
      break;
    }
    if (!matched) {
      // Recover a profile snapshot saved by an older build after its radius
      // was edited. Accept only one unambiguous topological identity match.
      for (std::size_t index = 0; index < source.size(); ++index) {
        if (used[index] || !sameIdentity(source[index], selected)) continue;
        if (matched) return false;
        matched = index;
      }
    }
    if (!matched) return false;
    used[*matched] = true;
    ids->push_back(idAt(*matched));
  }
  return true;
}

}  // namespace

ExtrudeFeature::ExtrudeFeature(SketchId profileSketchId, double lengthMm,
                               std::string name)
    : ExtrudeFeature(profileSketchId, lengthMm, std::move(name),
                     ExtrudeOperation::NewBody, false) {}

ExtrudeFeature::ExtrudeFeature(SketchId profileSketchId, double lengthMm,
                               std::string name, ExtrudeOperation operation,
                               bool reversed)
    : ShapeFeature(name.empty() ? "Extrude" : std::move(name)),
      source_(SketchExtrudeSource{profileSketchId}),
      lengthMm_(lengthMm), operation_(operation), reversed_(reversed) {}

ExtrudeFeature::ExtrudeFeature(FeatureId id, SketchId profileSketchId,
                               double lengthMm, std::string name)
    : ExtrudeFeature(id, profileSketchId, lengthMm, std::move(name),
                     ExtrudeOperation::NewBody, false) {}

ExtrudeFeature::ExtrudeFeature(FeatureId id, SketchId profileSketchId,
                               double lengthMm, std::string name,
                               ExtrudeOperation operation, bool reversed)
    : ShapeFeature(id, std::move(name)),
      source_(SketchExtrudeSource{profileSketchId}),
      lengthMm_(lengthMm), operation_(operation), reversed_(reversed) {}

ExtrudeFeature::ExtrudeFeature(FaceReference face, double lengthMm,
                               std::string name, ExtrudeOperation operation,
                               bool reversed)
    : ShapeFeature(name.empty() ? "Extrude" : std::move(name)),
      source_(FaceExtrudeSource{std::move(face)}),
      lengthMm_(lengthMm), operation_(operation), reversed_(reversed) {}

ExtrudeFeature::ExtrudeFeature(FeatureId id, FaceReference face,
                               double lengthMm, std::string name,
                               ExtrudeOperation operation, bool reversed)
    : ShapeFeature(id, std::move(name)),
      source_(FaceExtrudeSource{std::move(face)}),
      lengthMm_(lengthMm), operation_(operation), reversed_(reversed) {}

const ExtrudeSource& ExtrudeFeature::source() const noexcept { return source_; }

bool ExtrudeFeature::isFaceSource() const noexcept {
  return std::holds_alternative<FaceExtrudeSource>(source_);
}

SketchId ExtrudeFeature::profileSketchId() const noexcept {
  if (const auto* sketch = std::get_if<SketchExtrudeSource>(&source_))
    return sketch->sketchId;
  return kInvalidSketchId;
}

void ExtrudeFeature::setProfileSketchId(SketchId id) noexcept {
  if (!isFaceSource() && profileSketchId() == id) return;
  source_ = SketchExtrudeSource{id};
  profileOverride_.reset();
  profileSelectionIds_.reset();
  setDirty();
}

const std::optional<sketch::Sketch>&
ExtrudeFeature::profileOverride() const noexcept {
  return profileOverride_;
}

void ExtrudeFeature::setProfileOverride(
    std::optional<sketch::Sketch> profile) {
  profileOverride_ = std::move(profile);
  profileSelectionIds_.reset();
  setDirty();
}

std::optional<FaceReference> ExtrudeFeature::faceReference() const noexcept {
  if (const auto* face = std::get_if<FaceExtrudeSource>(&source_))
    return face->face;
  return std::nullopt;
}

double ExtrudeFeature::lengthMm() const noexcept { return lengthMm_; }

void ExtrudeFeature::setLengthMm(double value) noexcept {
  if (lengthMm_ == value) return;
  lengthMm_ = value;
  setDirty();
}

ExtrudeOperation ExtrudeFeature::operation() const noexcept { return operation_; }
void ExtrudeFeature::setOperation(ExtrudeOperation value) noexcept {
  if (operation_ == value) return;
  operation_ = value;
  setDirty();
}
bool ExtrudeFeature::reversed() const noexcept { return reversed_; }
void ExtrudeFeature::setReversed(bool value) noexcept {
  if (reversed_ == value) return;
  reversed_ = value;
  setDirty();
}


FeatureDependencies ExtrudeFeature::dependencies() const {
  FeatureDependencies result;
  const SketchId sketchId = profileSketchId();
  if (sketchId != kInvalidSketchId) result.sketchIds.push_back(sketchId);
  return result;
}

bool ExtrudeFeature::rebuildImpl(const RebuildContext& context) {
  if (!isFaceSource() &&
      (operation_ == ExtrudeOperation::Join ||
       operation_ == ExtrudeOperation::Cut) &&
      (context.previousFeature || context.previousShape)) {
    std::string contextError;
    if (!validateImmediatePredecessor(context, *this, &contextError)) {
      markError(std::move(contextError));
      return false;
    }
  }
  clearShape();
  if (!std::isfinite(lengthMm_) || lengthMm_ <= 0.0) {
    markError("Extrude length must be a finite positive value");
    return false;
  }

  if (const auto* faceSource = std::get_if<FaceExtrudeSource>(&source_)) {
    return rebuildFaceSource(context, *faceSource);
  }

  return rebuildSketchSource(context);
}

bool ExtrudeFeature::rebuildFaceSource(const RebuildContext& context,
                                       const FaceExtrudeSource& source) {
  if (operation_ == ExtrudeOperation::NewBody) {
    markError("Extrude New Body is not supported for a face source");
    return false;
  }
  if (!context.previousShape || context.previousShape->IsNull()) {
    markError(operation_ == ExtrudeOperation::Join
                  ? "Extrude Join base shape is missing"
                  : "Extrude Cut base shape is missing");
    return false;
  }
  if (!context.body || source.face.bodyId() != context.body->id()) {
    markError("Extrude face belongs to a different Body");
    return false;
  }
  if (!context.previousFeature ||
      source.face.featureId() != context.previousFeature->id()) {
    markError("Extrude face must belong to the immediate source Feature");
    return false;
  }
  try {
    std::string faceError;
    if (source.face.isV2()) {
      StrictBooleanExtrusion built;
      OperationFailureCode faceFailureCode{
          OperationFailureCode::GeometryOperationFailed};
      if (!buildStrictFaceBooleanExtrusion(
              context, *this, source.face, lengthMm_, reversed_, operation_,
              &built, &faceError, &faceFailureCode)) {
        markError(faceFailureCode,
                  faceError.empty() ? "Extrude face lineage rebuild failed"
                                    : std::move(faceError));
        return false;
      }
      if (!augmentStrictBooleanExtrusionEdges(id(), &built, &faceError)) {
        markError(std::move(faceError));
        return false;
      }
      setShapeAndTopologyLineage(std::move(built.shape),
                                 std::move(built.lineage));
      markValid();
      return true;
    }

    const auto topology = context.previousFeature->topologyIndex(&faceError);
    if (!topology) {
      markError(OperationFailureCode::TopologyIndexUnavailable,
                "Extrude topology could not be indexed: " + faceError);
      return false;
    }
    TopoDS_Shape result;
    FaceExtrudeGeometry geometry;
    OperationFailureCode faceFailureCode{
        OperationFailureCode::GeometryOperationFailed};
    if (!buildExtrusionFromFace(*context.previousShape, *topology, source.face,
                                lengthMm_, operation_, reversed_, &result,
                                &geometry, &faceError, nullptr,
                                &faceFailureCode)) {
      markError(faceFailureCode, "Extrude " + faceError);
      return false;
    }
    setShape(std::make_shared<TopoDS_Shape>(result));
    markValid();
    return true;
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    markError(OperationFailureCode::GeometryOperationFailed,
              message && *message
                  ? std::string("OCCT Extrude error: ") + message
                  : "OCCT Extrude operation failed");
    return false;
  } catch (const std::exception& failure) {
    markError(OperationFailureCode::GeometryOperationFailed,
              std::string("Extrude error: ") + failure.what());
    return false;
  } catch (...) {
    markError(OperationFailureCode::GeometryOperationFailed,
              "Unexpected Extrude geometry error");
    return false;
  }
}

bool ExtrudeFeature::rebuildSketchSource(const RebuildContext& context) {
  const SketchId profileId = profileSketchId();
  const auto* sourceProfile = context.document.findSketch(profileId);
  if (!sourceProfile) {
    markError("Extrude profile sketch was not found");
    return false;
  }

  // The document sketch may contain several closed contours.  A feature made
  // from one picked region must rebuild from that exact region, not from the
  // entire sketch.  Placement/support still come from the source DocumentSketch.
  DocumentSketch selectedProfile = *sourceProfile;
  if (profileOverride_) {
    std::string selectionError;
    if (!resolveProfileOverride(sourceProfile->geometry,
                                &selectedProfile.geometry,
                                &selectionError)) {
      markError("Extrude " + selectionError);
      return false;
    }
  }

  try {
    std::string profileError;
    if (operation_ == ExtrudeOperation::NewBody && !profileOverride_) {
      OperationFailureCode eligibilityCode = OperationFailureCode::None;
      std::string eligibilityError;
      if (isSupportedSingleSketchProfile(selectedProfile, &eligibilityError,
                                         &eligibilityCode)) {
        if (context.previousShape && !context.previousShape->IsNull()) {
          markError("Extrude New Body must be the first feature of a Body");
          return false;
        }
        SketchExtrusionWithLineage built;
        if (!buildSimpleNewBodyExtrusionWithLineage(
                selectedProfile, id(), lengthMm_, reversed_, &built,
                &profileError)) {
          markError("Extrude " + profileError);
          return false;
        }
        FaceBoundaryEdgeProvenanceFailure edgeFailure;
        const auto augmented =
            augmentCompleteFaceLineageWithUniqueManifoldEdges(
                TopologySourceSnapshot{id(), built.shape, built.lineage},
                &edgeFailure);
        if (!augmented) {
          markError("Extrude " +
                    (edgeFailure.detail.empty()
                         ? std::string("edge lineage is invalid")
                         : std::move(edgeFailure.detail)));
          return false;
        }
        setShapeAndTopologyLineage(std::move(built.shape),
                                   augmented->augmentedSource.lineage);
        markValid();
        return true;
      }
      // Only a profile that is valid under the existing multi-region
      // contract may intentionally remain on the legacy builder. Malformed
      // single-profile input must not turn a failed v2 eligibility check into
      // an implicit legacy retry.
      std::string legacyEligibilityError;
      OperationFailureCode legacyEligibilityCode = OperationFailureCode::None;
      if (!isSupportedSketchProfile(selectedProfile,
                                    &legacyEligibilityError,
                                    &legacyEligibilityCode)) {
        markError("Extrude " +
                  (legacyEligibilityError.empty() ? eligibilityError
                                                  : legacyEligibilityError));
        return false;
      }
    }

    if (operation_ == ExtrudeOperation::Join ||
        operation_ == ExtrudeOperation::Cut) {
      const ShapeFeature* baseFeature = context.previousFeature;
      const auto baseLineage =
          baseFeature ? baseFeature->topologyLineage()
                      : std::shared_ptr<const TopologyLineage>{};
      if (baseLineage && baseLineage->coverage() !=
                             TopologyLineageCoverage::CompleteFaces) {
        markError("Extrude base face lineage is incomplete");
        return false;
      }

      const bool useStrictLineage =
          baseLineage && !profileOverride_ &&
          isSupportedSingleSketchProfile(selectedProfile);
      if (useStrictLineage) {
        StrictBooleanExtrusion built;
        if (!context.previousShape || context.previousShape->IsNull() ||
            !buildStrictSketchBooleanExtrusion(
                selectedProfile, *baseFeature, *context.previousShape, id(),
                lengthMm_, reversed_, operation_, &built, &profileError)) {
          markError(profileError.empty()
                        ? (operation_ == ExtrudeOperation::Join
                               ? "Extrude Join base shape is missing"
                               : "Extrude Cut base shape is missing")
                        : std::move(profileError));
          return false;
        }
        if (!augmentStrictBooleanExtrusionEdges(id(), &built,
                                                &profileError)) {
          markError(std::move(profileError));
          return false;
        }
        setShapeAndTopologyLineage(std::move(built.shape),
                                   std::move(built.lineage));
        markValid();
        return true;
      }
    }

    TopoDS_Shape result;
    if (!buildExtrusionFromSketch(selectedProfile, context.previousShape,
                                  lengthMm_, operation_, reversed_, &result,
                                  nullptr, &profileError)) {
      markError("Extrude " + profileError);
      return false;
    }
    // Persist the refreshed fallback only after OCCT accepted the updated
    // profile. A loaded project can then infer the same stable IDs again.
    if (profileOverride_ && profileSelectionIds_)
      profileOverride_ = selectedProfile.geometry;
    setShape(std::make_shared<TopoDS_Shape>(result));
    markValid();
    return true;
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    markError(message && *message ? std::string("OCCT Extrude error: ") + message
                                  : "OCCT Extrude operation failed");
    return false;
  } catch (const std::exception& failure) {
    markError(std::string("Extrude error: ") + failure.what());
    return false;
  } catch (...) {
    markError("Unexpected Extrude geometry error");
    return false;
  }
}

bool ExtrudeFeature::resolveProfileOverride(const sketch::Sketch& source,
                                            sketch::Sketch* selected,
                                            std::string* error) {
  if (!selected || !profileOverride_) return false;

  if (!profileSelectionIds_) {
    ProfileSelectionIds matched;
    const auto& snapshot = *profileOverride_;
    const bool allMatched =
        matchPrimitiveIds(
            source.lines(), snapshot.lines(), sameLine, sameLine,
            [&source](std::size_t index) { return source.lineId(index); },
            &matched.lineIds) &&
        matchPrimitiveIds(
            source.circles(), snapshot.circles(), sameCircle,
            sameCircleIdentity,
            [&source](std::size_t index) { return source.circleId(index); },
            &matched.circleIds) &&
        matchPrimitiveIds(
            source.arcs(), snapshot.arcs(), sameArc, sameArcIdentity,
            [&source](std::size_t index) { return source.arcId(index); },
            &matched.arcIds) &&
        matchPrimitiveIds(
            source.beziers(), snapshot.beziers(), sameBezier, sameBezier,
            [&source](std::size_t index) { return source.bezierId(index); },
            &matched.bezierIds);

    if (!allMatched) {
      // Compatibility for older files and graph-split regions whose selected
      // boundary is not a one-to-one subset of source primitives.
      *selected = snapshot;
      return true;
    }
    profileSelectionIds_ = std::move(matched);
  }

  sketch::Sketch refreshed;
  refreshed.clear();
  for (const auto id : profileSelectionIds_->lineIds) {
    const auto index = source.lineIndex(id);
    if (!index) {
      if (error) *error = "selected profile line no longer exists";
      return false;
    }
    const auto& line = source.lines()[*index];
    refreshed.addLine(line.start, line.end, line.elementId);
  }
  for (const auto id : profileSelectionIds_->circleIds) {
    const auto index = source.circleIndex(id);
    if (!index) {
      if (error) *error = "selected profile circle no longer exists";
      return false;
    }
    const auto& circle = source.circles()[*index];
    refreshed.addCircle(circle.center, circle.radiusMm);
  }
  for (const auto id : profileSelectionIds_->arcIds) {
    const auto index = source.arcIndex(id);
    if (!index) {
      if (error) *error = "selected profile arc no longer exists";
      return false;
    }
    const auto& arc = source.arcs()[*index];
    refreshed.addArc(arc.center, arc.radiusMm, arc.startAngleRad,
                     arc.sweepAngleRad, arc.dashed);
  }
  for (const auto id : profileSelectionIds_->bezierIds) {
    const auto index = source.bezierIndex(id);
    if (!index) {
      if (error) *error = "selected profile Bezier no longer exists";
      return false;
    }
    const auto& bezier = source.beziers()[*index];
    refreshed.addBezier(bezier.points[0], bezier.points[1], bezier.points[2],
                        bezier.points[3], bezier.dashed);
  }

  *selected = std::move(refreshed);
  return true;
}

std::unique_ptr<Feature> ExtrudeFeature::clone() const {
  return std::make_unique<ExtrudeFeature>(*this);
}

void ExtrudeFeature::prepareForHistory() {
  // Region IDs are a rebuild cache derived from the retained profile override;
  // keeping them would make the history budget depend on an implementation
  // detail rather than the compact parametric definition.
  profileSelectionIds_.reset();
  ShapeFeature::prepareForHistory();
}

}  // namespace solidar
