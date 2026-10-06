#include "model/JoinBodiesFeature.h"

#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>
#include <Standard_Failure.hxx>

#include "model/Document.h"

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
                                              std::string* error) {
  if (first.IsNull() || second.IsNull()) {
    if (error) *error = "Join Bodies source shape is missing";
    return {};
  }
  TopoDS_Shape result;
  try {
    BRepAlgoAPI_Fuse fuse(first, second);
    fuse.Build();
    if (!fuse.IsDone() || fuse.Shape().IsNull()) {
      if (error) *error = "Join Bodies boolean operation failed";
      return {};
    }
    result = fuse.Shape();
  } catch (const Standard_Failure& failure) {
    if (error) {
      *error = "Join Bodies boolean operation failed";
      if (const char* detail = failure.what(); detail && *detail)
        *error += std::string(": ") + detail;
    }
    return {};
  }
  if (!BRepCheck_Analyzer(result).IsValid()) {
    if (error) *error = "Join Bodies produced an invalid shape";
    return {};
  }
  if (solidCount(result) != 1) {
    if (error) *error = "Join Bodies requires two touching or intersecting solids";
    return {};
  }
  if (error) error->clear();
  return std::make_shared<TopoDS_Shape>(result);
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

bool JoinBodiesFeature::dependsOnFeature(FeatureId featureId) const noexcept {
  return featureId == firstFeatureId_ || featureId == secondFeatureId_;
}

std::string JoinBodiesFeature::typeName() const { return "JoinBodies"; }

bool JoinBodiesFeature::rebuild(const RebuildContext& context) {
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
