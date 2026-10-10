#include "ui/BodyRenderMesh.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS.hxx>
#include <TopLoc_Location.hxx>
#include <Poly_Triangulation.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <array>
#include "TestAssertions.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

namespace {

bool samePoint(solidar::Point3d a, solidar::Point3d b) {
  return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool sameVector(solidar::Vector3d a, solidar::Vector3d b) {
  return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool sameMeshState(const solidar::BodyRenderMesh& a,
                   const solidar::BodyRenderMesh& b) {
  if (a.revision() != b.revision() || a.quality() != b.quality() ||
      a.diagonal() != b.diagonal() || a.faceCount() != b.faceCount() ||
      a.edgeSampleCount() != b.edgeSampleCount() ||
      a.rebuildMilliseconds() != b.rebuildMilliseconds() ||
      a.linearDeflection() != b.linearDeflection() ||
      a.angularDeflection() != b.angularDeflection() ||
      !samePoint(a.center(), b.center()) ||
      a.triangles().size() != b.triangles().size() ||
      a.vertices().size() != b.vertices().size() ||
      a.triangleIndices() != b.triangleIndices() ||
      a.edges().size() != b.edges().size())
    return false;

  for (std::size_t i = 0; i < a.triangles().size(); ++i) {
    const auto& left = a.triangles()[i];
    const auto& right = b.triangles()[i];
    if (!samePoint(left.a, right.a) || !samePoint(left.b, right.b) ||
        !samePoint(left.c, right.c) ||
        !sameVector(left.normal, right.normal) ||
        !sameVector(left.normalA, right.normalA) ||
        !sameVector(left.normalB, right.normalB) ||
        !sameVector(left.normalC, right.normalC) ||
        left.faceIndex != right.faceIndex)
      return false;
  }
  for (std::size_t i = 0; i < a.vertices().size(); ++i) {
    const auto& left = a.vertices()[i];
    const auto& right = b.vertices()[i];
    if (!samePoint(left.position, right.position) ||
        !sameVector(left.normal, right.normal) ||
        left.faceIndex != right.faceIndex)
      return false;
  }
  for (std::size_t i = 0; i < a.edges().size(); ++i) {
    const auto& left = a.edges()[i];
    const auto& right = b.edges()[i];
    if (left.edgeIndex != right.edgeIndex || left.kind != right.kind ||
        left.radius != right.radius || !samePoint(left.center, right.center) ||
        !samePoint(left.arcStart, right.arcStart) ||
        !samePoint(left.arcEnd, right.arcEnd) ||
        left.points.size() != right.points.size())
      return false;
    for (std::size_t pointIndex = 0; pointIndex < left.points.size();
         ++pointIndex) {
      if (!samePoint(left.points[pointIndex], right.points[pointIndex]))
        return false;
    }
  }
  return true;
}

bool hasTriangulation(const TopoDS_Shape& shape) {
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next()) {
    TopLoc_Location location;
    if (!BRep_Tool::Triangulation(TopoDS::Face(explorer.Current()), location)
             .IsNull())
      return true;
  }
  return false;
}

struct SourceTriangulationSnapshot {
  Handle(Poly_Triangulation) identity;
  double deflection{};
  bool hasNormals{};
  std::vector<solidar::Point3d> nodes;
  std::vector<solidar::Vector3d> normals;
  std::vector<std::array<int, 3>> triangles;
};

std::vector<SourceTriangulationSnapshot> snapshotTriangulations(
    const TopoDS_Shape& shape, bool* complete) {
  *complete = true;
  std::vector<SourceTriangulationSnapshot> snapshots;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next()) {
    TopLoc_Location location;
    const Handle(Poly_Triangulation) mesh =
        BRep_Tool::Triangulation(TopoDS::Face(explorer.Current()), location);
    if (mesh.IsNull()) {
      *complete = false;
      snapshots.push_back({});
      continue;
    }
    SourceTriangulationSnapshot snapshot;
    snapshot.identity = mesh;
    snapshot.deflection = mesh->Deflection();
    snapshot.hasNormals = mesh->HasNormals();
    snapshot.nodes.reserve(static_cast<std::size_t>(mesh->NbNodes()));
    if (snapshot.hasNormals)
      snapshot.normals.reserve(static_cast<std::size_t>(mesh->NbNodes()));
    for (int index = 1; index <= mesh->NbNodes(); ++index) {
      const gp_Pnt node = mesh->Node(index);
      snapshot.nodes.push_back({node.X(), node.Y(), node.Z()});
      if (snapshot.hasNormals) {
        const gp_Dir normal = mesh->Normal(index);
        snapshot.normals.push_back({normal.X(), normal.Y(), normal.Z()});
      }
    }
    snapshot.triangles.reserve(
        static_cast<std::size_t>(mesh->NbTriangles()));
    for (int index = 1; index <= mesh->NbTriangles(); ++index) {
      int a = 0;
      int b = 0;
      int c = 0;
      mesh->Triangle(index).Get(a, b, c);
      snapshot.triangles.push_back({a, b, c});
    }
    snapshots.push_back(std::move(snapshot));
  }
  return snapshots;
}

bool sameTriangulations(
    const std::vector<SourceTriangulationSnapshot>& before,
    const std::vector<SourceTriangulationSnapshot>& after) {
  if (before.size() != after.size()) return false;
  for (std::size_t face = 0; face < before.size(); ++face) {
    const auto& left = before[face];
    const auto& right = after[face];
    if (left.identity != right.identity ||
        left.deflection != right.deflection ||
        left.hasNormals != right.hasNormals ||
        left.nodes.size() != right.nodes.size() ||
        left.normals.size() != right.normals.size() ||
        left.triangles != right.triangles)
      return false;
    for (std::size_t node = 0; node < left.nodes.size(); ++node)
      if (!samePoint(left.nodes[node], right.nodes[node])) return false;
    for (std::size_t normal = 0; normal < left.normals.size(); ++normal)
      if (!sameVector(left.normals[normal], right.normals[normal])) return false;
  }
  return true;
}

}  // namespace

int main() {
  solidar::BodyRenderMesh cache;
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(80.0, 35.0, 50.0).Shape();
  cache.rebuild(box);
  CHECK(cache.faceCount() == 6);
  CHECK(!cache.triangles().empty());
  CHECK(!cache.edges().empty());
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
      CHECK(edge.edgeIndex == expectedIndex);

    // Every curved rim edge is classified as a native circle/arc (never a
    // tessellated chain) and preserves the source radius.
    CHECK(edge.kind == solidar::RenderEdge::Kind::Circle ||
           edge.kind == solidar::RenderEdge::Kind::Arc);
    CHECK(std::abs(edge.radius - 10.0) <= 1e-6);
  }
  CHECK(curvedEdges >= 2);

  // A true full circle edge is classified as a Circle (not an Arc).
  const gp_Circ fullCircle(
      gp_Ax2(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)), 5.0);
  TopoDS_Compound circleCompound;
  BRep_Builder circleBuilder;
  circleBuilder.MakeCompound(circleCompound);
  circleBuilder.Add(circleCompound, BRepBuilderAPI_MakeEdge(fullCircle).Edge());
  cache.rebuild(circleCompound);
  CHECK(cache.edges().size() == 1);
  CHECK(cache.edges()[0].kind == solidar::RenderEdge::Kind::Circle);
  CHECK(std::abs(cache.edges()[0].radius - 5.0) <= 1e-6);

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
  CHECK(cache.edges().size() == 1);
  CHECK(cache.edges()[0].kind == solidar::RenderEdge::Kind::Arc);
  CHECK(std::abs(cache.edges()[0].radius - 5.0) <= 1e-6);

  const TopoDS_Shape tool =
      BRepPrimAPI_MakeBox(gp_Pnt(30.0, 12.5, 30.0), 20.0, 10.0, 20.0).Shape();
  const TopoDS_Shape pocket = BRepAlgoAPI_Cut(box, tool).Shape();
  cache.rebuild(pocket);
  CHECK(cache.faceCount() > 6);
  CHECK(cache.triangles().size() > boxTriangles);
  CHECK(!cache.edges().empty());

  // A non-null but structurally invalid OCCT face must not replace the last
  // valid tessellation. Repeating the failure must also produce the same
  // caller-visible category/detail without advancing the cache revision.
  const solidar::BodyRenderMesh pocketSnapshot = cache;
  TopoDS_Face invalidFace;
  BRep_Builder invalidFaceBuilder;
  invalidFaceBuilder.MakeFace(invalidFace);
  CHECK(!invalidFace.IsNull());

  solidar::GeometryFailure firstFailure;
  CHECK(!cache.tryRebuild(invalidFace, solidar::ViewportMeshQuality::High,
                          &firstFailure));
  CHECK(firstFailure.kind == solidar::GeometryFailureKind::InvalidBRep);
  CHECK(!firstFailure.detail.empty());
  CHECK(sameMeshState(cache, pocketSnapshot));

  solidar::GeometryFailure repeatedFailure;
  CHECK(!cache.tryRebuild(invalidFace, solidar::ViewportMeshQuality::High,
                          &repeatedFailure));
  CHECK(repeatedFailure.kind == firstFailure.kind);
  CHECK(!repeatedFailure.detail.empty());
  CHECK(sameMeshState(cache, pocketSnapshot));

  const std::uint64_t revisionBeforeSuccess = cache.revision();
  solidar::GeometryFailure successFailure;
  CHECK(cache.tryRebuild(box, solidar::ViewportMeshQuality::High,
                         &successFailure));
  CHECK(successFailure.kind == solidar::GeometryFailureKind::None);
  CHECK(successFailure.detail.empty());
  CHECK(cache.revision() == revisionBeforeSuccess + 1);
  CHECK(cache.quality() == solidar::ViewportMeshQuality::High);
  CHECK(!cache.triangles().empty());

  // Tessellation is a private render-cache concern. OCCT attaches generated
  // Poly_Triangulation data to a TShape, so meshing a shared model Shape
  // directly would mutate the committed model even when the cache itself is
  // transactional. The isolated copy must leave the source topology clean.
  TopoDS_Shape isolatedSource =
      BRepPrimAPI_MakeBox(25.0, 20.0, 15.0).Shape();
  BRepTools::Clean(isolatedSource);
  CHECK(!hasTriangulation(isolatedSource));
  const std::uint64_t revisionBeforeIsolation = cache.revision();
  CHECK(cache.tryRebuild(isolatedSource, solidar::ViewportMeshQuality::Normal));
  CHECK(cache.revision() == revisionBeforeIsolation + 1);
  CHECK(!hasTriangulation(isolatedSource));

  // A pre-meshed source is the stronger isolation case: rebuilding at a much
  // finer quality must neither replace its Poly_Triangulation handles nor
  // modify their deflection, nodes, connectivity or normals in place.
  TopoDS_Shape premeshedSource =
      BRepPrimAPI_MakeCylinder(30.0, 60.0).Shape();
  BRepTools::Clean(premeshedSource);
  constexpr double sourceDeflection = 4.0;
  BRepMesh_IncrementalMesh sourceMesher(premeshedSource, sourceDeflection,
                                        false, 0.8, false);
  CHECK(sourceMesher.IsDone());
  bool sourceCompleteBefore = false;
  const auto sourceBefore =
      snapshotTriangulations(premeshedSource, &sourceCompleteBefore);
  CHECK(sourceCompleteBefore);
  CHECK(!sourceBefore.empty());

  const std::uint64_t revisionBeforePremeshed = cache.revision();
  CHECK(cache.tryRebuild(premeshedSource,
                         solidar::ViewportMeshQuality::High));
  CHECK(cache.revision() == revisionBeforePremeshed + 1);
  CHECK(cache.linearDeflection() < sourceDeflection);
  bool sourceCompleteAfter = false;
  const auto sourceAfter =
      snapshotTriangulations(premeshedSource, &sourceCompleteAfter);
  CHECK(sourceCompleteAfter);
  CHECK(sameTriangulations(sourceBefore, sourceAfter));

  // More than 32 simultaneously visible bodies must remain cache-resident.
  // The old fixed LRU cap caused a complete miss cascade on every refresh.
  {
    solidar::BodyRenderMeshCache bodyCache;
    std::vector<solidar::BodyMeshKey> active;
    active.reserve(40);
    for (std::uint64_t body = 1; body <= 40; ++body) {
      const solidar::BodyMeshKey key{
          body, body + 100, 1, &box,
          solidar::ViewportMeshQuality::Normal};
      active.push_back(key);
      CHECK(bodyCache.resolve(key, box));
    }
    CHECK(bodyCache.stats().buildAttempts == 40);
    bodyCache.retainActive(active);
    CHECK(bodyCache.entryCount() == 40);
    const auto attemptsBeforeRefresh = bodyCache.stats().buildAttempts;
    for (const auto& key : active) CHECK(bodyCache.resolve(key, box));
    CHECK(bodyCache.stats().buildAttempts == attemptsBeforeRefresh);
    CHECK(bodyCache.stats().hits == 40);

    // Obsolete generations are bounded without evicting current bodies.
    bodyCache.retainActive({active[0], active[1]}, 3);
    CHECK(bodyCache.entryCount() == 5);
    CHECK(bodyCache.resolve(active[0], box));
    CHECK(bodyCache.resolve(active[1], box));
  }

  cache.clear();
  CHECK(cache.triangles().empty());
  CHECK(cache.edges().empty());
}
