#include "TestAssertions.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "model/ChamferBuilder.h"
#include "model/ChamferFeature.h"
#include "model/Document.h"
#include "model/DraftBuilder.h"
#include "model/DraftFeature.h"
#include "model/ExtrudeFeature.h"
#include "model/FaceBoundaryEdgeProvenance.h"
#include "model/FilletBuilder.h"
#include "model/FilletFeature.h"
#include "model/RevolveFeature.h"
#include "model/TopologyReferenceResolver.h"

namespace {

struct ExtrudedBox {
  solidar::Document document;
  solidar::Body* body{};
  solidar::DocumentSketch* sketch{};
  solidar::ExtrudeFeature* feature{};
  std::array<solidar::sketch::GeometryId, 4> geometryIds{};
};

struct RevolvedRectangle {
  solidar::Document document;
  solidar::Body* body{};
  solidar::DocumentSketch* sketch{};
  solidar::RevolveFeature* feature{};
  std::array<solidar::sketch::GeometryId, 4> geometryIds{};
};

RevolvedRectangle makeRevolvedRectangle(double minimumRadius,
                                        double angleDeg) {
  RevolvedRectangle result;
  auto& sketch = result.document.addSketch("Production edge revolve");
  sketch.geometry.addRectangle({10.0, minimumRadius}, {30.0, 15.0});
  result.geometryIds = {sketch.geometry.lineId(0),
                        sketch.geometry.lineId(1),
                        sketch.geometry.lineId(2),
                        sketch.geometry.lineId(3)};
  auto& body = result.document.addBody();
  auto revolve = std::make_unique<solidar::RevolveFeature>(
      sketch.id,
      solidar::AxisReference{
          solidar::AxisReferenceType::SketchHorizontalAxis, sketch.id,
          solidar::sketch::kInvalidGeometryId},
      angleDeg, "Production edge Revolve",
      solidar::ExtrudeOperation::NewBody, false);
  result.feature = revolve.get();
  body.addFeature(std::move(revolve));
  result.body = &body;
  result.sketch = &sketch;
  CHECK(result.document.rebuild());
  return result;
}

ExtrudedBox makeBox(double length = 15.0) {
  ExtrudedBox result;
  auto& sketch = result.document.addSketch("Production edge rectangle");
  sketch.geometry.addRectangle({0.0, 0.0}, {30.0, 20.0});
  result.geometryIds = {sketch.geometry.lineId(0),
                        sketch.geometry.lineId(1),
                        sketch.geometry.lineId(2),
                        sketch.geometry.lineId(3)};
  auto& body = result.document.addBody();
  auto extrusion =
      std::make_unique<solidar::ExtrudeFeature>(sketch.id, length);
  result.feature = extrusion.get();
  body.addFeature(std::move(extrusion));
  result.body = &body;
  result.sketch = &sketch;
  CHECK(result.document.rebuild());
  return result;
}

std::vector<solidar::TopologyProvenanceKey> edgeKeys(
    const solidar::TopologyLineage& lineage) {
  std::vector<solidar::TopologyProvenanceKey> result;
  for (const auto& binding : lineage.bindings())
    if (binding.provenance.kind == solidar::TopologyKind::Edge)
      result.push_back(binding.provenance);
  std::sort(result.begin(), result.end(), [](const auto& left,
                                             const auto& right) {
    return left.localName < right.localName;
  });
  return result;
}

solidar::TopologyProvenanceKey authoredKey(
    solidar::AuthoredTopologySourceKind kind, solidar::SketchId sketchId,
    solidar::sketch::GeometryId geometryId, solidar::FeatureId featureId) {
  std::string error;
  const auto key = solidar::projectAuthoredTopologyToken(
      {kind, sketchId, geometryId}, featureId, &error);
  CHECK(key);
  CHECK(error.empty());
  return *key;
}

std::shared_ptr<const solidar::TopologyLineage> faceOnlyLineage(
    const solidar::TopologyLineage& lineage) {
  std::vector<solidar::TopologyLineageBinding> bindings;
  for (const auto& binding : lineage.bindings())
    if (binding.provenance.kind == solidar::TopologyKind::Face)
      bindings.push_back(binding);
  std::string error;
  auto result = solidar::TopologyLineage::create(
      std::move(bindings), solidar::TopologyLineageCoverage::CompleteFaces,
      &error);
  CHECK(result);
  CHECK(error.empty());
  return result;
}

std::string pointName(const gp_Pnt& point) {
  const auto coordinate = [](double value) {
    return std::to_string(static_cast<long long>(std::llround(value * 1000.0)));
  };
  return coordinate(point.X()) + "," + coordinate(point.Y()) + "," +
         coordinate(point.Z());
}

std::string endpointName(const TopoDS_Edge& edge) {
  TopoDS_Vertex first;
  TopoDS_Vertex last;
  TopExp::Vertices(edge, first, last);
  CHECK(!first.IsNull());
  CHECK(!last.IsNull());
  std::string a = pointName(BRep_Tool::Pnt(first));
  std::string b = pointName(BRep_Tool::Pnt(last));
  if (b < a) std::swap(a, b);
  return a + "->" + b;
}

bool near(double actual, double expected, double tolerance = 1e-5) {
  return std::abs(actual - expected) <= tolerance;
}

bool samePoint(const gp_Pnt& actual, const gp_Pnt& expected) {
  return near(actual.X(), expected.X()) && near(actual.Y(), expected.Y()) &&
         near(actual.Z(), expected.Z());
}

bool hasEndpoints(const TopoDS_Edge& edge, const gp_Pnt& expectedFirst,
                  const gp_Pnt& expectedLast) {
  TopoDS_Vertex first;
  TopoDS_Vertex last;
  TopExp::Vertices(edge, first, last);
  if (first.IsNull() || last.IsNull()) return false;
  const gp_Pnt firstPoint = BRep_Tool::Pnt(first);
  const gp_Pnt lastPoint = BRep_Tool::Pnt(last);
  return (samePoint(firstPoint, expectedFirst) &&
          samePoint(lastPoint, expectedLast)) ||
         (samePoint(firstPoint, expectedLast) &&
          samePoint(lastPoint, expectedFirst));
}

solidar::EdgeReference edgeForEndpoints(const ExtrudedBox& model,
                                        const gp_Pnt& expectedFirst,
                                        const gp_Pnt& expectedLast) {
  const auto index = model.feature->topologyIndex();
  CHECK(index);
  std::optional<solidar::EdgeReference> found;
  std::vector<TopoDS_Edge> seen;
  for (std::size_t raw = 0; raw < index->rawEdgeCount(); ++raw) {
    const auto legacy = index->createEdgeReference(
        model.body->id(), model.feature->id(), raw);
    CHECK(legacy);
    const auto resolved = index->resolveEdge(legacy.reference);
    CHECK(resolved);
    if (std::any_of(seen.begin(), seen.end(), [&](const auto& edge) {
          return edge.IsSame(*resolved.subshape);
        }))
      continue;
    seen.push_back(*resolved.subshape);
    if (!hasEndpoints(*resolved.subshape, expectedFirst, expectedLast))
      continue;
    const auto exact =
        model.feature->createEdgeReferenceV2(model.body->id(), raw);
    CHECK(exact);
    CHECK(!found);
    found = exact.reference;
  }
  CHECK(found);
  return *found;
}

solidar::FaceReference faceForKey(
    const ExtrudedBox& model,
    const solidar::TopologyProvenanceKey& expectedKey) {
  const auto index = model.feature->topologyIndex();
  CHECK(index);
  std::optional<solidar::FaceReference> found;
  for (std::size_t raw = 0; raw < index->rawFaceCount(); ++raw) {
    const auto exact =
        model.feature->createFaceReferenceV2(model.body->id(), raw);
    if (!exact || exact.reference.v2()->provenance != expectedKey) continue;
    CHECK(!found);
    found = exact.reference;
  }
  CHECK(found);
  return *found;
}

bool originLess(const solidar::TopologyProvenanceOrigin& left,
                const solidar::TopologyProvenanceOrigin& right) {
  const auto occurrence = [](const auto& origin) {
    return std::tuple{origin.occurrence.has_value(),
                      origin.occurrence
                          ? origin.occurrence->featureId
                          : solidar::kInvalidFeatureId,
                      origin.occurrence ? origin.occurrence->slot : 0};
  };
  const auto leftIdentity = std::tie(left.featureId, left.kind,
                                     left.operation, left.role,
                                     left.localName);
  const auto rightIdentity = std::tie(right.featureId, right.kind,
                                      right.operation, right.role,
                                      right.localName);
  if (leftIdentity != rightIdentity) return leftIdentity < rightIdentity;
  return occurrence(left) < occurrence(right);
}

std::vector<solidar::TopologyProvenanceOrigin> adjacentFaceOrigins(
    const solidar::ShapeFeature& feature, const TopoDS_Edge& edge) {
  std::vector<solidar::TopologyProvenanceOrigin> result;
  for (const auto& binding : feature.topologyLineage()->bindings()) {
    if (binding.provenance.kind != solidar::TopologyKind::Face) continue;
    std::size_t occurrences = 0;
    for (TopExp_Explorer explorer(*binding.subshape, TopAbs_EDGE);
         explorer.More(); explorer.Next())
      if (explorer.Current().IsSame(edge)) ++occurrences;
    if (occurrences != 0) {
      CHECK(occurrences == 1);
      result.push_back(binding.provenance.immediateOrigin());
    }
  }
  std::sort(result.begin(), result.end(), originLess);
  return result;
}

solidar::EdgeReference edgeForOrigins(
    const solidar::ShapeFeature& feature, solidar::BodyId bodyId,
    std::vector<solidar::TopologyProvenanceOrigin> expectedOrigins) {
  std::sort(expectedOrigins.begin(), expectedOrigins.end(), originLess);
  const auto index = feature.topologyIndex();
  CHECK(index);
  std::optional<solidar::EdgeReference> found;
  for (std::size_t raw = 0; raw < index->rawEdgeCount(); ++raw) {
    const auto exact = feature.createEdgeReferenceV2(bodyId, raw);
    if (!exact || exact.reference.v2()->provenance.origins != expectedOrigins)
      continue;
    if (found) {
      const auto first = index->resolveEdge(*found);
      const auto next = index->resolveEdge(exact.reference);
      CHECK(first);
      CHECK(next);
      CHECK(first.subshape->IsSame(*next.subshape));
      continue;
    }
    found = exact.reference;
  }
  CHECK(found);
  return *found;
}

double edgeLength(const TopoDS_Edge& edge) {
  GProp_GProps properties;
  BRepGProp::LinearProperties(edge, properties);
  return properties.Mass();
}

struct CurveExpectation {
  GeomAbs_CurveType type{GeomAbs_OtherCurve};
  double length{};
  std::optional<gp_Pnt> first;
  std::optional<gp_Pnt> last;
};

void checkRevolveCarrier(
    const solidar::RevolveFeature& feature, const solidar::EdgeReference& carrier,
    std::vector<solidar::TopologyProvenanceOrigin> expectedOrigins,
    const CurveExpectation& expected) {
  std::sort(expectedOrigins.begin(), expectedOrigins.end(), originLess);
  CHECK(carrier.v2());
  CHECK(carrier.v2()->provenance.origins == expectedOrigins);
  const auto index = feature.topologyIndex();
  CHECK(index);
  const auto resolved = index->resolveEdge(carrier);
  CHECK(resolved);
  CHECK(resolved.method == solidar::TopologyMatchMethod::OperationProvenance);
  CHECK(adjacentFaceOrigins(feature, *resolved.subshape) == expectedOrigins);
  CHECK(BRepAdaptor_Curve(*resolved.subshape).GetType() == expected.type);
  CHECK(near(edgeLength(*resolved.subshape), expected.length, 1e-4));
  if (expected.first && expected.last)
    CHECK(hasEndpoints(*resolved.subshape, *expected.first, *expected.last));
}

void checkEdgeMissing(const solidar::ShapeFeature& feature,
                      const solidar::EdgeReference& carrier) {
  const auto index = feature.topologyIndex();
  CHECK(index);
  const auto resolved = index->resolveEdge(carrier);
  CHECK(!resolved);
  CHECK(resolved.failure == solidar::TopologyResolutionFailure::Missing);
  CHECK(!resolved.subshape);
}

std::size_t edgeBindingCount(const solidar::ShapeFeature& feature) {
  const auto lineage = feature.topologyLineage();
  CHECK(lineage);
  return static_cast<std::size_t>(std::count_if(
      lineage->bindings().begin(), lineage->bindings().end(),
      [](const auto& binding) {
        return binding.provenance.kind == solidar::TopologyKind::Edge;
      }));
}

void checkAllCanonicalEdgesCreated(const solidar::ShapeFeature& feature,
                                   solidar::BodyId bodyId,
                                   std::size_t expectedCount) {
  const auto index = feature.topologyIndex();
  CHECK(index);
  std::vector<TopoDS_Edge> seen;
  for (std::size_t raw = 0; raw < index->rawEdgeCount(); ++raw) {
    const auto legacy =
        index->createEdgeReference(bodyId, feature.id(), raw);
    CHECK(legacy);
    const auto resolved = index->resolveEdge(legacy.reference);
    CHECK(resolved);
    if (std::any_of(seen.begin(), seen.end(), [&](const auto& edge) {
          return edge.IsSame(*resolved.subshape);
        }))
      continue;
    seen.push_back(*resolved.subshape);
    const auto exact = feature.createEdgeReferenceV2(bodyId, raw);
    CHECK(exact);
    CHECK(exact.status == solidar::EdgeReferenceV2CreationStatus::Created);
  }
  CHECK(seen.size() == expectedCount);
  CHECK(edgeBindingCount(feature) == expectedCount);
}

gp_Pnt revolvedPoint(double x, double radius, double angleDeg) {
  const double radians = angleDeg * std::numbers::pi / 180.0;
  return {x, radius * std::cos(radians), radius * std::sin(radians)};
}

void checkCarrier(
    const ExtrudedBox& model, const solidar::EdgeReference& carrier,
    const gp_Pnt& expectedFirst, const gp_Pnt& expectedLast,
    std::vector<solidar::TopologyProvenanceOrigin> expectedOrigins) {
  CHECK(carrier.v2());
  const auto index = model.feature->topologyIndex();
  CHECK(index);
  const auto resolved = index->resolveEdge(carrier);
  CHECK(resolved);
  CHECK(resolved.method == solidar::TopologyMatchMethod::OperationProvenance);
  CHECK(hasEndpoints(*resolved.subshape, expectedFirst, expectedLast));

  Bnd_Box bounds;
  BRepBndLib::Add(*resolved.subshape, bounds);
  double x0, y0, z0, x1, y1, z1;
  bounds.Get(x0, y0, z0, x1, y1, z1);
  CHECK(near(x0, std::min(expectedFirst.X(), expectedLast.X())));
  CHECK(near(y0, std::min(expectedFirst.Y(), expectedLast.Y())));
  CHECK(near(z0, std::min(expectedFirst.Z(), expectedLast.Z())));
  CHECK(near(x1, std::max(expectedFirst.X(), expectedLast.X())));
  CHECK(near(y1, std::max(expectedFirst.Y(), expectedLast.Y())));
  CHECK(near(z1, std::max(expectedFirst.Z(), expectedLast.Z())));

  std::sort(expectedOrigins.begin(), expectedOrigins.end(), originLess);
  CHECK(adjacentFaceOrigins(*model.feature, *resolved.subshape) ==
        expectedOrigins);
  CHECK(carrier.v2()->provenance.origins == expectedOrigins);
}

double volumeOf(const TopoDS_Shape& shape) {
  GProp_GProps properties;
  BRepGProp::VolumeProperties(shape, properties);
  return properties.Mass();
}

bool sameBindings(
    const std::vector<solidar::TopologyLineageBinding>& left,
    const std::vector<solidar::TopologyLineageBinding>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index)
    if (left[index].provenance != right[index].provenance ||
        left[index].subshape != right[index].subshape)
      return false;
  return true;
}

bool sameDerivedEdgeMapping(const solidar::TopologyLineage& left,
                            const solidar::TopologyLineage& right) {
  std::vector<const solidar::TopologyLineageBinding*> leftEdges;
  std::vector<const solidar::TopologyLineageBinding*> rightEdges;
  for (const auto& binding : left.bindings())
    if (binding.provenance.kind == solidar::TopologyKind::Edge)
      leftEdges.push_back(&binding);
  for (const auto& binding : right.bindings())
    if (binding.provenance.kind == solidar::TopologyKind::Edge)
      rightEdges.push_back(&binding);
  const auto order = [](const auto* first, const auto* second) {
    return first->provenance.localName < second->provenance.localName;
  };
  std::sort(leftEdges.begin(), leftEdges.end(), order);
  std::sort(rightEdges.begin(), rightEdges.end(), order);
  if (leftEdges.size() != rightEdges.size()) return false;
  for (std::size_t index = 0; index < leftEdges.size(); ++index)
    if (leftEdges[index]->provenance != rightEdges[index]->provenance ||
        !leftEdges[index]->subshape->IsSame(*rightEdges[index]->subshape))
      return false;
  return true;
}

std::vector<std::string> expectedBoxEndpointNames(double width, double height,
                                                  double length) {
  const std::array<gp_Pnt, 8> points{
      gp_Pnt(0.0, 0.0, 0.0),       gp_Pnt(width, 0.0, 0.0),
      gp_Pnt(width, height, 0.0),   gp_Pnt(0.0, height, 0.0),
      gp_Pnt(0.0, 0.0, length),    gp_Pnt(width, 0.0, length),
      gp_Pnt(width, height, length), gp_Pnt(0.0, height, length)};
  constexpr std::array<std::array<std::size_t, 2>, 12> endpointPairs{{
      {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
      {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}}};
  std::vector<std::string> result;
  result.reserve(endpointPairs.size());
  for (const auto& pair : endpointPairs) {
    std::string a = pointName(points[pair[0]]);
    std::string b = pointName(points[pair[1]]);
    if (b < a) std::swap(a, b);
    result.push_back(a + "->" + b);
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::pair<solidar::TopologyProvenanceKey, std::string>>
exactBoxEdges(const ExtrudedBox& model) {
  const auto index = model.feature->topologyIndex();
  CHECK(index);
  std::vector<TopoDS_Edge> seen;
  std::vector<std::pair<solidar::TopologyProvenanceKey, std::string>> result;
  for (std::size_t raw = 0; raw < index->rawEdgeCount(); ++raw) {
    const auto legacy = index->createEdgeReference(
        model.body->id(), model.feature->id(), raw);
    CHECK(legacy);
    const auto canonical = index->resolveEdge(legacy.reference);
    CHECK(canonical);
    if (std::any_of(seen.begin(), seen.end(), [&](const auto& edge) {
          return edge.IsSame(*canonical.subshape);
        }))
      continue;
    seen.push_back(*canonical.subshape);

    const auto exact =
        model.feature->createEdgeReferenceV2(model.body->id(), raw);
    CHECK(exact);
    CHECK(exact.status == solidar::EdgeReferenceV2CreationStatus::Created);
    CHECK(exact.reference.v2());
    const auto resolved = index->resolveEdge(exact.reference);
    CHECK(resolved);
    CHECK(resolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    CHECK(resolved.subshape->IsSame(*canonical.subshape));
    result.emplace_back(exact.reference.v2()->provenance,
                        endpointName(*resolved.subshape));
  }
  CHECK(seen.size() == 12);
  CHECK(result.size() == 12);
  std::vector<std::string> geometry;
  for (const auto& [key, name] : result) {
    CHECK(key.localName.starts_with("face-boundary-edge:v1;"));
    CHECK(key.origins.size() == 2);
    geometry.push_back(name);
  }
  std::sort(geometry.begin(), geometry.end());
  CHECK(std::adjacent_find(geometry.begin(), geometry.end()) == geometry.end());
  return result;
}

}  // namespace

int main() {
  using namespace solidar;

  // A real rectangular NewBody Extrude publishes exactly the twelve
  // geometric edges, each resolvable by its v2 face-pair identity.
  {
    auto model = makeBox();
    const auto original = exactBoxEdges(model);
    std::vector<std::string> originalGeometry;
    for (const auto& [key, geometry] : original) {
      (void)key;
      originalGeometry.push_back(geometry);
    }
    std::sort(originalGeometry.begin(), originalGeometry.end());
    CHECK(originalGeometry == expectedBoxEndpointNames(30.0, 20.0, 15.0));
    const auto originalKeys = edgeKeys(*model.feature->topologyLineage());
    CHECK(originalKeys.size() == 12);
    const auto bottomY0 = edgeForEndpoints(
        model, gp_Pnt(0.0, 0.0, 0.0), gp_Pnt(30.0, 0.0, 0.0));
    const auto verticalOrigin = edgeForEndpoints(
        model, gp_Pnt(0.0, 0.0, 0.0), gp_Pnt(0.0, 0.0, 15.0));
    const auto endY0 = edgeForEndpoints(
        model, gp_Pnt(0.0, 0.0, 15.0), gp_Pnt(30.0, 0.0, 15.0));
    const auto startOrigin =
        authoredKey(AuthoredTopologySourceKind::ProfileStartCap,
                    model.sketch->id, sketch::kInvalidGeometryId,
                    model.feature->id())
            .immediateOrigin();
    const auto endOrigin =
        authoredKey(AuthoredTopologySourceKind::ProfileEndCap,
                    model.sketch->id, sketch::kInvalidGeometryId,
                    model.feature->id())
            .immediateOrigin();
    const auto side0Origin =
        authoredKey(AuthoredTopologySourceKind::GeometrySide,
                    model.sketch->id, model.geometryIds[0],
                    model.feature->id())
            .immediateOrigin();
    const auto side3Origin =
        authoredKey(AuthoredTopologySourceKind::GeometrySide,
                    model.sketch->id, model.geometryIds[3],
                    model.feature->id())
            .immediateOrigin();
    checkCarrier(model, bottomY0, gp_Pnt(0.0, 0.0, 0.0),
                 gp_Pnt(30.0, 0.0, 0.0), {startOrigin, side0Origin});
    checkCarrier(model, verticalOrigin, gp_Pnt(0.0, 0.0, 0.0),
                 gp_Pnt(0.0, 0.0, 15.0), {side0Origin, side3Origin});
    checkCarrier(model, endY0, gp_Pnt(0.0, 0.0, 15.0),
                 gp_Pnt(30.0, 0.0, 15.0), {endOrigin, side0Origin});

    model.feature->setLengthMm(24.0);
    CHECK(model.document.rebuild());
    CHECK(edgeKeys(*model.feature->topologyLineage()) == originalKeys);
    exactBoxEdges(model);
    checkCarrier(model, bottomY0, gp_Pnt(0.0, 0.0, 0.0),
                 gp_Pnt(30.0, 0.0, 0.0), {startOrigin, side0Origin});
    checkCarrier(model, verticalOrigin, gp_Pnt(0.0, 0.0, 0.0),
                 gp_Pnt(0.0, 0.0, 24.0), {side0Origin, side3Origin});
    checkCarrier(model, endY0, gp_Pnt(0.0, 0.0, 24.0),
                 gp_Pnt(30.0, 0.0, 24.0), {endOrigin, side0Origin});

    sketch::Sketch resized;
    resized.addLine({0.0, 24.0}, {42.0, 24.0});
    resized.addLine({42.0, 0.0}, {0.0, 0.0});
    resized.addLine({0.0, 0.0}, {0.0, 24.0});
    resized.addLine({42.0, 24.0}, {42.0, 0.0});
    CHECK(resized.restoreGeometryIds(
        {model.geometryIds[2], model.geometryIds[0], model.geometryIds[3],
         model.geometryIds[1]},
        {}, {}, {}));
    CHECK(model.document.replaceSketchGeometry(model.sketch->id,
                                                std::move(resized)));
    CHECK(model.document.rebuild());
    CHECK(edgeKeys(*model.feature->topologyLineage()) == originalKeys);
    exactBoxEdges(model);
    checkCarrier(model, bottomY0, gp_Pnt(0.0, 0.0, 0.0),
                 gp_Pnt(42.0, 0.0, 0.0), {startOrigin, side0Origin});
    checkCarrier(model, verticalOrigin, gp_Pnt(0.0, 0.0, 0.0),
                 gp_Pnt(0.0, 0.0, 24.0), {side0Origin, side3Origin});
    checkCarrier(model, endY0, gp_Pnt(0.0, 0.0, 24.0),
                 gp_Pnt(42.0, 0.0, 24.0), {endOrigin, side0Origin});

    model.feature->setReversed(true);
    CHECK(model.document.rebuild());
    CHECK(edgeKeys(*model.feature->topologyLineage()) == originalKeys);
    exactBoxEdges(model);
    checkCarrier(model, bottomY0, gp_Pnt(0.0, 0.0, 0.0),
                 gp_Pnt(42.0, 0.0, 0.0), {startOrigin, side0Origin});
    checkCarrier(model, verticalOrigin, gp_Pnt(0.0, 0.0, 0.0),
                 gp_Pnt(0.0, 0.0, -24.0), {side0Origin, side3Origin});
    checkCarrier(model, endY0, gp_Pnt(0.0, 0.0, -24.0),
                 gp_Pnt(42.0, 0.0, -24.0), {endOrigin, side0Origin});
  }

  // A cylinder names its two cap rims. The side seam is unsupported and the
  // v2 factory reports Missing instead of manufacturing a fallback identity.
  {
    Document document;
    auto& sketch = document.addSketch("Production edge circle");
    sketch.geometry.addCircle({0.0, 0.0}, 8.0);
    auto& body = document.addBody();
    auto extrusion = std::make_unique<ExtrudeFeature>(sketch.id, 18.0);
    auto* feature = extrusion.get();
    body.addFeature(std::move(extrusion));
    CHECK(document.rebuild());
    CHECK(edgeKeys(*feature->topologyLineage()).size() == 2);
    const auto index = feature->topologyIndex();
    CHECK(index);
    std::vector<TopoDS_Edge> seen;
    std::size_t created = 0;
    std::size_t missing = 0;
    for (std::size_t raw = 0; raw < index->rawEdgeCount(); ++raw) {
      const auto legacy = index->createEdgeReference(body.id(), feature->id(),
                                                     raw);
      CHECK(legacy);
      const auto canonical = index->resolveEdge(legacy.reference);
      CHECK(canonical);
      if (std::any_of(seen.begin(), seen.end(), [&](const auto& edge) {
            return edge.IsSame(*canonical.subshape);
          }))
        continue;
      seen.push_back(*canonical.subshape);
      const auto exact = feature->createEdgeReferenceV2(body.id(), raw);
      if (exact) {
        ++created;
        CHECK(exact.status == EdgeReferenceV2CreationStatus::Created);
        CHECK(BRepAdaptor_Curve(*canonical.subshape).GetType() ==
              GeomAbs_Circle);
      } else {
        ++missing;
        CHECK(exact.status == EdgeReferenceV2CreationStatus::Missing);
        CHECK(!exact.reference.v2());
        CHECK(BRepAdaptor_Curve(*canonical.subshape).GetType() ==
              GeomAbs_Line);
      }
    }
    CHECK(seen.size() == 3);
    CHECK(created == 2);
    CHECK(missing == 1);
  }

  // Derivation is independent of input binding order, while incomplete face
  // identity and a forged key/edge pairing fail without mutating the source.
  {
    auto model = makeBox();
    const auto faceOnly = faceOnlyLineage(*model.feature->topologyLineage());
    const TopologySourceSnapshot source{model.feature->id(),
                                        model.feature->shape(), faceOnly};
    FaceBoundaryEdgeProvenanceFailure failure;
    const auto direct = augmentCompleteFaceLineageWithUniqueManifoldEdges(
        source, &failure);
    CHECK(direct);
    CHECK(failure.code == FaceBoundaryEdgeProvenanceFailureCode::None);
    CHECK(direct->addedBindingCount == 12);
    CHECK(direct->augmentedSource.lineage->coverage() ==
          TopologyLineageCoverage::CompleteFaces);
    const auto idempotent = augmentCompleteFaceLineageWithUniqueManifoldEdges(
        direct->augmentedSource, &failure);
    CHECK(idempotent);
    CHECK(idempotent->addedBindingCount == 0);
    CHECK(sameDerivedEdgeMapping(*idempotent->augmentedSource.lineage,
                                 *direct->augmentedSource.lineage));

    auto reversedBindings = faceOnly->bindings();
    std::reverse(reversedBindings.begin(), reversedBindings.end());
    std::string error;
    const auto reversedLineage = TopologyLineage::create(
        std::move(reversedBindings), TopologyLineageCoverage::CompleteFaces,
        &error);
    CHECK(reversedLineage);
    const auto reversed = augmentCompleteFaceLineageWithUniqueManifoldEdges(
        {model.feature->id(), model.feature->shape(), reversedLineage},
        &failure);
    CHECK(reversed);
    CHECK(sameDerivedEdgeMapping(*reversed->augmentedSource.lineage,
                                 *direct->augmentedSource.lineage));

    auto incompleteBindings = faceOnly->bindings();
    incompleteBindings.pop_back();
    const auto incomplete = TopologyLineage::create(
        std::move(incompleteBindings), TopologyLineageCoverage::CompleteFaces,
        &error);
    CHECK(incomplete);
    const TopologySourceSnapshot incompleteSource{
        model.feature->id(), model.feature->shape(), incomplete};
    const auto incompleteBefore = incomplete->bindings();
    CHECK(!augmentCompleteFaceLineageWithUniqueManifoldEdges(
        incompleteSource, &failure));
    CHECK(incompleteSource.lineage == incomplete);
    CHECK(sameBindings(incompleteBefore, incomplete->bindings()));

    std::vector<const TopologyLineageBinding*> generatedEdges;
    for (const auto& binding : direct->augmentedSource.lineage->bindings())
      if (binding.provenance.kind == TopologyKind::Edge)
        generatedEdges.push_back(&binding);
    CHECK(generatedEdges.size() == 12);
    auto conflictingBindings = faceOnly->bindings();
    conflictingBindings.push_back(
        {generatedEdges[0]->provenance, generatedEdges[1]->subshape});
    const auto conflicting = TopologyLineage::create(
        std::move(conflictingBindings),
        TopologyLineageCoverage::CompleteFaces, &error);
    CHECK(conflicting);
    const TopologySourceSnapshot conflictingSource{
        model.feature->id(), model.feature->shape(), conflicting};
    const auto conflictingBefore = conflicting->bindings();
    CHECK(!augmentCompleteFaceLineageWithUniqueManifoldEdges(
        conflictingSource, &failure));
    CHECK(failure.code ==
          FaceBoundaryEdgeProvenanceFailureCode::ConflictingEdgeIdentity);
    CHECK(conflictingSource.lineage == conflicting);
    CHECK(sameBindings(conflictingBefore, conflicting->bindings()));

    auto duplicateBindings = faceOnly->bindings();
    duplicateBindings.push_back(*generatedEdges[0]);
    duplicateBindings.push_back(*generatedEdges[0]);
    const auto duplicate = TopologyLineage::create(
        std::move(duplicateBindings),
        TopologyLineageCoverage::CompleteFaces, &error);
    CHECK(duplicate);
    const TopologySourceSnapshot duplicateSource{
        model.feature->id(), model.feature->shape(), duplicate};
    const auto duplicateBefore = duplicate->bindings();
    CHECK(!augmentCompleteFaceLineageWithUniqueManifoldEdges(
        duplicateSource, &failure));
    CHECK(failure.code ==
          FaceBoundaryEdgeProvenanceFailureCode::ConflictingEdgeIdentity);
    CHECK(duplicateSource.lineage == duplicate);
    CHECK(sameBindings(duplicateBefore, duplicate->bindings()));
  }

  // Three independent chains consume predefined geometric carriers. Their
  // adjacency identity survives an upstream edit and every operation has a
  // measurable material effect.
  {
    auto model = makeBox();
    const auto edge = edgeForEndpoints(
        model, gp_Pnt(0.0, 0.0, 0.0), gp_Pnt(0.0, 0.0, 15.0));
    const auto side0 = authoredKey(AuthoredTopologySourceKind::GeometrySide,
                                   model.sketch->id, model.geometryIds[0],
                                   model.feature->id())
                           .immediateOrigin();
    const auto side3 = authoredKey(AuthoredTopologySourceKind::GeometrySide,
                                   model.sketch->id, model.geometryIds[3],
                                   model.feature->id())
                           .immediateOrigin();
    checkCarrier(model, edge, gp_Pnt(0.0, 0.0, 0.0),
                 gp_Pnt(0.0, 0.0, 15.0), {side0, side3});
    auto fillet = std::make_unique<FilletFeature>(edge, 1.0);
    auto* filletFeature = fillet.get();
    model.body->addFeature(std::move(fillet));
    CHECK(model.document.rebuild());
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) < volumeOf(*model.feature->shape()));
    model.feature->setLengthMm(19.0);
    CHECK(model.document.rebuild());
    checkCarrier(model, edge, gp_Pnt(0.0, 0.0, 0.0),
                 gp_Pnt(0.0, 0.0, 19.0), {side0, side3});
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) < volumeOf(*model.feature->shape()));
  }
  {
    auto model = makeBox();
    const auto edge = edgeForEndpoints(
        model, gp_Pnt(0.0, 0.0, 0.0), gp_Pnt(0.0, 0.0, 15.0));
    const auto side0 = authoredKey(AuthoredTopologySourceKind::GeometrySide,
                                   model.sketch->id, model.geometryIds[0],
                                   model.feature->id())
                           .immediateOrigin();
    const auto side3 = authoredKey(AuthoredTopologySourceKind::GeometrySide,
                                   model.sketch->id, model.geometryIds[3],
                                   model.feature->id())
                           .immediateOrigin();
    checkCarrier(model, edge, gp_Pnt(0.0, 0.0, 0.0),
                 gp_Pnt(0.0, 0.0, 15.0), {side0, side3});
    auto chamfer = std::make_unique<ChamferFeature>(edge, 1.0);
    auto* chamferFeature = chamfer.get();
    model.body->addFeature(std::move(chamfer));
    CHECK(model.document.rebuild());
    CHECK(chamferFeature->shape());
    CHECK(volumeOf(*chamferFeature->shape()) <
          volumeOf(*model.feature->shape()));
    model.feature->setLengthMm(19.0);
    CHECK(model.document.rebuild());
    checkCarrier(model, edge, gp_Pnt(0.0, 0.0, 0.0),
                 gp_Pnt(0.0, 0.0, 19.0), {side0, side3});
    CHECK(chamferFeature->shape());
    CHECK(volumeOf(*chamferFeature->shape()) <
          volumeOf(*model.feature->shape()));
  }
  {
    auto model = makeBox();
    const auto edge = edgeForEndpoints(
        model, gp_Pnt(0.0, 0.0, 0.0), gp_Pnt(30.0, 0.0, 0.0));
    const auto sideKey = authoredKey(
        AuthoredTopologySourceKind::GeometrySide, model.sketch->id,
        model.geometryIds[0], model.feature->id());
    const auto face = faceForKey(model, sideKey);
    const auto start = authoredKey(
                           AuthoredTopologySourceKind::ProfileStartCap,
                           model.sketch->id, sketch::kInvalidGeometryId,
                           model.feature->id())
                           .immediateOrigin();
    const auto side = sideKey.immediateOrigin();
    checkCarrier(model, edge, gp_Pnt(0.0, 0.0, 0.0),
                 gp_Pnt(30.0, 0.0, 0.0), {start, side});
    auto draft = std::make_unique<DraftFeature>(
        model.feature->id(), std::vector<FaceReference>{face},
        PlaneReference{NeutralPlaneType::GlobalXY, std::nullopt},
        AxisReference{AxisReferenceType::GlobalZ, kInvalidSketchId,
                      sketch::kInvalidGeometryId},
        3.0, false, "Production edge Draft", edge);
    auto* draftFeature = draft.get();
    model.body->addFeature(std::move(draft));
    CHECK(model.document.rebuild());
    CHECK(draftFeature->shape());
    CHECK(!near(volumeOf(*draftFeature->shape()),
                volumeOf(*model.feature->shape())));
    model.feature->setLengthMm(19.0);
    CHECK(model.document.rebuild());
    checkCarrier(model, edge, gp_Pnt(0.0, 0.0, 0.0),
                 gp_Pnt(30.0, 0.0, 0.0), {start, side});
    const auto resolvedFace = model.feature->topologyIndex()->resolveFace(face);
    CHECK(resolvedFace);
    CHECK(BRepAdaptor_Surface(*resolvedFace.subshape).GetType() ==
          GeomAbs_Plane);
    CHECK(draftFeature->shape());
    CHECK(!near(volumeOf(*draftFeature->shape()),
                volumeOf(*model.feature->shape())));
  }

  // A failed parameter rebuild preserves the exact saved production carrier
  // only through the explicitly named last-valid index; recovery resolves the
  // same carrier against the new authoritative shape.
  {
    auto model = makeBox();
    const auto edge = edgeForEndpoints(
        model, gp_Pnt(0.0, 0.0, 0.0), gp_Pnt(0.0, 0.0, 15.0));
    const auto beforeIndex = model.feature->topologyIndex();
    const auto before = beforeIndex->resolveEdge(edge);
    CHECK(before);
    model.feature->setLengthMm(0.0);
    CHECK(!model.document.rebuild());
    std::string error;
    CHECK(!model.feature->topologyIndex(&error));
    CHECK(!error.empty());
    const auto lastValid = model.feature->lastValidTopologyIndex(&error);
    CHECK(lastValid);
    CHECK(error.empty());
    const auto retained = lastValid->resolveEdge(edge);
    CHECK(retained);
    CHECK(retained.subshape->IsSame(*before.subshape));
    model.feature->setLengthMm(22.0);
    CHECK(model.document.rebuild());
    const auto recovered = model.feature->topologyIndex()->resolveEdge(edge);
    CHECK(recovered);
    CHECK(hasEndpoints(*recovered.subshape, gp_Pnt(0.0, 0.0, 0.0),
                       gp_Pnt(0.0, 0.0, 22.0)));
  }

  // A partial annular-sector revolve publishes eight cap/side lines and four
  // side/side arcs. The carriers are selected by their authored face origins,
  // then retained while angle, direction, storage order, and dimensions
  // change.
  {
    auto model = makeRevolvedRectangle(5.0, 120.0);
    const auto start =
        authoredKey(AuthoredTopologySourceKind::ProfileStartCap,
                    model.sketch->id, sketch::kInvalidGeometryId,
                    model.feature->id())
            .immediateOrigin();
    const auto end =
        authoredKey(AuthoredTopologySourceKind::ProfileEndCap,
                    model.sketch->id, sketch::kInvalidGeometryId,
                    model.feature->id())
            .immediateOrigin();
    std::array<TopologyProvenanceOrigin, 4> sides;
    for (std::size_t index = 0; index < sides.size(); ++index)
      sides[index] =
          authoredKey(AuthoredTopologySourceKind::GeometrySide,
                      model.sketch->id, model.geometryIds[index],
                      model.feature->id())
              .immediateOrigin();

    std::array<EdgeReference, 4> startLines;
    std::array<EdgeReference, 4> endLines;
    for (std::size_t index = 0; index < sides.size(); ++index) {
      startLines[index] =
          edgeForOrigins(*model.feature, model.body->id(), {start, sides[index]});
      endLines[index] =
          edgeForOrigins(*model.feature, model.body->id(), {end, sides[index]});
    }
    constexpr std::array<std::array<std::size_t, 2>, 4> adjacentSides{{
        {{0, 1}}, {{1, 2}}, {{2, 3}}, {{3, 0}}}};
    std::array<EdgeReference, 4> sideArcs;
    for (std::size_t index = 0; index < sideArcs.size(); ++index)
      sideArcs[index] = edgeForOrigins(
          *model.feature, model.body->id(),
          {sides[adjacentSides[index][0]], sides[adjacentSides[index][1]]});

    const auto checkPartial = [&](double minimumX, double maximumX,
                                  double minimumRadius, double maximumRadius,
                                  double signedAngle) {
      const std::array<std::array<std::pair<double, double>, 2>, 4>
          sideEndpoints{{
              {{{minimumX, minimumRadius}, {maximumX, minimumRadius}}},
              {{{maximumX, minimumRadius}, {maximumX, maximumRadius}}},
              {{{maximumX, maximumRadius}, {minimumX, maximumRadius}}},
              {{{minimumX, maximumRadius}, {minimumX, minimumRadius}}},
          }};
      for (std::size_t index = 0; index < sides.size(); ++index) {
        const auto& endpoints = sideEndpoints[index];
        checkRevolveCarrier(
            *model.feature, startLines[index], {start, sides[index]},
            {GeomAbs_Line,
             std::hypot(endpoints[1].first - endpoints[0].first,
                        endpoints[1].second - endpoints[0].second),
             revolvedPoint(endpoints[0].first, endpoints[0].second, 0.0),
             revolvedPoint(endpoints[1].first, endpoints[1].second, 0.0)});
        checkRevolveCarrier(
            *model.feature, endLines[index], {end, sides[index]},
            {GeomAbs_Line,
             std::hypot(endpoints[1].first - endpoints[0].first,
                        endpoints[1].second - endpoints[0].second),
             revolvedPoint(endpoints[0].first, endpoints[0].second,
                           signedAngle),
             revolvedPoint(endpoints[1].first, endpoints[1].second,
                           signedAngle)});
      }

      const std::array<std::pair<double, double>, 4> arcVertices{{
          {maximumX, minimumRadius}, {maximumX, maximumRadius},
          {minimumX, maximumRadius}, {minimumX, minimumRadius}}};
      for (std::size_t index = 0; index < sideArcs.size(); ++index) {
        const auto [x, radius] = arcVertices[index];
        checkRevolveCarrier(
            *model.feature, sideArcs[index],
            {sides[adjacentSides[index][0]], sides[adjacentSides[index][1]]},
            {GeomAbs_Circle,
             radius * std::abs(signedAngle) * std::numbers::pi / 180.0,
             revolvedPoint(x, radius, 0.0),
             revolvedPoint(x, radius, signedAngle)});
      }
    };

    checkPartial(10.0, 30.0, 5.0, 15.0, 120.0);
    checkAllCanonicalEdgesCreated(*model.feature, model.body->id(), 12);
    model.feature->setAngleDeg(210.0);
    CHECK(model.document.recompute());
    checkPartial(10.0, 30.0, 5.0, 15.0, 210.0);
    model.feature->setReversed(true);
    CHECK(model.document.recompute());
    checkPartial(10.0, 30.0, 5.0, 15.0, -210.0);

    sketch::Sketch reordered;
    reordered.addLine({12.0, 17.0}, {34.0, 17.0});
    reordered.addLine({34.0, 6.0}, {12.0, 6.0});
    reordered.addLine({12.0, 6.0}, {12.0, 17.0});
    reordered.addLine({34.0, 17.0}, {34.0, 6.0});
    CHECK(reordered.restoreGeometryIds(
        {model.geometryIds[2], model.geometryIds[0], model.geometryIds[3],
         model.geometryIds[1]},
        {}, {}, {}));
    CHECK(model.document.replaceSketchGeometry(model.sketch->id,
                                               std::move(reordered)));
    CHECK(model.document.recompute());
    checkPartial(12.0, 34.0, 6.0, 17.0, -210.0);
    checkAllCanonicalEdgesCreated(*model.feature, model.body->id(), 12);

    // A closed sweep has no cap identities. The four side/side carriers become
    // full circles, while all cap/side carriers disappear. The only line
    // topology left is the two unsupported cylindrical seam edges.
    model.feature->setReversed(false);
    model.feature->setAngleDeg(360.0);
    CHECK(model.document.recompute());
    for (const auto& carrier : startLines) checkEdgeMissing(*model.feature, carrier);
    for (const auto& carrier : endLines) checkEdgeMissing(*model.feature, carrier);
    const std::array<double, 4> fullRadii{6.0, 17.0, 17.0, 6.0};
    for (std::size_t index = 0; index < sideArcs.size(); ++index)
      checkRevolveCarrier(
          *model.feature, sideArcs[index],
          {sides[adjacentSides[index][0]], sides[adjacentSides[index][1]]},
          {GeomAbs_Circle, 2.0 * std::numbers::pi * fullRadii[index],
           std::nullopt, std::nullopt});

    const auto fullIndex = model.feature->topologyIndex();
    CHECK(fullIndex);
    std::vector<TopoDS_Edge> seenFullEdges;
    std::size_t missingSeamLines = 0;
    std::size_t createdFullCircles = 0;
    for (std::size_t raw = 0; raw < fullIndex->rawEdgeCount(); ++raw) {
      const auto legacy = fullIndex->createEdgeReference(
          model.body->id(), model.feature->id(), raw);
      CHECK(legacy);
      const auto resolved = fullIndex->resolveEdge(legacy.reference);
      CHECK(resolved);
      if (std::any_of(seenFullEdges.begin(), seenFullEdges.end(),
                      [&](const auto& edge) {
                        return edge.IsSame(*resolved.subshape);
                      }))
        continue;
      seenFullEdges.push_back(*resolved.subshape);
      const auto exact =
          model.feature->createEdgeReferenceV2(model.body->id(), raw);
      const auto curveType = BRepAdaptor_Curve(*resolved.subshape).GetType();
      CHECK(curveType == GeomAbs_Line || curveType == GeomAbs_Circle);
      if (curveType == GeomAbs_Line) {
        CHECK(!exact);
        CHECK(exact.status == EdgeReferenceV2CreationStatus::Missing);
        ++missingSeamLines;
      } else if (curveType == GeomAbs_Circle) {
        CHECK(exact);
        CHECK(exact.status == EdgeReferenceV2CreationStatus::Created);
        ++createdFullCircles;
      }
    }
    CHECK(seenFullEdges.size() == 6);
    CHECK(missingSeamLines == 2);
    CHECK(createdFullCircles == 4);
    CHECK(edgeBindingCount(*model.feature) == 4);

    model.feature->setAngleDeg(150.0);
    CHECK(model.document.recompute());
    checkPartial(12.0, 34.0, 6.0, 17.0, 150.0);
    const auto validShape = model.feature->shape();
    const auto validLineage = model.feature->topologyLineage();
    const auto validIndex = model.feature->topologyIndex();
    const auto validRevision = model.feature->shapeRevision();
    model.feature->setAngleDeg(0.0);
    CHECK(!model.document.recompute());
    CHECK(!model.feature->shape());
    CHECK(!model.feature->topologyLineage());
    CHECK(model.feature->shapeRevision() == validRevision);
    std::string error;
    CHECK(!model.feature->topologyIndex(&error));
    CHECK(!error.empty());
    CHECK(model.feature->lastValidShape() == validShape);
    CHECK(model.feature->lastValidTopologyLineage() == validLineage);
    CHECK(model.feature->lastValidTopologyIndex(&error) == validIndex);
    CHECK(error.empty());
    for (const auto& carrier : startLines) CHECK(validIndex->resolveEdge(carrier));
    for (const auto& carrier : endLines) CHECK(validIndex->resolveEdge(carrier));
    for (const auto& carrier : sideArcs) CHECK(validIndex->resolveEdge(carrier));
    model.feature->setAngleDeg(120.0);
    CHECK(model.document.recompute());
    checkPartial(12.0, 34.0, 6.0, 17.0, 120.0);
  }

  // A profile touching the axis has one cap/cap edge during a partial sweep.
  // Closing the sweep removes it and leaves two named circular rims plus one
  // unsupported cylindrical seam; reopening recovers the saved carrier.
  {
    auto model = makeRevolvedRectangle(0.0, 120.0);
    const auto start =
        authoredKey(AuthoredTopologySourceKind::ProfileStartCap,
                    model.sketch->id, sketch::kInvalidGeometryId,
                    model.feature->id())
            .immediateOrigin();
    const auto end =
        authoredKey(AuthoredTopologySourceKind::ProfileEndCap,
                    model.sketch->id, sketch::kInvalidGeometryId,
                    model.feature->id())
            .immediateOrigin();
    const auto axisCarrier =
        edgeForOrigins(*model.feature, model.body->id(), {start, end});
    checkRevolveCarrier(*model.feature, axisCarrier, {start, end},
                        {GeomAbs_Line, 20.0, gp_Pnt(10.0, 0.0, 0.0),
                         gp_Pnt(30.0, 0.0, 0.0)});
    checkAllCanonicalEdgesCreated(*model.feature, model.body->id(), 9);

    model.feature->setAngleDeg(360.0);
    CHECK(model.document.recompute());
    checkEdgeMissing(*model.feature, axisCarrier);
    const auto fullIndex = model.feature->topologyIndex();
    CHECK(fullIndex);
    std::vector<TopoDS_Edge> seen;
    std::size_t createdRims = 0;
    std::size_t missingSeams = 0;
    for (std::size_t raw = 0; raw < fullIndex->rawEdgeCount(); ++raw) {
      const auto legacy = fullIndex->createEdgeReference(
          model.body->id(), model.feature->id(), raw);
      CHECK(legacy);
      const auto resolved = fullIndex->resolveEdge(legacy.reference);
      CHECK(resolved);
      if (std::any_of(seen.begin(), seen.end(), [&](const auto& edge) {
            return edge.IsSame(*resolved.subshape);
          }))
        continue;
      seen.push_back(*resolved.subshape);
      const auto exact =
          model.feature->createEdgeReferenceV2(model.body->id(), raw);
      if (BRepAdaptor_Curve(*resolved.subshape).GetType() == GeomAbs_Circle) {
        CHECK(exact);
        ++createdRims;
      } else {
        CHECK(BRepAdaptor_Curve(*resolved.subshape).GetType() == GeomAbs_Line);
        CHECK(!exact);
        CHECK(exact.status == EdgeReferenceV2CreationStatus::Missing);
        ++missingSeams;
      }
    }
    CHECK(createdRims == 2);
    CHECK(missingSeams == 1);

    model.feature->setAngleDeg(210.0);
    CHECK(model.document.recompute());
    checkRevolveCarrier(*model.feature, axisCarrier, {start, end},
                        {GeomAbs_Line, 20.0, gp_Pnt(10.0, 0.0, 0.0),
                         gp_Pnt(30.0, 0.0, 0.0)});
  }

  // A predefined outer rim feeds a downstream Fillet. Its exact carrier and
  // measurable material effect survive changing the upstream sweep angle.
  {
    auto model = makeRevolvedRectangle(0.0, 360.0);
    const auto outerEnd =
        authoredKey(AuthoredTopologySourceKind::GeometrySide,
                    model.sketch->id, model.geometryIds[1],
                    model.feature->id())
            .immediateOrigin();
    const auto outerCylinder =
        authoredKey(AuthoredTopologySourceKind::GeometrySide,
                    model.sketch->id, model.geometryIds[2],
                    model.feature->id())
            .immediateOrigin();
    const auto rim = edgeForOrigins(*model.feature, model.body->id(),
                                    {outerEnd, outerCylinder});
    checkRevolveCarrier(
        *model.feature, rim, {outerEnd, outerCylinder},
        {GeomAbs_Circle, 2.0 * std::numbers::pi * 15.0, std::nullopt,
         std::nullopt});
    auto fillet = std::make_unique<FilletFeature>(rim, 1.0);
    auto* filletFeature = fillet.get();
    model.body->addFeature(std::move(fillet));
    CHECK(model.document.recompute());
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) <
          volumeOf(*model.feature->shape()) - 1e-5);

    model.feature->setAngleDeg(210.0);
    CHECK(model.document.recompute());
    checkRevolveCarrier(
        *model.feature, rim, {outerEnd, outerCylinder},
        {GeomAbs_Circle, 15.0 * 210.0 * std::numbers::pi / 180.0,
         revolvedPoint(30.0, 15.0, 0.0),
         revolvedPoint(30.0, 15.0, 210.0)});
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) <
          volumeOf(*model.feature->shape()) - 1e-5);
  }

  return EXIT_SUCCESS;
}
