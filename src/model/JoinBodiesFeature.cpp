#include "model/JoinBodiesFeature.h"

#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include "model/Document.h"
#include "model/GeometryOperation.h"

namespace solidar {
namespace {

const TopoDS_Shape* resolveSource(const RebuildContext& context, BodyId bodyId,
                                  FeatureId featureId) {
  const Body* body = context.document.findBody(bodyId);
  if (!body) return nullptr;
  for (const auto& feature : body->features())
    if (feature->id() == featureId && feature->isValid() && feature->shape() &&
        !feature->shape()->IsNull())
      return feature->shape().get();
  return nullptr;
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
  const TopoDS_Shape* first =
      resolveSource(context, firstBodyId_, firstFeatureId_);
  const TopoDS_Shape* second =
      resolveSource(context, secondBodyId_, secondFeatureId_);
  if (!first || !second) {
    markError("Join Bodies source Feature could not be resolved");
    return false;
  }
  std::string error;
  auto result = buildJoinedBodiesShape(*first, *second, &error);
  if (!result) {
    markError(std::move(error));
    return false;
  }
  setShape(std::move(result));
  markValid();
  return true;
}

std::unique_ptr<Feature> JoinBodiesFeature::clone() const {
  return std::make_unique<JoinBodiesFeature>(*this);
}

}  // namespace solidar
