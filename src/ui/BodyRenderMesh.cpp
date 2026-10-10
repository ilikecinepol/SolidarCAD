#include "ui/BodyRenderMesh.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <GeomAbs_CurveType.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Circ.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <utility>

namespace solidar {
namespace {

constexpr std::size_t kMaxRenderFaces = 1'000'000;
constexpr std::size_t kMaxRenderEdges = 2'000'000;
constexpr std::size_t kMaxRenderVertices = 5'000'000;
constexpr std::size_t kMaxRenderTriangles = 10'000'000;
constexpr std::size_t kMaxRenderEdgeSamples = 10'000'000;

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

bool finite(Point3d value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

bool finite(Vector3d value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

bool setFailure(GeometryFailure* failure, GeometryFailureKind kind,
                const char* detail) {
  if (failure) {
    failure->kind = kind;
    failure->detail = detail;
  }
  return false;
}

}  // namespace

std::size_t BodyMeshKeyHash::operator()(const BodyMeshKey& key) const noexcept {
  // This mix is intentionally not treated as identity. unordered_map/cache
  // lookups always follow it with BodyMeshKey::operator==.
  std::size_t result = std::hash<std::uint64_t>{}(key.bodyId);
  const auto combine = [&result](std::size_t value) {
    result ^= value + 0x9e3779b97f4a7c15ULL + (result << 6U) +
              (result >> 2U);
  };
  combine(std::hash<std::uint64_t>{}(key.featureId));
  combine(std::hash<std::uint64_t>{}(key.shapeRevision));
  combine(std::hash<const void*>{}(key.shapeIdentity));
  combine(std::hash<int>{}(static_cast<int>(key.quality)));
  return result;
}

void BodyRenderMesh::rebuild(const TopoDS_Shape& shape) noexcept {
  (void)tryRebuild(shape, ViewportMeshQuality::Normal);
}

void BodyRenderMesh::rebuild(const TopoDS_Shape& shape,
                             ViewportMeshQuality quality) noexcept {
  (void)tryRebuild(shape, quality);
}

bool BodyRenderMesh::tryRebuild(const TopoDS_Shape& shape,
                                GeometryFailure* failure) noexcept {
  return tryRebuild(shape, ViewportMeshQuality::Normal, failure);
}

bool BodyRenderMesh::tryRebuild(const TopoDS_Shape& shape,
                                ViewportMeshQuality quality,
                                GeometryFailure* failure) noexcept {
  ++buildAttemptCount_;
  GeometryFailure localFailure;
  GeometryFailure* const result = failure ? failure : &localFailure;
  return runGeometryOperation(
      [&]() -> bool {
        const auto started = std::chrono::steady_clock::now();
        if (shape.IsNull())
          return setFailure(result, GeometryFailureKind::InvalidInput,
                            "BodyRenderMesh input shape is null");
        if (quality != ViewportMeshQuality::Normal &&
            quality != ViewportMeshQuality::High)
          return setFailure(result, GeometryFailureKind::InvalidInput,
                            "BodyRenderMesh quality is invalid");

        // Reject obviously oversized topology before validation or meshing
        // can allocate additional OCCT data. Post-mesh checks still cap the
        // generated vertices, triangles and edge samples.
        std::size_t inputFaceCount = 0;
        for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
             explorer.Next()) {
          if (inputFaceCount >= kMaxRenderFaces)
            return setFailure(result, GeometryFailureKind::ResourceLimit,
                              "BodyRenderMesh face limit exceeded");
          ++inputFaceCount;
        }
        std::size_t inputEdgeCount = 0;
        for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More();
             explorer.Next()) {
          if (inputEdgeCount >= kMaxRenderEdges)
            return setFailure(result, GeometryFailureKind::ResourceLimit,
                              "BodyRenderMesh edge limit exceeded");
          ++inputEdgeCount;
        }
        BRepBuilderAPI_Copy isolatedCopy;
        // OCCT meshing may update both topology-attached triangulation and
        // geometry-owned caches. Copy both topology and geometry, while
        // deliberately omitting existing triangulation, so the pending render
        // operation cannot mutate any model-owned state.
        isolatedCopy.Perform(shape, Standard_True, Standard_False);
        if (!isolatedCopy.IsDone() || isolatedCopy.Shape().IsNull())
          return setFailure(result, GeometryFailureKind::InvalidBRep,
                            "BodyRenderMesh could not isolate input B-Rep");
        const TopoDS_Shape pendingShape = isolatedCopy.Shape();
        if (!BRepCheck_Analyzer(pendingShape).IsValid())
          return setFailure(result, GeometryFailureKind::InvalidBRep,
                            "BodyRenderMesh input B-Rep is invalid");

        std::vector<RenderVertex> pendingVertices;
        std::vector<std::uint32_t> pendingTriangleIndices;
        std::vector<RenderEdge> pendingEdges;
        Point3d pendingCenter{};
        double pendingDiagonal = 0.0;
        std::size_t pendingFaceCount = 0;
        std::size_t pendingEdgeSampleCount = 0;

        Bnd_Box bounds;
        BRepBndLib::Add(pendingShape, bounds);
        if (bounds.IsVoid())
          return setFailure(result, GeometryFailureKind::InvalidBRep,
                            "BodyRenderMesh input has no finite bounds");
        double x0 = 0.0;
        double y0 = 0.0;
        double z0 = 0.0;
        double x1 = 0.0;
        double y1 = 0.0;
        double z1 = 0.0;
        bounds.Get(x0, y0, z0, x1, y1, z1);
        if (!std::isfinite(x0) || !std::isfinite(y0) ||
            !std::isfinite(z0) || !std::isfinite(x1) ||
            !std::isfinite(y1) || !std::isfinite(z1) || x1 < x0 || y1 < y0 ||
            z1 < z0)
          return setFailure(result, GeometryFailureKind::InvalidBRep,
                            "BodyRenderMesh input bounds are invalid");
        pendingCenter = {x0 + (x1 - x0) * 0.5, y0 + (y1 - y0) * 0.5,
                         z0 + (z1 - z0) * 0.5};
        pendingDiagonal = std::hypot(x1 - x0, y1 - y0, z1 - z0);
        if (!finite(pendingCenter) || !std::isfinite(pendingDiagonal))
          return setFailure(result, GeometryFailureKind::InvalidBRep,
                            "BodyRenderMesh input bounds are non-finite");

        // Both profiles are scale-aware. High reduces the chord error
        // fourfold and angular error by more than half.
        const double pendingLinearDeflection =
            quality == ViewportMeshQuality::High
                ? std::clamp(pendingDiagonal * 0.00035, 0.005, 0.35)
                : std::clamp(pendingDiagonal * 0.0012, 0.02, 0.9);
        const double pendingAngularDeflection =
            quality == ViewportMeshQuality::High ? 0.08 : 0.15;
        if (!std::isfinite(pendingLinearDeflection) ||
            pendingLinearDeflection <= 0.0)
          return setFailure(result, GeometryFailureKind::InvalidMesh,
                            "BodyRenderMesh deflection is invalid");

        BRepMesh_IncrementalMesh mesher(pendingShape, pendingLinearDeflection,
                                        false, pendingAngularDeflection, true);
        if (!mesher.IsDone())
          return setFailure(result, GeometryFailureKind::MeshingFailed,
                            "BodyRenderMesh tessellation did not complete");

        for (TopExp_Explorer explorer(pendingShape, TopAbs_FACE);
             explorer.More();
             explorer.Next()) {
          if (pendingFaceCount >= kMaxRenderFaces)
            return setFailure(result, GeometryFailureKind::ResourceLimit,
                              "BodyRenderMesh face limit exceeded");
          const std::size_t faceIndex = pendingFaceCount++;
          const TopoDS_Face face = TopoDS::Face(explorer.Current());
          TopLoc_Location location;
          const Handle(Poly_Triangulation) mesh =
              BRep_Tool::Triangulation(face, location);
          if (mesh.IsNull())
            return setFailure(result, GeometryFailureKind::MeshingFailed,
                              "BodyRenderMesh face has no tessellation");
          const int nodeCount = mesh->NbNodes();
          const int triangleCount = mesh->NbTriangles();
          if (nodeCount <= 0 || triangleCount <= 0)
            return setFailure(result, GeometryFailureKind::InvalidMesh,
                              "BodyRenderMesh face tessellation is empty");
          if (static_cast<std::size_t>(nodeCount) >
                  kMaxRenderVertices - pendingVertices.size() ||
              static_cast<std::size_t>(triangleCount) >
                  kMaxRenderTriangles - pendingTriangleIndices.size() / 3)
            return setFailure(result, GeometryFailureKind::ResourceLimit,
                              "BodyRenderMesh tessellation limit exceeded");
          if (!mesh->HasNormals()) mesh->ComputeNormals();
          if (!mesh->HasNormals())
            return setFailure(result, GeometryFailureKind::InvalidMesh,
                              "BodyRenderMesh normals are unavailable");

          const gp_Trsf transform = location.Transformation();
          const bool reversed = face.Orientation() == TopAbs_REVERSED;
          const std::uint32_t firstVertex =
              static_cast<std::uint32_t>(pendingVertices.size());
          pendingVertices.reserve(pendingVertices.size() +
                                  static_cast<std::size_t>(nodeCount));
          for (int index = 1; index <= nodeCount; ++index) {
            const gp_Pnt p = mesh->Node(index).Transformed(transform);
            gp_Vec n(mesh->Normal(index));
            n.Transform(transform);
            if (reversed) n.Reverse();
            if (n.SquareMagnitude() > 1e-24) n.Normalize();
            const Point3d position = point(p);
            const Vector3d vertexNormal{n.X(), n.Y(), n.Z()};
            if (!finite(position) || !finite(vertexNormal))
              return setFailure(result, GeometryFailureKind::InvalidMesh,
                                "BodyRenderMesh vertex is non-finite");
            pendingVertices.push_back(
                {position, vertexNormal, static_cast<std::uint32_t>(faceIndex)});
          }
          pendingTriangleIndices.reserve(
              pendingTriangleIndices.size() +
              static_cast<std::size_t>(triangleCount) * 3);
          for (int index = 1; index <= triangleCount; ++index) {
            int ia = 0;
            int ib = 0;
            int ic = 0;
            mesh->Triangle(index).Get(ia, ib, ic);
            if (ia < 1 || ia > nodeCount || ib < 1 || ib > nodeCount ||
                ic < 1 || ic > nodeCount)
              return setFailure(result, GeometryFailureKind::InvalidMesh,
                                "BodyRenderMesh triangle index is invalid");
            Point3d a = point(mesh->Node(ia).Transformed(transform));
            Point3d b = point(mesh->Node(ib).Transformed(transform));
            Point3d c = point(mesh->Node(ic).Transformed(transform));
            if (!finite(a) || !finite(b) || !finite(c))
              return setFailure(result, GeometryFailureKind::InvalidMesh,
                                "BodyRenderMesh triangle is non-finite");
            if (reversed) std::swap(b, c);
            std::uint32_t va =
                firstVertex + static_cast<std::uint32_t>(ia - 1);
            std::uint32_t vb =
                firstVertex + static_cast<std::uint32_t>(ib - 1);
            std::uint32_t vc =
                firstVertex + static_cast<std::uint32_t>(ic - 1);
            if (reversed) std::swap(vb, vc);
            if (va >= pendingVertices.size() || vb >= pendingVertices.size() ||
                vc >= pendingVertices.size())
              return setFailure(result, GeometryFailureKind::InvalidMesh,
                                "BodyRenderMesh triangle range is invalid");
            if (!finite(normal(a, b, c, false)))
              return setFailure(result, GeometryFailureKind::InvalidMesh,
                                "BodyRenderMesh normal is non-finite");
            pendingTriangleIndices.insert(pendingTriangleIndices.end(),
                                          {va, vb, vc});
          }
        }

        std::size_t edgeIndex = 0;
        for (TopExp_Explorer explorer(pendingShape, TopAbs_EDGE);
             explorer.More();
             explorer.Next(), ++edgeIndex) {
          if (edgeIndex >= kMaxRenderEdges)
            return setFailure(result, GeometryFailureKind::ResourceLimit,
                              "BodyRenderMesh edge limit exceeded");
          const TopoDS_Edge edge = TopoDS::Edge(explorer.Current());
          if (BRep_Tool::Degenerated(edge)) continue;
          RenderEdge rendered;
          rendered.edgeIndex = edgeIndex;
          BRepAdaptor_Curve curve(edge);

          // Preserve native circular geometry so Sketcher projection can use
          // an exact circle/arc instead of a tessellated segment chain.
          if (curve.GetType() == GeomAbs_Circle) {
            const gp_Circ circle = curve.Circle();
            rendered.center = point(circle.Location());
            rendered.radius = circle.Radius();
            const gp_Pnt startPoint = curve.Value(curve.FirstParameter());
            const gp_Pnt endPoint = curve.Value(curve.LastParameter());
            if (!finite(rendered.center) ||
                !std::isfinite(rendered.radius) || rendered.radius <= 0.0 ||
                !finite(point(startPoint)) || !finite(point(endPoint)))
              return setFailure(result, GeometryFailureKind::InvalidMesh,
                                "BodyRenderMesh circular edge is invalid");
            const bool fullCircle =
                startPoint.Distance(endPoint) <=
                1e-6 * std::max(1.0, circle.Radius());
            if (fullCircle) {
              rendered.kind = RenderEdge::Kind::Circle;
            } else {
              rendered.kind = RenderEdge::Kind::Arc;
              rendered.arcStart = point(startPoint);
              rendered.arcEnd = point(endPoint);
            }
          }

          GCPnts_QuasiUniformDeflection sampler(
              curve, pendingLinearDeflection * 0.65);
          if (sampler.IsDone()) {
            const int sampleCount = sampler.NbPoints();
            if (sampleCount < 0 ||
                static_cast<std::size_t>(sampleCount) >
                    kMaxRenderEdgeSamples - pendingEdgeSampleCount)
              return setFailure(result, GeometryFailureKind::ResourceLimit,
                                "BodyRenderMesh edge sample limit exceeded");
            rendered.points.reserve(static_cast<std::size_t>(sampleCount));
            for (int index = 1; index <= sampleCount; ++index) {
              const Point3d sampledPoint = point(sampler.Value(index));
              if (!finite(sampledPoint))
                return setFailure(result, GeometryFailureKind::InvalidMesh,
                                  "BodyRenderMesh edge sample is non-finite");
              rendered.points.push_back(sampledPoint);
            }
          }
          if (rendered.points.size() >= 2) {
            pendingEdgeSampleCount += rendered.points.size();
            pendingEdges.push_back(std::move(rendered));
          }
        }

        if (pendingFaceCount != inputFaceCount || edgeIndex != inputEdgeCount)
          return setFailure(result, GeometryFailureKind::InvalidMesh,
                            "BodyRenderMesh topology count changed");
        if (pendingTriangleIndices.empty() && pendingEdges.empty())
          return setFailure(result, GeometryFailureKind::InvalidMesh,
                            "BodyRenderMesh produced no renderable geometry");
        if (pendingTriangleIndices.size() % 3 != 0)
          return setFailure(result, GeometryFailureKind::InvalidMesh,
                            "BodyRenderMesh index count is inconsistent");
        for (const std::uint32_t index : pendingTriangleIndices) {
          if (index >= pendingVertices.size())
            return setFailure(result, GeometryFailureKind::InvalidMesh,
                              "BodyRenderMesh index is out of range");
        }
        const double elapsed = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();
        if (!std::isfinite(elapsed))
          return setFailure(result, GeometryFailureKind::InvalidMesh,
                            "BodyRenderMesh timing is invalid");

        // Commit is intentionally the only point that mutates the published
        // cache. Every validation and allocation above operates on locals.
        vertices_.swap(pendingVertices);
        triangleIndices_.swap(pendingTriangleIndices);
        edges_.swap(pendingEdges);
        center_ = pendingCenter;
        diagonal_ = pendingDiagonal;
        faceCount_ = pendingFaceCount;
        edgeSampleCount_ = pendingEdgeSampleCount;
        rebuildMilliseconds_ = elapsed;
        quality_ = quality;
        linearDeflection_ = pendingLinearDeflection;
        angularDeflection_ = pendingAngularDeflection;
        ++revision_;
        return true;
      },
      result);
}

void BodyRenderMesh::clear() noexcept {
  std::vector<RenderVertex>().swap(vertices_);
  std::vector<std::uint32_t>().swap(triangleIndices_);
  std::vector<RenderEdge>().swap(edges_);
  center_ = {};
  diagonal_ = 0.0;
  faceCount_ = 0;
  edgeSampleCount_ = 0;
  linearDeflection_ = 0.0;
  angularDeflection_ = 0.0;
  rebuildMilliseconds_ = 0.0;
  ++revision_;
}

BodyRenderMesh::TriangleView BodyRenderMesh::triangles() const noexcept {
  return TriangleView(this);
}

std::size_t BodyRenderMesh::triangleCount() const noexcept {
  return triangleIndices_.size() / 3;
}

RenderTriangle BodyRenderMesh::triangle(std::size_t index) const noexcept {
  const std::size_t offset = index * 3;
  if (offset + 2 >= triangleIndices_.size()) return {};
  const std::uint32_t ia = triangleIndices_[offset];
  const std::uint32_t ib = triangleIndices_[offset + 1];
  const std::uint32_t ic = triangleIndices_[offset + 2];
  if (ia >= vertices_.size() || ib >= vertices_.size() ||
      ic >= vertices_.size())
    return {};
  const auto& a = vertices_[ia];
  const auto& b = vertices_[ib];
  const auto& c = vertices_[ic];
  return {a.position, b.position, c.position,
          normal(a.position, b.position, c.position, false),
          a.normal, b.normal, c.normal, a.faceIndex};
}

RenderTriangle BodyRenderMesh::TriangleView::Iterator::operator*() const noexcept {
  return mesh_ ? mesh_->triangle(index_) : RenderTriangle{};
}
BodyRenderMesh::TriangleView::Iterator&
BodyRenderMesh::TriangleView::Iterator::operator++() noexcept {
  ++index_;
  return *this;
}
BodyRenderMesh::TriangleView::Iterator
BodyRenderMesh::TriangleView::Iterator::operator++(int) noexcept {
  Iterator copy = *this;
  ++*this;
  return copy;
}
BodyRenderMesh::TriangleView::Iterator&
BodyRenderMesh::TriangleView::Iterator::operator--() noexcept {
  --index_;
  return *this;
}
BodyRenderMesh::TriangleView::Iterator&
BodyRenderMesh::TriangleView::Iterator::operator+=(difference_type offset) noexcept {
  index_ = static_cast<std::size_t>(static_cast<difference_type>(index_) + offset);
  return *this;
}
BodyRenderMesh::TriangleView::Iterator&
BodyRenderMesh::TriangleView::Iterator::operator-=(difference_type offset) noexcept {
  return *this += -offset;
}
BodyRenderMesh::TriangleView::Iterator
BodyRenderMesh::TriangleView::Iterator::operator+(difference_type offset) const noexcept {
  Iterator copy = *this;
  copy += offset;
  return copy;
}
BodyRenderMesh::TriangleView::Iterator
BodyRenderMesh::TriangleView::Iterator::operator-(difference_type offset) const noexcept {
  Iterator copy = *this;
  copy -= offset;
  return copy;
}
BodyRenderMesh::TriangleView::Iterator::difference_type
BodyRenderMesh::TriangleView::Iterator::operator-(const Iterator& other) const noexcept {
  return static_cast<difference_type>(index_) -
         static_cast<difference_type>(other.index_);
}
std::size_t BodyRenderMesh::TriangleView::size() const noexcept {
  return mesh_ ? mesh_->triangleCount() : 0;
}
bool BodyRenderMesh::TriangleView::empty() const noexcept { return size() == 0; }
RenderTriangle BodyRenderMesh::TriangleView::operator[](std::size_t index) const noexcept {
  return mesh_ ? mesh_->triangle(index) : RenderTriangle{};
}
BodyRenderMesh::TriangleView::Iterator BodyRenderMesh::TriangleView::begin() const noexcept {
  return Iterator(mesh_, 0);
}
BodyRenderMesh::TriangleView::Iterator BodyRenderMesh::TriangleView::end() const noexcept {
  return Iterator(mesh_, size());
}
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
std::uint64_t BodyRenderMesh::buildAttemptCount() const noexcept {
  return buildAttemptCount_;
}
std::size_t BodyRenderMesh::ownedBytes() const noexcept {
  std::size_t bytes = vertices_.capacity() * sizeof(RenderVertex) +
                      triangleIndices_.capacity() * sizeof(std::uint32_t) +
                      edges_.capacity() * sizeof(RenderEdge);
  for (const auto& edge : edges_)
    bytes += edge.points.capacity() * sizeof(Point3d);
  return bytes;
}

std::shared_ptr<const BodyRenderMesh> BodyRenderMeshCache::resolve(
    const BodyMeshKey& key, const TopoDS_Shape& shape,
    GeometryFailure* failure) {
  ++useClock_;
  for (auto& entry : entries_) {
    if (entry.key == key) {
      entry.lastUse = useClock_;
      ++stats_.hits;
      return entry.mesh;
    }
  }
  ++stats_.misses;
  ++stats_.buildAttempts;
  auto pending = std::make_shared<BodyRenderMesh>();
  if (!pending->tryRebuild(shape, key.quality, failure)) return {};
  entries_.push_back({key, pending, useClock_});
  return pending;
}

void BodyRenderMeshCache::retainActive(
    const std::vector<BodyMeshKey>& activeKeys,
    std::size_t maximumObsoleteEntries) {
  const auto isActive = [&activeKeys](const BodyMeshKey& key) {
    return std::find(activeKeys.begin(), activeKeys.end(), key) !=
           activeKeys.end();
  };
  std::vector<std::size_t> obsolete;
  obsolete.reserve(entries_.size());
  for (std::size_t index = 0; index < entries_.size(); ++index)
    if (!isActive(entries_[index].key)) obsolete.push_back(index);
  if (obsolete.size() <= maximumObsoleteEntries) return;
  std::sort(obsolete.begin(), obsolete.end(), [this](std::size_t left,
                                                     std::size_t right) {
    return entries_[left].lastUse > entries_[right].lastUse;
  });
  obsolete.resize(maximumObsoleteEntries);
  std::vector<Entry> retained;
  retained.reserve(activeKeys.size() + obsolete.size());
  for (auto& entry : entries_) {
    const bool keepObsolete =
        std::find(obsolete.begin(), obsolete.end(),
                  static_cast<std::size_t>(&entry - entries_.data())) !=
        obsolete.end();
    if (isActive(entry.key) || keepObsolete)
      retained.push_back(std::move(entry));
  }
  entries_ = std::move(retained);
}

void BodyRenderMeshCache::clear() noexcept {
  std::vector<Entry>().swap(entries_);
  ++useClock_;
}
const BodyMeshCacheStats& BodyRenderMeshCache::stats() const noexcept {
  return stats_;
}
std::size_t BodyRenderMeshCache::entryCount() const noexcept {
  return entries_.size();
}
std::size_t BodyRenderMeshCache::ownedBytes() const noexcept {
  std::size_t result = entries_.capacity() * sizeof(Entry);
  for (const auto& entry : entries_)
    if (entry.mesh) result += entry.mesh->ownedBytes();
  return result;
}

}  // namespace solidar
