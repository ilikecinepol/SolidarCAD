#include "model/JoinBodiesFeature.h"

#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include <array>
#include <span>

#include "model/Document.h"
#include "model/GeometryOperation.h"
#include "model/OcctBooleanTopologyHistoryCapture.h"
#include "model/TopologyLineage.h"

namespace solidar {
namespace {

const ShapeFeature* resolveSource(const RebuildContext& context,
                                  BodyId bodyId, FeatureId featureId) {
  const auto* feature = context.document.findFeature(bodyId, featureId);
  if (!feature || !feature->isValid() || !feature->shape() ||
      feature->shape()->IsNull())
    return nullptr;
  return feature;
}

int solidCount(const TopoDS_Shape& shape) {
  int count = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_SOLID); explorer.More();
       explorer.Next())
    ++count;
  return count;
}

}  // namespace

ShapeFeature::ShapePtr buildJoinedBodiesShape(const TopoDS_Shape& first,
                                              const TopoDS_Shape& second,
                                              std::string* error,
                                              OperationFailureCode* code) {
  if (code) *code = OperationFailureCode::None;
  if (first.IsNull() || second.IsNull()) {
    if (code) *code = OperationFailureCode::MissingSource;
    if (error) *error = "Join Bodies source shape is missing";
    return {};
  }
  GeometryFailure failure;
  if (!runGeometryOperation(
          [&] {
            return BRepCheck_Analyzer(first).IsValid() &&
                   BRepCheck_Analyzer(second).IsValid();
          },
          &failure)) {
    if (code) *code = OperationFailureCode::GeometryOperationFailed;
    if (error)
      *error = failure.kind == GeometryFailureKind::None
                   ? "Join Bodies source B-Rep is invalid"
                   : failure.kind == GeometryFailureKind::OcctException
                         ? "Join Bodies OpenCASCADE validation failed"
                         : "Join Bodies source validation failed";
    return {};
  }
  TopoDS_Shape candidate;
  const bool completed = runGeometryOperation(
      [&]() -> bool {
        BRepAlgoAPI_Fuse fuse(first, second);
        fuse.Build();
        if (!fuse.IsDone() || fuse.Shape().IsNull()) {
          if (code) *code = OperationFailureCode::GeometryOperationFailed;
          if (error) *error = "Join Bodies boolean operation failed";
          return false;
        }
        candidate = fuse.Shape();
        if (!BRepCheck_Analyzer(candidate).IsValid()) {
          if (code) *code = OperationFailureCode::GeometryOperationFailed;
          if (error) *error = "Join Bodies produced an invalid shape";
          return false;
        }
        if (solidCount(candidate) != 1) {
          if (code) *code = OperationFailureCode::BodiesDoNotTouch;
          if (error)
            *error =
                "Join Bodies requires two touching or intersecting solids";
          return false;
        }
        return true;
      },
      &failure);
  if (!completed) {
    if (code && *code == OperationFailureCode::None)
      *code = OperationFailureCode::GeometryOperationFailed;
    if (failure.kind != GeometryFailureKind::None && error)
      *error = failure.kind == GeometryFailureKind::OcctException
                   ? "Join Bodies OpenCASCADE operation failed"
                   : "Join Bodies geometry operation failed";
    return {};
  }
  if (error) error->clear();
  return std::make_shared<TopoDS_Shape>(std::move(candidate));
}

JoinBodiesFeature::JoinBodiesFeature(BodyId firstBodyId,
                                     FeatureId firstFeatureId,
                                     BodyId secondBodyId,
                                     FeatureId secondFeatureId,
                                     std::string name)
    : ShapeFeature(name.empty() ? "Join Bodies" : std::move(name)),
      firstBodyId_(firstBodyId),
      firstFeatureId_(firstFeatureId),
      secondBodyId_(secondBodyId),
      secondFeatureId_(secondFeatureId) {}

JoinBodiesFeature::JoinBodiesFeature(FeatureId id, BodyId firstBodyId,
                                     FeatureId firstFeatureId,
                                     BodyId secondBodyId,
                                     FeatureId secondFeatureId,
                                     std::string name)
    : ShapeFeature(id, std::move(name)),
      firstBodyId_(firstBodyId),
      firstFeatureId_(firstFeatureId),
      secondBodyId_(secondBodyId),
      secondFeatureId_(secondFeatureId) {}

BodyId JoinBodiesFeature::firstBodyId() const noexcept { return firstBodyId_; }
FeatureId JoinBodiesFeature::firstFeatureId() const noexcept {
  return firstFeatureId_;
}
BodyId JoinBodiesFeature::secondBodyId() const noexcept { return secondBodyId_; }
FeatureId JoinBodiesFeature::secondFeatureId() const noexcept {
  return secondFeatureId_;
}

void JoinBodiesFeature::setInputs(BodyId firstBodyId, FeatureId firstFeatureId,
                                  BodyId secondBodyId,
                                  FeatureId secondFeatureId) {
  firstBodyId_ = firstBodyId;
  firstFeatureId_ = firstFeatureId;
  secondBodyId_ = secondBodyId;
  secondFeatureId_ = secondFeatureId;
  setDirty();
}

FeatureDependencies JoinBodiesFeature::dependencies() const {
  FeatureDependencies result;
  if (firstFeatureId_ != kInvalidFeatureId)
    result.featureIds.push_back(firstFeatureId_);
  if (secondFeatureId_ != kInvalidFeatureId &&
      secondFeatureId_ != firstFeatureId_)
    result.featureIds.push_back(secondFeatureId_);
  return result;
}


bool JoinBodiesFeature::rebuildImpl(const RebuildContext& context) {
  clearShape();
  if (firstBodyId_ == secondBodyId_) {
    markError("Join Bodies requires two distinct source Bodies");
    return false;
  }
  if (context.body &&
      (context.body->id() == firstBodyId_ ||
       context.body->id() == secondBodyId_)) {
    markError("Join Bodies result Body must not alias a source Body");
    return false;
  }

  const ShapeFeature* first =
      resolveSource(context, firstBodyId_, firstFeatureId_);
  const ShapeFeature* second =
      resolveSource(context, secondBodyId_, secondFeatureId_);
  if (!first || !second) {
    markError("Join Bodies source Feature could not be resolved");
    return false;
  }
  const auto firstShape = first->shape();
  const auto secondShape = second->shape();
  const auto firstLineage = first->topologyLineage();
  const auto secondLineage = second->topologyLineage();

  if ((firstLineage && firstLineage->coverage() !=
                           TopologyLineageCoverage::CompleteFaces) ||
      (secondLineage && secondLineage->coverage() !=
                            TopologyLineageCoverage::CompleteFaces)) {
    markError("Join Bodies source face lineage is incomplete");
    return false;
  }

  std::string error;
  if (!firstLineage || !secondLineage) {
    auto result = buildJoinedBodiesShape(*firstShape, *secondShape, &error);
    if (!result) {
      markError(std::move(error));
      return false;
    }
    setShape(std::move(result));
    markValid();
    return true;
  }

  if (!BRepCheck_Analyzer(*firstShape).IsValid() ||
      !BRepCheck_Analyzer(*secondShape).IsValid()) {
    markError("Join Bodies source B-Rep is invalid");
    return false;
  }

  BRepAlgoAPI_Fuse fuse(*firstShape, *secondShape);
  fuse.Build();
  if (!fuse.IsDone() || fuse.Shape().IsNull()) {
    markError("Join Bodies boolean operation failed");
    return false;
  }

  const std::array<BooleanTopologySource, 2> sources{{
      {BooleanOperandRole::Object,
       {first->id(), firstShape, firstLineage}},
      {BooleanOperandRole::Tool,
       {second->id(), secondShape, secondLineage}},
  }};
  BooleanTopologyCaptureOptions options;
  options.simplifyResult = true;
  BooleanTopologyCaptureFailure captureFailure;
  auto captured = captureBooleanFaceHistory(
      fuse, id(), std::span<const BooleanTopologySource>(sources), {},
      options, &captureFailure);
  if (!captured) {
    markError(captureFailure.detail.empty()
                  ? "Join Bodies face history capture failed"
                  : std::move(captureFailure.detail));
    return false;
  }
  if (solidCount(*captured->outputShape) != 1) {
    markError("Join Bodies requires two touching or intersecting solids");
    return false;
  }
  auto lineage = captured->builder.build(&error);
  if (!lineage) {
    markError(error.empty() ? "Join Bodies face lineage build failed"
                            : std::move(error));
    return false;
  }
  setShapeAndTopologyLineage(std::move(captured->outputShape),
                             std::move(lineage));
  markValid();
  return true;
}

std::unique_ptr<Feature> JoinBodiesFeature::clone() const {
  return std::make_unique<JoinBodiesFeature>(*this);
}

}  // namespace solidar
