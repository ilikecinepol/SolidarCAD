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
#include <chrono>
#include <cmath>
#include <limits>

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
  rebuild(shape, ViewportMeshQuality::Normal);
}

void BodyRenderMesh::rebuild(const TopoDS_Shape& shape,
                             ViewportMeshQuality quality) {
  const auto started = std::chrono::steady_clock::now();
  clear();
  quality_ = quality;
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
  // Both profiles are scale-aware. High reduces the chord error fourfold and
  // angular error by more than half; it is intended for close inspection and
  // screenshots rather than being paid for on every camera movement.
  if (quality == ViewportMeshQuality::High) {
    linearDeflection_ = std::clamp(diagonal_ * 0.00035, 0.005, 0.35);
    angularDeflection_ = 0.08;
  } else {
    linearDeflection_ = std::clamp(diagonal_ * 0.0012, 0.02, 0.9);
    angularDeflection_ = 0.15;
  }
  BRepMesh_IncrementalMesh mesher(shape, linearDeflection_, false,
                                  angularDeflection_, true);

  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next(), ++faceCount_) {
    const TopoDS_Face face = TopoDS::Face(explorer.Current());
    TopLoc_Location location;
    const Handle(Poly_Triangulation) mesh = BRep_Tool::Triangulation(face, location);
    if (mesh.IsNull()) continue;
    if (!mesh->HasNormals()) mesh->ComputeNormals();
    const gp_Trsf transform = location.Transformation();
    const bool reversed = face.Orientation() == TopAbs_REVERSED;
    const std::uint32_t firstVertex =
        static_cast<std::uint32_t>(vertices_.size());
    vertices_.reserve(vertices_.size() + static_cast<std::size_t>(mesh->NbNodes()));
    for (int index = 1; index <= mesh->NbNodes(); ++index) {
      const gp_Pnt p = mesh->Node(index).Transformed(transform);
      gp_Vec n(mesh->Normal(index));
      n.Transform(transform);
      if (reversed) n.Reverse();
      if (n.SquareMagnitude() > 1e-24) n.Normalize();
      vertices_.push_back({point(p), {n.X(), n.Y(), n.Z()},
                           static_cast<std::uint32_t>(faceCount_)});
    }
    for (int index = 1; index <= mesh->NbTriangles(); ++index) {
      Standard_Integer ia, ib, ic;
      mesh->Triangle(index).Get(ia, ib, ic);
      Point3d a = point(mesh->Node(ia).Transformed(transform));
      Point3d b = point(mesh->Node(ib).Transformed(transform));
      Point3d c = point(mesh->Node(ic).Transformed(transform));
      if (reversed) std::swap(b, c);
      std::uint32_t va = firstVertex + static_cast<std::uint32_t>(ia - 1);
      std::uint32_t vb = firstVertex + static_cast<std::uint32_t>(ib - 1);
      std::uint32_t vc = firstVertex + static_cast<std::uint32_t>(ic - 1);
      if (reversed) std::swap(vb, vc);
      triangleIndices_.insert(triangleIndices_.end(), {va, vb, vc});
      const Vector3d flat = normal(a, b, c, false);
      triangles_.push_back({a, b, c, flat, vertices_[va].normal,
                            vertices_[vb].normal, vertices_[vc].normal,
                            faceCount_});
    }
  }

  std::size_t edgeIndex = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More();
       explorer.Next(), ++edgeIndex) {
    const TopoDS_Edge edge = TopoDS::Edge(explorer.Current());
    RenderEdge rendered;
    rendered.edgeIndex = edgeIndex;
    BRepAdaptor_Curve curve(edge);
    GCPnts_QuasiUniformDeflection sampler(curve, linearDeflection_ * 0.65);
    if (sampler.IsDone()) {
      rendered.points.reserve(static_cast<std::size_t>(sampler.NbPoints()));
      for (int index = 1; index <= sampler.NbPoints(); ++index)
        rendered.points.push_back(point(sampler.Value(index)));
    }
    if (rendered.points.size() >= 2) {
      edgeSampleCount_ += rendered.points.size();
      edges_.push_back(std::move(rendered));
    }
  }
  rebuildMilliseconds_ = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - started).count();
}

void BodyRenderMesh::clear() noexcept {
  triangles_.clear();
  vertices_.clear();
  triangleIndices_.clear();
  edges_.clear();
  center_ = {};
  diagonal_ = 0.0;
  faceCount_ = 0;
  edgeSampleCount_ = 0;
  linearDeflection_ = 0.0;
  angularDeflection_ = 0.0;
  rebuildMilliseconds_ = 0.0;
  ++revision_;
}

const std::vector<RenderTriangle>& BodyRenderMesh::triangles() const noexcept { return triangles_; }
const std::vector<RenderVertex>& BodyRenderMesh::vertices() const noexcept { return vertices_; }
const std::vector<std::uint32_t>& BodyRenderMesh::triangleIndices() const noexcept {
  return triangleIndices_;
}
const std::vector<RenderEdge>& BodyRenderMesh::edges() const noexcept { return edges_; }
Point3d BodyRenderMesh::center() const noexcept { return center_; }
double BodyRenderMesh::diagonal() const noexcept { return diagonal_; }
std::size_t BodyRenderMesh::faceCount() const noexcept { return faceCount_; }
std::size_t BodyRenderMesh::edgeSampleCount() const noexcept { return edgeSampleCount_; }
std::uint64_t BodyRenderMesh::revision() const noexcept { return revision_; }
double BodyRenderMesh::rebuildMilliseconds() const noexcept { return rebuildMilliseconds_; }
ViewportMeshQuality BodyRenderMesh::quality() const noexcept { return quality_; }
double BodyRenderMesh::linearDeflection() const noexcept { return linearDeflection_; }
double BodyRenderMesh::angularDeflection() const noexcept { return angularDeflection_; }

}  // namespace solidar
