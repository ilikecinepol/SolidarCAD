#include "ui/BodyRenderMesh.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>

#include <cassert>

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
  }
  assert(curvedEdges >= 2);

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
