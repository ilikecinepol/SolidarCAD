#include "model/EdgeManipulatorGeometry.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepTools.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <cmath>

namespace solidar {

std::optional<EdgeManipulatorGeometry> localEdgeManipulatorGeometry(
    const TopoDS_Shape& body, const TopoDS_Shape& edgeShape) {
  if (body.IsNull() || edgeShape.IsNull()) return std::nullopt;
  const TopoDS_Edge edge = TopoDS::Edge(edgeShape);
  BRepAdaptor_Curve curve(edge);
  const double parameter =
      (curve.FirstParameter() + curve.LastParameter()) * 0.5;
  gp_Pnt midpoint;
  gp_Vec tangent;
  curve.D1(parameter, midpoint, tangent);

  gp_Vec sum(0.0, 0.0, 0.0);
  for (TopExp_Explorer faceExplorer(body, TopAbs_FACE);
       faceExplorer.More(); faceExplorer.Next()) {
    const TopoDS_Face face = TopoDS::Face(faceExplorer.Current());
    bool containsEdge = false;
    for (TopExp_Explorer edgeExplorer(face, TopAbs_EDGE);
         edgeExplorer.More(); edgeExplorer.Next()) {
      if (edgeExplorer.Current().IsSame(edge)) {
        containsEdge = true;
        break;
      }
    }
    if (containsEdge) {
      double u0, u1, v0, v1;
      BRepTools::UVBounds(face, u0, u1, v0, v1);
      BRepAdaptor_Surface surface(face);
      gp_Pnt sample;
      gp_Vec du, dv;
      surface.D1((u0 + u1) * 0.5, (v0 + v1) * 0.5, sample, du, dv);
      gp_Vec normal = du.Crossed(dv);
      if (normal.SquareMagnitude() <= 1e-16) continue;
      normal.Normalize();
      if (face.Orientation() == TopAbs_REVERSED) normal.Reverse();
      sum += normal;
    }
  }
  if (sum.SquareMagnitude() <= 1e-16) {
    if (tangent.SquareMagnitude() <= 1e-16) tangent = gp_Vec(1, 0, 0);
    tangent.Normalize();
    gp_Vec reference = std::abs(tangent.Z()) < 0.85 ? gp_Vec(0, 0, 1)
                                                     : gp_Vec(0, 1, 0);
    sum = tangent.Crossed(reference);
  }
  if (sum.SquareMagnitude() <= 1e-16) sum = gp_Vec(0, 0, 1);
  sum.Normalize();
  return EdgeManipulatorGeometry{{midpoint.X(), midpoint.Y(), midpoint.Z()},
                                 {sum.X(), sum.Y(), sum.Z()}};
}

}  // namespace solidar
