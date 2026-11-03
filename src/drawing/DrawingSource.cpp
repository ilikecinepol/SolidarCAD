#include "drawing/DrawingSource.h"

#include <BRep_Builder.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>
#include <vector>

#include "model/Document.h"
#include "model/ProductDocument.h"

namespace solidar::drawing {
namespace {

DrawingSource collectBodies(const std::vector<const Body*>& bodies) {
  TopoDS_Compound compound;
  BRep_Builder builder;
  builder.MakeCompound(compound);

  DrawingSource result;
  for (const Body* body : bodies) {
    const auto shape = body->resultShape();
    if (!shape || shape->IsNull()) continue;
    builder.Add(compound, *shape);
    ++result.bodyCount;
  }

  if (result.bodyCount == 0) return result;
  result.shape = std::make_shared<TopoDS_Shape>(compound);
  for (TopExp_Explorer explorer(*result.shape, TopAbs_SOLID); explorer.More();
       explorer.Next())
    ++result.solidCount;
  return result;
}

}  // namespace

DrawingSource collectDrawingSource(const Document& document) {
  std::vector<const Body*> bodies;
  bodies.reserve(document.bodies().size());
  for (const Body& body : document.bodies()) bodies.push_back(&body);
  return collectBodies(bodies);
}

DrawingSource collectDrawingSource(const ComponentDrawing& drawing,
                                   const Document& owningDocument) {
  const bool validPaper = drawing.paper() == ComponentDrawingPaper::A4;
  const bool validOrientation =
      drawing.orientation() == ComponentDrawingOrientation::Portrait ||
      drawing.orientation() == ComponentDrawingOrientation::Landscape;
  if (!drawing.id().isValid() || drawing.nameUtf8().empty() ||
      drawing.nameUtf8().size() > kComponentDrawingMaximumNameUtf8Bytes ||
      !validPaper || !validOrientation || !std::isfinite(drawing.scale()) ||
      drawing.scale() < kComponentDrawingMinimumScale ||
      drawing.scale() > kComponentDrawingMaximumScale)
    return {DrawingSourceStatus::InvalidDrawing};
  const auto& sourceBodyIds = drawing.sourceBodyIds();
  if (sourceBodyIds.empty()) return collectDrawingSource(owningDocument);
  if (!std::is_sorted(sourceBodyIds.begin(), sourceBodyIds.end()) ||
      std::adjacent_find(sourceBodyIds.begin(), sourceBodyIds.end()) !=
          sourceBodyIds.end() ||
      std::find(sourceBodyIds.begin(), sourceBodyIds.end(), kInvalidBodyId) !=
          sourceBodyIds.end())
    return {DrawingSourceStatus::InvalidDrawing};

  std::vector<const Body*> bodies;
  bodies.reserve(sourceBodyIds.size());
  for (const BodyId bodyId : sourceBodyIds) {
    const Body* body = owningDocument.findBody(bodyId);
    if (!body) return {DrawingSourceStatus::MissingSourceBody};
    bodies.push_back(body);
  }
  return collectBodies(bodies);
}

}  // namespace solidar::drawing
