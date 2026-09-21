#include "ui/BodyRenderMesh.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <cassert>
#include <cmath>

int main() {
  solidar::BodyRenderMesh cache;
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(80.0, 35.0, 50.0).Shape();
  cache.rebuild(box);
  assert(cache.faceCount() == 6);
  assert(!cache.triangles().empty());
  assert(!cache.edges().empty());
  const std::size_t boxTriangles = cache.triangles().size();

  const TopoDS_Shape cylinder = BRepPrimAPI_MakeCylinder(10.0, 50.0).Shape();
  cache.rebuild(cylinder);
  std::size_t curvedEdges = 0;
  for (const auto& edge : cache.edges()) {
    if (edge.points.size() <= 3) continue;
    ++curvedEdges;
    const std::size_t expectedIndex = edge.edgeIndex;
    // Every polyline segment retains the same OCCT edgeIndex, so selecting
    // any segment highlights the complete RenderEdge polyline.
    for (std::size_t segment = 1; segment < edge.points.size(); ++segment)
      assert(edge.edgeIndex == expectedIndex);

    // Every curved rim edge is classified as a native circle/arc (never a
    // tessellated chain) and preserves the source radius.
    assert(edge.kind == solidar::RenderEdge::Kind::Circle ||
           edge.kind == solidar::RenderEdge::Kind::Arc);
    assert(std::abs(edge.radius - 10.0) <= 1e-6);
  }
  assert(curvedEdges >= 2);

  // A true full circle edge is classified as a Circle (not an Arc).
  const gp_Circ fullCircle(
      gp_Ax2(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)), 5.0);
  TopoDS_Compound circleCompound;
  BRep_Builder circleBuilder;
  circleBuilder.MakeCompound(circleCompound);
  circleBuilder.Add(circleCompound, BRepBuilderAPI_MakeEdge(fullCircle).Edge());
  cache.rebuild(circleCompound);
  assert(cache.edges().size() == 1);
  assert(cache.edges()[0].kind == solidar::RenderEdge::Kind::Circle);
  assert(std::abs(cache.edges()[0].radius - 5.0) <= 1e-6);

  // A native quarter-circle arc edge is classified as an Arc, not a Circle.
  const gp_Circ arcCircle(
      gp_Ax2(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)), 5.0);
  TopoDS_Compound arcCompound;
  BRep_Builder arcBuilder;
  arcBuilder.MakeCompound(arcCompound);
  arcBuilder.Add(arcCompound,
                 BRepBuilderAPI_MakeEdge(arcCircle, 0.0, 1.5707963267948966)
                     .Edge());
  cache.rebuild(arcCompound);
  assert(cache.edges().size() == 1);
  assert(cache.edges()[0].kind == solidar::RenderEdge::Kind::Arc);
  assert(std::abs(cache.edges()[0].radius - 5.0) <= 1e-6);

  const TopoDS_Shape tool =
      BRepPrimAPI_MakeBox(gp_Pnt(30.0, 12.5, 30.0), 20.0, 10.0, 20.0).Shape();
  const TopoDS_Shape pocket = BRepAlgoAPI_Cut(box, tool).Shape();
  cache.rebuild(pocket);
  assert(cache.faceCount() > 6);
  assert(cache.triangles().size() > boxTriangles);
  assert(!cache.edges().empty());

  cache.clear();
  assert(cache.triangles().empty());
  assert(cache.edges().empty());
}
