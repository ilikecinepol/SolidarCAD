#include "model/TopologyReferenceResolver.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <NCollection_IndexedMap.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Circ.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace solidar {
namespace {

std::atomic<std::uint64_t> topologyBuildAttempts{0};

using ShapeIndexedMap =
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;

double distance(TopologyPoint3d a, TopologyPoint3d b) {
  return std::hypot(std::hypot(a.x - b.x, a.y - b.y), a.z - b.z);
}

double dot(TopologyPoint3d a, TopologyPoint3d b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

TopologyPoint3d normalized(TopologyPoint3d value) {
  const double length = std::hypot(std::hypot(value.x, value.y), value.z);
  return length > 1e-12
             ? TopologyPoint3d{value.x / length, value.y / length,
                               value.z / length}
             : TopologyPoint3d{};
}

bool finitePoint(TopologyPoint3d value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

bool finiteBounds(const TopologyBounds& bounds) noexcept {
  return finitePoint(bounds.minimum) && finitePoint(bounds.maximum) &&
         bounds.minimum.x <= bounds.maximum.x &&
         bounds.minimum.y <= bounds.maximum.y &&
         bounds.minimum.z <= bounds.maximum.z;
}

bool finiteFaceSignature(const FaceSignature& signature) noexcept {
  return std::isfinite(signature.area) && signature.area >= 0.0 &&
         finitePoint(signature.centroid) && finitePoint(signature.normal) &&
         finiteBounds(signature.bounds) && std::isfinite(signature.radius) &&
         signature.radius >= 0.0 && finitePoint(signature.axis);
}

bool finiteEdgeSignature(const EdgeSignature& signature) noexcept {
  return std::isfinite(signature.length) && signature.length >= 0.0 &&
         finitePoint(signature.midpoint) && finitePoint(signature.tangent) &&
         finiteBounds(signature.bounds) && std::isfinite(signature.radius) &&
         signature.radius >= 0.0 && finitePoint(signature.center);
}

std::optional<TopologyBounds> boundsOf(const TopoDS_Shape& shape,
                                       std::string* error) {
  Bnd_Box box;
  BRepBndLib::Add(shape, box);
  if (box.IsVoid()) {
    if (error) *error = "Topology source shape has no finite bounds";
    return std::nullopt;
  }
  TopologyBounds result;
  box.Get(result.minimum.x, result.minimum.y, result.minimum.z,
          result.maximum.x, result.maximum.y, result.maximum.z);
  if (!finiteBounds(result)) {
    if (error) *error = "Topology source shape has non-finite bounds";
    return std::nullopt;
  }
  return result;
}

double diagonal(const TopologyBounds& bounds) {
  return std::max(distance(bounds.minimum, bounds.maximum), 1e-9);
}

double relativeDifference(double first, double second) {
  return std::abs(first - second) /
         std::max({std::abs(first), std::abs(second), 1e-9});
}

SurfaceKind surfaceKind(GeomAbs_SurfaceType type) {
  if (type == GeomAbs_Plane) return SurfaceKind::Plane;
  if (type == GeomAbs_Cylinder) return SurfaceKind::Cylinder;
  return SurfaceKind::Unknown;
}

CurveKind curveKind(GeomAbs_CurveType type) {
  switch (type) {
    case GeomAbs_Line: return CurveKind::Line;
    case GeomAbs_Circle: return CurveKind::Circle;
    case GeomAbs_Ellipse: return CurveKind::Ellipse;
    default: return CurveKind::Other;
  }
}

FaceSignature signatureOf(const TopoDS_Face& face, std::string* error) {
  FaceSignature signature;
  GProp_GProps properties;
  BRepGProp::SurfaceProperties(face, properties);
  signature.area = properties.Mass();
  const gp_Pnt center = properties.CentreOfMass();
  signature.centroid = {center.X(), center.Y(), center.Z()};
  const auto bounds = boundsOf(face, error);
  if (!bounds) return {};
  signature.bounds = *bounds;

  BRepAdaptor_Surface surface(face, true);
  signature.surface = surfaceKind(surface.GetType());
  if (signature.surface == SurfaceKind::Plane) {
    auto direction = surface.Plane().Axis().Direction();
    if (face.Orientation() == TopAbs_REVERSED) direction.Reverse();
    signature.normal = {direction.X(), direction.Y(), direction.Z()};
  } else if (signature.surface == SurfaceKind::Cylinder) {
    const auto cylinder = surface.Cylinder();
    signature.radius = cylinder.Radius();
    const auto direction = cylinder.Axis().Direction();
    signature.axis = {direction.X(), direction.Y(), direction.Z()};
  }
  if (!finiteFaceSignature(signature) && error)
    *error = "Topology face signature contains non-finite data";
  return signature;
}

EdgeSignature signatureOf(const TopoDS_Edge& edge, std::string* error) {
  EdgeSignature signature;
  GProp_GProps properties;
  BRepGProp::LinearProperties(edge, properties);
  signature.length = properties.Mass();
  const gp_Pnt center = properties.CentreOfMass();
  signature.midpoint = {center.X(), center.Y(), center.Z()};
  const auto bounds = boundsOf(edge, error);
  if (!bounds) return {};
  signature.bounds = *bounds;

  BRepAdaptor_Curve curve(edge);
  signature.curve = curveKind(curve.GetType());
  const double parameter =
      (curve.FirstParameter() + curve.LastParameter()) * 0.5;
  gp_Pnt point;
  gp_Vec derivative;
  curve.D1(parameter, point, derivative);
  signature.tangent = normalized(
      {derivative.X(), derivative.Y(), derivative.Z()});
  if (signature.curve == CurveKind::Circle) {
    const gp_Circ circle = curve.Circle();
    signature.radius = circle.Radius();
    const gp_Pnt location = circle.Location();
    signature.center = {location.X(), location.Y(), location.Z()};
  }
  if (!finiteEdgeSignature(signature) && error)
    *error = "Topology edge signature contains non-finite data";
  return signature;
}

std::string automaticFaceTag(const FaceSignature& face,
                             const TopologyBounds& shapeBounds) {
  if (face.surface != SurfaceKind::Plane) return {};
  struct Axis {
    double normal;
    double center;
    double minimum;
    double maximum;
    const char* name;
  };
  const Axis axes[]{{face.normal.x, face.centroid.x, shapeBounds.minimum.x,
                     shapeBounds.maximum.x, "x"},
                    {face.normal.y, face.centroid.y, shapeBounds.minimum.y,
                     shapeBounds.maximum.y, "y"},
                    {face.normal.z, face.centroid.z, shapeBounds.minimum.z,
                     shapeBounds.maximum.z, "z"}};
  const double tolerance = diagonal(shapeBounds) * 1e-6 + 1e-7;
  for (const auto& axis : axes) {
    if (axis.normal > 0.999 && std::abs(axis.center - axis.maximum) <= tolerance)
      return std::string("planar:max-") + axis.name;
    if (axis.normal < -0.999 && std::abs(axis.center - axis.minimum) <= tolerance)
      return std::string("planar:min-") + axis.name;
  }
  return {};
}

TopologyPoint3d boundsCenter(const TopologyBounds& bounds) {
  return {(bounds.minimum.x + bounds.maximum.x) * 0.5,
          (bounds.minimum.y + bounds.maximum.y) * 0.5,
          (bounds.minimum.z + bounds.maximum.z) * 0.5};
}

TopologyPoint3d boundsExtent(const TopologyBounds& bounds) {
  return {bounds.maximum.x - bounds.minimum.x,
          bounds.maximum.y - bounds.minimum.y,
          bounds.maximum.z - bounds.minimum.z};
}

std::optional<double> boundsScore(const TopologyBounds& saved,
                                  const TopologyBounds& candidate,
                                  double targetShapeDiagonal) {
  if (!finiteBounds(saved) || !finiteBounds(candidate)) return std::nullopt;
  const auto savedExtent = boundsExtent(saved);
  const auto candidateExtent = boundsExtent(candidate);
  const double extent = std::max(
      {relativeDifference(savedExtent.x, candidateExtent.x),
       relativeDifference(savedExtent.y, candidateExtent.y),
       relativeDifference(savedExtent.z, candidateExtent.z)});
  const double position =
      distance(boundsCenter(saved), boundsCenter(candidate)) /
      std::max(targetShapeDiagonal, 1e-9);
  if (extent > kTopologyMeasureRelativeTolerance ||
      position > kTopologyPositionRelativeTolerance)
    return std::nullopt;
  return extent + position;
}

std::optional<double> faceScore(const FaceSignature& saved,
                                const FaceSignature& candidate,
                                double targetShapeDiagonal) {
  if (!finiteFaceSignature(saved) || !finiteFaceSignature(candidate) ||
      saved.surface != candidate.surface)
    return std::nullopt;
  const double measure = relativeDifference(saved.area, candidate.area);
  const double position = distance(saved.centroid, candidate.centroid) /
                          std::max(targetShapeDiagonal, 1e-9);
  if (measure > kTopologyMeasureRelativeTolerance ||
      position > kTopologyPositionRelativeTolerance)
    return std::nullopt;
  const auto bounds =
      boundsScore(saved.bounds, candidate.bounds, targetShapeDiagonal);
  if (!bounds) return std::nullopt;
  double direction = 0.0;
  if (saved.surface == SurfaceKind::Plane) {
    const double cosine =
        dot(normalized(saved.normal), normalized(candidate.normal));
    direction = 1.0 - cosine;
    if (cosine < kTopologyDirectionCosineTolerance) return std::nullopt;
  } else if (saved.surface == SurfaceKind::Cylinder) {
    const double cosine = std::abs(dot(normalized(saved.axis),
                                       normalized(candidate.axis)));
    direction = 1.0 - cosine;
    if (cosine < kTopologyDirectionCosineTolerance) return std::nullopt;
    if (relativeDifference(saved.radius, candidate.radius) >
        kTopologyMeasureRelativeTolerance)
      return std::nullopt;
  }
  return measure + position + direction + *bounds;
}

std::optional<double> edgeScore(const EdgeSignature& saved,
                                const EdgeSignature& candidate,
                                double targetShapeDiagonal) {
  if (!finiteEdgeSignature(saved) || !finiteEdgeSignature(candidate) ||
      saved.curve != candidate.curve)
    return std::nullopt;
  const double measure = relativeDifference(saved.length, candidate.length);
  const double position = distance(saved.midpoint, candidate.midpoint) /
                          std::max(targetShapeDiagonal, 1e-9);
  const double direction =
      1.0 - std::abs(dot(normalized(saved.tangent),
                         normalized(candidate.tangent)));
  if (measure > kTopologyMeasureRelativeTolerance ||
      position > kTopologyPositionRelativeTolerance ||
      direction > 1.0 - kTopologyDirectionCosineTolerance)
    return std::nullopt;
  const auto bounds =
      boundsScore(saved.bounds, candidate.bounds, targetShapeDiagonal);
  if (!bounds) return std::nullopt;
  if (saved.curve == CurveKind::Circle) {
    if (relativeDifference(saved.radius, candidate.radius) >
            kTopologyMeasureRelativeTolerance ||
        distance(saved.center, candidate.center) /
                std::max(targetShapeDiagonal, 1e-9) >
            kTopologyPositionRelativeTolerance)
      return std::nullopt;
  }
  return measure + position + direction + *bounds;
}

}  // namespace

struct TopologyIndex::Data {
  struct FaceCandidate {
    TopoDS_Face face;
    std::size_t index{};
    FaceSignature signature;
    std::string tag;
    std::vector<std::size_t> rawAliases;
  };
  struct EdgeCandidate {
    TopoDS_Edge edge;
    std::size_t index{};
    EdgeSignature signature;
    std::string tag;
    std::vector<std::size_t> rawAliases;
  };

  ShapePtr shape;
  ShapeRevision revision{kInvalidShapeRevision};
  TopologyBounds bounds;
  double shapeDiagonal{1e-9};
  std::vector<FaceCandidate> faces;
  std::vector<EdgeCandidate> edges;
  std::vector<std::size_t> faceByRawOrdinal;
  std::vector<std::size_t> edgeByRawOrdinal;
};

namespace {

FaceResolution resolvedFace(const TopologyIndex::Data::FaceCandidate& candidate,
                            TopologyMatchMethod method) {
  FaceResolution result;
  result.subshape = candidate.face;
  result.index = candidate.index;
  result.method = method;
  return result;
}

EdgeResolution resolvedEdge(const TopologyIndex::Data::EdgeCandidate& candidate,
                            TopologyMatchMethod method) {
  EdgeResolution result;
  result.subshape = candidate.edge;
  result.index = candidate.index;
  result.method = method;
  return result;
}

template <typename Candidate, typename Signature, typename Score>
std::optional<std::size_t> uniqueBest(
    const std::vector<Candidate>& candidates, const Signature& signature,
    double shapeDiagonal, const std::vector<std::size_t>& subset, Score score,
    bool* ambiguous) {
  std::vector<std::pair<double, std::size_t>> matches;
  matches.reserve(subset.size());
  for (const std::size_t index : subset)
    if (const auto value = score(signature, candidates[index].signature,
                                 shapeDiagonal))
      matches.emplace_back(*value, index);
  std::sort(matches.begin(), matches.end(), [](const auto& left,
                                                const auto& right) {
    if (left.first != right.first) return left.first < right.first;
    return left.second < right.second;
  });
  if (matches.empty()) return std::nullopt;
  if (matches.size() > 1 &&
      matches[1].first - matches[0].first <=
          kTopologyAmbiguityScoreTolerance) {
    if (ambiguous) *ambiguous = true;
    return std::nullopt;
  }
  return matches.front().second;
}

std::vector<std::size_t> allIndices(std::size_t size) {
  std::vector<std::size_t> result(size);
  for (std::size_t index = 0; index < size; ++index) result[index] = index;
  return result;
}

}  // namespace

TopologyIndex::TopologyIndex(std::shared_ptr<const Data> data) noexcept
    : data_(std::move(data)) {}

std::shared_ptr<const TopologyIndex> TopologyIndex::build(
    ShapePtr shape, ShapeRevision revision, std::string* error) {
  topologyBuildAttempts.fetch_add(1, std::memory_order_relaxed);
  if (error) error->clear();
  if (!shape || shape->IsNull()) {
    if (error) *error = "Topology source shape is unavailable";
    return {};
  }
  try {
    auto data = std::make_shared<Data>();
    data->shape = std::move(shape);
    data->revision = revision;
    std::string diagnostic;
    const auto shapeBounds = boundsOf(*data->shape, &diagnostic);
    if (!shapeBounds) {
      if (error) *error = std::move(diagnostic);
      return {};
    }
    data->bounds = *shapeBounds;
    data->shapeDiagonal = diagonal(*shapeBounds);

    ShapeIndexedMap faceMap;
    std::size_t rawIndex = 0;
    for (TopExp_Explorer explorer(*data->shape, TopAbs_FACE); explorer.More();
         explorer.Next(), ++rawIndex) {
      const TopoDS_Face face = TopoDS::Face(explorer.Current());
      int mapped = faceMap.FindIndex(face);
      if (mapped == 0) {
        faceMap.Add(face);
        mapped = faceMap.FindIndex(face);
        std::string signatureError;
        const FaceSignature signature = signatureOf(face, &signatureError);
        if (!signatureError.empty() || !finiteFaceSignature(signature)) {
          if (error)
            *error = signatureError.empty()
                         ? "Topology face signature is invalid"
                         : std::move(signatureError);
          return {};
        }
        data->faces.push_back({face, rawIndex, signature,
                               automaticFaceTag(signature, data->bounds),
                               {rawIndex}});
      } else {
        data->faces[static_cast<std::size_t>(mapped - 1)]
            .rawAliases.push_back(rawIndex);
      }
      data->faceByRawOrdinal.push_back(
          static_cast<std::size_t>(mapped - 1));
    }

    ShapeIndexedMap edgeMap;
    rawIndex = 0;
    for (TopExp_Explorer explorer(*data->shape, TopAbs_EDGE); explorer.More();
         explorer.Next(), ++rawIndex) {
      const TopoDS_Edge edge = TopoDS::Edge(explorer.Current());
      int mapped = edgeMap.FindIndex(edge);
      if (mapped == 0) {
        edgeMap.Add(edge);
        mapped = edgeMap.FindIndex(edge);
        std::string signatureError;
        const EdgeSignature signature = signatureOf(edge, &signatureError);
        if (!signatureError.empty() || !finiteEdgeSignature(signature)) {
          if (error)
            *error = signatureError.empty()
                         ? "Topology edge signature is invalid"
                         : std::move(signatureError);
          return {};
        }
        data->edges.push_back(
            {edge, rawIndex, signature, {}, {rawIndex}});
      } else {
        data->edges[static_cast<std::size_t>(mapped - 1)]
            .rawAliases.push_back(rawIndex);
      }
      data->edgeByRawOrdinal.push_back(
          static_cast<std::size_t>(mapped - 1));
    }

    return std::shared_ptr<const TopologyIndex>(
        new TopologyIndex(std::move(data)));
  } catch (const Standard_Failure& failure) {
    if (error) {
      const char* message = failure.GetMessageString();
      *error = message && *message
                   ? std::string("OpenCASCADE topology indexing failed: ") +
                         message
                   : "OpenCASCADE topology indexing failed";
    }
  } catch (const std::exception& failure) {
    if (error)
      *error = std::string("Topology indexing failed: ") + failure.what();
  } catch (...) {
    if (error) *error = "Unexpected failure while indexing topology";
  }
  return {};
}

std::uint64_t TopologyIndex::buildAttemptCount() noexcept {
  return topologyBuildAttempts.load(std::memory_order_relaxed);
}

std::shared_ptr<const TopologyIndex> TopologyIndex::build(
    const TopoDS_Shape& shape, ShapeRevision revision,
    std::string* error) {
  return build(std::make_shared<TopoDS_Shape>(shape), revision, error);
}

const TopologyIndex::ShapePtr& TopologyIndex::shape() const noexcept {
  return data_->shape;
}

ShapeRevision TopologyIndex::revision() const noexcept {
  return data_->revision;
}

std::size_t TopologyIndex::faceCount() const noexcept {
  return data_->faces.size();
}

std::size_t TopologyIndex::edgeCount() const noexcept {
  return data_->edges.size();
}

std::size_t TopologyIndex::rawFaceCount() const noexcept {
  return data_->faceByRawOrdinal.size();
}

std::size_t TopologyIndex::rawEdgeCount() const noexcept {
  return data_->edgeByRawOrdinal.size();
}

FaceResolution TopologyIndex::resolveFace(
    const TopologyReference& reference) const {
  FaceResolution failure;
  if (reference.kind != TopologyKind::Face || !reference.hasValidOwner()) {
    failure.failure = TopologyResolutionFailure::InvalidReference;
    failure.error = "Topology reference has invalid face owner identity";
    return failure;
  }
  try {
    if (reference.faceSignature &&
        !finiteFaceSignature(*reference.faceSignature)) {
      failure.failure = TopologyResolutionFailure::InvalidReference;
      failure.error = "Topology face reference contains non-finite signature data";
      return failure;
    }

    if (!reference.persistentTag.empty()) {
      std::vector<std::size_t> tagged;
      for (std::size_t index = 0; index < data_->faces.size(); ++index)
        if (data_->faces[index].tag == reference.persistentTag)
          tagged.push_back(index);

      if (reference.faceSignature) {
        bool ambiguous = false;
        if (const auto best = uniqueBest(
                data_->faces, *reference.faceSignature,
                data_->shapeDiagonal, tagged, faceScore, &ambiguous)) {
          return resolvedFace(data_->faces[*best],
                              tagged.size() == 1
                                  ? TopologyMatchMethod::SemanticTag
                                  : TopologyMatchMethod::GeometricSignature);
        }
        if (ambiguous) {
          failure.failure = TopologyResolutionFailure::Ambiguous;
          failure.error = "Topology face reference is ambiguous";
          return failure;
        }
      } else {
        if (tagged.size() == 1)
          return resolvedFace(data_->faces[tagged.front()],
                              TopologyMatchMethod::SemanticTag);
        failure.failure = tagged.empty() ? TopologyResolutionFailure::Missing
                                         : TopologyResolutionFailure::Ambiguous;
        failure.error = tagged.empty()
                            ? "Topology face semantic tag no longer exists"
                            : "Topology face reference is ambiguous by semantic tag";
        return failure;
      }
    }

    if (reference.faceSignature) {
      bool ambiguous = false;
      const auto best = uniqueBest(
          data_->faces, *reference.faceSignature, data_->shapeDiagonal,
          allIndices(data_->faces.size()), faceScore, &ambiguous);
      if (best)
        return resolvedFace(data_->faces[*best],
                            TopologyMatchMethod::GeometricSignature);
      failure.failure = ambiguous ? TopologyResolutionFailure::Ambiguous
                                  : TopologyResolutionFailure::Missing;
      failure.error = ambiguous ? "Topology face reference is ambiguous"
                                : "Topology face no longer has a geometric match";
      return failure;
    }

    if (reference.legacyIndex < data_->faceByRawOrdinal.size())
      return resolvedFace(
          data_->faces[data_->faceByRawOrdinal[reference.legacyIndex]],
          TopologyMatchMethod::LegacyIndex);
    failure.failure = TopologyResolutionFailure::Missing;
    failure.error = "Topology face legacy fallback failed";
  } catch (...) {
    failure.failure = TopologyResolutionFailure::Unexpected;
    failure.error = "Unexpected failure while resolving topology face";
  }
  return failure;
}

EdgeResolution TopologyIndex::resolveEdge(
    const TopologyReference& reference) const {
  EdgeResolution failure;
  if (reference.kind != TopologyKind::Edge || !reference.hasValidOwner()) {
    failure.failure = TopologyResolutionFailure::InvalidReference;
    failure.error = "Topology reference has invalid edge owner identity";
    return failure;
  }
  try {
    if (reference.edgeSignature &&
        !finiteEdgeSignature(*reference.edgeSignature)) {
      failure.failure = TopologyResolutionFailure::InvalidReference;
      failure.error = "Topology edge reference contains non-finite signature data";
      return failure;
    }
    if (reference.edgeSignature) {
      bool ambiguous = false;
      const auto best = uniqueBest(
          data_->edges, *reference.edgeSignature, data_->shapeDiagonal,
          allIndices(data_->edges.size()), edgeScore, &ambiguous);
      if (best)
        return resolvedEdge(data_->edges[*best],
                            TopologyMatchMethod::GeometricSignature);
      failure.failure = ambiguous ? TopologyResolutionFailure::Ambiguous
                                  : TopologyResolutionFailure::Missing;
      failure.error = ambiguous ? "Topology edge reference is ambiguous"
                                : "Topology edge no longer has a geometric match";
      return failure;
    }
    if (!reference.persistentTag.empty()) {
      failure.failure = TopologyResolutionFailure::Missing;
      failure.error = "Topology edge semantic tag no longer exists";
      return failure;
    }
    if (reference.legacyIndex < data_->edgeByRawOrdinal.size())
      return resolvedEdge(
          data_->edges[data_->edgeByRawOrdinal[reference.legacyIndex]],
          TopologyMatchMethod::LegacyIndex);
    failure.failure = TopologyResolutionFailure::Missing;
    failure.error = "Topology edge legacy fallback failed";
  } catch (...) {
    failure.failure = TopologyResolutionFailure::Unexpected;
    failure.error = "Unexpected failure while resolving topology edge";
  }
  return failure;
}

std::vector<FaceResolution> TopologyIndex::resolveFaces(
    std::span<const TopologyReference> references) const {
  std::vector<FaceResolution> result;
  result.reserve(references.size());
  for (const auto& reference : references) result.push_back(resolveFace(reference));
  return result;
}

std::vector<EdgeResolution> TopologyIndex::resolveEdges(
    std::span<const TopologyReference> references) const {
  std::vector<EdgeResolution> result;
  result.reserve(references.size());
  for (const auto& reference : references) result.push_back(resolveEdge(reference));
  return result;
}

FaceReferenceCreation TopologyIndex::createFaceReference(
    BodyId bodyId, FeatureId featureId, std::size_t rawFaceIndex,
    std::string semanticTag) const {
  FaceReferenceCreation result;
  if (bodyId == kInvalidBodyId || featureId == kInvalidFeatureId) {
    result.error = "Topology face owner identity is invalid";
    return result;
  }
  if (rawFaceIndex >= data_->faceByRawOrdinal.size()) {
    result.error = "Topology face traversal index is out of range";
    return result;
  }
  const auto& candidate =
      data_->faces[data_->faceByRawOrdinal[rawFaceIndex]];
  result.reference = {bodyId, featureId, candidate.index};
  result.reference.signature = candidate.signature;
  result.reference.persistentTag =
      semanticTag.empty() ? candidate.tag : std::move(semanticTag);
  return result;
}

EdgeReferenceCreation TopologyIndex::createEdgeReference(
    BodyId bodyId, FeatureId featureId, std::size_t rawEdgeIndex,
    std::string semanticTag) const {
  EdgeReferenceCreation result;
  if (bodyId == kInvalidBodyId || featureId == kInvalidFeatureId) {
    result.error = "Topology edge owner identity is invalid";
    return result;
  }
  if (rawEdgeIndex >= data_->edgeByRawOrdinal.size()) {
    result.error = "Topology edge traversal index is out of range";
    return result;
  }
  const auto& candidate =
      data_->edges[data_->edgeByRawOrdinal[rawEdgeIndex]];
  result.reference = {bodyId, featureId, candidate.index};
  result.reference.signature = candidate.signature;
  result.reference.persistentTag =
      semanticTag.empty() ? candidate.tag : std::move(semanticTag);
  return result;
}

FaceReferenceCreation createFaceReference(
    const TopologyIndex& index, BodyId bodyId, FeatureId featureId,
    std::size_t faceIndex, std::string semanticTag) {
  return index.createFaceReference(bodyId, featureId, faceIndex,
                                   std::move(semanticTag));
}

EdgeReferenceCreation createEdgeReference(
    const TopologyIndex& index, BodyId bodyId, FeatureId featureId,
    std::size_t edgeIndex, std::string semanticTag) {
  return index.createEdgeReference(bodyId, featureId, edgeIndex,
                                   std::move(semanticTag));
}

FaceReference makeFaceReference(const TopoDS_Shape& shape, BodyId bodyId,
                                FeatureId featureId, std::size_t faceIndex,
                                std::string semanticTag) {
  std::string error;
  const auto index = TopologyIndex::build(shape, kInvalidShapeRevision, &error);
  if (!index) return {};
  const auto created = index->createFaceReference(
      bodyId, featureId, faceIndex, std::move(semanticTag));
  return created ? created.reference : FaceReference{};
}

EdgeReference makeEdgeReference(const TopoDS_Shape& shape, BodyId bodyId,
                                FeatureId featureId, std::size_t edgeIndex,
                                std::string semanticTag) {
  std::string error;
  const auto index = TopologyIndex::build(shape, kInvalidShapeRevision, &error);
  if (!index) return {};
  const auto created = index->createEdgeReference(
      bodyId, featureId, edgeIndex, std::move(semanticTag));
  return created ? created.reference : EdgeReference{};
}

FaceResolution resolveFaceReference(
    const TopologyIndex& index,
    const TopologyReference& reference) {
  return index.resolveFace(reference);
}

EdgeResolution resolveEdgeReference(
    const TopologyIndex& index,
    const TopologyReference& reference) {
  return index.resolveEdge(reference);
}

std::vector<FaceResolution> resolveFaceReferences(
    const TopologyIndex& index,
    std::span<const TopologyReference> references) {
  return index.resolveFaces(references);
}

std::vector<EdgeResolution> resolveEdgeReferences(
    const TopologyIndex& index,
    std::span<const TopologyReference> references) {
  return index.resolveEdges(references);
}

FaceResolution resolveFaceReference(const TopoDS_Shape& shape,
                                    const TopologyReference& reference) {
  FaceResolution failure;
  std::string error;
  const auto index = TopologyIndex::build(shape, kInvalidShapeRevision, &error);
  if (!index) {
    failure.failure = TopologyResolutionFailure::Unexpected;
    failure.error = std::move(error);
    return failure;
  }
  return index->resolveFace(reference);
}

EdgeResolution resolveEdgeReference(const TopoDS_Shape& shape,
                                    const TopologyReference& reference) {
  EdgeResolution failure;
  std::string error;
  const auto index = TopologyIndex::build(shape, kInvalidShapeRevision, &error);
  if (!index) {
    failure.failure = TopologyResolutionFailure::Unexpected;
    failure.error = std::move(error);
    return failure;
  }
  return index->resolveEdge(reference);
}

std::optional<TopoDS_Edge> resolveEdge(const TopoDS_Shape& shape,
                                       std::size_t edgeIndex) {
  std::string error;
  const auto index = TopologyIndex::build(shape, kInvalidShapeRevision, &error);
  if (!index) return std::nullopt;
  TopologyReference reference;
  reference.bodyId = 1;
  reference.featureId = 1;
  reference.kind = TopologyKind::Edge;
  reference.legacyIndex = edgeIndex;
  return index->resolveEdge(reference).subshape;
}

std::optional<TopoDS_Edge> resolveEdge(
    const TopoDS_Shape& shape, const TopologyReference& reference) {
  return resolveEdgeReference(shape, reference).subshape;
}

}  // namespace solidar
