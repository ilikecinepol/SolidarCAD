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
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "model/ChamferBuilder.h"
#include "model/ChamferFeature.h"
#include "model/CircularPatternFeature.h"
#include "model/Document.h"
#include "model/DraftBuilder.h"
#include "model/DraftFeature.h"
#include "model/ExtrudeFeature.h"
#include "model/FaceBoundaryEdgeProvenance.h"
#include "model/FilletBuilder.h"
#include "model/FilletFeature.h"
#include "model/JoinBodiesFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/MirrorFeature.h"
#include "model/MoveFeature.h"
#include "model/PocketFeature.h"
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

struct PocketedBox {
  solidar::Document document;
  solidar::Body* body{};
  solidar::ExtrudeFeature* extrusion{};
  solidar::PocketFeature* pocket{};
  solidar::SketchId pocketSketchId{solidar::kInvalidSketchId};
  solidar::sketch::GeometryId circleId{
      solidar::sketch::kInvalidGeometryId};
};

struct JoinedBoxes {
  solidar::Document document;
  solidar::Body* resultBody{};
  solidar::ExtrudeFeature* first{};
  solidar::ExtrudeFeature* second{};
  solidar::JoinBodiesFeature* join{};
  solidar::BodyId firstBodyId{solidar::kInvalidBodyId};
  solidar::BodyId secondBodyId{solidar::kInvalidBodyId};
  solidar::SketchId firstSketchId{solidar::kInvalidSketchId};
  solidar::SketchId secondSketchId{solidar::kInvalidSketchId};
  std::array<solidar::sketch::GeometryId, 4> firstGeometryIds{};
  std::array<solidar::sketch::GeometryId, 4> secondGeometryIds{};
};

struct MovedBox {
  solidar::Document document;
  solidar::Body* body{};
  solidar::SketchId sketchId{solidar::kInvalidSketchId};
  solidar::ExtrudeFeature* extrusion{};
  solidar::MoveFeature* move{};
  std::array<solidar::sketch::GeometryId, 4> geometryIds{};
};

struct MirroredBox {
  solidar::Document document;
  solidar::Body* body{};
  solidar::SketchId sketchId{solidar::kInvalidSketchId};
  solidar::ExtrudeFeature* extrusion{};
  solidar::MirrorFeature* mirror{};
  std::array<solidar::sketch::GeometryId, 4> geometryIds{};
};

struct LinearPatternedBox {
  solidar::Document document;
  solidar::Body* sourceBody{};
  solidar::Body* patternBody{};
  solidar::SketchId sketchId{solidar::kInvalidSketchId};
  solidar::ExtrudeFeature* extrusion{};
  solidar::LinearPatternFeature* pattern{};
  std::array<solidar::sketch::GeometryId, 4> geometryIds{};
};

struct CircularPatternedBox {
  solidar::Document document;
  solidar::Body* sourceBody{};
  solidar::Body* patternBody{};
  solidar::SketchId sketchId{solidar::kInvalidSketchId};
  solidar::ExtrudeFeature* extrusion{};
  solidar::CircularPatternFeature* pattern{};
  std::array<solidar::sketch::GeometryId, 4> geometryIds{};
};

CircularPatternedBox makeCircularPatternedBox(
    solidar::PatternOperation operation = solidar::PatternOperation::Join) {
  CircularPatternedBox result;
  auto& sketch =
      result.document.addSketch("Production edge circular pattern source");
  sketch.geometry.addRectangle({10.0, 2.0}, {14.0, 5.0});
  result.sketchId = sketch.id;
  result.geometryIds = {
      sketch.geometry.lineId(0), sketch.geometry.lineId(1),
      sketch.geometry.lineId(2), sketch.geometry.lineId(3)};
  auto& sourceBody = result.document.addBody("Circular pattern source");
  const auto sourceBodyId = sourceBody.id();
  auto extrusion =
      std::make_unique<solidar::ExtrudeFeature>(sketch.id, 4.0);
  result.extrusion = extrusion.get();
  const auto extrusionId = extrusion->id();
  sourceBody.addFeature(std::move(extrusion));

  auto patternBodyId = sourceBodyId;
  if (operation == solidar::PatternOperation::Join) {
    auto pattern = std::make_unique<solidar::CircularPatternFeature>(
        extrusionId, solidar::PrincipalAxis::Z, 5, 240.0);
    result.pattern = pattern.get();
    sourceBody.addFeature(std::move(pattern));
  } else {
    auto& copiesBody = result.document.addBody("Circular pattern copies");
    patternBodyId = copiesBody.id();
    auto pattern = std::make_unique<solidar::CircularPatternFeature>(
        sourceBodyId, extrusionId, solidar::PrincipalAxis::Z, 5, 240.0,
        solidar::PatternOperation::NewBody);
    result.pattern = pattern.get();
    copiesBody.addFeature(std::move(pattern));
  }
  result.sourceBody = result.document.findBody(sourceBodyId);
  result.patternBody = result.document.findBody(patternBodyId);
  CHECK(result.sourceBody);
  CHECK(result.patternBody);
  CHECK(result.document.rebuild());
  return result;
}

LinearPatternedBox makeLinearPatternedBox(
    solidar::PatternOperation operation = solidar::PatternOperation::Join) {
  LinearPatternedBox result;
  auto& sketch =
      result.document.addSketch("Production edge linear pattern source");
  sketch.geometry.addRectangle({0.0, 0.0}, {8.0, 6.0});
  result.sketchId = sketch.id;
  result.geometryIds = {
      sketch.geometry.lineId(0), sketch.geometry.lineId(1),
      sketch.geometry.lineId(2), sketch.geometry.lineId(3)};
  auto& sourceBody = result.document.addBody("Linear pattern source");
  const auto sourceBodyId = sourceBody.id();
  auto extrusion =
      std::make_unique<solidar::ExtrudeFeature>(sketch.id, 5.0);
  result.extrusion = extrusion.get();
  const auto extrusionId = extrusion->id();
  sourceBody.addFeature(std::move(extrusion));

  auto patternBodyId = sourceBodyId;
  if (operation == solidar::PatternOperation::Join) {
    auto pattern = std::make_unique<solidar::LinearPatternFeature>(
        extrusionId, solidar::PrincipalAxis::X, 5, 12.0);
    result.pattern = pattern.get();
    sourceBody.addFeature(std::move(pattern));
  } else {
    auto& copiesBody = result.document.addBody("Linear pattern copies");
    patternBodyId = copiesBody.id();
    auto pattern = std::make_unique<solidar::LinearPatternFeature>(
        sourceBodyId, extrusionId, solidar::PrincipalAxis::X, 5, 12.0,
        solidar::PatternOperation::NewBody);
    result.pattern = pattern.get();
    copiesBody.addFeature(std::move(pattern));
  }
  result.sourceBody = result.document.findBody(sourceBodyId);
  result.patternBody = result.document.findBody(patternBodyId);
  CHECK(result.sourceBody);
  CHECK(result.patternBody);
  CHECK(result.document.rebuild());
  return result;
}

MirroredBox makeMirroredBox(
    solidar::MirrorPlane plane = solidar::MirrorPlane::YZ) {
  MirroredBox result;
  auto& sketch = result.document.addSketch("Production edge mirror source");
  sketch.geometry.addRectangle({10.0, 3.0}, {20.0, 9.0});
  result.sketchId = sketch.id;
  result.geometryIds = {
      sketch.geometry.lineId(0), sketch.geometry.lineId(1),
      sketch.geometry.lineId(2), sketch.geometry.lineId(3)};
  auto& body = result.document.addBody();
  auto extrusion =
      std::make_unique<solidar::ExtrudeFeature>(sketch.id, 5.0);
  result.extrusion = extrusion.get();
  const auto extrusionId = extrusion->id();
  body.addFeature(std::move(extrusion));
  auto mirror =
      std::make_unique<solidar::MirrorFeature>(extrusionId, plane);
  result.mirror = mirror.get();
  body.addFeature(std::move(mirror));
  result.body = &body;
  CHECK(result.document.rebuild());
  return result;
}

MovedBox makeMovedBox(solidar::Vector3d offset = {5.0, 7.0, 3.0}) {
  MovedBox result;
  auto& sketch = result.document.addSketch("Production edge move source");
  sketch.geometry.addRectangle({0.0, 0.0}, {30.0, 20.0});
  result.sketchId = sketch.id;
  result.geometryIds = {
      sketch.geometry.lineId(0), sketch.geometry.lineId(1),
      sketch.geometry.lineId(2), sketch.geometry.lineId(3)};
  auto& body = result.document.addBody();
  auto extrusion =
      std::make_unique<solidar::ExtrudeFeature>(sketch.id, 15.0);
  result.extrusion = extrusion.get();
  const auto extrusionId = extrusion->id();
  body.addFeature(std::move(extrusion));
  auto move = std::make_unique<solidar::MoveFeature>(extrusionId, offset);
  result.move = move.get();
  body.addFeature(std::move(move));
  result.body = &body;
  CHECK(result.document.rebuild());
  return result;
}

JoinedBoxes makeJoinedBoxes() {
  JoinedBoxes result;
  auto& firstSketch =
      result.document.addSketch("Production edge join source A");
  firstSketch.geometry.addRectangle({0.0, 0.0}, {20.0, 10.0});
  result.firstSketchId = firstSketch.id;
  result.firstGeometryIds = {
      firstSketch.geometry.lineId(0), firstSketch.geometry.lineId(1),
      firstSketch.geometry.lineId(2), firstSketch.geometry.lineId(3)};
  auto& firstBody = result.document.addBody("Join source A");
  result.firstBodyId = firstBody.id();
  auto first =
      std::make_unique<solidar::ExtrudeFeature>(firstSketch.id, 10.0);
  result.first = first.get();
  firstBody.addFeature(std::move(first));

  auto& secondSketch =
      result.document.addSketch("Production edge join source B");
  secondSketch.geometry.addRectangle({10.0, 0.0}, {30.0, 10.0});
  result.secondSketchId = secondSketch.id;
  result.secondGeometryIds = {
      secondSketch.geometry.lineId(0), secondSketch.geometry.lineId(1),
      secondSketch.geometry.lineId(2), secondSketch.geometry.lineId(3)};
  auto& secondBody = result.document.addBody("Join source B");
  result.secondBodyId = secondBody.id();
  auto second =
      std::make_unique<solidar::ExtrudeFeature>(secondSketch.id, 10.0);
  result.second = second.get();
  secondBody.addFeature(std::move(second));

  auto& resultBody = result.document.addBody("Join result");
  auto join = std::make_unique<solidar::JoinBodiesFeature>(
      result.firstBodyId, result.first->id(), result.secondBodyId,
      result.second->id());
  result.join = join.get();
  resultBody.addFeature(std::move(join));
  result.resultBody = &resultBody;
  CHECK(result.document.rebuild());
  return result;
}

void replaceJoinedRectangle(
    JoinedBoxes& model, bool second, double minimumX, double maximumX) {
  solidar::sketch::Sketch replacement;
  replacement.addRectangle({minimumX, 0.0}, {maximumX, 10.0});
  const auto& ids =
      second ? model.secondGeometryIds : model.firstGeometryIds;
  CHECK(replacement.restoreGeometryIds({ids[0], ids[1], ids[2], ids[3]},
                                       {}, {}, {}));
  CHECK(model.document.replaceSketchGeometry(
      second ? model.secondSketchId : model.firstSketchId,
      std::move(replacement)));
}

PocketedBox makePocketedBox(double depthMm = 6.0,
                            solidar::sketch::Point center = {15.0, 14.0},
                            double radius = 4.0) {
  PocketedBox result;
  auto& base = result.document.addSketch("Production edge pocket base");
  base.geometry.addRectangle({0.0, 0.0}, {40.0, 30.0});
  auto& body = result.document.addBody();
  auto extrusion = std::make_unique<solidar::ExtrudeFeature>(base.id, 20.0);
  result.extrusion = extrusion.get();
  body.addFeature(std::move(extrusion));

  auto& profile =
      result.document.addSketch("Production edge circular pocket");
  profile.placement.origin.z = 20.0;
  profile.geometry.addCircle(center, radius);
  result.pocketSketchId = profile.id;
  result.circleId = profile.geometry.circleId(0);
  auto pocket =
      std::make_unique<solidar::PocketFeature>(profile.id, depthMm);
  result.pocket = pocket.get();
  body.addFeature(std::move(pocket));
  result.body = &body;
  CHECK(result.document.rebuild());
  return result;
}

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

void checkExactCarrier(
    const solidar::ShapeFeature& feature, const solidar::EdgeReference& carrier,
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

solidar::EdgeReference edgeForExactEndpoints(
    const solidar::ShapeFeature& feature, solidar::BodyId bodyId,
    const gp_Pnt& expectedFirst, const gp_Pnt& expectedLast) {
  const auto index = feature.topologyIndex();
  CHECK(index);
  std::optional<solidar::EdgeReference> found;
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
    if (!hasEndpoints(*resolved.subshape, expectedFirst, expectedLast))
      continue;
    const auto exact = feature.createEdgeReferenceV2(bodyId, raw);
    CHECK(exact);
    CHECK(exact.status == solidar::EdgeReferenceV2CreationStatus::Created);
    CHECK(!found);
    found = exact.reference;
  }
  CHECK(found);
  return *found;
}

solidar::EdgeReference edgeForCircle(
    const solidar::ShapeFeature& feature, solidar::BodyId bodyId,
    const gp_Pnt& expectedCenter, double expectedRadius) {
  const auto index = feature.topologyIndex();
  CHECK(index);
  std::optional<solidar::EdgeReference> found;
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
    BRepAdaptor_Curve curve(*resolved.subshape);
    if (curve.GetType() != GeomAbs_Circle ||
        !samePoint(curve.Circle().Location(), expectedCenter) ||
        !near(curve.Circle().Radius(), expectedRadius))
      continue;
    const auto exact = feature.createEdgeReferenceV2(bodyId, raw);
    CHECK(exact);
    CHECK(exact.status == solidar::EdgeReferenceV2CreationStatus::Created);
    CHECK(!found);
    found = exact.reference;
  }
  CHECK(found);
  return *found;
}

struct ShapeBounds {
  double x0{};
  double y0{};
  double z0{};
  double x1{};
  double y1{};
  double z1{};
};

bool hasBounds(const TopoDS_Shape& shape, const ShapeBounds& expected) {
  Bnd_Box bounds;
  BRepBndLib::Add(shape, bounds);
  double x0, y0, z0, x1, y1, z1;
  bounds.Get(x0, y0, z0, x1, y1, z1);
  return near(x0, expected.x0) && near(y0, expected.y0) &&
         near(z0, expected.z0) && near(x1, expected.x1) &&
         near(y1, expected.y1) && near(z1, expected.z1);
}

solidar::TopologyProvenanceOrigin faceOriginForBounds(
    const solidar::ShapeFeature& feature, const ShapeBounds& expected) {
  const auto lineage = feature.topologyLineage();
  CHECK(lineage);
  std::optional<solidar::TopologyProvenanceOrigin> found;
  for (const auto& binding : lineage->bindings()) {
    if (binding.provenance.kind != solidar::TopologyKind::Face ||
        !hasBounds(*binding.subshape, expected))
      continue;
    CHECK(!found);
    found = binding.provenance.immediateOrigin();
  }
  CHECK(found);
  return *found;
}

void checkCircleCarrier(
    const solidar::ShapeFeature& feature, const solidar::EdgeReference& carrier,
    std::vector<solidar::TopologyProvenanceOrigin> expectedOrigins,
    const gp_Pnt& expectedCenter, double expectedRadius) {
  checkExactCarrier(
      feature, carrier, std::move(expectedOrigins),
      {GeomAbs_Circle, 2.0 * std::numbers::pi * expectedRadius,
       std::nullopt, std::nullopt});
  const auto resolved = feature.topologyIndex()->resolveEdge(carrier);
  CHECK(resolved);
  const BRepAdaptor_Curve curve(*resolved.subshape);
  CHECK(curve.GetType() == GeomAbs_Circle);
  CHECK(samePoint(curve.Circle().Location(), expectedCenter));
  CHECK(near(curve.Circle().Radius(), expectedRadius));
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

void auditJoinedBoxEdges(const solidar::ShapeFeature& join,
                         solidar::BodyId bodyId) {
  const auto index = join.topologyIndex();
  CHECK(index);
  std::vector<TopoDS_Edge> seen;
  for (std::size_t raw = 0; raw < index->rawEdgeCount(); ++raw) {
    const auto legacy = index->createEdgeReference(bodyId, join.id(), raw);
    CHECK(legacy);
    const auto rawEdge = index->resolveEdge(legacy.reference);
    CHECK(rawEdge);
    if (std::any_of(seen.begin(), seen.end(), [&](const auto& edge) {
          return edge.IsSame(*rawEdge.subshape);
        }))
      continue;
    seen.push_back(*rawEdge.subshape);

    const auto exact = join.createEdgeReferenceV2(bodyId, raw);
    CHECK(exact);
    CHECK(exact.status == solidar::EdgeReferenceV2CreationStatus::Created);
    CHECK(exact.reference.v2());
    const auto& key = exact.reference.v2()->provenance;
    CHECK(key.featureId == join.id());
    CHECK(key.kind == solidar::TopologyKind::Edge);
    CHECK(key.operation == solidar::TopologyProvenanceOperation::Generated);
    CHECK(key.localName.starts_with("face-boundary-edge:v1;"));
    CHECK(key.origins.size() == 2);
    for (const auto& origin : key.origins) {
      CHECK(origin.featureId == join.id());
      CHECK(origin.kind == solidar::TopologyKind::Face);
    }
    CHECK(key.origins == adjacentFaceOrigins(join, *rawEdge.subshape));
    const auto resolved = index->resolveEdge(exact.reference);
    CHECK(resolved);
    CHECK(resolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    CHECK(resolved.subshape->IsSame(*rawEdge.subshape));
  }
  CHECK(seen.size() == 12);
  CHECK(edgeBindingCount(join) == 12);
}

void auditMirroredBoxEdges(const solidar::ShapeFeature& mirror,
                           solidar::BodyId bodyId) {
  const auto index = mirror.topologyIndex();
  CHECK(index);
  std::vector<TopoDS_Edge> seen;
  std::array<std::size_t, 2> slotCounts{};
  for (std::size_t raw = 0; raw < index->rawEdgeCount(); ++raw) {
    const auto legacy =
        index->createEdgeReference(bodyId, mirror.id(), raw);
    CHECK(legacy);
    const auto rawEdge = index->resolveEdge(legacy.reference);
    CHECK(rawEdge);
    if (std::any_of(seen.begin(), seen.end(), [&](const auto& edge) {
          return edge.IsSame(*rawEdge.subshape);
        }))
      continue;
    seen.push_back(*rawEdge.subshape);

    const auto exact = mirror.createEdgeReferenceV2(bodyId, raw);
    CHECK(exact);
    CHECK(exact.status ==
          solidar::EdgeReferenceV2CreationStatus::Created);
    CHECK(exact.reference.v2());
    const auto& key = exact.reference.v2()->provenance;
    CHECK(key.featureId == mirror.id());
    CHECK(key.kind == solidar::TopologyKind::Edge);
    CHECK(key.operation ==
          solidar::TopologyProvenanceOperation::Generated);
    CHECK(!key.occurrence);
    CHECK(key.localName.starts_with("face-boundary-edge:v1;"));
    CHECK(key.origins.size() == 2);
    CHECK(key.origins == adjacentFaceOrigins(mirror, *rawEdge.subshape));
    CHECK(key.origins.front().occurrence);
    const auto slot = key.origins.front().occurrence->slot;
    CHECK(slot < slotCounts.size());
    for (const auto& origin : key.origins) {
      CHECK(origin.featureId == mirror.id());
      CHECK(origin.kind == solidar::TopologyKind::Face);
      CHECK(origin.occurrence);
      CHECK(origin.occurrence->featureId == mirror.id());
      CHECK(origin.occurrence->slot == slot);
    }
    ++slotCounts[slot];

    const auto resolved = index->resolveEdge(exact.reference);
    CHECK(resolved);
    CHECK(resolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    CHECK(resolved.subshape->IsSame(*rawEdge.subshape));
  }
  CHECK(seen.size() == 24);
  CHECK(edgeBindingCount(mirror) == 24);
  CHECK(slotCounts[0] == 12);
  CHECK(slotCounts[1] == 12);
}

void auditOccurrencePatternEdges(
    const solidar::ShapeFeature& pattern, solidar::BodyId bodyId,
    const std::vector<std::uint64_t>& expectedSlots) {
  const auto index = pattern.topologyIndex();
  CHECK(index);
  std::vector<TopoDS_Edge> seen;
  std::vector<std::size_t> slotCounts(expectedSlots.size(), 0);
  for (std::size_t raw = 0; raw < index->rawEdgeCount(); ++raw) {
    const auto legacy =
        index->createEdgeReference(bodyId, pattern.id(), raw);
    CHECK(legacy);
    const auto rawEdge = index->resolveEdge(legacy.reference);
    CHECK(rawEdge);
    if (std::any_of(seen.begin(), seen.end(), [&](const auto& edge) {
          return edge.IsSame(*rawEdge.subshape);
        }))
      continue;
    seen.push_back(*rawEdge.subshape);

    const auto exact = pattern.createEdgeReferenceV2(bodyId, raw);
    CHECK(exact);
    CHECK(exact.status ==
          solidar::EdgeReferenceV2CreationStatus::Created);
    CHECK(exact.reference.v2());
    const auto& key = exact.reference.v2()->provenance;
    CHECK(key.featureId == pattern.id());
    CHECK(key.kind == solidar::TopologyKind::Edge);
    CHECK(key.operation ==
          solidar::TopologyProvenanceOperation::Generated);
    CHECK(!key.occurrence);
    CHECK(key.localName.starts_with("face-boundary-edge:v1;"));
    CHECK(key.origins.size() == 2);
    CHECK(key.origins == adjacentFaceOrigins(pattern, *rawEdge.subshape));
    CHECK(key.origins.front().occurrence);
    const auto slot = key.origins.front().occurrence->slot;
    const auto slotPosition =
        std::find(expectedSlots.begin(), expectedSlots.end(), slot);
    CHECK(slotPosition != expectedSlots.end());
    for (const auto& origin : key.origins) {
      CHECK(origin.featureId == pattern.id());
      CHECK(origin.kind == solidar::TopologyKind::Face);
      CHECK(origin.occurrence);
      CHECK(origin.occurrence->featureId == pattern.id());
      CHECK(origin.occurrence->slot == slot);
    }
    ++slotCounts[static_cast<std::size_t>(
        std::distance(expectedSlots.begin(), slotPosition))];

    const auto resolved = index->resolveEdge(exact.reference);
    CHECK(resolved);
    CHECK(resolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    CHECK(resolved.subshape->IsSame(*rawEdge.subshape));
  }
  CHECK(seen.size() == expectedSlots.size() * 12);
  CHECK(edgeBindingCount(pattern) == expectedSlots.size() * 12);
  for (const auto count : slotCounts) CHECK(count == 12);
}

std::size_t solidCount(const TopoDS_Shape& shape) {
  std::size_t result = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_SOLID); explorer.More();
       explorer.Next())
    ++result;
  return result;
}

void auditBlindPocketEdges(const solidar::ShapeFeature& feature,
                           solidar::BodyId bodyId) {
  const auto index = feature.topologyIndex();
  CHECK(index);
  std::vector<TopoDS_Edge> seen;
  std::vector<solidar::TopologyProvenanceKey> createdKeys;
  std::size_t createdLines = 0;
  std::size_t createdCircles = 0;
  std::size_t missingLines = 0;
  for (std::size_t raw = 0; raw < index->rawEdgeCount(); ++raw) {
    const auto legacy =
        index->createEdgeReference(bodyId, feature.id(), raw);
    CHECK(legacy);
    const auto rawEdge = index->resolveEdge(legacy.reference);
    CHECK(rawEdge);
    if (std::any_of(seen.begin(), seen.end(), [&](const auto& edge) {
          return edge.IsSame(*rawEdge.subshape);
        }))
      continue;
    seen.push_back(*rawEdge.subshape);

    const auto type = BRepAdaptor_Curve(*rawEdge.subshape).GetType();
    CHECK(type == GeomAbs_Line || type == GeomAbs_Circle);
    const auto exact = feature.createEdgeReferenceV2(bodyId, raw);
    if (exact) {
      CHECK(exact.status == solidar::EdgeReferenceV2CreationStatus::Created);
      const auto resolved = index->resolveEdge(exact.reference);
      CHECK(resolved);
      CHECK(resolved.method ==
            solidar::TopologyMatchMethod::OperationProvenance);
      CHECK(resolved.subshape->IsSame(*rawEdge.subshape));
      CHECK(exact.reference.v2());
      createdKeys.push_back(exact.reference.v2()->provenance);
      if (type == GeomAbs_Line)
        ++createdLines;
      else
        ++createdCircles;
    } else {
      CHECK(exact.status == solidar::EdgeReferenceV2CreationStatus::Missing);
      CHECK(type == GeomAbs_Line);
      ++missingLines;
    }
  }
  CHECK(seen.size() == 15);
  CHECK(createdLines == 12);
  CHECK(createdCircles == 2);
  CHECK(missingLines == 1);
  CHECK(createdKeys.size() == 14);
  CHECK(edgeBindingCount(feature) == 14);
  for (const auto& binding : feature.topologyLineage()->bindings()) {
    if (binding.provenance.kind != solidar::TopologyKind::Edge) continue;
    CHECK(std::count(createdKeys.begin(), createdKeys.end(),
                     binding.provenance) == 1);
  }
}

gp_Pnt revolvedPoint(double x, double radius, double angleDeg) {
  const double radians = angleDeg * std::numbers::pi / 180.0;
  return {x, radius * std::cos(radians), radius * std::sin(radians)};
}

double circularSlotAngle(double angleDeg, int count, std::uint64_t slot) {
  const bool full = std::abs(angleDeg - 360.0) < 1e-9;
  return angleDeg / static_cast<double>(full ? count : count - 1) *
         static_cast<double>(slot);
}

gp_Pnt rotatedPoint(const gp_Pnt& point, solidar::PrincipalAxis axis,
                    double angleDeg) {
  const double radians = angleDeg * std::numbers::pi / 180.0;
  const double cosine = std::cos(radians);
  const double sine = std::sin(radians);
  if (axis == solidar::PrincipalAxis::X)
    return {point.X(), point.Y() * cosine - point.Z() * sine,
            point.Y() * sine + point.Z() * cosine};
  if (axis == solidar::PrincipalAxis::Y)
    return {point.X() * cosine + point.Z() * sine, point.Y(),
            -point.X() * sine + point.Z() * cosine};
  return {point.X() * cosine - point.Y() * sine,
          point.X() * sine + point.Y() * cosine, point.Z()};
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
        checkExactCarrier(
            *model.feature, startLines[index], {start, sides[index]},
            {GeomAbs_Line,
             std::hypot(endpoints[1].first - endpoints[0].first,
                        endpoints[1].second - endpoints[0].second),
             revolvedPoint(endpoints[0].first, endpoints[0].second, 0.0),
             revolvedPoint(endpoints[1].first, endpoints[1].second, 0.0)});
        checkExactCarrier(
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
        checkExactCarrier(
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
      checkExactCarrier(
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
    checkExactCarrier(*model.feature, axisCarrier, {start, end},
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
    checkExactCarrier(*model.feature, axisCarrier, {start, end},
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
    checkExactCarrier(
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
    checkExactCarrier(
        *model.feature, rim, {outerEnd, outerCylinder},
        {GeomAbs_Circle, 15.0 * 210.0 * std::numbers::pi / 180.0,
         revolvedPoint(30.0, 15.0, 0.0),
         revolvedPoint(30.0, 15.0, 210.0)});
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) <
          volumeOf(*model.feature->shape()) - 1e-5);
  }

  // A strict blind circular Pocket publishes every closed two-face manifold
  // edge except the cylindrical seam. Three carriers are chosen solely by
  // their geometry and then retained across depth and GeometryId-preserving
  // profile edits.
  {
    auto model = makePocketedBox();
    constexpr double kTop = 20.0;
    double centerX = 15.0;
    double centerY = 14.0;
    double radius = 4.0;
    double depth = 6.0;

    const auto outerVertical = edgeForExactEndpoints(
        *model.pocket, model.body->id(), gp_Pnt(0.0, 0.0, 0.0),
        gp_Pnt(0.0, 0.0, kTop));
    const auto topRim = edgeForCircle(
        *model.pocket, model.body->id(), gp_Pnt(centerX, centerY, kTop),
        radius);
    const auto floorRim = edgeForCircle(
        *model.pocket, model.body->id(),
        gp_Pnt(centerX, centerY, kTop - depth), radius);

    const auto outerXOrigin = faceOriginForBounds(
        *model.pocket, {0.0, 0.0, 0.0, 0.0, 30.0, kTop});
    const auto outerYOrigin = faceOriginForBounds(
        *model.pocket, {0.0, 0.0, 0.0, 40.0, 0.0, kTop});
    const auto topOrigin = faceOriginForBounds(
        *model.pocket, {0.0, 0.0, kTop, 40.0, 30.0, kTop});
    const auto wallOrigin = faceOriginForBounds(
        *model.pocket,
        {centerX - radius, centerY - radius, kTop - depth,
         centerX + radius, centerY + radius, kTop});
    const auto floorOrigin = faceOriginForBounds(
        *model.pocket,
        {centerX - radius, centerY - radius, kTop - depth,
         centerX + radius, centerY + radius, kTop - depth});

    const auto checkBlind = [&] {
      CHECK(faceOriginForBounds(
                *model.pocket, {0.0, 0.0, 0.0, 0.0, 30.0, kTop}) ==
            outerXOrigin);
      CHECK(faceOriginForBounds(
                *model.pocket, {0.0, 0.0, 0.0, 40.0, 0.0, kTop}) ==
            outerYOrigin);
      CHECK(faceOriginForBounds(
                *model.pocket, {0.0, 0.0, kTop, 40.0, 30.0, kTop}) ==
            topOrigin);
      CHECK(faceOriginForBounds(
                *model.pocket,
                {centerX - radius, centerY - radius, kTop - depth,
                 centerX + radius, centerY + radius, kTop}) == wallOrigin);
      CHECK(faceOriginForBounds(
                *model.pocket,
                {centerX - radius, centerY - radius, kTop - depth,
                 centerX + radius, centerY + radius, kTop - depth}) ==
            floorOrigin);
      checkExactCarrier(
          *model.pocket, outerVertical, {outerXOrigin, outerYOrigin},
          {GeomAbs_Line, kTop, gp_Pnt(0.0, 0.0, 0.0),
           gp_Pnt(0.0, 0.0, kTop)});
      checkCircleCarrier(*model.pocket, topRim, {topOrigin, wallOrigin},
                         gp_Pnt(centerX, centerY, kTop), radius);
      checkCircleCarrier(*model.pocket, floorRim,
                         {floorOrigin, wallOrigin},
                         gp_Pnt(centerX, centerY, kTop - depth), radius);
      CHECK(near(volumeOf(*model.pocket->shape()),
                 40.0 * 30.0 * kTop -
                     std::numbers::pi * radius * radius * depth,
                 1e-3));
    };

    checkBlind();
    auditBlindPocketEdges(*model.pocket, model.body->id());

    centerX = 18.0;
    centerY = 12.0;
    radius = 5.0;
    depth = 9.0;
    model.pocket->setDepthMm(depth);
    sketch::Sketch changedCircle;
    changedCircle.addCircle({centerX, centerY}, radius);
    CHECK(changedCircle.restoreGeometryIds({}, {model.circleId}, {}, {}));
    CHECK(model.document.replaceSketchGeometry(model.pocketSketchId,
                                               std::move(changedCircle)));
    CHECK(model.document.recompute());
    checkBlind();
    auditBlindPocketEdges(*model.pocket, model.body->id());

    // Once the cut exits the bottom, the blind-floor provenance disappears.
    // The geometrically selected exit rim is independently V2-authorable.
    model.pocket->setDepthMm(30.0);
    CHECK(model.document.recompute());
    checkEdgeMissing(*model.pocket, floorRim);
    checkExactCarrier(
        *model.pocket, outerVertical, {outerXOrigin, outerYOrigin},
        {GeomAbs_Line, kTop, gp_Pnt(0.0, 0.0, 0.0),
         gp_Pnt(0.0, 0.0, kTop)});
    const auto throughWallOrigin = faceOriginForBounds(
        *model.pocket,
        {centerX - radius, centerY - radius, 0.0,
         centerX + radius, centerY + radius, kTop});
    const auto bottomOrigin = faceOriginForBounds(
        *model.pocket, {0.0, 0.0, 0.0, 40.0, 30.0, 0.0});
    const auto exitRim = edgeForCircle(
        *model.pocket, model.body->id(), gp_Pnt(centerX, centerY, 0.0),
        radius);
    checkCircleCarrier(*model.pocket, exitRim,
                       {bottomOrigin, throughWallOrigin},
                       gp_Pnt(centerX, centerY, 0.0), radius);

    depth = 7.0;
    model.pocket->setDepthMm(depth);
    CHECK(model.document.recompute());
    checkBlind();

    const auto validShape = model.pocket->shape();
    const auto validLineage = model.pocket->topologyLineage();
    const auto validIndex = model.pocket->topologyIndex();
    const auto validRevision = model.pocket->shapeRevision();
    model.pocket->setDepthMm(0.0);
    CHECK(!model.document.recompute());
    CHECK(!model.pocket->shape());
    CHECK(!model.pocket->topologyLineage());
    CHECK(model.pocket->shapeRevision() == validRevision);
    std::string error;
    CHECK(!model.pocket->topologyIndex(&error));
    CHECK(!error.empty());
    CHECK(model.pocket->lastValidShape() == validShape);
    CHECK(model.pocket->lastValidTopologyLineage() == validLineage);
    CHECK(model.pocket->lastValidTopologyIndex(&error) == validIndex);
    CHECK(error.empty());
    CHECK(validIndex->resolveEdge(outerVertical));
    CHECK(validIndex->resolveEdge(topRim));
    CHECK(validIndex->resolveEdge(floorRim));

    depth = 8.0;
    model.pocket->setDepthMm(depth);
    CHECK(model.document.recompute());
    checkBlind();
  }

  // A fixed convex outer Pocket edge feeds Fillet without probing alternate
  // edges. The exact carrier and strict material removal survive a Pocket
  // parameter/profile edit.
  {
    auto model = makePocketedBox();
    const auto outer = edgeForExactEndpoints(
        *model.pocket, model.body->id(), gp_Pnt(0.0, 0.0, 0.0),
        gp_Pnt(0.0, 0.0, 20.0));
    const auto outerX = faceOriginForBounds(
        *model.pocket, {0.0, 0.0, 0.0, 0.0, 30.0, 20.0});
    const auto outerY = faceOriginForBounds(
        *model.pocket, {0.0, 0.0, 0.0, 40.0, 0.0, 20.0});
    checkExactCarrier(
        *model.pocket, outer, {outerX, outerY},
        {GeomAbs_Line, 20.0, gp_Pnt(0.0, 0.0, 0.0),
         gp_Pnt(0.0, 0.0, 20.0)});
    auto fillet = std::make_unique<FilletFeature>(outer, 1.0);
    auto* filletFeature = fillet.get();
    model.body->addFeature(std::move(fillet));
    CHECK(model.document.recompute());
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) <
          volumeOf(*model.pocket->shape()) - 1e-5);

    model.pocket->setDepthMm(9.0);
    sketch::Sketch changedCircle;
    changedCircle.addCircle({18.0, 12.0}, 5.0);
    CHECK(changedCircle.restoreGeometryIds({}, {model.circleId}, {}, {}));
    CHECK(model.document.replaceSketchGeometry(model.pocketSketchId,
                                               std::move(changedCircle)));
    CHECK(model.document.recompute());
    checkExactCarrier(
        *model.pocket, outer, {outerX, outerY},
        {GeomAbs_Line, 20.0, gp_Pnt(0.0, 0.0, 0.0),
         gp_Pnt(0.0, 0.0, 20.0)});
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) <
          volumeOf(*model.pocket->shape()) - 1e-5);
  }

  // A strict both-lineaged Join reduces two overlapping rectangle extrusions
  // to one rectangular prism. Saved carriers are selected geometrically from
  // the Join result and retain Join-owned adjacency identity across source
  // edits and operand order.
  {
    auto model = makeJoinedBoxes();
    double minimumX = 0.0;
    double maximumX = 30.0;
    double height = 10.0;
    const auto mergedBottomFront = edgeForExactEndpoints(
        *model.join, model.resultBody->id(), gp_Pnt(minimumX, 0.0, 0.0),
        gp_Pnt(maximumX, 0.0, 0.0));
    const auto firstLeft = edgeForExactEndpoints(
        *model.join, model.resultBody->id(), gp_Pnt(minimumX, 0.0, 0.0),
        gp_Pnt(minimumX, 0.0, height));
    const auto secondRight = edgeForExactEndpoints(
        *model.join, model.resultBody->id(), gp_Pnt(maximumX, 0.0, 0.0),
        gp_Pnt(maximumX, 0.0, height));

    const auto bottomOrigin = faceOriginForBounds(
        *model.join, {minimumX, 0.0, 0.0, maximumX, 10.0, 0.0});
    const auto frontOrigin = faceOriginForBounds(
        *model.join, {minimumX, 0.0, 0.0, maximumX, 0.0, height});
    const auto firstLeftOrigin = faceOriginForBounds(
        *model.join, {minimumX, 0.0, 0.0, minimumX, 10.0, height});
    const auto secondRightOrigin = faceOriginForBounds(
        *model.join, {maximumX, 0.0, 0.0, maximumX, 10.0, height});

    const auto checkJoined = [&] {
      CHECK(faceOriginForBounds(
                *model.join,
                {minimumX, 0.0, 0.0, maximumX, 10.0, 0.0}) ==
            bottomOrigin);
      CHECK(faceOriginForBounds(
                *model.join,
                {minimumX, 0.0, 0.0, maximumX, 0.0, height}) ==
            frontOrigin);
      CHECK(faceOriginForBounds(
                *model.join,
                {minimumX, 0.0, 0.0, minimumX, 10.0, height}) ==
            firstLeftOrigin);
      CHECK(faceOriginForBounds(
                *model.join,
                {maximumX, 0.0, 0.0, maximumX, 10.0, height}) ==
            secondRightOrigin);
      checkExactCarrier(
          *model.join, mergedBottomFront, {bottomOrigin, frontOrigin},
          {GeomAbs_Line, maximumX - minimumX,
           gp_Pnt(minimumX, 0.0, 0.0), gp_Pnt(maximumX, 0.0, 0.0)});
      checkExactCarrier(
          *model.join, firstLeft, {firstLeftOrigin, frontOrigin},
          {GeomAbs_Line, height, gp_Pnt(minimumX, 0.0, 0.0),
           gp_Pnt(minimumX, 0.0, height)});
      checkExactCarrier(
          *model.join, secondRight, {secondRightOrigin, frontOrigin},
          {GeomAbs_Line, height, gp_Pnt(maximumX, 0.0, 0.0),
           gp_Pnt(maximumX, 0.0, height)});
      CHECK(near(volumeOf(*model.join->shape()),
                 (maximumX - minimumX) * 10.0 * height, 1e-3));
    };

    checkJoined();
    checkAllCanonicalEdgesCreated(*model.join, model.resultBody->id(), 12);
    auditJoinedBoxEdges(*model.join, model.resultBody->id());
    const auto outputEdgeKeys = edgeKeys(*model.join->topologyLineage());
    const auto firstSourceEdgeKeys = edgeKeys(*model.first->topologyLineage());
    const auto secondSourceEdgeKeys =
        edgeKeys(*model.second->topologyLineage());
    CHECK(outputEdgeKeys.size() == 12);
    CHECK(!firstSourceEdgeKeys.empty());
    CHECK(!secondSourceEdgeKeys.empty());
    CHECK(firstSourceEdgeKeys.size() == 12);
    CHECK(secondSourceEdgeKeys.size() == 12);
    for (const auto& key : outputEdgeKeys) {
      CHECK(key.featureId == model.join->id());
      CHECK(std::find(firstSourceEdgeKeys.begin(), firstSourceEdgeKeys.end(),
                      key) == firstSourceEdgeKeys.end());
      CHECK(std::find(secondSourceEdgeKeys.begin(), secondSourceEdgeKeys.end(),
                      key) == secondSourceEdgeKeys.end());
    }

    model.first->setLengthMm(14.0);
    CHECK(model.document.recompute());
    checkExactCarrier(
        *model.join, mergedBottomFront, {bottomOrigin, frontOrigin},
        {GeomAbs_Line, 30.0, gp_Pnt(0.0, 0.0, 0.0),
         gp_Pnt(30.0, 0.0, 0.0)});
    checkExactCarrier(
        *model.join, firstLeft, {firstLeftOrigin, frontOrigin},
        {GeomAbs_Line, 14.0, gp_Pnt(0.0, 0.0, 0.0),
         gp_Pnt(0.0, 0.0, 14.0)});
    checkExactCarrier(
        *model.join, secondRight, {secondRightOrigin, frontOrigin},
        {GeomAbs_Line, 10.0, gp_Pnt(30.0, 0.0, 0.0),
         gp_Pnt(30.0, 0.0, 10.0)});

    height = 14.0;
    model.second->setLengthMm(height);
    CHECK(model.document.recompute());
    checkJoined();

    maximumX = 32.0;
    replaceJoinedRectangle(model, true, 12.0, maximumX);
    CHECK(model.document.recompute());
    checkJoined();
    checkAllCanonicalEdgesCreated(*model.join, model.resultBody->id(), 12);

    model.join->setInputs(model.secondBodyId, model.second->id(),
                          model.firstBodyId, model.first->id());
    CHECK(model.document.recompute());
    checkJoined();

    // Moving B to the other side makes the saved A-left and B-right roles
    // internal. The merged edge keeps its face-pair identity while the new
    // exterior side edges are V2-authorable by their geometry.
    minimumX = -12.0;
    maximumX = 20.0;
    replaceJoinedRectangle(model, true, minimumX, 8.0);
    CHECK(model.document.recompute());
    checkEdgeMissing(*model.join, firstLeft);
    checkEdgeMissing(*model.join, secondRight);
    checkExactCarrier(
        *model.join, mergedBottomFront, {bottomOrigin, frontOrigin},
        {GeomAbs_Line, maximumX - minimumX,
         gp_Pnt(minimumX, 0.0, 0.0), gp_Pnt(maximumX, 0.0, 0.0)});
    const auto newLeftOrigin = faceOriginForBounds(
        *model.join, {minimumX, 0.0, 0.0, minimumX, 10.0, height});
    const auto newRightOrigin = faceOriginForBounds(
        *model.join, {maximumX, 0.0, 0.0, maximumX, 10.0, height});
    const auto newLeft = edgeForExactEndpoints(
        *model.join, model.resultBody->id(), gp_Pnt(minimumX, 0.0, 0.0),
        gp_Pnt(minimumX, 0.0, height));
    const auto newRight = edgeForExactEndpoints(
        *model.join, model.resultBody->id(), gp_Pnt(maximumX, 0.0, 0.0),
        gp_Pnt(maximumX, 0.0, height));
    checkExactCarrier(
        *model.join, newLeft, {newLeftOrigin, frontOrigin},
        {GeomAbs_Line, height, gp_Pnt(minimumX, 0.0, 0.0),
         gp_Pnt(minimumX, 0.0, height)});
    checkExactCarrier(
        *model.join, newRight, {newRightOrigin, frontOrigin},
        {GeomAbs_Line, height, gp_Pnt(maximumX, 0.0, 0.0),
         gp_Pnt(maximumX, 0.0, height)});

    minimumX = 0.0;
    maximumX = 32.0;
    replaceJoinedRectangle(model, true, 12.0, maximumX);
    CHECK(model.document.recompute());
    checkJoined();

    const auto validShape = model.join->shape();
    const auto validLineage = model.join->topologyLineage();
    const auto validIndex = model.join->topologyIndex();
    const auto validRevision = model.join->shapeRevision();
    replaceJoinedRectangle(model, true, 40.0, 60.0);
    CHECK(!model.document.recompute());
    CHECK(!model.join->shape());
    CHECK(!model.join->topologyLineage());
    CHECK(model.join->shapeRevision() == validRevision);
    CHECK(model.join->lastValidShape() == validShape);
    CHECK(model.join->lastValidTopologyLineage() == validLineage);
    std::string error;
    CHECK(!model.join->topologyIndex(&error));
    CHECK(!error.empty());
    CHECK(model.join->lastValidTopologyIndex(&error) == validIndex);
    CHECK(error.empty());
    CHECK(validIndex->resolveEdge(mergedBottomFront));
    CHECK(validIndex->resolveEdge(firstLeft));
    CHECK(validIndex->resolveEdge(secondRight));

    replaceJoinedRectangle(model, true, 12.0, maximumX);
    CHECK(model.document.recompute());
    CHECK(model.join->shapeRevision() > validRevision);
    checkJoined();
  }

  // A fixed merged convex edge feeds Fillet without probing alternatives.
  // Both its exact Join identity and strict volume decrease survive a source
  // profile shift.
  {
    auto model = makeJoinedBoxes();
    const auto merged = edgeForExactEndpoints(
        *model.join, model.resultBody->id(), gp_Pnt(0.0, 0.0, 0.0),
        gp_Pnt(30.0, 0.0, 0.0));
    const auto bottom = faceOriginForBounds(
        *model.join, {0.0, 0.0, 0.0, 30.0, 10.0, 0.0});
    const auto front = faceOriginForBounds(
        *model.join, {0.0, 0.0, 0.0, 30.0, 0.0, 10.0});
    checkExactCarrier(
        *model.join, merged, {bottom, front},
        {GeomAbs_Line, 30.0, gp_Pnt(0.0, 0.0, 0.0),
         gp_Pnt(30.0, 0.0, 0.0)});
    auto fillet = std::make_unique<FilletFeature>(merged, 1.0);
    auto* filletFeature = fillet.get();
    model.resultBody->addFeature(std::move(fillet));
    CHECK(model.document.recompute());
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) <
          volumeOf(*model.join->shape()) - 1e-5);

    replaceJoinedRectangle(model, true, 12.0, 32.0);
    CHECK(model.document.recompute());
    checkExactCarrier(
        *model.join, merged, {bottom, front},
        {GeomAbs_Line, 32.0, gp_Pnt(0.0, 0.0, 0.0),
         gp_Pnt(32.0, 0.0, 0.0)});
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) <
          volumeOf(*model.join->shape()) - 1e-5);
  }

  // A strict source-lineaged Move owns a complete edge namespace derived
  // from its own exact face adjacency. The saved vertical carrier is selected
  // only by endpoints and survives both transform and source-profile edits.
  {
    auto model = makeMovedBox();
    double sourceX0 = 0.0;
    double sourceY0 = 0.0;
    double sourceX1 = 30.0;
    double sourceY1 = 20.0;
    double length = 15.0;
    solidar::Vector3d offset{5.0, 7.0, 3.0};

    const auto vertical = edgeForExactEndpoints(
        *model.move, model.body->id(),
        gp_Pnt(sourceX0 + offset.x, sourceY0 + offset.y, offset.z),
        gp_Pnt(sourceX0 + offset.x, sourceY0 + offset.y,
               offset.z + length));
    CHECK(vertical.v2());
    const auto verticalKey = vertical.v2()->provenance;
    const auto leftOrigin = faceOriginForBounds(
        *model.move,
        {sourceX0 + offset.x, sourceY0 + offset.y, offset.z,
         sourceX0 + offset.x, sourceY1 + offset.y, offset.z + length});
    const auto frontOrigin = faceOriginForBounds(
        *model.move,
        {sourceX0 + offset.x, sourceY0 + offset.y, offset.z,
         sourceX1 + offset.x, sourceY0 + offset.y, offset.z + length});
    CHECK(verticalKey.featureId == model.move->id());
    CHECK(verticalKey.kind == TopologyKind::Edge);
    CHECK(verticalKey.operation == TopologyProvenanceOperation::Generated);
    CHECK(verticalKey.origins.size() == 2);
    for (const auto& origin : verticalKey.origins) {
      CHECK(origin.featureId == model.move->id());
      CHECK(origin.kind == TopologyKind::Face);
      CHECK(!origin.occurrence);
    }

    const auto checkMoved = [&] {
      CHECK(faceOriginForBounds(
                *model.move,
                {sourceX0 + offset.x, sourceY0 + offset.y, offset.z,
                 sourceX0 + offset.x, sourceY1 + offset.y,
                 offset.z + length}) == leftOrigin);
      CHECK(faceOriginForBounds(
                *model.move,
                {sourceX0 + offset.x, sourceY0 + offset.y, offset.z,
                 sourceX1 + offset.x, sourceY0 + offset.y,
                 offset.z + length}) == frontOrigin);
      const gp_Pnt first(sourceX0 + offset.x, sourceY0 + offset.y,
                         offset.z);
      const gp_Pnt last(sourceX0 + offset.x, sourceY0 + offset.y,
                        offset.z + length);
      checkExactCarrier(*model.move, vertical, {leftOrigin, frontOrigin},
                        {GeomAbs_Line, length, first, last});
      const auto rediscovered = edgeForExactEndpoints(
          *model.move, model.body->id(), first, last);
      CHECK(rediscovered.v2());
      CHECK(rediscovered.v2()->provenance == verticalKey);
      const auto savedResolved = model.move->topologyIndex()->resolveEdge(
          vertical);
      const auto rediscoveredResolved =
          model.move->topologyIndex()->resolveEdge(rediscovered);
      CHECK(savedResolved);
      CHECK(rediscoveredResolved);
      CHECK(savedResolved.subshape->IsSame(*rediscoveredResolved.subshape));
    };

    checkMoved();
    checkAllCanonicalEdgesCreated(*model.move, model.body->id(), 12);
    const auto moveSnapshot =
        model.body->topologySnapshot(model.move->id());
    CHECK(moveSnapshot);
    CHECK(moveSnapshot->v2Authoritative());
    const auto sourceKeys = edgeKeys(*model.extrusion->topologyLineage());
    const auto movedKeys = edgeKeys(*model.move->topologyLineage());
    CHECK(sourceKeys.size() == 12);
    CHECK(movedKeys.size() == 12);
    for (const auto& key : movedKeys) {
      CHECK(key.featureId == model.move->id());
      CHECK(std::find(sourceKeys.begin(), sourceKeys.end(), key) ==
            sourceKeys.end());
    }

    offset = {8.0, -2.0, 4.0};
    model.move->setOffsetMm(offset);
    CHECK(model.document.recompute());
    checkMoved();

    length = 19.0;
    model.extrusion->setLengthMm(length);
    CHECK(model.document.recompute());
    checkMoved();

    sourceX0 = 2.0;
    sourceY0 = 3.0;
    sourceX1 = 36.0;
    sourceY1 = 25.0;
    sketch::Sketch reordered;
    reordered.addLine({sourceX0, sourceY1}, {sourceX1, sourceY1});
    reordered.addLine({sourceX1, sourceY0}, {sourceX0, sourceY0});
    reordered.addLine({sourceX0, sourceY0}, {sourceX0, sourceY1});
    reordered.addLine({sourceX1, sourceY1}, {sourceX1, sourceY0});
    CHECK(reordered.restoreGeometryIds(
        {model.geometryIds[2], model.geometryIds[0], model.geometryIds[3],
         model.geometryIds[1]},
        {}, {}, {}));
    CHECK(model.document.replaceSketchGeometry(model.sketchId,
                                               std::move(reordered)));
    CHECK(model.document.recompute());
    checkMoved();
    checkAllCanonicalEdgesCreated(*model.move, model.body->id(), 12);

    const auto validShape = model.move->shape();
    const auto validLineage = model.move->topologyLineage();
    const auto validIndex = model.move->topologyIndex();
    const auto validRevision = model.move->shapeRevision();
    const auto validResolved = validIndex->resolveEdge(vertical);
    CHECK(validResolved);
    model.move->setOffsetMm(
        {std::numeric_limits<double>::quiet_NaN(), offset.y, offset.z});
    CHECK(!model.document.recompute());
    CHECK(!model.move->shape());
    CHECK(!model.move->topologyLineage());
    CHECK(model.move->shapeRevision() == validRevision);
    std::string error;
    CHECK(!model.body->topologySnapshot(model.move->id(), &error));
    CHECK(!error.empty());
    error.clear();
    CHECK(!model.move->topologyIndex(&error));
    CHECK(!error.empty());
    CHECK(model.move->lastValidShape() == validShape);
    CHECK(model.move->lastValidTopologyLineage() == validLineage);
    CHECK(model.move->lastValidTopologyIndex(&error) == validIndex);
    CHECK(error.empty());
    const auto rolledBack = validIndex->resolveEdge(vertical);
    CHECK(rolledBack);
    CHECK(rolledBack.subshape->IsSame(*validResolved.subshape));

    model.move->setOffsetMm(offset);
    CHECK(model.document.recompute());
    CHECK(model.move->shapeRevision() > validRevision);
    CHECK(model.body->topologySnapshot(model.move->id()));
    checkMoved();
  }

  // A fixed convex Move edge is a strict V2 Fillet input. Translation and an
  // upstream length edit preserve its exact identity and the material effect.
  {
    auto model = makeMovedBox();
    const auto vertical = edgeForExactEndpoints(
        *model.move, model.body->id(), gp_Pnt(5.0, 7.0, 3.0),
        gp_Pnt(5.0, 7.0, 18.0));
    const auto left = faceOriginForBounds(
        *model.move, {5.0, 7.0, 3.0, 5.0, 27.0, 18.0});
    const auto front = faceOriginForBounds(
        *model.move, {5.0, 7.0, 3.0, 35.0, 7.0, 18.0});
    checkExactCarrier(
        *model.move, vertical, {left, front},
        {GeomAbs_Line, 15.0, gp_Pnt(5.0, 7.0, 3.0),
         gp_Pnt(5.0, 7.0, 18.0)});
    auto fillet = std::make_unique<FilletFeature>(vertical, 1.0);
    auto* filletFeature = fillet.get();
    model.body->addFeature(std::move(fillet));
    CHECK(model.document.recompute());
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) <
          volumeOf(*model.move->shape()) - 1e-5);

    model.move->setOffsetMm({-4.0, 6.0, 2.0});
    model.extrusion->setLengthMm(18.0);
    CHECK(model.document.recompute());
    checkExactCarrier(
        *model.move, vertical, {left, front},
        {GeomAbs_Line, 18.0, gp_Pnt(-4.0, 6.0, 2.0),
         gp_Pnt(-4.0, 6.0, 20.0)});
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) <
          volumeOf(*model.move->shape()) - 1e-5);
  }

  // The legacy no-lineage transform path remains shape-only. A disjoint
  // multi-region Extrude can be moved, but cannot author a V2 edge.
  {
    Document document;
    auto& profile = document.addSketch("Legacy multi-region move");
    profile.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    profile.geometry.addRectangle({20.0, 0.0}, {30.0, 10.0});
    auto& body = document.addBody();
    auto extrusion = std::make_unique<ExtrudeFeature>(profile.id, 8.0);
    auto* extrusionFeature = extrusion.get();
    const auto extrusionId = extrusion->id();
    body.addFeature(std::move(extrusion));
    auto move =
        std::make_unique<MoveFeature>(extrusionId, Vector3d{3.0, 4.0, 5.0});
    auto* moveFeature = move.get();
    body.addFeature(std::move(move));
    CHECK(document.recompute());
    CHECK(extrusionFeature->shape());
    CHECK(!extrusionFeature->topologyLineage());
    CHECK(moveFeature->shape());
    CHECK(!moveFeature->topologyLineage());
    const auto index = moveFeature->topologyIndex();
    CHECK(index);
    CHECK(index->rawEdgeCount() > 0);
    const auto exact = moveFeature->createEdgeReferenceV2(body.id(), 0);
    CHECK(!exact);
    CHECK(exact.status == EdgeReferenceV2CreationStatus::Missing);
  }

  // A strict Mirror publishes two disjoint, occurrence-scoped edge sets. The
  // corresponding original/reflected vertical carriers are selected only by
  // geometry and retain their keys across plane and source-profile edits.
  {
    auto model = makeMirroredBox();
    double sourceX0 = 10.0;
    double sourceY0 = 3.0;
    double sourceX1 = 20.0;
    double sourceY1 = 9.0;
    double length = 5.0;
    MirrorPlane plane = MirrorPlane::YZ;

    const auto sourceVertical = edgeForExactEndpoints(
        *model.extrusion, model.body->id(),
        gp_Pnt(sourceX0, sourceY0, 0.0),
        gp_Pnt(sourceX0, sourceY0, length));
    const auto original = edgeForExactEndpoints(
        *model.mirror, model.body->id(),
        gp_Pnt(sourceX0, sourceY0, 0.0),
        gp_Pnt(sourceX0, sourceY0, length));
    const auto reflected = edgeForExactEndpoints(
        *model.mirror, model.body->id(),
        gp_Pnt(-sourceX0, sourceY0, 0.0),
        gp_Pnt(-sourceX0, sourceY0, length));
    CHECK(sourceVertical.v2());
    CHECK(original.v2());
    CHECK(reflected.v2());
    const auto originalKey = original.v2()->provenance;
    const auto reflectedKey = reflected.v2()->provenance;

    const auto originalLeftOrigin = faceOriginForBounds(
        *model.mirror,
        {sourceX0, sourceY0, 0.0, sourceX0, sourceY1, length});
    const auto originalFrontOrigin = faceOriginForBounds(
        *model.mirror,
        {sourceX0, sourceY0, 0.0, sourceX1, sourceY0, length});
    const auto reflectedLeftOrigin = faceOriginForBounds(
        *model.mirror,
        {-sourceX0, sourceY0, 0.0, -sourceX0, sourceY1, length});
    const auto reflectedFrontOrigin = faceOriginForBounds(
        *model.mirror,
        {-sourceX1, sourceY0, 0.0, -sourceX0, sourceY0, length});

    const auto checkOccurrence = [&](const TopologyProvenanceKey& key,
                                     std::uint64_t slot) {
      CHECK(key.featureId == model.mirror->id());
      CHECK(key.kind == TopologyKind::Edge);
      CHECK(key.operation == TopologyProvenanceOperation::Generated);
      CHECK(!key.occurrence);
      CHECK(key.origins.size() == 2);
      for (const auto& origin : key.origins) {
        CHECK(origin.featureId == model.mirror->id());
        CHECK(origin.kind == TopologyKind::Face);
        CHECK(origin.occurrence);
        CHECK(origin.occurrence->featureId == model.mirror->id());
        CHECK(origin.occurrence->slot == slot);
      }
    };
    checkOccurrence(originalKey, 0);
    checkOccurrence(reflectedKey, 1);

    const auto checkMirrored = [&] {
      CHECK(faceOriginForBounds(
                *model.mirror,
                {sourceX0, sourceY0, 0.0, sourceX0, sourceY1, length}) ==
            originalLeftOrigin);
      CHECK(faceOriginForBounds(
                *model.mirror,
                {sourceX0, sourceY0, 0.0, sourceX1, sourceY0, length}) ==
            originalFrontOrigin);

      gp_Pnt reflectedFirst;
      gp_Pnt reflectedLast;
      if (plane == MirrorPlane::YZ) {
        reflectedFirst = gp_Pnt(-sourceX0, sourceY0, 0.0);
        reflectedLast = gp_Pnt(-sourceX0, sourceY0, length);
        CHECK(faceOriginForBounds(
                  *model.mirror,
                  {-sourceX0, sourceY0, 0.0, -sourceX0, sourceY1,
                   length}) == reflectedLeftOrigin);
        CHECK(faceOriginForBounds(
                  *model.mirror,
                  {-sourceX1, sourceY0, 0.0, -sourceX0, sourceY0,
                   length}) == reflectedFrontOrigin);
      } else {
        CHECK(plane == MirrorPlane::XZ);
        reflectedFirst = gp_Pnt(sourceX0, -sourceY0, 0.0);
        reflectedLast = gp_Pnt(sourceX0, -sourceY0, length);
        CHECK(faceOriginForBounds(
                  *model.mirror,
                  {sourceX0, -sourceY1, 0.0, sourceX0, -sourceY0,
                   length}) == reflectedLeftOrigin);
        CHECK(faceOriginForBounds(
                  *model.mirror,
                  {sourceX0, -sourceY0, 0.0, sourceX1, -sourceY0,
                   length}) == reflectedFrontOrigin);
      }

      const gp_Pnt originalFirst(sourceX0, sourceY0, 0.0);
      const gp_Pnt originalLast(sourceX0, sourceY0, length);
      checkExactCarrier(
          *model.mirror, original,
          {originalLeftOrigin, originalFrontOrigin},
          {GeomAbs_Line, length, originalFirst, originalLast});
      checkExactCarrier(
          *model.mirror, reflected,
          {reflectedLeftOrigin, reflectedFrontOrigin},
          {GeomAbs_Line, length, reflectedFirst, reflectedLast});

      const auto rediscoveredOriginal = edgeForExactEndpoints(
          *model.mirror, model.body->id(), originalFirst, originalLast);
      const auto rediscoveredReflected = edgeForExactEndpoints(
          *model.mirror, model.body->id(), reflectedFirst, reflectedLast);
      CHECK(rediscoveredOriginal.v2());
      CHECK(rediscoveredReflected.v2());
      CHECK(rediscoveredOriginal.v2()->provenance == originalKey);
      CHECK(rediscoveredReflected.v2()->provenance == reflectedKey);

      const auto sourceResolved =
          model.extrusion->topologyIndex()->resolveEdge(sourceVertical);
      const auto originalResolved =
          model.mirror->topologyIndex()->resolveEdge(original);
      const auto reflectedResolved =
          model.mirror->topologyIndex()->resolveEdge(reflected);
      CHECK(sourceResolved);
      CHECK(originalResolved);
      CHECK(reflectedResolved);
      CHECK(originalResolved.subshape->IsSame(*sourceResolved.subshape));
      CHECK(!reflectedResolved.subshape->IsSame(*sourceResolved.subshape));
    };

    checkMirrored();
    auditMirroredBoxEdges(*model.mirror, model.body->id());
    const auto sourceKeys = edgeKeys(*model.extrusion->topologyLineage());
    const auto mirrorKeys = edgeKeys(*model.mirror->topologyLineage());
    CHECK(sourceKeys.size() == 12);
    CHECK(mirrorKeys.size() == 24);
    for (const auto& key : mirrorKeys) {
      CHECK(key.featureId == model.mirror->id());
      CHECK(std::find(sourceKeys.begin(), sourceKeys.end(), key) ==
            sourceKeys.end());
    }

    plane = MirrorPlane::XZ;
    model.mirror->setPlane(plane);
    CHECK(model.document.recompute());
    checkMirrored();
    auditMirroredBoxEdges(*model.mirror, model.body->id());

    length = 8.0;
    model.extrusion->setLengthMm(length);
    CHECK(model.document.recompute());
    checkMirrored();
    auditMirroredBoxEdges(*model.mirror, model.body->id());

    sourceX0 = 12.0;
    sourceY0 = 4.0;
    sourceX1 = 24.0;
    sourceY1 = 11.0;
    sketch::Sketch reordered;
    reordered.addLine({sourceX0, sourceY1}, {sourceX1, sourceY1});
    reordered.addLine({sourceX1, sourceY0}, {sourceX0, sourceY0});
    reordered.addLine({sourceX0, sourceY0}, {sourceX0, sourceY1});
    reordered.addLine({sourceX1, sourceY1}, {sourceX1, sourceY0});
    CHECK(reordered.restoreGeometryIds(
        {model.geometryIds[2], model.geometryIds[0], model.geometryIds[3],
         model.geometryIds[1]},
        {}, {}, {}));
    CHECK(model.document.replaceSketchGeometry(model.sketchId,
                                               std::move(reordered)));
    CHECK(model.document.recompute());
    checkMirrored();
    auditMirroredBoxEdges(*model.mirror, model.body->id());

    const auto validShape = model.mirror->shape();
    const auto validLineage = model.mirror->topologyLineage();
    const auto validIndex = model.mirror->topologyIndex();
    const auto validRevision = model.mirror->shapeRevision();
    const auto validOriginal = validIndex->resolveEdge(original);
    const auto validReflected = validIndex->resolveEdge(reflected);
    CHECK(validOriginal);
    CHECK(validReflected);
    model.mirror->setPlane(static_cast<MirrorPlane>(255));
    CHECK(!model.document.recompute());
    CHECK(!model.mirror->shape());
    CHECK(!model.mirror->topologyLineage());
    CHECK(model.mirror->shapeRevision() == validRevision);
    std::string error;
    CHECK(!model.body->topologySnapshot(model.mirror->id(), &error));
    CHECK(!error.empty());
    error.clear();
    CHECK(!model.mirror->topologyIndex(&error));
    CHECK(!error.empty());
    CHECK(model.mirror->lastValidShape() == validShape);
    CHECK(model.mirror->lastValidTopologyLineage() == validLineage);
    CHECK(model.mirror->lastValidTopologyIndex(&error) == validIndex);
    CHECK(error.empty());
    const auto rolledBackOriginal = validIndex->resolveEdge(original);
    const auto rolledBackReflected = validIndex->resolveEdge(reflected);
    CHECK(rolledBackOriginal);
    CHECK(rolledBackReflected);
    CHECK(rolledBackOriginal.subshape->IsSame(*validOriginal.subshape));
    CHECK(rolledBackReflected.subshape->IsSame(*validReflected.subshape));

    model.mirror->setPlane(plane);
    CHECK(model.document.recompute());
    CHECK(model.mirror->shapeRevision() > validRevision);
    CHECK(model.body->topologySnapshot(model.mirror->id()));
    checkMirrored();
    auditMirroredBoxEdges(*model.mirror, model.body->id());
  }

  // A fixed reflected Mirror edge feeds Fillet without probing. Its exact
  // slot-1 identity and strict material effect survive a plane/length edit.
  {
    auto model = makeMirroredBox();
    const auto reflected = edgeForExactEndpoints(
        *model.mirror, model.body->id(), gp_Pnt(-10.0, 3.0, 0.0),
        gp_Pnt(-10.0, 3.0, 5.0));
    const auto left = faceOriginForBounds(
        *model.mirror, {-10.0, 3.0, 0.0, -10.0, 9.0, 5.0});
    const auto front = faceOriginForBounds(
        *model.mirror, {-20.0, 3.0, 0.0, -10.0, 3.0, 5.0});
    checkExactCarrier(
        *model.mirror, reflected, {left, front},
        {GeomAbs_Line, 5.0, gp_Pnt(-10.0, 3.0, 0.0),
         gp_Pnt(-10.0, 3.0, 5.0)});
    auto fillet = std::make_unique<FilletFeature>(reflected, 0.75);
    auto* filletFeature = fillet.get();
    model.body->addFeature(std::move(fillet));
    CHECK(model.document.recompute());
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) <
          volumeOf(*model.mirror->shape()) - 1e-5);

    model.mirror->setPlane(MirrorPlane::XZ);
    model.extrusion->setLengthMm(8.0);
    CHECK(model.document.recompute());
    checkExactCarrier(
        *model.mirror, reflected, {left, front},
        {GeomAbs_Line, 8.0, gp_Pnt(10.0, -3.0, 0.0),
         gp_Pnt(10.0, -3.0, 8.0)});
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) <
          volumeOf(*model.mirror->shape()) - 1e-5);
  }

  // The legacy no-lineage Mirror path remains shape-only. Its multi-solid
  // compound can be mirrored but cannot author an exact edge reference.
  {
    Document document;
    auto& profile = document.addSketch("Legacy multi-region mirror");
    profile.geometry.addRectangle({10.0, 3.0}, {20.0, 9.0});
    profile.geometry.addRectangle({30.0, 3.0}, {40.0, 9.0});
    auto& body = document.addBody();
    auto extrusion = std::make_unique<ExtrudeFeature>(profile.id, 5.0);
    auto* extrusionFeature = extrusion.get();
    const auto extrusionId = extrusion->id();
    body.addFeature(std::move(extrusion));
    auto mirror =
        std::make_unique<MirrorFeature>(extrusionId, MirrorPlane::YZ);
    auto* mirrorFeature = mirror.get();
    body.addFeature(std::move(mirror));
    CHECK(document.recompute());
    CHECK(extrusionFeature->shape());
    CHECK(!extrusionFeature->topologyLineage());
    CHECK(mirrorFeature->shape());
    CHECK(!mirrorFeature->topologyLineage());
    const auto index = mirrorFeature->topologyIndex();
    CHECK(index);
    CHECK(index->rawEdgeCount() > 0);
    const auto exact = mirrorFeature->createEdgeReferenceV2(body.id(), 0);
    CHECK(!exact);
    CHECK(exact.status == EdgeReferenceV2CreationStatus::Missing);
  }

  // Join Linear Pattern publishes one exact edge set per authored occurrence.
  // Saved slot-1 and slot-4 carriers are selected only by geometry and retain
  // their keys through spacing/count/source edits and slot disappearance.
  {
    auto model = makeLinearPatternedBox();
    double sourceX0 = 0.0;
    double sourceY0 = 0.0;
    double sourceX1 = 8.0;
    double sourceY1 = 6.0;
    double length = 5.0;
    double spacing = 12.0;

    const auto sourceVertical = edgeForExactEndpoints(
        *model.extrusion, model.sourceBody->id(),
        gp_Pnt(sourceX0, sourceY0, 0.0),
        gp_Pnt(sourceX0, sourceY0, length));
    const auto slot0 = edgeForExactEndpoints(
        *model.pattern, model.patternBody->id(),
        gp_Pnt(sourceX0, sourceY0, 0.0),
        gp_Pnt(sourceX0, sourceY0, length));
    const auto slot1 = edgeForExactEndpoints(
        *model.pattern, model.patternBody->id(),
        gp_Pnt(sourceX0 + spacing, sourceY0, 0.0),
        gp_Pnt(sourceX0 + spacing, sourceY0, length));
    const auto slot4 = edgeForExactEndpoints(
        *model.pattern, model.patternBody->id(),
        gp_Pnt(sourceX0 + 4.0 * spacing, sourceY0, 0.0),
        gp_Pnt(sourceX0 + 4.0 * spacing, sourceY0, length));
    CHECK(sourceVertical.v2());
    CHECK(slot0.v2());
    CHECK(slot1.v2());
    CHECK(slot4.v2());
    const auto slot1Key = slot1.v2()->provenance;
    const auto slot4Key = slot4.v2()->provenance;
    const auto slot1LeftOrigin = faceOriginForBounds(
        *model.pattern,
        {sourceX0 + spacing, sourceY0, 0.0, sourceX0 + spacing,
         sourceY1, length});
    const auto slot1FrontOrigin = faceOriginForBounds(
        *model.pattern,
        {sourceX0 + spacing, sourceY0, 0.0, sourceX1 + spacing,
         sourceY0, length});
    const auto slot4LeftOrigin = faceOriginForBounds(
        *model.pattern,
        {sourceX0 + 4.0 * spacing, sourceY0, 0.0,
         sourceX0 + 4.0 * spacing, sourceY1, length});
    const auto slot4FrontOrigin = faceOriginForBounds(
        *model.pattern,
        {sourceX0 + 4.0 * spacing, sourceY0, 0.0,
         sourceX1 + 4.0 * spacing, sourceY0, length});

    const auto checkOccurrence = [&](const TopologyProvenanceKey& key,
                                     std::uint64_t expectedSlot) {
      CHECK(key.featureId == model.pattern->id());
      CHECK(key.kind == TopologyKind::Edge);
      CHECK(key.operation == TopologyProvenanceOperation::Generated);
      CHECK(!key.occurrence);
      CHECK(key.origins.size() == 2);
      for (const auto& origin : key.origins) {
        CHECK(origin.featureId == model.pattern->id());
        CHECK(origin.kind == TopologyKind::Face);
        CHECK(origin.occurrence);
        CHECK(origin.occurrence->featureId == model.pattern->id());
        CHECK(origin.occurrence->slot == expectedSlot);
      }
    };
    checkOccurrence(slot1Key, 1);
    checkOccurrence(slot4Key, 4);

    const auto checkSlot = [&](const EdgeReference& carrier,
                               const TopologyProvenanceKey& savedKey,
                               std::uint64_t slot,
                               const std::vector<TopologyProvenanceOrigin>&
                                   expectedOrigins) {
      const double translatedX =
          sourceX0 + spacing * static_cast<double>(slot);
      const gp_Pnt first(translatedX, sourceY0, 0.0);
      const gp_Pnt last(translatedX, sourceY0, length);
      checkExactCarrier(*model.pattern, carrier, expectedOrigins,
                        {GeomAbs_Line, length, first, last});
      const auto rediscovered = edgeForExactEndpoints(
          *model.pattern, model.patternBody->id(), first, last);
      CHECK(rediscovered.v2());
      CHECK(rediscovered.v2()->provenance == savedKey);
    };
    const auto checkBothSavedSlots = [&] {
      CHECK(faceOriginForBounds(
                *model.pattern,
                {sourceX0 + spacing, sourceY0, 0.0, sourceX0 + spacing,
                 sourceY1, length}) == slot1LeftOrigin);
      CHECK(faceOriginForBounds(
                *model.pattern,
                {sourceX0 + spacing, sourceY0, 0.0, sourceX1 + spacing,
                 sourceY0, length}) == slot1FrontOrigin);
      CHECK(faceOriginForBounds(
                *model.pattern,
                {sourceX0 + 4.0 * spacing, sourceY0, 0.0,
                 sourceX0 + 4.0 * spacing, sourceY1, length}) ==
            slot4LeftOrigin);
      CHECK(faceOriginForBounds(
                *model.pattern,
                {sourceX0 + 4.0 * spacing, sourceY0, 0.0,
                 sourceX1 + 4.0 * spacing, sourceY0, length}) ==
            slot4FrontOrigin);
      checkSlot(slot1, slot1Key, 1,
                {slot1LeftOrigin, slot1FrontOrigin});
      checkSlot(slot4, slot4Key, 4,
                {slot4LeftOrigin, slot4FrontOrigin});
    };

    checkBothSavedSlots();
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                            {0, 1, 2, 3, 4});
    const auto sourceKeys = edgeKeys(*model.extrusion->topologyLineage());
    const auto patternKeys = edgeKeys(*model.pattern->topologyLineage());
    CHECK(sourceKeys.size() == 12);
    CHECK(patternKeys.size() == 60);
    for (const auto& key : patternKeys) {
      CHECK(key.featureId == model.pattern->id());
      CHECK(std::find(sourceKeys.begin(), sourceKeys.end(), key) ==
            sourceKeys.end());
    }
    const auto sourceResolved =
        model.extrusion->topologyIndex()->resolveEdge(sourceVertical);
    const auto slot0Resolved = model.pattern->topologyIndex()->resolveEdge(slot0);
    const auto slot1Resolved = model.pattern->topologyIndex()->resolveEdge(slot1);
    const auto slot4Resolved = model.pattern->topologyIndex()->resolveEdge(slot4);
    CHECK(sourceResolved);
    CHECK(slot0Resolved);
    CHECK(slot1Resolved);
    CHECK(slot4Resolved);
    CHECK(slot0Resolved.subshape->IsSame(*sourceResolved.subshape));
    CHECK(!slot1Resolved.subshape->IsSame(*sourceResolved.subshape));
    CHECK(!slot4Resolved.subshape->IsSame(*sourceResolved.subshape));
    CHECK(!slot1Resolved.subshape->IsSame(*slot4Resolved.subshape));

    spacing = 15.0;
    model.pattern->setSpacingMm(spacing);
    CHECK(model.document.recompute());
    checkBothSavedSlots();
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                            {0, 1, 2, 3, 4});

    model.pattern->setCount(3);
    CHECK(model.document.recompute());
    checkSlot(slot1, slot1Key, 1,
              {slot1LeftOrigin, slot1FrontOrigin});
    const auto missingSlot4 =
        model.pattern->topologyIndex()->resolveEdge(slot4);
    CHECK(!missingSlot4);
    CHECK(missingSlot4.failure == TopologyResolutionFailure::Missing);
    CHECK(missingSlot4.method == TopologyMatchMethod::None);
    CHECK(!missingSlot4.subshape);
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                            {0, 1, 2});

    model.pattern->setCount(5);
    CHECK(model.document.recompute());
    checkBothSavedSlots();
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                            {0, 1, 2, 3, 4});

    length = 8.0;
    model.extrusion->setLengthMm(length);
    CHECK(model.document.recompute());
    checkBothSavedSlots();
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                            {0, 1, 2, 3, 4});

    sourceX0 = 2.0;
    sourceY0 = 3.0;
    sourceX1 = 11.0;
    sourceY1 = 10.0;
    sketch::Sketch reordered;
    reordered.addLine({sourceX0, sourceY1}, {sourceX1, sourceY1});
    reordered.addLine({sourceX1, sourceY0}, {sourceX0, sourceY0});
    reordered.addLine({sourceX0, sourceY0}, {sourceX0, sourceY1});
    reordered.addLine({sourceX1, sourceY1}, {sourceX1, sourceY0});
    CHECK(reordered.restoreGeometryIds(
        {model.geometryIds[2], model.geometryIds[0], model.geometryIds[3],
         model.geometryIds[1]},
        {}, {}, {}));
    CHECK(model.document.replaceSketchGeometry(model.sketchId,
                                               std::move(reordered)));
    CHECK(model.document.recompute());
    checkBothSavedSlots();
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                            {0, 1, 2, 3, 4});

    const auto validShape = model.pattern->shape();
    const auto validLineage = model.pattern->topologyLineage();
    const auto validIndex = model.pattern->topologyIndex();
    const auto validRevision = model.pattern->shapeRevision();
    const auto validSlot1 = validIndex->resolveEdge(slot1);
    const auto validSlot4 = validIndex->resolveEdge(slot4);
    CHECK(validSlot1);
    CHECK(validSlot4);
    model.pattern->setSpacingMm(
        std::numeric_limits<double>::quiet_NaN());
    CHECK(!model.document.recompute());
    CHECK(!model.pattern->shape());
    CHECK(!model.pattern->topologyLineage());
    CHECK(model.pattern->shapeRevision() == validRevision);
    std::string error;
    CHECK(!model.patternBody->topologySnapshot(model.pattern->id(), &error));
    CHECK(!error.empty());
    error.clear();
    CHECK(!model.pattern->topologyIndex(&error));
    CHECK(!error.empty());
    CHECK(model.pattern->lastValidShape() == validShape);
    CHECK(model.pattern->lastValidTopologyLineage() == validLineage);
    CHECK(model.pattern->lastValidTopologyIndex(&error) == validIndex);
    CHECK(error.empty());
    const auto rolledBackSlot1 = validIndex->resolveEdge(slot1);
    const auto rolledBackSlot4 = validIndex->resolveEdge(slot4);
    CHECK(rolledBackSlot1);
    CHECK(rolledBackSlot4);
    CHECK(rolledBackSlot1.subshape->IsSame(*validSlot1.subshape));
    CHECK(rolledBackSlot4.subshape->IsSame(*validSlot4.subshape));

    model.pattern->setSpacingMm(spacing);
    CHECK(model.document.recompute());
    CHECK(model.pattern->shapeRevision() > validRevision);
    checkBothSavedSlots();
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                            {0, 1, 2, 3, 4});
  }

  // A fixed slot-1 edge feeds Fillet without probing. Count, spacing and
  // upstream length edits preserve the exact carrier and material removal.
  {
    auto model = makeLinearPatternedBox();
    double spacing = 12.0;
    double length = 5.0;
    const auto slot1 = edgeForExactEndpoints(
        *model.pattern, model.patternBody->id(),
        gp_Pnt(spacing, 0.0, 0.0), gp_Pnt(spacing, 0.0, length));
    const auto left = faceOriginForBounds(
        *model.pattern, {spacing, 0.0, 0.0, spacing, 6.0, length});
    const auto front = faceOriginForBounds(
        *model.pattern, {spacing, 0.0, 0.0, spacing + 8.0, 0.0, length});
    checkExactCarrier(
        *model.pattern, slot1, {left, front},
        {GeomAbs_Line, length, gp_Pnt(spacing, 0.0, 0.0),
         gp_Pnt(spacing, 0.0, length)});
    auto fillet = std::make_unique<FilletFeature>(slot1, 0.75);
    auto* filletFeature = fillet.get();
    model.patternBody->addFeature(std::move(fillet));
    CHECK(model.document.recompute());
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) <
          volumeOf(*model.pattern->shape()) - 1e-5);

    spacing = 15.0;
    length = 8.0;
    model.pattern->setCount(3);
    model.pattern->setSpacingMm(spacing);
    model.extrusion->setLengthMm(length);
    CHECK(model.document.recompute());
    checkExactCarrier(
        *model.pattern, slot1, {left, front},
        {GeomAbs_Line, length, gp_Pnt(spacing, 0.0, 0.0),
         gp_Pnt(spacing, 0.0, length)});
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) <
          volumeOf(*model.pattern->shape()) - 1e-5);
  }

  // NewBody Linear Pattern publishes only generated slots 1..count-1. Slot 4
  // disappears at count 3 and recovers with the same exact public carrier.
  {
    auto model = makeLinearPatternedBox(PatternOperation::NewBody);
    const auto slot1 = edgeForExactEndpoints(
        *model.pattern, model.patternBody->id(), gp_Pnt(12.0, 0.0, 0.0),
        gp_Pnt(12.0, 0.0, 5.0));
    const auto slot4 = edgeForExactEndpoints(
        *model.pattern, model.patternBody->id(), gp_Pnt(48.0, 0.0, 0.0),
        gp_Pnt(48.0, 0.0, 5.0));
    CHECK(slot1.v2());
    CHECK(slot4.v2());
    const auto slot1Key = slot1.v2()->provenance;
    const auto slot4Key = slot4.v2()->provenance;
    const auto slot1Left = faceOriginForBounds(
        *model.pattern, {12.0, 0.0, 0.0, 12.0, 6.0, 5.0});
    const auto slot1Front = faceOriginForBounds(
        *model.pattern, {12.0, 0.0, 0.0, 20.0, 0.0, 5.0});
    const auto slot4Left = faceOriginForBounds(
        *model.pattern, {48.0, 0.0, 0.0, 48.0, 6.0, 5.0});
    const auto slot4Front = faceOriginForBounds(
        *model.pattern, {48.0, 0.0, 0.0, 56.0, 0.0, 5.0});
    checkExactCarrier(
        *model.pattern, slot1, {slot1Left, slot1Front},
        {GeomAbs_Line, 5.0, gp_Pnt(12.0, 0.0, 0.0),
         gp_Pnt(12.0, 0.0, 5.0)});
    checkExactCarrier(
        *model.pattern, slot4, {slot4Left, slot4Front},
        {GeomAbs_Line, 5.0, gp_Pnt(48.0, 0.0, 0.0),
         gp_Pnt(48.0, 0.0, 5.0)});
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                            {1, 2, 3, 4});
    CHECK(solidCount(*model.pattern->shape()) == 4);

    model.pattern->setCount(3);
    CHECK(model.document.recompute());
    checkExactCarrier(
        *model.pattern, slot1, {slot1Left, slot1Front},
        {GeomAbs_Line, 5.0, gp_Pnt(12.0, 0.0, 0.0),
         gp_Pnt(12.0, 0.0, 5.0)});
    checkEdgeMissing(*model.pattern, slot4);
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                                {1, 2});
    CHECK(solidCount(*model.pattern->shape()) == 2);

    model.pattern->setCount(5);
    CHECK(model.document.recompute());
    CHECK(slot1.v2()->provenance == slot1Key);
    CHECK(slot4.v2()->provenance == slot4Key);
    checkExactCarrier(
        *model.pattern, slot4, {slot4Left, slot4Front},
        {GeomAbs_Line, 5.0, gp_Pnt(48.0, 0.0, 0.0),
         gp_Pnt(48.0, 0.0, 5.0)});
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                            {1, 2, 3, 4});
    CHECK(solidCount(*model.pattern->shape()) == 4);
  }

  // Legacy multi-region Linear Pattern remains shape-only and cannot author
  // an exact edge reference.
  {
    Document document;
    auto& profile = document.addSketch("Legacy multi-region linear pattern");
    profile.geometry.addRectangle({0.0, 0.0}, {8.0, 6.0});
    profile.geometry.addRectangle({20.0, 0.0}, {28.0, 6.0});
    auto& body = document.addBody();
    auto extrusion = std::make_unique<ExtrudeFeature>(profile.id, 5.0);
    auto* extrusionFeature = extrusion.get();
    const auto extrusionId = extrusion->id();
    body.addFeature(std::move(extrusion));
    auto pattern = std::make_unique<LinearPatternFeature>(
        extrusionId, PrincipalAxis::X, 3, 40.0);
    auto* patternFeature = pattern.get();
    body.addFeature(std::move(pattern));
    CHECK(document.recompute());
    CHECK(extrusionFeature->shape());
    CHECK(!extrusionFeature->topologyLineage());
    CHECK(patternFeature->shape());
    CHECK(!patternFeature->topologyLineage());
    const auto index = patternFeature->topologyIndex();
    CHECK(index);
    CHECK(index->rawEdgeCount() > 0);
    const auto exact = patternFeature->createEdgeReferenceV2(body.id(), 0);
    CHECK(!exact);
    CHECK(exact.status == EdgeReferenceV2CreationStatus::Missing);
  }

  // Join Circular Pattern publishes one exact edge set per authored
  // occurrence. Saved slot-1/slot-4 carriers follow the full/partial angle
  // formulas, count disappearance, axis changes, and source edits.
  {
    auto model = makeCircularPatternedBox();
    double sourceX0 = 10.0;
    double sourceY0 = 2.0;
    double sourceX1 = 14.0;
    double sourceY1 = 5.0;
    double length = 4.0;
    double angle = 240.0;
    int count = 5;
    PrincipalAxis axis = PrincipalAxis::Z;

    const auto sourceVertical = edgeForExactEndpoints(
        *model.extrusion, model.sourceBody->id(),
        gp_Pnt(sourceX0, sourceY0, 0.0),
        gp_Pnt(sourceX0, sourceY0, length));
    const auto slot0 = edgeForExactEndpoints(
        *model.pattern, model.patternBody->id(),
        gp_Pnt(sourceX0, sourceY0, 0.0),
        gp_Pnt(sourceX0, sourceY0, length));
    const auto slot1First = rotatedPoint(
        gp_Pnt(sourceX0, sourceY0, 0.0), axis,
        circularSlotAngle(angle, count, 1));
    const auto slot1Last = rotatedPoint(
        gp_Pnt(sourceX0, sourceY0, length), axis,
        circularSlotAngle(angle, count, 1));
    const auto slot4First = rotatedPoint(
        gp_Pnt(sourceX0, sourceY0, 0.0), axis,
        circularSlotAngle(angle, count, 4));
    const auto slot4Last = rotatedPoint(
        gp_Pnt(sourceX0, sourceY0, length), axis,
        circularSlotAngle(angle, count, 4));
    const auto slot1 = edgeForExactEndpoints(
        *model.pattern, model.patternBody->id(), slot1First, slot1Last);
    const auto slot4 = edgeForExactEndpoints(
        *model.pattern, model.patternBody->id(), slot4First, slot4Last);
    CHECK(sourceVertical.v2());
    CHECK(slot0.v2());
    CHECK(slot1.v2());
    CHECK(slot4.v2());
    const auto slot1Key = slot1.v2()->provenance;
    const auto slot4Key = slot4.v2()->provenance;
    const auto initialSlot1 =
        model.pattern->topologyIndex()->resolveEdge(slot1);
    const auto initialSlot4 =
        model.pattern->topologyIndex()->resolveEdge(slot4);
    CHECK(initialSlot1);
    CHECK(initialSlot4);
    const auto slot1Origins =
        adjacentFaceOrigins(*model.pattern, *initialSlot1.subshape);
    const auto slot4Origins =
        adjacentFaceOrigins(*model.pattern, *initialSlot4.subshape);

    const auto checkOccurrence = [&](const TopologyProvenanceKey& key,
                                     std::uint64_t expectedSlot) {
      CHECK(key.featureId == model.pattern->id());
      CHECK(key.kind == TopologyKind::Edge);
      CHECK(key.operation == TopologyProvenanceOperation::Generated);
      CHECK(!key.occurrence);
      CHECK(key.origins.size() == 2);
      for (const auto& origin : key.origins) {
        CHECK(origin.featureId == model.pattern->id());
        CHECK(origin.kind == TopologyKind::Face);
        CHECK(origin.occurrence);
        CHECK(origin.occurrence->featureId == model.pattern->id());
        CHECK(origin.occurrence->slot == expectedSlot);
      }
    };
    checkOccurrence(slot1Key, 1);
    checkOccurrence(slot4Key, 4);

    const auto checkSlot = [&](const EdgeReference& carrier,
                               const TopologyProvenanceKey& savedKey,
                               std::uint64_t slot,
                               const std::vector<TopologyProvenanceOrigin>&
                                   expectedOrigins) {
      const double slotAngle = circularSlotAngle(angle, count, slot);
      const gp_Pnt first = rotatedPoint(
          gp_Pnt(sourceX0, sourceY0, 0.0), axis, slotAngle);
      const gp_Pnt last = rotatedPoint(
          gp_Pnt(sourceX0, sourceY0, length), axis, slotAngle);
      checkExactCarrier(*model.pattern, carrier, expectedOrigins,
                        {GeomAbs_Line, length, first, last});
      const auto rediscovered = edgeForExactEndpoints(
          *model.pattern, model.patternBody->id(), first, last);
      CHECK(rediscovered.v2());
      CHECK(rediscovered.v2()->provenance == savedKey);
    };
    const auto checkBothSlots = [&] {
      checkSlot(slot1, slot1Key, 1, slot1Origins);
      checkSlot(slot4, slot4Key, 4, slot4Origins);
    };

    checkBothSlots();
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                                {0, 1, 2, 3, 4});
    const auto sourceKeys = edgeKeys(*model.extrusion->topologyLineage());
    const auto patternKeys = edgeKeys(*model.pattern->topologyLineage());
    CHECK(sourceKeys.size() == 12);
    CHECK(patternKeys.size() == 60);
    for (const auto& key : patternKeys) {
      CHECK(key.featureId == model.pattern->id());
      CHECK(std::find(sourceKeys.begin(), sourceKeys.end(), key) ==
            sourceKeys.end());
    }
    const auto sourceResolved =
        model.extrusion->topologyIndex()->resolveEdge(sourceVertical);
    const auto slot0Resolved = model.pattern->topologyIndex()->resolveEdge(slot0);
    const auto slot1Resolved = model.pattern->topologyIndex()->resolveEdge(slot1);
    const auto slot4Resolved = model.pattern->topologyIndex()->resolveEdge(slot4);
    CHECK(sourceResolved);
    CHECK(slot0Resolved);
    CHECK(slot1Resolved);
    CHECK(slot4Resolved);
    CHECK(slot0Resolved.subshape->IsSame(*sourceResolved.subshape));
    CHECK(!slot1Resolved.subshape->IsSame(*sourceResolved.subshape));
    CHECK(!slot4Resolved.subshape->IsSame(*sourceResolved.subshape));
    CHECK(!slot1Resolved.subshape->IsSame(*slot4Resolved.subshape));

    angle = 360.0;
    model.pattern->setAngleDeg(angle);
    CHECK(model.document.recompute());
    checkBothSlots();
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                                {0, 1, 2, 3, 4});

    angle = 180.0;
    model.pattern->setAngleDeg(angle);
    CHECK(model.document.recompute());
    checkBothSlots();
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                                {0, 1, 2, 3, 4});

    count = 3;
    model.pattern->setCount(count);
    CHECK(model.document.recompute());
    checkSlot(slot1, slot1Key, 1, slot1Origins);
    const auto missingSlot4 =
        model.pattern->topologyIndex()->resolveEdge(slot4);
    CHECK(!missingSlot4);
    CHECK(missingSlot4.failure == TopologyResolutionFailure::Missing);
    CHECK(missingSlot4.method == TopologyMatchMethod::None);
    CHECK(!missingSlot4.subshape);
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                                {0, 1, 2});

    count = 5;
    angle = 300.0;
    model.pattern->setCount(count);
    model.pattern->setAngleDeg(angle);
    CHECK(model.document.recompute());
    checkBothSlots();
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                                {0, 1, 2, 3, 4});

    axis = PrincipalAxis::Y;
    model.pattern->setAxis(axis);
    CHECK(model.document.recompute());
    checkBothSlots();
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                                {0, 1, 2, 3, 4});

    length = 7.0;
    model.extrusion->setLengthMm(length);
    CHECK(model.document.recompute());
    checkBothSlots();
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                                {0, 1, 2, 3, 4});

    sourceX0 = 12.0;
    sourceY0 = 3.0;
    sourceX1 = 17.0;
    sourceY1 = 7.0;
    sketch::Sketch reordered;
    reordered.addLine({sourceX0, sourceY1}, {sourceX1, sourceY1});
    reordered.addLine({sourceX1, sourceY0}, {sourceX0, sourceY0});
    reordered.addLine({sourceX0, sourceY0}, {sourceX0, sourceY1});
    reordered.addLine({sourceX1, sourceY1}, {sourceX1, sourceY0});
    CHECK(reordered.restoreGeometryIds(
        {model.geometryIds[2], model.geometryIds[0], model.geometryIds[3],
         model.geometryIds[1]},
        {}, {}, {}));
    CHECK(model.document.replaceSketchGeometry(model.sketchId,
                                               std::move(reordered)));
    CHECK(model.document.recompute());
    checkBothSlots();
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                                {0, 1, 2, 3, 4});

    const auto validShape = model.pattern->shape();
    const auto validLineage = model.pattern->topologyLineage();
    const auto validIndex = model.pattern->topologyIndex();
    const auto validRevision = model.pattern->shapeRevision();
    const auto validSlot1 = validIndex->resolveEdge(slot1);
    const auto validSlot4 = validIndex->resolveEdge(slot4);
    CHECK(validSlot1);
    CHECK(validSlot4);
    model.pattern->setAngleDeg(0.0);
    CHECK(!model.document.recompute());
    CHECK(!model.pattern->shape());
    CHECK(!model.pattern->topologyLineage());
    CHECK(model.pattern->shapeRevision() == validRevision);
    std::string error;
    CHECK(!model.patternBody->topologySnapshot(model.pattern->id(), &error));
    CHECK(!error.empty());
    error.clear();
    CHECK(!model.pattern->topologyIndex(&error));
    CHECK(!error.empty());
    CHECK(model.pattern->lastValidShape() == validShape);
    CHECK(model.pattern->lastValidTopologyLineage() == validLineage);
    CHECK(model.pattern->lastValidTopologyIndex(&error) == validIndex);
    CHECK(error.empty());
    const auto rolledBackSlot1 = validIndex->resolveEdge(slot1);
    const auto rolledBackSlot4 = validIndex->resolveEdge(slot4);
    CHECK(rolledBackSlot1);
    CHECK(rolledBackSlot4);
    CHECK(rolledBackSlot1.subshape->IsSame(*validSlot1.subshape));
    CHECK(rolledBackSlot4.subshape->IsSame(*validSlot4.subshape));

    model.pattern->setAngleDeg(angle);
    CHECK(model.document.recompute());
    CHECK(model.pattern->shapeRevision() > validRevision);
    checkBothSlots();
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                                {0, 1, 2, 3, 4});
  }

  // A fixed surviving slot-1 Circular Pattern edge feeds Fillet without
  // probing and preserves strict material removal across all parameter axes.
  {
    auto model = makeCircularPatternedBox();
    double angle = 240.0;
    int count = 5;
    double length = 4.0;
    PrincipalAxis axis = PrincipalAxis::Z;
    const auto first = rotatedPoint(
        gp_Pnt(10.0, 2.0, 0.0), axis,
        circularSlotAngle(angle, count, 1));
    const auto last = rotatedPoint(
        gp_Pnt(10.0, 2.0, length), axis,
        circularSlotAngle(angle, count, 1));
    const auto slot1 = edgeForExactEndpoints(
        *model.pattern, model.patternBody->id(), first, last);
    const auto initial = model.pattern->topologyIndex()->resolveEdge(slot1);
    CHECK(initial);
    const auto origins = adjacentFaceOrigins(*model.pattern, *initial.subshape);
    checkExactCarrier(*model.pattern, slot1, origins,
                      {GeomAbs_Line, length, first, last});
    auto fillet = std::make_unique<FilletFeature>(slot1, 0.5);
    auto* filletFeature = fillet.get();
    model.patternBody->addFeature(std::move(fillet));
    CHECK(model.document.recompute());
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) <
          volumeOf(*model.pattern->shape()) - 1e-5);

    angle = 300.0;
    count = 3;
    model.pattern->setAngleDeg(angle);
    model.pattern->setCount(count);
    CHECK(model.document.recompute());
    auto editedFirst = rotatedPoint(
        gp_Pnt(10.0, 2.0, 0.0), axis,
        circularSlotAngle(angle, count, 1));
    auto editedLast = rotatedPoint(
        gp_Pnt(10.0, 2.0, length), axis,
        circularSlotAngle(angle, count, 1));
    checkExactCarrier(*model.pattern, slot1, origins,
                      {GeomAbs_Line, length, editedFirst, editedLast});
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) <
          volumeOf(*model.pattern->shape()) - 1e-5);

    axis = PrincipalAxis::Y;
    length = 6.0;
    model.pattern->setAxis(axis);
    model.extrusion->setLengthMm(length);
    CHECK(model.document.recompute());
    editedFirst = rotatedPoint(
        gp_Pnt(10.0, 2.0, 0.0), axis,
        circularSlotAngle(angle, count, 1));
    editedLast = rotatedPoint(
        gp_Pnt(10.0, 2.0, length), axis,
        circularSlotAngle(angle, count, 1));
    checkExactCarrier(*model.pattern, slot1, origins,
                      {GeomAbs_Line, length, editedFirst, editedLast});
    CHECK(filletFeature->shape());
    CHECK(volumeOf(*filletFeature->shape()) <
          volumeOf(*model.pattern->shape()) - 1e-5);
  }

  // NewBody Circular Pattern contains only generated slots 1..count-1.
  {
    auto model = makeCircularPatternedBox(PatternOperation::NewBody);
    double angle = 240.0;
    int count = 5;
    const auto slot1First = rotatedPoint(
        gp_Pnt(10.0, 2.0, 0.0), PrincipalAxis::Z,
        circularSlotAngle(angle, count, 1));
    const auto slot1Last = rotatedPoint(
        gp_Pnt(10.0, 2.0, 4.0), PrincipalAxis::Z,
        circularSlotAngle(angle, count, 1));
    const auto slot4First = rotatedPoint(
        gp_Pnt(10.0, 2.0, 0.0), PrincipalAxis::Z,
        circularSlotAngle(angle, count, 4));
    const auto slot4Last = rotatedPoint(
        gp_Pnt(10.0, 2.0, 4.0), PrincipalAxis::Z,
        circularSlotAngle(angle, count, 4));
    const auto slot1 = edgeForExactEndpoints(
        *model.pattern, model.patternBody->id(), slot1First, slot1Last);
    const auto slot4 = edgeForExactEndpoints(
        *model.pattern, model.patternBody->id(), slot4First, slot4Last);
    const auto initialSlot1 =
        model.pattern->topologyIndex()->resolveEdge(slot1);
    const auto initialSlot4 =
        model.pattern->topologyIndex()->resolveEdge(slot4);
    CHECK(initialSlot1);
    CHECK(initialSlot4);
    const auto slot1Origins =
        adjacentFaceOrigins(*model.pattern, *initialSlot1.subshape);
    const auto slot4Origins =
        adjacentFaceOrigins(*model.pattern, *initialSlot4.subshape);
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                                {1, 2, 3, 4});
    CHECK(solidCount(*model.pattern->shape()) == 4);

    angle = 180.0;
    count = 3;
    model.pattern->setAngleDeg(angle);
    model.pattern->setCount(count);
    CHECK(model.document.recompute());
    const auto editedSlot1First = rotatedPoint(
        gp_Pnt(10.0, 2.0, 0.0), PrincipalAxis::Z,
        circularSlotAngle(angle, count, 1));
    const auto editedSlot1Last = rotatedPoint(
        gp_Pnt(10.0, 2.0, 4.0), PrincipalAxis::Z,
        circularSlotAngle(angle, count, 1));
    checkExactCarrier(
        *model.pattern, slot1, slot1Origins,
        {GeomAbs_Line, 4.0, editedSlot1First, editedSlot1Last});
    checkEdgeMissing(*model.pattern, slot4);
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                                {1, 2});
    CHECK(solidCount(*model.pattern->shape()) == 2);

    angle = 300.0;
    count = 5;
    model.pattern->setAngleDeg(angle);
    model.pattern->setCount(count);
    CHECK(model.document.recompute());
    const auto recoveredSlot4First = rotatedPoint(
        gp_Pnt(10.0, 2.0, 0.0), PrincipalAxis::Z,
        circularSlotAngle(angle, count, 4));
    const auto recoveredSlot4Last = rotatedPoint(
        gp_Pnt(10.0, 2.0, 4.0), PrincipalAxis::Z,
        circularSlotAngle(angle, count, 4));
    checkExactCarrier(
        *model.pattern, slot4, slot4Origins,
        {GeomAbs_Line, 4.0, recoveredSlot4First, recoveredSlot4Last});
    auditOccurrencePatternEdges(*model.pattern, model.patternBody->id(),
                                {1, 2, 3, 4});
    CHECK(solidCount(*model.pattern->shape()) == 4);
  }

  // Legacy multi-region Circular Pattern remains shape-only and cannot
  // author a V2 edge.
  {
    Document document;
    auto& profile = document.addSketch("Legacy multi-region circular pattern");
    profile.geometry.addRectangle({10.0, 2.0}, {14.0, 5.0});
    profile.geometry.addRectangle({20.0, 2.0}, {24.0, 5.0});
    auto& body = document.addBody();
    auto extrusion = std::make_unique<ExtrudeFeature>(profile.id, 4.0);
    auto* extrusionFeature = extrusion.get();
    const auto extrusionId = extrusion->id();
    body.addFeature(std::move(extrusion));
    auto pattern = std::make_unique<CircularPatternFeature>(
        extrusionId, PrincipalAxis::Z, 3, 180.0);
    auto* patternFeature = pattern.get();
    body.addFeature(std::move(pattern));
    CHECK(document.recompute());
    CHECK(extrusionFeature->shape());
    CHECK(!extrusionFeature->topologyLineage());
    CHECK(patternFeature->shape());
    CHECK(!patternFeature->topologyLineage());
    const auto index = patternFeature->topologyIndex();
    CHECK(index);
    CHECK(index->rawEdgeCount() > 0);
    const auto exact = patternFeature->createEdgeReferenceV2(body.id(), 0);
    CHECK(!exact);
    CHECK(exact.status == EdgeReferenceV2CreationStatus::Missing);
  }

  return EXIT_SUCCESS;
}
