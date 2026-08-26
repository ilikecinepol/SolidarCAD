#include "model/FilletFeature.h"

#include <BRepFilletAPI_MakeFillet.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>
#include <memory>
#include <utility>

#include "model/Body.h"
#include "model/TopologyReferenceResolver.h"

namespace solidar {

FilletFeature::FilletFeature(EdgeReference edge, double radiusMm,
                             std::string name)
    : ShapeFeature(name.empty() ? "Fillet" : std::move(name)),
      edge_(edge),
      radiusMm_(radiusMm) {}

FilletFeature::FilletFeature(FeatureId id, EdgeReference edge,
                             double radiusMm, std::string name)
    : ShapeFeature(id, std::move(name)), edge_(edge), radiusMm_(radiusMm) {}

const EdgeReference& FilletFeature::edge() const noexcept { return edge_; }
double FilletFeature::radiusMm() const noexcept { return radiusMm_; }

void FilletFeature::setEdge(EdgeReference edge) noexcept {
  if (edge_.bodyId == edge.bodyId && edge_.featureId == edge.featureId &&
      edge_.edgeIndex == edge.edgeIndex)
    return;
  edge_ = edge;
  setDirty();
}

void FilletFeature::setRadiusMm(double value) noexcept {
  if (radiusMm_ == value) return;
  radiusMm_ = value;
  setDirty();
}

std::string FilletFeature::typeName() const { return "Fillet"; }

bool FilletFeature::rebuild(const RebuildContext& context) {
  clearShape();
  if (!std::isfinite(radiusMm_) || radiusMm_ <= 0.0) {
    markError("Fillet radius must be a finite positive value");
    return false;
  }
  if (!context.previousShape || context.previousShape->IsNull()) {
    markError("Fillet base shape is missing");
    return false;
  }
  if (!context.body || edge_.bodyId != context.body->id()) {
    markError("Fillet edge belongs to a different Body");
    return false;
  }

  // The selected topology belongs to the result immediately preceding this
  // feature. edgeIndex remains temporary until persistent topological naming
  // is implemented.
  const auto& features = context.body->features();
  std::size_t ownIndex = features.size();
  for (std::size_t index = 0; index < features.size(); ++index)
    if (features[index].get() == this) {
      ownIndex = index;
      break;
    }
  if (ownIndex == 0 || ownIndex == features.size() ||
      features[ownIndex - 1]->id() != edge_.featureId) {
    markError("Fillet source Feature could not be resolved");
    return false;
  }

  const auto edge = resolveEdge(*context.previousShape, edge_.edgeIndex);
  if (!edge) {
    markError("Fillet edge could not be resolved");
    return false;
  }

  try {
    BRepFilletAPI_MakeFillet maker(*context.previousShape);
    maker.Add(radiusMm_, *edge);
    maker.Build();
    if (!maker.IsDone() || maker.Shape().IsNull()) {
      markError("Fillet could not be built with the requested radius");
      return false;
    }

    TopoDS_Shape result = maker.Shape();
    if (result.ShapeType() != TopAbs_SOLID) {
      TopExp_Explorer solids(result, TopAbs_SOLID);
      if (!solids.More()) {
        markError("Fillet result does not contain a solid");
        return false;
      }
      result = solids.Current();
      solids.Next();
      if (solids.More()) {
        markError("Fillet result contains multiple solids");
        return false;
      }
    }
    setShape(std::make_shared<TopoDS_Shape>(result));
    markValid();
    return true;
  } catch (const Standard_Failure&) {
    markError("Fillet could not be built with the requested radius");
    return false;
  } catch (...) {
    markError("Unexpected Fillet geometry error");
    return false;
  }
}

std::unique_ptr<Feature> FilletFeature::clone() const {
  return std::make_unique<FilletFeature>(*this);
}

}  // namespace solidar
