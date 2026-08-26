#include "ui/BodyRenderMesh.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>

#include <cassert>

int main() {
  solidar::BodyRenderMesh cache;
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(80.0, 35.0, 50.0).Shape();
  cache.rebuild(box);
  assert(cache.faceCount() == 6);
  assert(!cache.triangles().empty());
  assert(!cache.edges().empty());
  const std::size_t boxTriangles = cache.triangles().size();

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
