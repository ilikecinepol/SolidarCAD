#include "ui/BodyRenderMesh.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>

namespace solidar {
namespace {

Point3d point(const gp_Pnt& value) {
  return {value.X(), value.Y(), value.Z()};
}

Vector3d normal(Point3d a, Point3d b, Point3d c, bool reversed) {
  const double ux = b.x - a.x;
  const double uy = b.y - a.y;
  const double uz = b.z - a.z;
  const double vx = c.x - a.x;
  const double vy = c.y - a.y;
  const double vz = c.z - a.z;
  Vector3d result{uy * vz - uz * vy, uz * vx - ux * vz,
                  ux * vy - uy * vx};
  const double length = std::sqrt(result.x * result.x + result.y * result.y +
                                  result.z * result.z);
  if (length > 1e-12) {
    const double sign = reversed ? -1.0 : 1.0;
    result = {sign * result.x / length, sign * result.y / length,
              sign * result.z / length};
  }
  return result;
}

}  // namespace

void BodyRenderMesh::rebuild(const TopoDS_Shape& shape) {
  clear();
  if (shape.IsNull()) return;

  Bnd_Box bounds;
  BRepBndLib::Add(shape, bounds);
  if (!bounds.IsVoid()) {
    double x0, y0, z0, x1, y1, z1;
    bounds.Get(x0, y0, z0, x1, y1, z1);
    center_ = {(x0 + x1) * 0.5, (y0 + y1) * 0.5, (z0 + z1) * 0.5};
    diagonal_ = std::sqrt((x1 - x0) * (x1 - x0) +
                          (y1 - y0) * (y1 - y0) +
                          (z1 - z0) * (z1 - z0));
  }
  const double deflection = std::clamp(diagonal_ * 0.002, 0.05, 2.0);
  BRepMesh_IncrementalMesh mesher(shape, deflection, false, 0.35, true);

  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next(), ++faceCount_) {
    const TopoDS_Face face = TopoDS::Face(explorer.Current());
    TopLoc_Location location;
    const Handle(Poly_Triangulation) mesh = BRep_Tool::Triangulation(face, location);
    if (mesh.IsNull()) continue;
    const gp_Trsf transform = location.Transformation();
    for (int index = 1; index <= mesh->NbTriangles(); ++index) {
      Standard_Integer ia, ib, ic;
      mesh->Triangle(index).Get(ia, ib, ic);
      Point3d a = point(mesh->Node(ia).Transformed(transform));
      Point3d b = point(mesh->Node(ib).Transformed(transform));
      Point3d c = point(mesh->Node(ic).Transformed(transform));
      const bool reversed = face.Orientation() == TopAbs_REVERSED;
      if (reversed) std::swap(b, c);
      triangles_.push_back({a, b, c, normal(a, b, c, false), faceCount_});
    }
  }

  std::size_t edgeIndex = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More();
       explorer.Next(), ++edgeIndex) {
    const TopoDS_Edge edge = TopoDS::Edge(explorer.Current());
    RenderEdge rendered;
    rendered.edgeIndex = edgeIndex;
    BRepAdaptor_Curve curve(edge);
    GCPnts_QuasiUniformDeflection sampler(curve, deflection);
    if (sampler.IsDone()) {
      rendered.points.reserve(static_cast<std::size_t>(sampler.NbPoints()));
      for (int index = 1; index <= sampler.NbPoints(); ++index)
        rendered.points.push_back(point(sampler.Value(index)));
    }
    if (rendered.points.size() >= 2) edges_.push_back(std::move(rendered));
  }
}

void BodyRenderMesh::clear() noexcept {
  triangles_.clear();
  edges_.clear();
  center_ = {};
  diagonal_ = 0.0;
  faceCount_ = 0;
}

const std::vector<RenderTriangle>& BodyRenderMesh::triangles() const noexcept { return triangles_; }
const std::vector<RenderEdge>& BodyRenderMesh::edges() const noexcept { return edges_; }
Point3d BodyRenderMesh::center() const noexcept { return center_; }
double BodyRenderMesh::diagonal() const noexcept { return diagonal_; }
std::size_t BodyRenderMesh::faceCount() const noexcept { return faceCount_; }

}  // namespace solidar
