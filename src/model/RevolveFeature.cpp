#include "model/RevolveFeature.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "model/FaceBoundaryEdgeProvenance.h"
#include "model/OcctBooleanTopologyHistoryCapture.h"
#include "model/OcctRevolveTopologyHistoryCapture.h"
#include "model/SketchExtrudeBuilder.h"
#include "model/SketchProfileBuilder.h"

namespace solidar {
namespace {
bool resolveAxis(const Document& document, const AxisReference& reference,
                 gp_Ax1* result, std::string* error) {
  if (!isKnownAxisReferenceType(reference.type)) {
    *error = "Revolve axis type is unsupported";
    return false;
  }
  if (reference.type == AxisReferenceType::GlobalX ||
      reference.type == AxisReferenceType::GlobalY ||
      reference.type == AxisReferenceType::GlobalZ) {
    const Vector3d direction = reference.type == AxisReferenceType::GlobalX
                                   ? Vector3d{1.0, 0.0, 0.0}
                                   : reference.type == AxisReferenceType::GlobalY
                                         ? Vector3d{0.0, 1.0, 0.0}
                                         : Vector3d{0.0, 0.0, 1.0};
    *result = gp_Ax1(gp_Pnt(0.0, 0.0, 0.0),
                     gp_Dir(direction.x, direction.y, direction.z));
    return true;
  }
  const auto* sketch = document.findSketch(reference.sketchId);
  if (!sketch) { *error = "Revolve axis sketch was not found"; return false; }
  const auto& placement = sketch->placement;
  Point3d origin = placement.origin;
  Vector3d direction{};
  if (reference.type == AxisReferenceType::SketchHorizontalAxis) {
    direction = placement.xDirection;
  } else if (reference.type == AxisReferenceType::SketchVerticalAxis) {
    direction = placement.yDirection;
  } else {
    const auto index = sketch->geometry.lineIndex(reference.lineId);
    if (!index) { *error = "Revolve axis line was not found"; return false; }
    const auto& line = sketch->geometry.lines()[*index];
    origin = placement.toWorld(line.start.xMm, line.start.yMm);
    const auto end = placement.toWorld(line.end.xMm, line.end.yMm);
    direction = {end.x - origin.x, end.y - origin.y, end.z - origin.z};
  }
  const double length = std::sqrt(direction.x * direction.x +
                                  direction.y * direction.y +
                                  direction.z * direction.z);
  if (!std::isfinite(length) || length <= 1e-12) {
    *error = "Revolve axis is invalid"; return false;
  }
  *result = gp_Ax1(gp_Pnt(origin.x, origin.y, origin.z),
                   gp_Dir(direction.x, direction.y, direction.z));
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

bool validateImmediatePredecessor(const RebuildContext& context,
                                  const ShapeFeature& current,
                                  std::string* error) {
  const auto fail = [&] {
    if (error)
      *error = "Revolve base Feature is not the immediate predecessor";
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

bool validateRawBooleanResult(const TopoDS_Shape& baseShape,
                              const TopoDS_Shape& result,
                              ExtrudeOperation operation,
                              std::string* error) {
  const std::size_t beforeSolidCount = solidCount(baseShape);
  const std::size_t resultSolidCount = solidCount(result);
  if (resultSolidCount == 0) {
    if (error) *error = "Revolve result does not contain a solid";
    return false;
  }
  if (operation == ExtrudeOperation::Join &&
      resultSolidCount > beforeSolidCount) {
    if (error) *error = "Revolve Join does not intersect the body";
    return false;
  }
  if (operation == ExtrudeOperation::Cut &&
      resultSolidCount > beforeSolidCount) {
    if (error) *error = "Revolve Cut would split the body";
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
    if (error) *error = "Revolve Join does not intersect the body";
    return false;
  }
  if (operation == ExtrudeOperation::Cut && after >= before - tolerance) {
    if (error) *error = "Revolve Cut does not intersect the body";
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
    if (error) *error = "Revolve result is invalid";
    return false;
  }
  return true;
}

bool validateCapturedSnapshot(const BooleanTopologyCaptureResult& captured,
                              const TopologyLineage& lineage,
                              std::string* error) {
  if (!captured.outputShape || captured.outputShape->IsNull()) {
    if (error) *error = "Revolve face history output shape is invalid";
    return false;
  }
  if (lineage.coverage() != TopologyLineageCoverage::CompleteFaces) {
    if (error) *error = "Revolve face lineage is incomplete";
    return false;
  }
  for (const auto& binding : lineage.bindings()) {
    if (!binding.subshape || binding.subshape->IsNull() ||
        !containsFace(*captured.outputShape, *binding.subshape)) {
      if (error) *error = "Revolve captured face is outside the output shape";
      return false;
    }
  }
  return true;
}

struct StrictRevolveTool {
  ShapeFeature::ShapePtr shape;
  std::shared_ptr<const TopologyLineage> lineage;
};

bool buildStrictRevolveTool(const DocumentSketch& profile,
                            const gp_Ax1& axis, double radians,
                            FeatureId producerFeatureId,
                            StrictRevolveTool* result, std::string* error) {
  SketchProfileFaceBuild profileBuild;
  if (!buildPlanarFaceFromSketchWithEdges(profile, &profileBuild, error))
    return false;

  std::vector<RevolveProfileEdgeSeed> seeds;
  seeds.reserve(profileBuild.edges.size());
  for (const auto& edge : profileBuild.edges)
    seeds.push_back({edge.geometryId, edge.edge});

  BRepPrimAPI_MakeRevol maker(*profileBuild.face, axis, radians, false);
  maker.Build();
  if (!maker.IsDone() || maker.Shape().IsNull() ||
      solidCount(maker.Shape()) == 0) {
    if (error) *error = "Revolve could not build a valid selected region";
    return false;
  }

  RevolveTopologyHistoryFailure failure;
  auto builder = captureRevolveTopologyHistory(
      maker, producerFeatureId, profile.id, profileBuild.face, seeds, &failure);
  if (!builder) {
    if (error)
      *error = failure.detail.empty() ? "Revolve topology capture failed"
                                      : std::move(failure.detail);
    return false;
  }
  const auto outputShape = builder->artifact().outputShape;
  if (!outputShape || outputShape->IsNull() ||
      !outputShape->IsSame(maker.Shape())) {
    if (error)
      *error = "Revolve topology capture output does not match the maker";
    return false;
  }
  std::string lineageError;
  auto lineage = builder->build(&lineageError);
  if (!lineage) {
    if (error)
      *error = lineageError.empty() ? "Revolve topology lineage build failed"
                                    : std::move(lineageError);
    return false;
  }
  if (lineage->coverage() != TopologyLineageCoverage::CompleteFaces) {
    if (error) *error = "Revolve topology lineage is incomplete";
    return false;
  }
  result->shape = outputShape;
  result->lineage = std::move(lineage);
  return true;
}

bool buildStrictBooleanRevolve(const RebuildContext& context,
                               const ShapeFeature& current,
                               const DocumentSketch& profile,
                               const gp_Ax1& axis, double radians,
                               ExtrudeOperation operation,
                               StrictRevolveTool* result,
                               std::string* error) {
  StrictRevolveTool tool;
  if (!buildStrictRevolveTool(profile, axis, radians, current.id(), &tool,
                              error))
    return false;

  const auto& baseFeature = *context.previousFeature;
  const std::array<BooleanTopologySource, 2> sources{{
      {BooleanOperandRole::Object,
       {baseFeature.id(), baseFeature.shape(), baseFeature.topologyLineage()}},
      {BooleanOperandRole::Tool,
       {current.id(), tool.shape, tool.lineage}},
  }};
  BooleanTopologyCaptureOptions options;
  options.simplifyResult = operation == ExtrudeOperation::Join;
  BooleanTopologyCaptureFailure failure;
  std::optional<BooleanTopologyCaptureResult> captured;
  if (operation == ExtrudeOperation::Join) {
    BRepAlgoAPI_Fuse fuse(*context.previousShape, *tool.shape);
    fuse.Build();
    if (!fuse.IsDone() || fuse.Shape().IsNull()) {
      if (error) *error = "Revolve Join boolean fuse failed";
      return false;
    }
    if (!validateRawBooleanResult(*context.previousShape, fuse.Shape(),
                                  operation, error))
      return false;
    captured = captureBooleanFaceHistory(
        fuse, current.id(), std::span<const BooleanTopologySource>(sources),
        {}, options, &failure);
  } else {
    BRepAlgoAPI_Cut cut(*context.previousShape, *tool.shape);
    cut.Build();
    if (!cut.IsDone() || cut.Shape().IsNull()) {
      if (error) *error = "Revolve Cut boolean cut failed";
      return false;
    }
    if (!validateRawBooleanResult(*context.previousShape, cut.Shape(),
                                  operation, error))
      return false;
    captured = captureBooleanFaceHistory(
        cut, current.id(), std::span<const BooleanTopologySource>(sources),
        {}, options, &failure);
  }
  if (!captured) {
    if (error)
      *error = failure.detail.empty() ? "Revolve face history capture failed"
                                      : std::move(failure.detail);
    return false;
  }
  if (!captured->outputShape || captured->outputShape->IsNull() ||
      !validateFinalBooleanResult(*context.previousShape,
                                  *captured->outputShape, operation, error))
    return false;

  std::string lineageError;
  auto lineage = captured->builder.build(&lineageError);
  if (!lineage) {
    if (error)
      *error = lineageError.empty() ? "Revolve face lineage build failed"
                                    : std::move(lineageError);
    return false;
  }
  if (!validateCapturedSnapshot(*captured, *lineage, error)) return false;
  result->shape = std::move(captured->outputShape);
  result->lineage = std::move(lineage);
  return true;
}
}

RevolveFeature::RevolveFeature(SketchId profile, AxisReference axis,
                               double angle, std::string name,
                               ExtrudeOperation operation, bool reversed)
    : ShapeFeature(name.empty() ? "Revolve" : std::move(name)),
      profileSketchId_(profile), axis_(axis), angleDeg_(angle),
      operation_(operation), reversed_(reversed) {}
RevolveFeature::RevolveFeature(FeatureId id, SketchId profile,
                               AxisReference axis, double angle,
                               std::string name, ExtrudeOperation operation,
                               bool reversed)
    : ShapeFeature(id, std::move(name)), profileSketchId_(profile), axis_(axis),
      angleDeg_(angle), operation_(operation), reversed_(reversed) {}
SketchId RevolveFeature::profileSketchId() const noexcept { return profileSketchId_; }
const AxisReference& RevolveFeature::axis() const noexcept { return axis_; }
double RevolveFeature::angleDeg() const noexcept { return angleDeg_; }
ExtrudeOperation RevolveFeature::operation() const noexcept { return operation_; }
bool RevolveFeature::reversed() const noexcept { return reversed_; }
void RevolveFeature::setProfileSketchId(SketchId value) noexcept {
  if (profileSketchId_ == value) return;
  profileSketchId_ = value;
  profileOverride_.reset();
  setDirty();
}
const std::optional<sketch::Sketch>&
RevolveFeature::profileOverride() const noexcept {
  return profileOverride_;
}
void RevolveFeature::setProfileOverride(
    std::optional<sketch::Sketch> profile) {
  profileOverride_ = std::move(profile);
  setDirty();
}
void RevolveFeature::setAxis(AxisReference value) noexcept { if (axis_ != value) { axis_ = value; setDirty(); } }
void RevolveFeature::setAngleDeg(double value) noexcept { if (angleDeg_ != value) { angleDeg_ = value; setDirty(); } }
void RevolveFeature::setOperation(ExtrudeOperation value) noexcept { if (operation_ != value) { operation_ = value; setDirty(); } }
void RevolveFeature::setReversed(bool value) noexcept { if (reversed_ != value) { reversed_ = value; setDirty(); } }
FeatureDependencies RevolveFeature::dependencies() const {
  FeatureDependencies result;
  if (profileSketchId_ != kInvalidSketchId)
    result.sketchIds.push_back(profileSketchId_);
  if (isSketchAxisReferenceType(axis_.type) &&
      axis_.sketchId != kInvalidSketchId &&
      axis_.sketchId != profileSketchId_)
    result.sketchIds.push_back(axis_.sketchId);
  return result;
}

bool RevolveFeature::rebuildImpl(const RebuildContext& context) {
  clearShape();
  if (!std::isfinite(angleDeg_) || angleDeg_ <= 0.0 || angleDeg_ > 360.0) {
    markError("Revolve angle must be finite and in the range (0, 360]");
    return false;
  }
  const auto* profile = context.document.findSketch(profileSketchId_);
  if (!profile) { markError("Revolve profile sketch was not found"); return false; }
  try {
    DocumentSketch selectedProfile = *profile;
    if (profileOverride_) selectedProfile.geometry = *profileOverride_;
    std::string error;

    if (operation_ == ExtrudeOperation::Join ||
        operation_ == ExtrudeOperation::Cut) {
      const auto* registeredBody =
          context.document.findBodyForFeature(id());
      const auto registeredIndex =
          registeredBody ? registeredBody->featureIndex(id()) : std::nullopt;
      const bool registered =
          registeredIndex &&
          registeredBody->features()[*registeredIndex].get() == this;
      const bool validatePredecessor =
          registered &&
          (*registeredIndex > 0 || context.previousShape ||
           context.previousFeature);
      if (validatePredecessor &&
          !validateImmediatePredecessor(context, *this, &error)) {
        markError(error);
        return false;
      }
      if (registered && *registeredIndex > 0) {
        const auto baseLineage = context.previousFeature->topologyLineage();
        if (baseLineage &&
            baseLineage->coverage() !=
                TopologyLineageCoverage::CompleteFaces) {
          markError("Revolve base face lineage is incomplete");
          return false;
        }
        if (baseLineage && !profileOverride_ &&
            isSupportedSingleSketchProfile(selectedProfile)) {
          gp_Ax1 axis;
          if (!resolveAxis(context.document, axis_, &axis, &error)) {
            markError(error);
            return false;
          }
          const double radians = angleDeg_ * std::numbers::pi / 180.0 *
                                 (reversed_ ? -1.0 : 1.0);
          StrictRevolveTool built;
          if (!buildStrictBooleanRevolve(context, *this, selectedProfile,
                                         axis, radians, operation_, &built,
                                         &error)) {
            markError(error);
            return false;
          }
          FaceBoundaryEdgeProvenanceFailure edgeFailure;
          const auto augmented =
              augmentCompleteFaceLineageWithUniqueManifoldEdges(
                  {id(), built.shape, built.lineage}, &edgeFailure);
          if (!augmented) {
            markError(
                edgeFailure.detail.empty()
                    ? "Revolve edge lineage augmentation failed"
                    : "Revolve edge lineage augmentation failed: " +
                          edgeFailure.detail);
            return false;
          }
          setShapeAndTopologyLineage(
              std::move(built.shape),
              std::move(augmented->augmentedSource.lineage));
          markValid();
          return true;
        }
      }
    }

    if (operation_ == ExtrudeOperation::NewBody && !profileOverride_ &&
        isSupportedSingleSketchProfile(selectedProfile)) {
      if (context.previousShape && !context.previousShape->IsNull()) {
        markError("Revolve New Body must be the first feature of a Body");
        return false;
      }

      SketchProfileFaceBuild profileBuild;
      if (!buildPlanarFaceFromSketchWithEdges(selectedProfile, &profileBuild,
                                               &error)) {
        markError("Revolve " + error);
        return false;
      }
      gp_Ax1 axis;
      if (!resolveAxis(context.document, axis_, &axis, &error)) {
        markError(error);
        return false;
      }
      const double radians = angleDeg_ * std::numbers::pi / 180.0 *
                             (reversed_ ? -1.0 : 1.0);
      std::vector<RevolveProfileEdgeSeed> seeds;
      seeds.reserve(profileBuild.edges.size());
      for (const auto& edge : profileBuild.edges)
        seeds.push_back({edge.geometryId, edge.edge});

      BRepPrimAPI_MakeRevol builder(*profileBuild.face, axis, radians, false);
      builder.Build();
      if (!builder.IsDone() || builder.Shape().IsNull() ||
          solidCount(builder.Shape()) == 0) {
        markError("Revolve could not build a valid selected region");
        return false;
      }

      RevolveTopologyHistoryFailure captureFailure;
      auto captured = captureRevolveTopologyHistory(
          builder, id(), selectedProfile.id, profileBuild.face, seeds,
          &captureFailure);
      if (!captured) {
        markError(captureFailure.detail.empty()
                      ? "Revolve topology capture failed"
                      : captureFailure.detail);
        return false;
      }
      const auto& artifact = captured->artifact();
      if (!artifact.outputShape || artifact.outputShape->IsNull() ||
          !artifact.outputShape->IsSame(builder.Shape())) {
        markError("Revolve topology capture output does not match the maker");
        return false;
      }
      auto lineage = captured->build(&error);
      if (!lineage) {
        markError(error.empty() ? "Revolve topology lineage build failed"
                                : "Revolve " + error);
        return false;
      }
      if (lineage->coverage() != TopologyLineageCoverage::CompleteFaces) {
        markError("Revolve topology lineage is incomplete");
        return false;
      }
      FaceBoundaryEdgeProvenanceFailure edgeFailure;
      const auto augmented =
          augmentCompleteFaceLineageWithUniqueManifoldEdges(
              TopologySourceSnapshot{id(), artifact.outputShape, lineage},
              &edgeFailure);
      if (!augmented) {
        markError("Revolve " +
                  (edgeFailure.detail.empty()
                       ? std::string("edge lineage is invalid")
                       : std::move(edgeFailure.detail)));
        return false;
      }
      setShapeAndTopologyLineage(
          artifact.outputShape, augmented->augmentedSource.lineage);
      markValid();
      return true;
    }

    std::vector<TopoDS_Face> regionFaces;
    if (!buildSketchProfileFaces(selectedProfile, &regionFaces, &error)) {
      markError("Revolve " + error); return false;
    }
    gp_Ax1 axis;
    if (!resolveAxis(context.document, axis_, &axis, &error)) {
      markError(error); return false;
    }
    const double radians = angleDeg_ * std::numbers::pi / 180.0 *
                           (reversed_ ? -1.0 : 1.0);
    std::vector<TopoDS_Shape> revolvedRegions;
    revolvedRegions.reserve(regionFaces.size());
    for (const auto& face : regionFaces) {
      BRepPrimAPI_MakeRevol builder(face, axis, radians, true);
      builder.Build();
      if (!builder.IsDone() || builder.Shape().IsNull() ||
          solidCount(builder.Shape()) == 0) {
        markError("Revolve could not build a valid selected region");
        return false;
      }
      BRepCheck_Analyzer analyzer(builder.Shape());
      if (!analyzer.IsValid()) {
        markError("Revolve selected region result is invalid");
        return false;
      }
      revolvedRegions.push_back(builder.Shape());
    }

    TopoDS_Shape revolveTool = revolvedRegions.front();
    for (std::size_t index = 1; index < revolvedRegions.size(); ++index) {
      BRepAlgoAPI_Fuse fuse(revolveTool, revolvedRegions[index]);
      fuse.Build();
      if (!fuse.IsDone() || fuse.Shape().IsNull()) {
        markError("Revolve could not combine selected profile regions");
        return false;
      }
      revolveTool = fuse.Shape();
    }

    TopoDS_Shape result = revolveTool;
    if (operation_ == ExtrudeOperation::NewBody) {
      if (context.previousShape && !context.previousShape->IsNull()) {
        markError("Revolve New Body must be the first feature of a Body"); return false;
      }
    } else {
      if (!context.previousShape || context.previousShape->IsNull()) {
        markError(operation_ == ExtrudeOperation::Join
                      ? "Revolve Join base shape is missing"
                      : "Revolve Cut base shape is missing");
        return false;
      }
      if (operation_ == ExtrudeOperation::Join) {
        BRepAlgoAPI_Fuse boolean(*context.previousShape, revolveTool); boolean.Build();
        if (!boolean.IsDone() || boolean.Shape().IsNull()) { markError("Revolve Join boolean fuse failed"); return false; }
        result = boolean.Shape();
      } else {
        BRepAlgoAPI_Cut boolean(*context.previousShape, revolveTool); boolean.Build();
        if (!boolean.IsDone() || boolean.Shape().IsNull()) { markError("Revolve Cut boolean cut failed"); return false; }
        result = boolean.Shape();
      }
    }
    if (solidCount(result) == 0) {
      markError("Revolve result does not contain a solid"); return false;
    }
    BRepCheck_Analyzer resultAnalyzer(result);
    if (!resultAnalyzer.IsValid()) {
      markError("Revolve result is invalid"); return false;
    }
    setShape(std::make_shared<TopoDS_Shape>(result)); markValid(); return true;
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    markError(message && *message ? std::string("OCCT Revolve error: ") + message
                                  : "OCCT Revolve operation failed");
  } catch (const std::exception& failure) {
    markError(std::string("Revolve error: ") + failure.what());
  } catch (...) { markError("Unexpected Revolve geometry error"); }
  return false;
}
std::unique_ptr<Feature> RevolveFeature::clone() const { return std::make_unique<RevolveFeature>(*this); }
}  // namespace solidar
