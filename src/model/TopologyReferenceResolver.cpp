#include "model/TopologyReferenceResolver.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Circ.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "model/SketchPlacement.h"

namespace solidar {
namespace {

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

TopologyBounds boundsOf(const TopoDS_Shape& shape) {
  Bnd_Box box;
  BRepBndLib::Add(shape, box);
  TopologyBounds result;
  if (!box.IsVoid())
    box.Get(result.minimum.x, result.minimum.y, result.minimum.z,
            result.maximum.x, result.maximum.y, result.maximum.z);
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

FaceSignature signatureOf(const TopoDS_Face& face) {
  FaceSignature signature;
  GProp_GProps properties;
  BRepGProp::SurfaceProperties(face, properties);
  signature.area = properties.Mass();
  const gp_Pnt center = properties.CentreOfMass();
  signature.centroid = {center.X(), center.Y(), center.Z()};
  signature.bounds = boundsOf(face);

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
  return signature;
}

EdgeSignature signatureOf(const TopoDS_Edge& edge) {
  EdgeSignature signature;
  GProp_GProps properties;
  BRepGProp::LinearProperties(edge, properties);
  signature.length = properties.Mass();
  const gp_Pnt center = properties.CentreOfMass();
  signature.midpoint = {center.X(), center.Y(), center.Z()};
  signature.bounds = boundsOf(edge);

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
  return signature;
}

std::string automaticFaceTag(const FaceSignature& face,
                             const TopologyBounds& shapeBounds) {
  if (face.surface != SurfaceKind::Plane) return {};
  struct Axis { double normal; double center; double minimum; double maximum; const char* name; };
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

struct FaceCandidate { TopoDS_Face face; std::size_t index; FaceSignature signature; std::string tag; };
struct EdgeCandidate { TopoDS_Edge edge; std::size_t index; EdgeSignature signature; };

std::vector<FaceCandidate> faceCandidates(const TopoDS_Shape& shape) {
  std::vector<FaceCandidate> result;
  const auto shapeBounds = boundsOf(shape);
  std::size_t index = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next(), ++index) {
    const auto face = TopoDS::Face(explorer.Current());
    // TopExp_Explorer can encounter the same underlying subshape through
    // multiple topology paths.  Keep the original traversal index for file
    // compatibility, but do not let duplicate occurrences turn an otherwise
    // unique persistent match into an ambiguity.
    if (std::any_of(result.begin(), result.end(), [&face](const auto& item) {
          return item.face.IsSame(face);
        }))
      continue;
    auto signature = signatureOf(face);
    result.push_back(
        {face, index, signature, automaticFaceTag(signature, shapeBounds)});
  }
  return result;
}

std::vector<EdgeCandidate> edgeCandidates(const TopoDS_Shape& shape) {
  std::vector<EdgeCandidate> result;
  std::size_t index = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More();
       explorer.Next(), ++index) {
    const auto edge = TopoDS::Edge(explorer.Current());
    if (std::any_of(result.begin(), result.end(), [&edge](const auto& item) {
          return item.edge.IsSame(edge);
        }))
      continue;
    result.push_back({edge, index, signatureOf(edge)});
  }
  return result;
}

std::optional<double> faceScore(const FaceSignature& saved,
                                const FaceSignature& candidate,
                                double shapeDiagonal) {
  // Position is normalized by the current shape so translated faces remain
  // resolvable after ordinary upstream dimension edits.
  if (saved.surface != candidate.surface) return std::nullopt;
  const double measure = relativeDifference(saved.area, candidate.area);
  const double position = distance(saved.centroid, candidate.centroid) /
                          std::max(shapeDiagonal, 1e-9);
  if (measure > kTopologyMeasureRelativeTolerance ||
      position > kTopologyPositionRelativeTolerance)
    return std::nullopt;
  double direction = 0.0;
  if (saved.surface == SurfaceKind::Plane) {
    direction = 1.0 - dot(normalized(saved.normal), normalized(candidate.normal));
    if (dot(normalized(saved.normal), normalized(candidate.normal)) <
        kTopologyDirectionCosineTolerance)
      return std::nullopt;
  } else if (saved.surface == SurfaceKind::Cylinder) {
    direction = 1.0 - std::abs(dot(normalized(saved.axis),
                                   normalized(candidate.axis)));
    if (relativeDifference(saved.radius, candidate.radius) >
        kTopologyMeasureRelativeTolerance)
      return std::nullopt;
  }
  return measure + position + direction;
}

std::optional<double> edgeScore(const EdgeSignature& saved,
                                const EdgeSignature& candidate,
                                double shapeDiagonal) {
  if (saved.curve != candidate.curve) return std::nullopt;
  const double measure = relativeDifference(saved.length, candidate.length);
  const double position = distance(saved.midpoint, candidate.midpoint) /
                          std::max(shapeDiagonal, 1e-9);
  const double direction =
      1.0 - std::abs(dot(normalized(saved.tangent),
                         normalized(candidate.tangent)));
  if (measure > kTopologyMeasureRelativeTolerance ||
      position > kTopologyPositionRelativeTolerance ||
      direction > 1.0 - kTopologyDirectionCosineTolerance)
    return std::nullopt;
  if (saved.curve == CurveKind::Circle &&
      (relativeDifference(saved.radius, candidate.radius) >
           kTopologyMeasureRelativeTolerance ||
       distance(saved.center, candidate.center) / shapeDiagonal >
           kTopologyPositionRelativeTolerance))
    return std::nullopt;
  return measure + position + direction;
}

FaceResolution resolvedFace(const FaceCandidate& candidate,
                            TopologyMatchMethod method) {
  FaceResolution result;
  result.subshape = candidate.face;
  result.index = candidate.index;
  result.method = method;
  return result;
}

EdgeResolution resolvedEdge(const EdgeCandidate& candidate,
                            TopologyMatchMethod method) {
  EdgeResolution result;
  result.subshape = candidate.edge;
  result.index = candidate.index;
  result.method = method;
  return result;
}

}  // namespace

FaceReference makeFaceReference(const TopoDS_Shape& shape, BodyId bodyId,
                                FeatureId featureId, std::size_t faceIndex,
                                std::string semanticTag) {
  FaceReference result{bodyId, featureId, faceIndex};
  try {
    const auto candidates = faceCandidates(shape);
    TopoDS_Face requested;
    std::size_t index = 0;
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
         explorer.Next(), ++index)
      if (index == faceIndex) {
        requested = TopoDS::Face(explorer.Current());
        break;
      }
    const auto canonical = std::find_if(
        candidates.begin(), candidates.end(), [&requested](const auto& item) {
          return !requested.IsNull() && item.face.IsSame(requested);
        });
    if (canonical == candidates.end()) return result;
    result.faceIndex = canonical->index;
    result.signature = canonical->signature;
    result.persistentTag = semanticTag.empty() ? canonical->tag
                                                : std::move(semanticTag);
  } catch (...) {
  }
  return result;
}

EdgeReference makeEdgeReference(const TopoDS_Shape& shape, BodyId bodyId,
                                FeatureId featureId, std::size_t edgeIndex,
                                std::string semanticTag) {
  EdgeReference result{bodyId, featureId, edgeIndex};
  try {
    const auto candidates = edgeCandidates(shape);
    TopoDS_Edge requested;
    std::size_t index = 0;
    for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More();
         explorer.Next(), ++index)
      if (index == edgeIndex) {
        requested = TopoDS::Edge(explorer.Current());
        break;
      }
    const auto canonical = std::find_if(
        candidates.begin(), candidates.end(), [&requested](const auto& item) {
          return !requested.IsNull() && item.edge.IsSame(requested);
        });
    if (canonical == candidates.end()) return result;
    result.edgeIndex = canonical->index;
    result.signature = canonical->signature;
    result.persistentTag = std::move(semanticTag);
  } catch (...) {
  }
  return result;
}

FaceResolution resolveFaceReference(const TopoDS_Shape& shape,
                                    const TopologyReference& reference) {
  FaceResolution failure;
  if (shape.IsNull()) { failure.error = "Topology source shape is unavailable"; return failure; }
  if (reference.kind != TopologyKind::Face || !reference.hasValidOwner()) {
    failure.error = "Topology reference has invalid face owner identity";
    return failure;
  }
  try {
    const auto candidates = faceCandidates(shape);
    if (!reference.persistentTag.empty()) {
      std::vector<const FaceCandidate*> tagged;
      for (const auto& candidate : candidates)
        if (candidate.tag == reference.persistentTag) tagged.push_back(&candidate);
      if (tagged.size() == 1)
        return resolvedFace(*tagged.front(), TopologyMatchMethod::SemanticTag);
      if (tagged.size() > 1) {
        failure.error = "Topology face reference is ambiguous by semantic tag";
        return failure;
      }
    }
    if (reference.faceSignature) {
      std::vector<std::pair<double, const FaceCandidate*>> matches;
      const double size = diagonal(boundsOf(shape));
      for (const auto& candidate : candidates)
        if (const auto score = faceScore(*reference.faceSignature,
                                         candidate.signature, size))
          matches.emplace_back(*score, &candidate);
      std::sort(matches.begin(), matches.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      if (matches.empty()) {
        failure.error = "Topology face no longer has a geometric match";
        return failure;
      }
      if (matches.size() > 1 &&
          matches[1].first - matches[0].first <=
              kTopologyAmbiguityScoreTolerance) {
        failure.error = "Topology face reference is ambiguous";
        return failure;
      }
      return resolvedFace(*matches.front().second,
                          TopologyMatchMethod::GeometricSignature);
    }
    const auto legacy = std::find_if(
        candidates.begin(), candidates.end(), [&reference](const auto& item) {
          return item.index == reference.legacyIndex;
        });
    if (legacy != candidates.end())
      return resolvedFace(*legacy, TopologyMatchMethod::LegacyIndex);
    failure.error = "Topology face legacy fallback failed";
  } catch (const Standard_Failure& error) {
    failure.error = std::string("OCCT failed to resolve face: ") + error.GetMessageString();
  } catch (...) {
    failure.error = "Unexpected failure while resolving topology face";
  }
  return failure;
}

EdgeResolution resolveEdgeReference(const TopoDS_Shape& shape,
                                    const TopologyReference& reference) {
  EdgeResolution failure;
  if (shape.IsNull()) { failure.error = "Topology source shape is unavailable"; return failure; }
  if (reference.kind != TopologyKind::Edge || !reference.hasValidOwner()) {
    failure.error = "Topology reference has invalid edge owner identity";
    return failure;
  }
  try {
    const auto candidates = edgeCandidates(shape);
    if (reference.edgeSignature) {
      std::vector<std::pair<double, const EdgeCandidate*>> matches;
      const double size = diagonal(boundsOf(shape));
      for (const auto& candidate : candidates)
        if (const auto score = edgeScore(*reference.edgeSignature,
                                         candidate.signature, size))
          matches.emplace_back(*score, &candidate);
      std::sort(matches.begin(), matches.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      if (matches.empty()) {
        failure.error = "Topology edge no longer has a geometric match";
        return failure;
      }
      if (matches.size() > 1 &&
          matches[1].first - matches[0].first <=
              kTopologyAmbiguityScoreTolerance) {
        failure.error = "Topology edge reference is ambiguous";
        return failure;
      }
      return resolvedEdge(*matches.front().second,
                          TopologyMatchMethod::GeometricSignature);
    }
    const auto legacy = std::find_if(
        candidates.begin(), candidates.end(), [&reference](const auto& item) {
          return item.index == reference.legacyIndex;
        });
    if (legacy != candidates.end())
      return resolvedEdge(*legacy, TopologyMatchMethod::LegacyIndex);
    failure.error = "Topology edge legacy fallback failed";
  } catch (const Standard_Failure& error) {
    failure.error = std::string("OCCT failed to resolve edge: ") + error.GetMessageString();
  } catch (...) {
    failure.error = "Unexpected failure while resolving topology edge";
  }
  return failure;
}

std::optional<TopoDS_Edge> resolveEdge(const TopoDS_Shape& shape,
                                       std::size_t edgeIndex) {
  if (shape.IsNull()) return std::nullopt;
  try {
    std::size_t current = 0;
    for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More();
         explorer.Next(), ++current)
      if (current == edgeIndex) return TopoDS::Edge(explorer.Current());
  } catch (...) {
  }
  return std::nullopt;
}

std::optional<TopoDS_Edge> resolveEdge(
    const TopoDS_Shape& shape, const TopologyReference& reference) {
  return resolveEdgeReference(shape, reference).subshape;
}

}  // namespace solidar
