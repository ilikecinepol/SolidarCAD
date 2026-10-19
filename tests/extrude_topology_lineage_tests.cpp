#include "TestAssertions.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/TopologyLineageBuilder.h"
#include "model/TopologyReferenceResolver.h"

namespace {

struct ExpectedSide {
  solidar::sketch::GeometryId geometryId;
  double x;
  double y;
};

gp_Pnt faceCenter(const TopoDS_Face& face) {
  GProp_GProps properties;
  BRepGProp::SurfaceProperties(face, properties);
  return properties.CentreOfMass();
}

double faceArea(const TopoDS_Face& face) {
  GProp_GProps properties;
  BRepGProp::SurfaceProperties(face, properties);
  return properties.Mass();
}

GeomAbs_SurfaceType surfaceType(const TopoDS_Face& face) {
  return BRepAdaptor_Surface(face).GetType();
}

double volumeOf(const TopoDS_Shape& shape) {
  GProp_GProps properties;
  BRepGProp::VolumeProperties(shape, properties);
  return properties.Mass();
}

bool near(double actual, double expected, double tolerance = 1e-5) {
  return std::abs(actual - expected) <= tolerance;
}

solidar::TopologyProvenanceKey authoredKey(
    solidar::AuthoredTopologySourceKind kind, solidar::SketchId sketchId,
    solidar::sketch::GeometryId geometryId,
    solidar::FeatureId featureId) {
  std::string error;
  const auto key = solidar::projectAuthoredTopologyToken(
      {kind, sketchId, geometryId}, featureId, &error);
  CHECK(key);
  CHECK(error.empty());
  return *key;
}

void checkCompleteFaceLineage(
    const solidar::ExtrudeFeature& feature, solidar::BodyId bodyId,
    solidar::SketchId sketchId,
    std::span<const solidar::sketch::GeometryId> geometryIds,
    std::size_t expectedFaceCount, double startZ, double endZ,
    std::span<const ExpectedSide> expectedSides = {}) {
  const auto lineage = feature.topologyLineage();
  CHECK(lineage);
  CHECK(lineage->coverage() ==
        solidar::TopologyLineageCoverage::CompleteFaces);
  CHECK(lineage->bindings().size() == expectedFaceCount);
  std::string error;
  const auto index = feature.topologyIndex(&error);
  CHECK(index);
  CHECK(error.empty());
  CHECK(index->faceCount() == expectedFaceCount);

  const auto startKey = authoredKey(
      solidar::AuthoredTopologySourceKind::ProfileStartCap, sketchId,
      solidar::sketch::kInvalidGeometryId, feature.id());
  const auto endKey = authoredKey(
      solidar::AuthoredTopologySourceKind::ProfileEndCap, sketchId,
      solidar::sketch::kInvalidGeometryId, feature.id());
  const auto start = index->resolveFace({bodyId, startKey});
  const auto end = index->resolveFace({bodyId, endKey});
  CHECK(start);
  CHECK(end);
  CHECK(start.method == solidar::TopologyMatchMethod::OperationProvenance);
  CHECK(end.method == solidar::TopologyMatchMethod::OperationProvenance);
  CHECK(near(faceCenter(*start.subshape).Z(), startZ));
  CHECK(near(faceCenter(*end.subshape).Z(), endZ));

  for (const auto geometryId : geometryIds) {
    const auto key = authoredKey(
        solidar::AuthoredTopologySourceKind::GeometrySide, sketchId,
        geometryId, feature.id());
    const auto resolved = index->resolveFace({bodyId, key});
    CHECK(resolved);
    CHECK(resolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    const auto expected = std::find_if(
        expectedSides.begin(), expectedSides.end(), [&](const auto& side) {
          return side.geometryId == geometryId;
        });
    if (expected != expectedSides.end()) {
      const auto center = faceCenter(*resolved.subshape);
      CHECK(near(center.X(), expected->x));
      CHECK(near(center.Y(), expected->y));
      CHECK(near(center.Z(), (startZ + endZ) * 0.5));
    }
  }
}

void checkLegacyV1(const solidar::ExtrudeFeature& feature,
                   solidar::BodyId bodyId) {
  CHECK(!feature.topologyLineage());
  CHECK(feature.shape());
  CHECK(feature.shape()->ShapeType() == TopAbs_SOLID ||
        feature.shape()->ShapeType() == TopAbs_COMPOUND);
  std::string error;
  const auto index = feature.topologyIndex(&error);
  CHECK(index);
  CHECK(error.empty());
  TopExp_Explorer faces(*feature.shape(), TopAbs_FACE);
  CHECK(faces.More());
  const auto expected = TopoDS::Face(faces.Current());
  const solidar::TopologyReference legacy{
      bodyId, feature.id(), solidar::TopologyKind::Face, 0};
  CHECK(legacy.kind == solidar::TopologyKind::Face);
  const auto resolved = solidar::resolveFaceReference(*index, legacy);
  CHECK(resolved);
  CHECK(resolved.method == solidar::TopologyMatchMethod::LegacyIndex);
  CHECK(resolved.subshape->IsSame(expected));

  const auto unavailable = authoredKey(
      solidar::AuthoredTopologySourceKind::ProfileStartCap,
      static_cast<solidar::SketchId>(1),
      solidar::sketch::kInvalidGeometryId, feature.id());
  const auto exact = index->resolveFace({bodyId, std::move(unavailable)});
  CHECK(!exact);
  CHECK(!exact.subshape);
  CHECK(exact.method == solidar::TopologyMatchMethod::None);
  CHECK(exact.failure == solidar::TopologyResolutionFailure::Missing);
}

}  // namespace

int main() {
  using namespace solidar;

  // Production Document rectangle: exact authored sides, caps and atomic
  // lifecycle across length, reverse, failure and recovery.
  {
    Document document;
    auto& sketch = document.addSketch("RT1 rectangle");
    sketch.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
    const std::array<sketch::GeometryId, 4> ids{
        sketch.geometry.lineId(0), sketch.geometry.lineId(1),
        sketch.geometry.lineId(2), sketch.geometry.lineId(3)};
    sketch.geometry.addLine({-10.0, -10.0}, {-5.0, -5.0});
    const auto constructionId = sketch.geometry.lineId(4);
    sketch.geometry.setLineDashedById(constructionId, true);
    const std::array<ExpectedSide, 4> expected{{
        {ids[0], 40.0, 0.0}, {ids[1], 80.0, 17.5},
        {ids[2], 40.0, 35.0}, {ids[3], 0.0, 17.5}}};
    auto& body = document.addBody();
    auto extrusion =
        std::make_unique<ExtrudeFeature>(sketch.id, 50.0, "RT1");
    auto* feature = extrusion.get();
    body.addFeature(std::move(extrusion));
    CHECK(document.rebuild());
    checkCompleteFaceLineage(*feature, body.id(), sketch.id, ids, 6, 0.0,
                             50.0, expected);
    const auto constructionKey = authoredKey(
        AuthoredTopologySourceKind::GeometrySide, sketch.id, constructionId,
        feature->id());
    const auto construction =
        feature->topologyIndex()->resolveFace({body.id(), constructionKey});
    CHECK(!construction);
    CHECK(construction.method == TopologyMatchMethod::None);
    CHECK(construction.failure == TopologyResolutionFailure::Missing);

    feature->setLengthMm(80.0);
    CHECK(document.rebuild());
    checkCompleteFaceLineage(*feature, body.id(), sketch.id, ids, 6, 0.0,
                             80.0, expected);
    feature->setReversed(true);
    CHECK(document.rebuild());
    checkCompleteFaceLineage(*feature, body.id(), sketch.id, ids, 6, 0.0,
                             -80.0, expected);

    const auto lastShape = feature->shape();
    const auto lastLineage = feature->topologyLineage();
    const auto baselineRevision = feature->shapeRevision();
    std::string error;
    const auto cachedIndex = feature->topologyIndex(&error);
    CHECK(cachedIndex);
    CHECK(error.empty());
    const TopologyReferenceV2 startReference{
        body.id(), authoredKey(AuthoredTopologySourceKind::ProfileStartCap,
                               sketch.id, sketch::kInvalidGeometryId,
                               feature->id())};
    const TopologyReferenceV2 endReference{
        body.id(), authoredKey(AuthoredTopologySourceKind::ProfileEndCap,
                               sketch.id, sketch::kInvalidGeometryId,
                               feature->id())};
    const TopologyReferenceV2 sideReference{
        body.id(), authoredKey(AuthoredTopologySourceKind::GeometrySide,
                               sketch.id, ids[0], feature->id())};
    const auto expectedStart = cachedIndex->resolveFace(startReference);
    const auto expectedEnd = cachedIndex->resolveFace(endReference);
    const auto expectedSide = cachedIndex->resolveFace(sideReference);
    CHECK(expectedStart);
    CHECK(expectedEnd);
    CHECK(expectedSide);
    feature->setLengthMm(0.0);
    CHECK(!document.rebuild());
    CHECK(!feature->shape());
    CHECK(!feature->topologyLineage());
    CHECK(feature->shapeRevision() == baselineRevision);
    CHECK(feature->lastValidShape() == lastShape);
    CHECK(feature->lastValidTopologyLineage() == lastLineage);
    CHECK(!feature->topologyIndex(&error));
    CHECK(!error.empty());
    const auto lastIndex = feature->lastValidTopologyIndex(&error);
    CHECK(lastIndex == cachedIndex);
    CHECK(error.empty());
    const auto lastStart = lastIndex->resolveFace(startReference);
    const auto lastEnd = lastIndex->resolveFace(endReference);
    const auto lastSide = lastIndex->resolveFace(sideReference);
    CHECK(lastStart);
    CHECK(lastEnd);
    CHECK(lastSide);
    CHECK(lastStart.subshape->IsSame(*expectedStart.subshape));
    CHECK(lastEnd.subshape->IsSame(*expectedEnd.subshape));
    CHECK(lastSide.subshape->IsSame(*expectedSide.subshape));
    feature->setLengthMm(25.0);
    CHECK(document.rebuild());
    CHECK(feature->shapeRevision() > baselineRevision);
    checkCompleteFaceLineage(*feature, body.id(), sketch.id, ids, 6, 0.0,
                             -25.0, expected);
    const auto recoveredIndex = feature->topologyIndex(&error);
    CHECK(recoveredIndex);
    const auto recoveredStart = recoveredIndex->resolveFace(startReference);
    const auto recoveredEnd = recoveredIndex->resolveFace(endReference);
    const auto recoveredSide = recoveredIndex->resolveFace(sideReference);
    CHECK(recoveredStart);
    CHECK(recoveredEnd);
    CHECK(recoveredSide);
    CHECK(near(faceCenter(*recoveredStart.subshape).Z(), 0.0));
    CHECK(near(faceCenter(*recoveredEnd.subshape).Z(), -25.0));
    const auto recoveredSideCenter = faceCenter(*recoveredSide.subshape);
    CHECK(near(recoveredSideCenter.X(), 40.0));
    CHECK(near(recoveredSideCenter.Y(), 0.0));
    CHECK(near(recoveredSideCenter.Z(), -12.5));
  }

  // Reordering and reversing primitives while restoring their authored IDs
  // preserves the semantic GeometrySide keys.
  {
    Document document;
    auto& sketch = document.addSketch("Reordered rectangle");
    sketch.geometry.addLine({0.0, 0.0}, {20.0, 0.0});
    sketch.geometry.addLine({20.0, 0.0}, {20.0, 10.0});
    sketch.geometry.addLine({20.0, 10.0}, {0.0, 10.0});
    sketch.geometry.addLine({0.0, 10.0}, {0.0, 0.0});
    const std::array<sketch::GeometryId, 4> ids{
        sketch.geometry.lineId(0), sketch.geometry.lineId(1),
        sketch.geometry.lineId(2), sketch.geometry.lineId(3)};
    const std::array<ExpectedSide, 4> expected{{
        {ids[0], 10.0, 0.0}, {ids[1], 20.0, 5.0},
        {ids[2], 10.0, 10.0}, {ids[3], 0.0, 5.0}}};
    auto& body = document.addBody();
    auto extrusion = std::make_unique<ExtrudeFeature>(sketch.id, 6.0);
    auto* feature = extrusion.get();
    body.addFeature(std::move(extrusion));
    CHECK(document.rebuild());
    checkCompleteFaceLineage(*feature, body.id(), sketch.id, ids, 6, 0.0,
                             6.0, expected);

    sketch::Sketch reordered;
    reordered.addLine({0.0, 10.0}, {20.0, 10.0});
    reordered.addLine({20.0, 0.0}, {0.0, 0.0});
    reordered.addLine({0.0, 0.0}, {0.0, 10.0});
    reordered.addLine({20.0, 10.0}, {20.0, 0.0});
    CHECK(reordered.restoreGeometryIds(
        {ids[2], ids[0], ids[3], ids[1]}, {}, {}, {}));
    CHECK(document.replaceSketchGeometry(sketch.id, std::move(reordered)));
    CHECK(document.rebuild());
    checkCompleteFaceLineage(*feature, body.id(), sketch.id, ids, 6, 0.0,
                             6.0, expected);
  }

  // Circle, D-profile and Bezier profile all publish complete face coverage.
  {
    Document circleDocument;
    auto& circle = circleDocument.addSketch("Circle");
    circle.geometry.addCircle({0.0, 0.0}, 5.0);
    const std::array circleIds{circle.geometry.circleId(0)};
    auto& circleBody = circleDocument.addBody();
    auto circleExtrude =
        std::make_unique<ExtrudeFeature>(circle.id, 12.0);
    auto* circleFeature = circleExtrude.get();
    circleBody.addFeature(std::move(circleExtrude));
    CHECK(circleDocument.rebuild());
    checkCompleteFaceLineage(*circleFeature, circleBody.id(), circle.id,
                             circleIds, 3, 0.0, 12.0);
    const auto circleSide = circleFeature->topologyIndex()->resolveFace(
        {circleBody.id(), authoredKey(AuthoredTopologySourceKind::GeometrySide,
                                      circle.id, circleIds[0],
                                      circleFeature->id())});
    CHECK(circleSide);
    CHECK(surfaceType(*circleSide.subshape) == GeomAbs_Cylinder);
    CHECK(near(faceArea(*circleSide.subshape),
               2.0 * std::numbers::pi * 5.0 * 12.0, 1e-4));

    Document dDocument;
    auto& d = dDocument.addSketch("D profile");
    d.geometry.addLine({-5.0, 0.0}, {5.0, 0.0});
    d.geometry.addArc({0.0, 0.0}, 5.0, 0.0, std::numbers::pi);
    const std::array dIds{d.geometry.lineId(0), d.geometry.arcId(0)};
    auto& dBody = dDocument.addBody();
    auto dExtrude = std::make_unique<ExtrudeFeature>(d.id, 7.0);
    auto* dFeature = dExtrude.get();
    dBody.addFeature(std::move(dExtrude));
    CHECK(dDocument.rebuild());
    checkCompleteFaceLineage(*dFeature, dBody.id(), d.id, dIds, 4, 0.0,
                             7.0);
    const auto dIndex = dFeature->topologyIndex();
    const auto dLine = dIndex->resolveFace(
        {dBody.id(), authoredKey(AuthoredTopologySourceKind::GeometrySide,
                                 d.id, dIds[0], dFeature->id())});
    const auto dArc = dIndex->resolveFace(
        {dBody.id(), authoredKey(AuthoredTopologySourceKind::GeometrySide,
                                 d.id, dIds[1], dFeature->id())});
    CHECK(dLine);
    CHECK(dArc);
    CHECK(surfaceType(*dLine.subshape) == GeomAbs_Plane);
    CHECK(surfaceType(*dArc.subshape) == GeomAbs_Cylinder);
    CHECK(near(faceArea(*dLine.subshape), 10.0 * 7.0, 1e-5));
    CHECK(near(faceArea(*dArc.subshape),
               std::numbers::pi * 5.0 * 7.0, 1e-4));

    Document bezierDocument;
    auto& bezier = bezierDocument.addSketch("Bezier profile");
    bezier.geometry.addBezier({0.0, 0.0}, {2.0, 6.0}, {8.0, 6.0},
                              {10.0, 0.0});
    bezier.geometry.addLine({10.0, 0.0}, {0.0, 0.0});
    const std::array bezierIds{bezier.geometry.bezierId(0),
                               bezier.geometry.lineId(0)};
    auto& bezierBody = bezierDocument.addBody();
    auto bezierExtrude =
        std::make_unique<ExtrudeFeature>(bezier.id, 4.0);
    auto* bezierFeature = bezierExtrude.get();
    bezierBody.addFeature(std::move(bezierExtrude));
    CHECK(bezierDocument.rebuild());
    checkCompleteFaceLineage(*bezierFeature, bezierBody.id(), bezier.id,
                             bezierIds, 4, 0.0, 4.0);
    const auto bezierIndex = bezierFeature->topologyIndex();
    const auto bezierSide = bezierIndex->resolveFace(
        {bezierBody.id(),
         authoredKey(AuthoredTopologySourceKind::GeometrySide, bezier.id,
                     bezierIds[0], bezierFeature->id())});
    const auto bezierLine = bezierIndex->resolveFace(
        {bezierBody.id(),
         authoredKey(AuthoredTopologySourceKind::GeometrySide, bezier.id,
                     bezierIds[1], bezierFeature->id())});
    CHECK(bezierSide);
    CHECK(bezierLine);
    CHECK(surfaceType(*bezierLine.subshape) == GeomAbs_Plane);
    CHECK(surfaceType(*bezierSide.subshape) == GeomAbs_SurfaceOfExtrusion);
    CHECK(near(faceArea(*bezierLine.subshape), 10.0 * 4.0, 1e-5));
    CHECK(near(faceArea(*bezierSide.subshape), 57.5250020184, 1e-4));
  }

  // Out-of-scope valid modes remain legacy and retain working v1 references.
  {
    Document multiDocument;
    auto& multi = multiDocument.addSketch("Multi");
    multi.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    multi.geometry.addRectangle({20.0, 0.0}, {30.0, 10.0});
    auto& multiBody = multiDocument.addBody();
    auto multiExtrude = std::make_unique<ExtrudeFeature>(multi.id, 5.0);
    auto* multiFeature = multiExtrude.get();
    multiBody.addFeature(std::move(multiExtrude));
    CHECK(multiDocument.rebuild());
    CHECK(near(volumeOf(*multiFeature->shape()), 1000.0, 1e-4));
    checkLegacyV1(*multiFeature, multiBody.id());

    Document holeDocument;
    auto& hole = holeDocument.addSketch("Hole");
    hole.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
    hole.geometry.addCircle({10.0, 10.0}, 3.0);
    auto& holeBody = holeDocument.addBody();
    auto holeExtrude = std::make_unique<ExtrudeFeature>(hole.id, 5.0);
    auto* holeFeature = holeExtrude.get();
    holeBody.addFeature(std::move(holeExtrude));
    CHECK(holeDocument.rebuild());
    CHECK(near(volumeOf(*holeFeature->shape()),
               (400.0 - std::numbers::pi * 9.0) * 5.0, 1e-4));
    checkLegacyV1(*holeFeature, holeBody.id());

    Document overrideDocument;
    auto& source = overrideDocument.addSketch("Override");
    source.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    auto& overrideBody = overrideDocument.addBody();
    auto overrideExtrude =
        std::make_unique<ExtrudeFeature>(source.id, 5.0);
    overrideExtrude->setProfileOverride(source.geometry);
    auto* overrideFeature = overrideExtrude.get();
    overrideBody.addFeature(std::move(overrideExtrude));
    CHECK(overrideDocument.rebuild());
    CHECK(near(volumeOf(*overrideFeature->shape()), 500.0, 1e-4));
    checkLegacyV1(*overrideFeature, overrideBody.id());

    Document joinDocument;
    auto& base = joinDocument.addSketch("Base");
    base.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    auto& joinBody = joinDocument.addBody();
    joinBody.addFeature(std::make_unique<ExtrudeFeature>(base.id, 5.0));
    auto& join = joinDocument.addSketch("Join");
    join.geometry.addRectangle({5.0, 0.0}, {15.0, 10.0});
    auto joinExtrude = std::make_unique<ExtrudeFeature>(
        join.id, 5.0, "Join", ExtrudeOperation::Join);
    auto* joinFeature = joinExtrude.get();
    joinBody.addFeature(std::move(joinExtrude));
    CHECK(joinDocument.rebuild());
    CHECK(near(volumeOf(*joinFeature->shape()), 750.0, 1e-4));
    checkLegacyV1(*joinFeature, joinBody.id());

    auto& cut = joinDocument.addSketch("Cut");
    cut.geometry.addRectangle({1.0, 1.0}, {3.0, 3.0});
    auto cutExtrude = std::make_unique<ExtrudeFeature>(
        cut.id, 5.0, "Cut", ExtrudeOperation::Cut);
    auto* cutFeature = cutExtrude.get();
    joinBody.addFeature(std::move(cutExtrude));
    CHECK(joinDocument.rebuild());
    CHECK(near(volumeOf(*cutFeature->shape()), 730.0, 1e-4));
    checkLegacyV1(*cutFeature, joinBody.id());
  }

  // A malformed single contour is not an "unsupported but valid" legacy
  // case: v2 eligibility failure stays a rebuild failure with no publication.
  {
    Document document;
    auto& open = document.addSketch("Open invalid RT1 profile");
    open.geometry.addLine({0.0, 0.0}, {10.0, 0.0});
    open.geometry.addLine({10.0, 0.0}, {10.0, 10.0});
    auto& body = document.addBody();
    auto extrusion = std::make_unique<ExtrudeFeature>(open.id, 5.0);
    auto* feature = extrusion.get();
    body.addFeature(std::move(extrusion));
    CHECK(!document.rebuild());
    CHECK(!feature->shape());
    CHECK(!feature->topologyLineage());
    CHECK(!feature->error().empty());
  }

  return EXIT_SUCCESS;
}
