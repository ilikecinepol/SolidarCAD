#include "TestAssertions.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
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
#include "model/ImportedShapeFeature.h"
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
  CHECK(static_cast<std::size_t>(std::count_if(
            lineage->bindings().begin(), lineage->bindings().end(),
            [](const auto& binding) {
              return binding.provenance.kind == solidar::TopologyKind::Face;
            })) == expectedFaceCount);
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

std::vector<solidar::FaceReference> checkStrictBooleanLineage(
    const solidar::ExtrudeFeature& feature, solidar::BodyId bodyId,
    solidar::FeatureId baseFeatureId, std::size_t expectedFaceCount) {
  const auto lineage = feature.topologyLineage();
  CHECK(lineage);
  CHECK(lineage->coverage() ==
        solidar::TopologyLineageCoverage::CompleteFaces);
  CHECK(lineage->bindings().size() == expectedFaceCount);
  const auto index = feature.topologyIndex();
  CHECK(index);
  CHECK(index->faceCount() == expectedFaceCount);
  CHECK(index->rawFaceCount() == expectedFaceCount);
  const solidar::TopologyReference legacy{
      bodyId, feature.id(), solidar::TopologyKind::Face, 0};
  const auto legacyResolved = solidar::resolveFaceReference(*index, legacy);
  CHECK(legacyResolved);
  CHECK(legacyResolved.method == solidar::TopologyMatchMethod::LegacyIndex);

  bool foundBaseOrigin = false;
  bool foundToolOrigin = false;
  for (const auto& binding : lineage->bindings()) {
    CHECK(binding.provenance.featureId == feature.id());
    CHECK(!binding.provenance.origins.empty());
    for (const auto& origin : binding.provenance.origins) {
      foundBaseOrigin = foundBaseOrigin || origin.featureId == baseFeatureId;
      foundToolOrigin = foundToolOrigin || origin.featureId == feature.id();
    }
  }
  CHECK(foundBaseOrigin);
  CHECK(foundToolOrigin);

  std::vector<solidar::FaceReference> references;
  references.reserve(index->rawFaceCount());
  for (std::size_t raw = 0; raw < index->rawFaceCount(); ++raw) {
    auto created = feature.createFaceReferenceV2(bodyId, raw);
    CHECK(created);
    CHECK(created.status ==
          solidar::FaceReferenceV2CreationStatus::Created);
    CHECK(created.reference.v2());
    const auto resolved = index->resolveFace(created.reference);
    CHECK(resolved);
    CHECK(resolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    references.push_back(std::move(created.reference));
  }
  return references;
}

std::size_t countProvenanceOperation(
    const solidar::ShapeFeature& feature,
    solidar::TopologyProvenanceOperation operation) {
  const auto lineage = feature.topologyLineage();
  CHECK(lineage);
  return static_cast<std::size_t>(std::count_if(
      lineage->bindings().begin(), lineage->bindings().end(),
      [&](const auto& binding) {
        return binding.provenance.operation == operation;
      }));
}

void checkReferencesResolve(
    const solidar::ExtrudeFeature& feature,
    std::span<const solidar::FaceReference> references) {
  const auto index = feature.topologyIndex();
  CHECK(index);
  for (const auto& reference : references) {
    const auto resolved = index->resolveFace(reference);
    CHECK(resolved);
    CHECK(resolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
  }
}

void checkSingleOriginDistribution(const solidar::ShapeFeature& feature,
                                   solidar::FeatureId objectFeatureId,
                                   std::size_t expectedObjectOrigins,
                                   std::size_t expectedToolOrigins) {
  const auto lineage = feature.topologyLineage();
  CHECK(lineage);
  std::size_t objectOrigins = 0;
  std::size_t toolOrigins = 0;
  for (const auto& binding : lineage->bindings()) {
    CHECK(binding.provenance.featureId == feature.id());
    CHECK(binding.provenance.origins.size() == 1);
    const auto source = binding.provenance.origins.front().featureId;
    CHECK(source == objectFeatureId || source == feature.id());
    objectOrigins += source == objectFeatureId ? 1 : 0;
    toolOrigins += source == feature.id() ? 1 : 0;
  }
  CHECK(objectOrigins == expectedObjectOrigins);
  CHECK(toolOrigins == expectedToolOrigins);
}

solidar::FaceReference referenceForOrigin(
    const solidar::ShapeFeature& feature, solidar::BodyId bodyId,
    const solidar::TopologyProvenanceOrigin& expectedOrigin) {
  const auto index = feature.topologyIndex();
  CHECK(index);
  std::optional<solidar::FaceReference> found;
  for (std::size_t raw = 0; raw < index->rawFaceCount(); ++raw) {
    const auto created = feature.createFaceReferenceV2(bodyId, raw);
    CHECK(created);
    const auto* v2 = created.reference.v2();
    CHECK(v2);
    if (v2->provenance.origins.size() != 1 ||
        v2->provenance.origins.front() != expectedOrigin)
      continue;
    CHECK(!found);
    found = created.reference;
  }
  CHECK(found);
  return *found;
}

class PartialLineageBoxFeature final : public solidar::ShapeFeature {
 public:
  PartialLineageBoxFeature() : ShapeFeature("Partial lineage box") {}

  [[nodiscard]] std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<PartialLineageBoxFeature>(*this);
  }

 protected:
  bool rebuildImpl(const solidar::RebuildContext&) override {
    clearShape();
    auto shape = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape());
    TopExp_Explorer faces(*shape, TopAbs_FACE);
    if (!faces.More()) {
      markError("Partial lineage fixture has no face");
      return false;
    }
    solidar::TopologyProvenanceKey key{
        id(), solidar::TopologyKind::Face,
        solidar::TopologyProvenanceOperation::Seed,
        solidar::TopologySemanticRole::Custom, "partial-face"};
    std::string error;
    auto lineage = solidar::TopologyLineage::create(
        {{std::move(key),
          std::make_shared<TopoDS_Shape>(faces.Current())}},
        solidar::TopologyLineageCoverage::PartialFaces, &error);
    if (!lineage) {
      markError(error.empty() ? "Partial lineage fixture is invalid"
                              : std::move(error));
      return false;
    }
    setShapeAndTopologyLineage(std::move(shape), std::move(lineage));
    markValid();
    return true;
  }
};

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

  // Out-of-scope valid profiles remain legacy and retain working v1
  // references. Simple Join/Cut profiles over a CompleteFaces base take the
  // strict boolean history path.
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
    auto baseExtrude = std::make_unique<ExtrudeFeature>(base.id, 5.0);
    auto* baseFeature = baseExtrude.get();
    joinBody.addFeature(std::move(baseExtrude));
    auto& join = joinDocument.addSketch("Join");
    join.geometry.addRectangle({5.0, 0.0}, {15.0, 10.0});
    const auto joinSketchId = join.id;
    const std::array joinGeometryIds{
        join.geometry.lineId(0), join.geometry.lineId(1),
        join.geometry.lineId(2), join.geometry.lineId(3)};
    auto joinExtrude = std::make_unique<ExtrudeFeature>(
        joinSketchId, 5.0, "Join", ExtrudeOperation::Join);
    auto* joinFeature = joinExtrude.get();
    joinBody.addFeature(std::move(joinExtrude));
    CHECK(joinDocument.rebuild());
    CHECK(near(volumeOf(*joinFeature->shape()), 750.0, 1e-4));
    const auto joinReferences = checkStrictBooleanLineage(
        *joinFeature, joinBody.id(), baseFeature->id(), 6);
    CHECK(countProvenanceOperation(*joinFeature,
                                   TopologyProvenanceOperation::Retained) ==
          2);
    CHECK(countProvenanceOperation(*joinFeature,
                                   TopologyProvenanceOperation::Merge) == 4);
    CHECK(std::all_of(
        joinFeature->topologyLineage()->bindings().begin(),
        joinFeature->topologyLineage()->bindings().end(),
        [](const auto& binding) {
          return binding.provenance.operation !=
                     TopologyProvenanceOperation::Merge ||
                 binding.provenance.origins.size() == 2;
        }));

    auto& cut = joinDocument.addSketch("Cut");
    cut.geometry.addRectangle({1.0, 1.0}, {3.0, 3.0});
    auto cutExtrude = std::make_unique<ExtrudeFeature>(
        cut.id, 5.0, "Cut", ExtrudeOperation::Cut);
    auto* cutFeature = cutExtrude.get();
    joinBody.addFeature(std::move(cutExtrude));
    CHECK(joinDocument.rebuild());
    CHECK(near(volumeOf(*cutFeature->shape()), 730.0, 1e-4));
    checkStrictBooleanLineage(*cutFeature, joinBody.id(), joinFeature->id(),
                              10);

    // Saved v2 Join references survive changes to the source extrusion and
    // the authored profile while their semantic faces remain present.
    baseFeature->setLengthMm(8.0);
    joinFeature->setLengthMm(8.0);
    CHECK(joinDocument.rebuild());
    checkReferencesResolve(*joinFeature, joinReferences);

    sketch::Sketch widerJoin;
    widerJoin.addRectangle({4.0, 0.0}, {16.0, 10.0});
    CHECK(widerJoin.restoreGeometryIds(
        {joinGeometryIds[0], joinGeometryIds[1], joinGeometryIds[2],
         joinGeometryIds[3]},
        {}, {}, {}));
    CHECK(joinDocument.replaceSketchGeometry(joinSketchId,
                                              std::move(widerJoin)));
    CHECK(joinDocument.rebuild());
    checkReferencesResolve(*joinFeature, joinReferences);
  }

  // Circle Join/Cut publish every actual output face, retain both object and
  // tool origins, and expose an exact factory-created v2 reference per face.
  {
    Document joinDocument;
    auto& base = joinDocument.addSketch("Circle Join base");
    base.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
    auto& body = joinDocument.addBody();
    auto baseExtrude = std::make_unique<ExtrudeFeature>(base.id, 10.0);
    auto* baseFeature = baseExtrude.get();
    body.addFeature(std::move(baseExtrude));
    auto& circle = joinDocument.addSketch("Circle Join");
    circle.placement.origin.z = 10.0;
    circle.geometry.addCircle({10.0, 10.0}, 5.0);
    auto join = std::make_unique<ExtrudeFeature>(
        circle.id, 5.0, "Circle Join", ExtrudeOperation::Join);
    auto* joinFeature = join.get();
    body.addFeature(std::move(join));
    CHECK(joinDocument.rebuild());
    CHECK(near(volumeOf(*joinFeature->shape()),
               4000.0 + std::numbers::pi * 125.0, 1e-3));
    checkStrictBooleanLineage(*joinFeature, body.id(), baseFeature->id(), 8);
    CHECK(countProvenanceOperation(*joinFeature,
                                   TopologyProvenanceOperation::Retained) ==
          7);
    CHECK(countProvenanceOperation(*joinFeature,
                                   TopologyProvenanceOperation::Modified) ==
          1);
    CHECK(countProvenanceOperation(*joinFeature,
                                   TopologyProvenanceOperation::Merge) == 0);
    checkSingleOriginDistribution(*joinFeature, baseFeature->id(), 6, 2);

    Document cutDocument;
    auto& cutBase = cutDocument.addSketch("Circle Cut base");
    cutBase.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
    auto& cutBody = cutDocument.addBody();
    auto cutBaseExtrude =
        std::make_unique<ExtrudeFeature>(cutBase.id, 10.0);
    auto* cutBaseFeature = cutBaseExtrude.get();
    cutBody.addFeature(std::move(cutBaseExtrude));
    auto& circleCut = cutDocument.addSketch("Circle Cut");
    circleCut.placement.origin.z = 10.0;
    circleCut.geometry.addCircle({10.0, 10.0}, 3.0);
    const auto circleId = circleCut.geometry.circleId(0);
    auto cut = std::make_unique<ExtrudeFeature>(
        circleCut.id, 5.0, "Circle Cut", ExtrudeOperation::Cut, true);
    auto* cutFeature = cut.get();
    cutBody.addFeature(std::move(cut));
    CHECK(cutDocument.rebuild());
    CHECK(near(volumeOf(*cutFeature->shape()),
               4000.0 - std::numbers::pi * 45.0, 1e-3));
    const auto cutReferences = checkStrictBooleanLineage(
        *cutFeature, cutBody.id(), cutBaseFeature->id(), 8);
    CHECK(countProvenanceOperation(*cutFeature,
                                   TopologyProvenanceOperation::Retained) ==
          7);
    CHECK(countProvenanceOperation(*cutFeature,
                                   TopologyProvenanceOperation::Modified) ==
          1);
    CHECK(countProvenanceOperation(*cutFeature,
                                   TopologyProvenanceOperation::Merge) == 0);
    checkSingleOriginDistribution(*cutFeature, cutBaseFeature->id(), 6, 2);

    const auto floorOrigin =
        authoredKey(AuthoredTopologySourceKind::ProfileEndCap, circleCut.id,
                    sketch::kInvalidGeometryId, cutFeature->id())
            .immediateOrigin();
    const auto floorReference = referenceForOrigin(
        *cutFeature, cutBody.id(), floorOrigin);
    cutFeature->setLengthMm(10.0);
    CHECK(cutDocument.rebuild());
    const auto throughResolution =
        cutFeature->topologyIndex()->resolveFace(floorReference);
    CHECK(!throughResolution);
    CHECK(throughResolution.failure == TopologyResolutionFailure::Missing);
    cutFeature->setLengthMm(5.0);
    CHECK(cutDocument.rebuild());
    const auto recoveredFloor =
        cutFeature->topologyIndex()->resolveFace(floorReference);
    CHECK(recoveredFloor);
    CHECK(recoveredFloor.method ==
          TopologyMatchMethod::OperationProvenance);
    checkReferencesResolve(*cutFeature, cutReferences);

    sketch::Sketch resized;
    resized.addCircle({10.0, 10.0}, 4.0);
    CHECK(resized.restoreGeometryIds({}, {circleId}, {}, {}));
    CHECK(cutDocument.replaceSketchGeometry(circleCut.id,
                                             std::move(resized)));
    cutFeature->setLengthMm(6.0);
    cutBaseFeature->setLengthMm(12.0);
    circleCut.placement.origin.z = 12.0;
    CHECK(cutDocument.rebuild());
    checkReferencesResolve(*cutFeature, cutReferences);

    const auto lastShape = cutFeature->shape();
    const auto lastLineage = cutFeature->topologyLineage();
    const auto lastRevision = cutFeature->shapeRevision();
    const auto lastIndex = cutFeature->topologyIndex();
    CHECK(lastIndex);
    sketch::Sketch consuming;
    consuming.addCircle({10.0, 10.0}, 40.0);
    CHECK(consuming.restoreGeometryIds({}, {circleId}, {}, {}));
    CHECK(cutDocument.replaceSketchGeometry(circleCut.id,
                                             std::move(consuming)));
    cutFeature->setLengthMm(12.0);
    CHECK(!cutDocument.rebuild());
    CHECK(!cutFeature->shape());
    CHECK(!cutFeature->topologyLineage());
    CHECK(cutFeature->lastValidShape() == lastShape);
    CHECK(cutFeature->lastValidTopologyLineage() == lastLineage);
    CHECK(cutFeature->shapeRevision() == lastRevision);
    CHECK(cutFeature->lastValidTopologyIndex() == lastIndex);
    CHECK(lastIndex->resolveFace(floorReference));

    sketch::Sketch recovered;
    recovered.addCircle({10.0, 10.0}, 4.0);
    CHECK(recovered.restoreGeometryIds({}, {circleId}, {}, {}));
    CHECK(cutDocument.replaceSketchGeometry(circleCut.id,
                                             std::move(recovered)));
    cutFeature->setLengthMm(6.0);
    CHECK(cutDocument.rebuild());
    checkReferencesResolve(*cutFeature, cutReferences);
  }

  // A strict Cut that splits the current solid fails atomically and recovers
  // with the same authored GeometryIds after the profile is narrowed.
  {
    Document document;
    auto& base = document.addSketch("Split base");
    base.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
    auto& body = document.addBody();
    body.addFeature(std::make_unique<ExtrudeFeature>(base.id, 10.0));
    auto& cutSketch = document.addSketch("Split cut");
    cutSketch.geometry.addRectangle({0.0, 0.0}, {5.0, 5.0});
    const std::array ids{cutSketch.geometry.lineId(0),
                         cutSketch.geometry.lineId(1),
                         cutSketch.geometry.lineId(2),
                         cutSketch.geometry.lineId(3)};
    auto cut = std::make_unique<ExtrudeFeature>(
        cutSketch.id, 10.0, "Split Cut", ExtrudeOperation::Cut);
    auto* cutFeature = cut.get();
    body.addFeature(std::move(cut));
    CHECK(document.rebuild());
    const auto lastShape = cutFeature->shape();
    const auto lastLineage = cutFeature->topologyLineage();
    const auto lastRevision = cutFeature->shapeRevision();
    const auto lastIndex = cutFeature->topologyIndex();
    CHECK(lastIndex);

    sketch::Sketch splitting;
    splitting.addRectangle({9.0, 0.0}, {11.0, 20.0});
    CHECK(splitting.restoreGeometryIds(
        {ids[0], ids[1], ids[2], ids[3]}, {}, {}, {}));
    CHECK(document.replaceSketchGeometry(cutSketch.id,
                                         std::move(splitting)));
    CHECK(!document.rebuild());
    CHECK(!cutFeature->shape());
    CHECK(!cutFeature->topologyLineage());
    CHECK(cutFeature->error() == "Extrude Cut would split the body");
    CHECK(cutFeature->lastValidShape() == lastShape);
    CHECK(cutFeature->lastValidTopologyLineage() == lastLineage);
    CHECK(cutFeature->shapeRevision() == lastRevision);
    CHECK(cutFeature->lastValidTopologyIndex() == lastIndex);

    sketch::Sketch notch;
    notch.addRectangle({0.0, 0.0}, {5.0, 5.0});
    CHECK(notch.restoreGeometryIds({ids[0], ids[1], ids[2], ids[3]}, {}, {},
                                   {}));
    CHECK(document.replaceSketchGeometry(cutSketch.id, std::move(notch)));
    CHECK(document.rebuild());
    CHECK(cutFeature->topologyLineage());
  }

  // Missing lineage intentionally stays on the exact legacy builder, while
  // an advertised but incomplete base lineage fails closed.
  {
    Document importedDocument;
    auto& importedBody = importedDocument.addBody();
    importedBody.addFeature(std::make_unique<ImportedShapeFeature>(
        std::make_shared<TopoDS_Shape>(
            BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape())));
    auto& joinSketch = importedDocument.addSketch("Legacy Join");
    joinSketch.geometry.addRectangle({15.0, 0.0}, {25.0, 20.0});
    auto legacyJoin = std::make_unique<ExtrudeFeature>(
        joinSketch.id, 10.0, "Legacy Join", ExtrudeOperation::Join);
    auto* legacyJoinFeature = legacyJoin.get();
    importedBody.addFeature(std::move(legacyJoin));
    CHECK(importedDocument.rebuild());
    checkLegacyV1(*legacyJoinFeature, importedBody.id());

    Document partialDocument;
    auto& partialBody = partialDocument.addBody();
    partialBody.addFeature(std::make_unique<PartialLineageBoxFeature>());
    auto& partialSketch = partialDocument.addSketch("Partial Join");
    partialSketch.geometry.addRectangle({15.0, 0.0}, {25.0, 20.0});
    auto partialJoin = std::make_unique<ExtrudeFeature>(
        partialSketch.id, 10.0, "Partial Join", ExtrudeOperation::Join);
    auto* partialJoinFeature = partialJoin.get();
    partialBody.addFeature(std::move(partialJoin));
    CHECK(!partialDocument.rebuild());
    CHECK(partialJoinFeature->error() ==
          "Extrude base face lineage is incomplete");
    CHECK(!partialJoinFeature->shape());
    CHECK(!partialJoinFeature->topologyLineage());
  }

  // Strict eligibility is only a selector. Unsupported/malformed profiles
  // retain the exact legacy diagnostics, independent of whether the base
  // advertises CompleteFaces lineage.
  {
    Document strictDocument;
    auto& strictBase = strictDocument.addSketch("Strict diagnostic base");
    strictBase.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
    auto& strictBody = strictDocument.addBody();
    strictBody.addFeature(
        std::make_unique<ExtrudeFeature>(strictBase.id, 10.0));
    auto& strictOpen = strictDocument.addSketch("Strict open profile");
    strictOpen.geometry.addLine({2.0, 2.0}, {8.0, 2.0});
    auto strictJoin = std::make_unique<ExtrudeFeature>(
        strictOpen.id, 5.0, "Strict malformed", ExtrudeOperation::Join);
    auto* strictJoinFeature = strictJoin.get();
    strictBody.addFeature(std::move(strictJoin));
    CHECK(!strictDocument.rebuild());
    const auto strictError = strictJoinFeature->error();
    CHECK(!strictError.empty());

    Document legacyDocument;
    auto& legacyBody = legacyDocument.addBody();
    legacyBody.addFeature(std::make_unique<ImportedShapeFeature>(
        std::make_shared<TopoDS_Shape>(
            BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape())));
    auto& legacyOpen = legacyDocument.addSketch("Legacy open profile");
    legacyOpen.geometry.addLine({2.0, 2.0}, {8.0, 2.0});
    auto legacyJoin = std::make_unique<ExtrudeFeature>(
        legacyOpen.id, 5.0, "Legacy malformed", ExtrudeOperation::Join);
    auto* legacyJoinFeature = legacyJoin.get();
    legacyBody.addFeature(std::move(legacyJoin));
    CHECK(!legacyDocument.rebuild());
    CHECK(legacyJoinFeature->error() == strictError);

    Document overrideDocument;
    auto& overrideBase = overrideDocument.addSketch("Override base");
    overrideBase.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
    auto& overrideBody = overrideDocument.addBody();
    overrideBody.addFeature(
        std::make_unique<ExtrudeFeature>(overrideBase.id, 10.0));
    auto& overrideSketch = overrideDocument.addSketch("Override Join");
    overrideSketch.geometry.addRectangle({15.0, 0.0}, {25.0, 20.0});
    auto overrideJoin = std::make_unique<ExtrudeFeature>(
        overrideSketch.id, 10.0, "Override Join", ExtrudeOperation::Join);
    overrideJoin->setProfileOverride(overrideSketch.geometry);
    auto* overrideJoinFeature = overrideJoin.get();
    overrideBody.addFeature(std::move(overrideJoin));
    CHECK(overrideDocument.rebuild());
    checkLegacyV1(*overrideJoinFeature, overrideBody.id());
  }

  // Forged rebuild contexts must not let a strict Extrude consume any
  // CompleteFaces snapshot except its exact current same-Body predecessor.
  {
    Document document;
    auto& baseSketch = document.addSketch("Context base");
    baseSketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    const auto baseSketchId = baseSketch.id;
    auto& firstJoinSketch = document.addSketch("Context first Join");
    firstJoinSketch.geometry.addRectangle({5.0, 0.0}, {15.0, 10.0});
    const auto firstJoinSketchId = firstJoinSketch.id;
    auto& targetSketch = document.addSketch("Context target Join");
    targetSketch.geometry.addRectangle({10.0, 0.0}, {20.0, 10.0});
    const auto targetSketchId = targetSketch.id;
    auto& foreignSketch = document.addSketch("Context foreign base");
    foreignSketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    const auto foreignSketchId = foreignSketch.id;

    auto& targetBodyRef = document.addBody("Context target body");
    const auto targetBodyId = targetBodyRef.id();
    auto base = std::make_unique<ExtrudeFeature>(baseSketchId, 5.0);
    auto* baseFeature = base.get();
    targetBodyRef.addFeature(std::move(base));
    auto firstJoin = std::make_unique<ExtrudeFeature>(
        firstJoinSketchId, 5.0, "First Join", ExtrudeOperation::Join);
    auto* firstJoinFeature = firstJoin.get();
    targetBodyRef.addFeature(std::move(firstJoin));
    auto target = std::make_unique<ExtrudeFeature>(
        targetSketchId, 5.0, "Target Join", ExtrudeOperation::Join);
    auto* targetFeature = target.get();
    targetBodyRef.addFeature(std::move(target));

    auto& foreignBodyRef = document.addBody("Context foreign body");
    const auto foreignBodyId = foreignBodyRef.id();
    auto foreign =
        std::make_unique<ExtrudeFeature>(foreignSketchId, 5.0);
    auto* foreignFeature = foreign.get();
    foreignBodyRef.addFeature(std::move(foreign));
    CHECK(document.rebuild());

    const auto* targetBody = document.findBody(targetBodyId);
    const auto* foreignBody = document.findBody(foreignBodyId);
    CHECK(targetBody);
    CHECK(foreignBody);
    CHECK(document.findFeature(targetBodyId, baseFeature->id()) ==
          baseFeature);
    CHECK(document.findFeature(targetBodyId, firstJoinFeature->id()) ==
          firstJoinFeature);
    CHECK(document.findFeature(foreignBodyId, foreignFeature->id()) ==
          foreignFeature);

    const auto checkRollback = [&] {
      CHECK(!targetFeature->shape());
      CHECK(!targetFeature->topologyLineage());
      CHECK(targetFeature->error() ==
            "Extrude base Feature is not the immediate predecessor");
    };

    auto validShape = targetFeature->shape();
    auto validLineage = targetFeature->topologyLineage();
    auto validIndex = targetFeature->topologyIndex();
    auto validRevision = targetFeature->shapeRevision();
    targetFeature->setDirty();
    const RebuildContext foreignContext{
        document, targetBody, foreignFeature->shape().get(), foreignFeature};
    CHECK(!targetFeature->rebuild(foreignContext));
    checkRollback();
    CHECK(targetFeature->lastValidShape() == validShape);
    CHECK(targetFeature->lastValidTopologyLineage() == validLineage);
    CHECK(targetFeature->lastValidTopologyIndex() == validIndex);
    CHECK(targetFeature->shapeRevision() == validRevision);

    targetFeature->setDirty();
    const RebuildContext validContext{
        document, targetBody, firstJoinFeature->shape().get(),
        firstJoinFeature};
    CHECK(targetFeature->rebuild(validContext));
    validShape = targetFeature->shape();
    validLineage = targetFeature->topologyLineage();
    validIndex = targetFeature->topologyIndex();
    validRevision = targetFeature->shapeRevision();

    targetFeature->setDirty();
    const RebuildContext earlierContext{
        document, targetBody, baseFeature->shape().get(), baseFeature};
    CHECK(!targetFeature->rebuild(earlierContext));
    checkRollback();
    CHECK(targetFeature->lastValidShape() == validShape);
    CHECK(targetFeature->lastValidTopologyLineage() == validLineage);
    CHECK(targetFeature->lastValidTopologyIndex() == validIndex);
    CHECK(targetFeature->shapeRevision() == validRevision);
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
