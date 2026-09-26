#include "drawing/DrawingSource.h"

#include <BRep_Builder.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>

#include "model/Document.h"

namespace solidar::drawing {

DrawingSource collectDrawingSource(const Document& document) {
  TopoDS_Compound compound;
  BRep_Builder builder;
  builder.MakeCompound(compound);

  DrawingSource result;
  for (const Body& body : document.bodies()) {
    const auto shape = body.resultShape();
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

}  // namespace solidar::drawing
