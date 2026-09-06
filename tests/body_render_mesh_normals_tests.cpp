#include "ui/BodyRenderMesh.h"
#include "model/TopologyReferenceResolver.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <vector>

namespace {

double dot(solidar::Vector3d a, solidar::Vector3d b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

double distance(solidar::Point3d a, solidar::Point3d b) {
  return std::sqrt((a.x - b.x) * (a.x - b.x) +
                   (a.y - b.y) * (a.y - b.y) +
                   (a.z - b.z) * (a.z - b.z));
}

double circlePolylineError(const solidar::BodyRenderMesh& mesh, double radius) {
  double worst = 0.0;
  for (const auto& edge : mesh.edges()) {
    if (edge.points.size() < 3) continue;
    for (std::size_t i = 1; i < edge.points.size(); ++i) {
      const auto& a = edge.points[i - 1];
      const auto& b = edge.points[i];
      if (std::abs(std::hypot(a.x, a.y) - radius) > 1e-5 ||
          std::abs(std::hypot(b.x, b.y) - radius) > 1e-5)
        continue;
      const double mx = (a.x + b.x) * 0.5;
      const double my = (a.y + b.y) * 0.5;
      worst = std::max(worst, radius - std::hypot(mx, my));
    }
  }
  return worst;
}

}  // namespace

int main() {
  using solidar::BodyRenderMesh;
  using solidar::ViewportMeshQuality;

  BodyRenderMesh box;
  box.rebuild(BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape());
  assert(box.vertices().size() > 6);
  assert(box.triangleIndices().size() == box.triangles().size() * 3);

  // Normals within every planar topological face are constant.
  std::map<std::uint32_t, solidar::Vector3d> planarNormals;
  for (const auto& vertex : box.vertices()) {
    const auto [it, inserted] = planarNormals.emplace(vertex.faceIndex, vertex.normal);
    if (!inserted) assert(dot(it->second, vertex.normal) > 0.9999);
  }

  // Coincident vertices on different box faces keep the sharp discontinuity.
  bool foundSharpBoundary = false;
  for (std::size_t a = 0; a < box.vertices().size(); ++a) {
    for (std::size_t b = a + 1; b < box.vertices().size(); ++b) {
      if (box.vertices()[a].faceIndex == box.vertices()[b].faceIndex) continue;
      if (distance(box.vertices()[a].position, box.vertices()[b].position) < 1e-7 &&
          dot(box.vertices()[a].normal, box.vertices()[b].normal) < 0.1)
        foundSharpBoundary = true;
    }
  }
  assert(foundSharpBoundary);

  BodyRenderMesh normalCylinder;
  BodyRenderMesh highCylinder;
  const auto cylinder = BRepPrimAPI_MakeCylinder(35.0, 80.0).Shape();
  normalCylinder.rebuild(cylinder, ViewportMeshQuality::Normal);
  highCylinder.rebuild(cylinder, ViewportMeshQuality::High);
  assert(highCylinder.triangles().size() > normalCylinder.triangles().size());
  assert(highCylinder.edgeSampleCount() >= normalCylinder.edgeSampleCount());
  assert(highCylinder.linearDeflection() < normalCylinder.linearDeflection());
  assert(highCylinder.angularDeflection() < normalCylinder.angularDeflection());
  const double normalCircleError = circlePolylineError(normalCylinder, 35.0);
  const double highCircleError = circlePolylineError(highCylinder, 35.0);
  assert(normalCircleError > 0.0);
  assert(highCircleError > 0.0);
  assert(highCircleError < normalCircleError);

  const auto referenceBefore = solidar::makeEdgeReference(cylinder, 7, 11, 0);
  BodyRenderMesh qualitySwitch;
  qualitySwitch.rebuild(cylinder, ViewportMeshQuality::Normal);
  qualitySwitch.rebuild(cylinder, ViewportMeshQuality::High);
  const auto referenceAfter = solidar::makeEdgeReference(cylinder, 7, 11, 0);
  assert(referenceBefore == referenceAfter);

  std::cout << "cylinder Normal: vertices=" << normalCylinder.vertices().size()
            << " triangles=" << normalCylinder.triangles().size()
            << " edge_samples=" << normalCylinder.edgeSampleCount()
            << " rebuild_ms=" << normalCylinder.rebuildMilliseconds()
            << " chord_error=" << normalCircleError << '\n';
  std::cout << "cylinder High: vertices=" << highCylinder.vertices().size()
            << " triangles=" << highCylinder.triangles().size()
            << " edge_samples=" << highCylinder.edgeSampleCount()
            << " rebuild_ms=" << highCylinder.rebuildMilliseconds()
            << " chord_error=" << highCircleError << '\n';

  // A curved face has smoothly varying per-node normals, not one flat normal.
  bool foundSmoothFace = false;
  for (std::size_t a = 0; a < normalCylinder.vertices().size(); ++a) {
    for (std::size_t b = a + 1; b < normalCylinder.vertices().size(); ++b) {
      if (normalCylinder.vertices()[a].faceIndex !=
          normalCylinder.vertices()[b].faceIndex)
        continue;
      const double d = dot(normalCylinder.vertices()[a].normal,
                           normalCylinder.vertices()[b].normal);
      if (d > -0.2 && d < 0.98) foundSmoothFace = true;
    }
  }
  assert(foundSmoothFace);

  const TopoDS_Face plane = BRepBuilderAPI_MakeFace(
      gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), -10.0, 10.0, -10.0, 10.0);
  BodyRenderMesh forward;
  BodyRenderMesh reversed;
  forward.rebuild(plane);
  reversed.rebuild(plane.Reversed());
  assert(!forward.vertices().empty() && !reversed.vertices().empty());
  assert(dot(forward.vertices().front().normal,
             reversed.vertices().front().normal) < -0.999);
}
