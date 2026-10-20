#include "TestAssertions.h"

#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "model/Document.h"
#include "model/RevolveFeature.h"
#include "model/TopologyLineageBuilder.h"
#include "model/TopologyReferenceResolver.h"

namespace {

constexpr double kTolerance = 1.0e-5;

struct SideExpectation {
  solidar::sketch::GeometryId geometryId;
  GeomAbs_SurfaceType surfaceType;
  double measure;
};

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

bool near(double actual, double expected) {
  return std::abs(actual - expected) <= kTolerance;
}

std::vector<TopoDS_Face> canonicalFaces(const TopoDS_Shape& shape) {
  std::vector<TopoDS_Face> result;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next()) {
    const auto candidate = TopoDS::Face(explorer.Current());
    if (std::none_of(result.begin(), result.end(), [&](const auto& existing) {
          return existing.IsSame(candidate);
        }))
      result.push_back(candidate);
  }
  return result;
}

TopoDS_Face resolveAuthoredFace(
    const solidar::RevolveFeature& feature, solidar::BodyId bodyId,
    const solidar::TopologyProvenanceKey& key) {
  const auto index = feature.topologyIndex();
  CHECK(index);
  const auto resolved = index->resolveFace({bodyId, key});
  CHECK(resolved);
  CHECK(resolved.method ==
        solidar::TopologyMatchMethod::OperationProvenance);
  return *resolved.subshape;
}

void checkSideGeometry(const solidar::RevolveFeature& feature,
                       solidar::BodyId bodyId, solidar::SketchId sketchId,
                       const SideExpectation& expected) {
  const auto key = authoredKey(
      solidar::AuthoredTopologySourceKind::GeometrySide, sketchId,
      expected.geometryId, feature.id());
  const auto face = resolveAuthoredFace(feature, bodyId, key);
  BRepAdaptor_Surface surface(face);
  CHECK(surface.GetType() == expected.surfaceType);
  if (expected.surfaceType == GeomAbs_Cylinder)
    CHECK(near(surface.Cylinder().Radius(), expected.measure));
  else {
    CHECK(expected.surfaceType == GeomAbs_Plane);
    CHECK(near(surface.Plane().Location().X(), expected.measure));
  }
}

void checkExactLineage(
    const solidar::RevolveFeature& feature, solidar::BodyId bodyId,
    solidar::SketchId sketchId,
    const std::array<solidar::sketch::GeometryId, 4>& geometryIds,
    bool expectCaps) {
  const auto lineage = feature.topologyLineage();
  CHECK(lineage);
  CHECK(lineage->coverage() ==
        solidar::TopologyLineageCoverage::CompleteFaces);

  std::vector<solidar::TopologyProvenanceKey> expected;
  expected.reserve(expectCaps ? 6U : 4U);
  if (expectCaps) {
    expected.push_back(authoredKey(
        solidar::AuthoredTopologySourceKind::ProfileStartCap, sketchId,
        solidar::sketch::kInvalidGeometryId, feature.id()));
    expected.push_back(authoredKey(
        solidar::AuthoredTopologySourceKind::ProfileEndCap, sketchId,
        solidar::sketch::kInvalidGeometryId, feature.id()));
  }
  for (const auto geometryId : geometryIds) {
    expected.push_back(authoredKey(
        solidar::AuthoredTopologySourceKind::GeometrySide, sketchId,
        geometryId, feature.id()));
  }

  CHECK(lineage->bindings().size() == expected.size());
  for (const auto& binding : lineage->bindings()) {
    CHECK(binding.provenance.operation ==
          solidar::TopologyProvenanceOperation::Seed);
    CHECK(binding.provenance.origins.empty());
    const auto match = std::find(expected.begin(), expected.end(),
                                 binding.provenance);
    CHECK(match != expected.end());
    CHECK(std::count(expected.begin(), expected.end(), binding.provenance) ==
          1);
  }

  const auto index = feature.topologyIndex();
  CHECK(index);
  CHECK(index->faceCount() == expected.size());
  CHECK(index->rawFaceCount() == expected.size());
  CHECK(canonicalFaces(*feature.shape()).size() == expected.size());
  for (const auto& key : expected)
    static_cast<void>(resolveAuthoredFace(feature, bodyId, key));
}

std::vector<solidar::FaceReference> createAllFaceReferences(
    const solidar::RevolveFeature& feature, solidar::BodyId bodyId) {
  const auto index = feature.topologyIndex();
  CHECK(index);
  std::vector<solidar::FaceReference> references;
  references.reserve(index->rawFaceCount());
  for (std::size_t raw = 0; raw < index->rawFaceCount(); ++raw) {
    const auto created = feature.createFaceReferenceV2(bodyId, raw);
    CHECK(created);
    CHECK(created.status == solidar::FaceReferenceV2CreationStatus::Created);
    CHECK(created.reference.v2());
    const auto resolved = index->resolveFace(created.reference);
    CHECK(resolved);
    CHECK(resolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    references.push_back(created.reference);
  }
  return references;
}

const solidar::FaceReference& referenceFor(
    const std::vector<solidar::FaceReference>& references,
    const solidar::TopologyProvenanceKey& key) {
  const auto found = std::find_if(
      references.begin(), references.end(), [&](const auto& reference) {
        const auto* v2 = reference.v2();
        return v2 && v2->provenance == key;
      });
  CHECK(found != references.end());
  return *found;
}

void checkReferenceResolves(const solidar::TopologyIndex& index,
                            const solidar::FaceReference& reference) {
  const auto resolved = index.resolveFace(reference);
  CHECK(resolved);
  CHECK(resolved.method ==
        solidar::TopologyMatchMethod::OperationProvenance);
}

void checkReferenceMissing(const solidar::TopologyIndex& index,
                           const solidar::FaceReference& reference) {
  const auto resolved = index.resolveFace(reference);
  CHECK(!resolved);
  CHECK(!resolved.subshape);
  CHECK(resolved.method == solidar::TopologyMatchMethod::None);
  CHECK(resolved.failure == solidar::TopologyResolutionFailure::Missing);
}

}  // namespace

int main() {
  using namespace solidar;

  Document document;
  auto& profile = document.addSketch("Revolve lineage rectangle");
  profile.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
  const std::array<sketch::GeometryId, 4> geometryIds{
      profile.geometry.lineId(0), profile.geometry.lineId(1),
      profile.geometry.lineId(2), profile.geometry.lineId(3)};
  const std::array<SideExpectation, 4> expectedSides{{
      {geometryIds[0], GeomAbs_Cylinder, 5.0},
      {geometryIds[1], GeomAbs_Plane, 30.0},
      {geometryIds[2], GeomAbs_Cylinder, 15.0},
      {geometryIds[3], GeomAbs_Plane, 10.0},
  }};

  auto& body = document.addBody("Revolve lineage body");
  auto revolve = std::make_unique<RevolveFeature>(
      profile.id,
      AxisReference{AxisReferenceType::SketchHorizontalAxis, profile.id,
                    sketch::kInvalidGeometryId},
      120.0, "Revolve lineage", ExtrudeOperation::NewBody, false);
  auto* feature = revolve.get();
  body.addFeature(std::move(revolve));

  // The production NewBody path publishes exactly two caps and one side for
  // each authored rectangle edge. Every raw face can create only a v2
  // operation-provenance reference.
  CHECK(document.recompute());
  checkExactLineage(*feature, body.id(), profile.id, geometryIds, true);
  for (const auto& expected : expectedSides)
    checkSideGeometry(*feature, body.id(), profile.id, expected);
  const auto references = createAllFaceReferences(*feature, body.id());
  CHECK(references.size() == 6);
  const auto startKey = authoredKey(
      AuthoredTopologySourceKind::ProfileStartCap, profile.id,
      sketch::kInvalidGeometryId, feature->id());
  const auto endKey = authoredKey(
      AuthoredTopologySourceKind::ProfileEndCap, profile.id,
      sketch::kInvalidGeometryId, feature->id());
  const auto& startReference = referenceFor(references, startKey);
  const auto& endReference = referenceFor(references, endKey);

  // Closing the sweep removes only the cap identities. All four authored
  // sides survive through their GeometryIds.
  feature->setAngleDeg(360.0);
  CHECK(document.recompute());
  checkExactLineage(*feature, body.id(), profile.id, geometryIds, false);
  const auto fullIndex = feature->topologyIndex();
  CHECK(fullIndex);
  checkReferenceMissing(*fullIndex, startReference);
  checkReferenceMissing(*fullIndex, endReference);
  for (const auto& expected : expectedSides) {
    checkSideGeometry(*feature, body.id(), profile.id, expected);
    const auto key = authoredKey(AuthoredTopologySourceKind::GeometrySide,
                                 profile.id, expected.geometryId,
                                 feature->id());
    checkReferenceResolves(*fullIndex, referenceFor(references, key));
  }

  // Reversing a partial sweep does not change authored identities.
  feature->setAngleDeg(120.0);
  feature->setReversed(true);
  CHECK(document.recompute());
  checkExactLineage(*feature, body.id(), profile.id, geometryIds, true);
  auto reversedIndex = feature->topologyIndex();
  CHECK(reversedIndex);
  for (const auto& reference : references)
    checkReferenceResolves(*reversedIndex, reference);
  for (const auto& expected : expectedSides)
    checkSideGeometry(*feature, body.id(), profile.id, expected);

  // Reorder and reverse every line, but restore the authored IDs. Geometry
  // mapping and all existing v2 references must remain independent of array
  // and wire traversal order.
  sketch::Sketch reordered;
  reordered.addLine({10.0, 15.0}, {30.0, 15.0});
  reordered.addLine({30.0, 5.0}, {10.0, 5.0});
  reordered.addLine({10.0, 5.0}, {10.0, 15.0});
  reordered.addLine({30.0, 15.0}, {30.0, 5.0});
  CHECK(reordered.restoreGeometryIds(
      {geometryIds[2], geometryIds[0], geometryIds[3], geometryIds[1]},
      {}, {}, {}));
  CHECK(document.replaceSketchGeometry(profile.id, std::move(reordered)));
  CHECK(document.recompute());
  checkExactLineage(*feature, body.id(), profile.id, geometryIds, true);
  const auto reorderedIndex = feature->topologyIndex();
  CHECK(reorderedIndex);
  for (const auto& reference : references)
    checkReferenceResolves(*reorderedIndex, reference);
  for (const auto& expected : expectedSides)
    checkSideGeometry(*feature, body.id(), profile.id, expected);

  // A failed rebuild exposes no current shape, lineage, or index, while the
  // complete prior snapshot and its references remain available atomically.
  const auto lastShape = feature->shape();
  const auto lastLineage = feature->topologyLineage();
  const auto lastIndex = feature->topologyIndex();
  const auto lastRevision = feature->shapeRevision();
  CHECK(lastShape);
  CHECK(lastLineage);
  CHECK(lastIndex);
  feature->setAngleDeg(0.0);
  CHECK(!document.recompute());
  CHECK(!feature->shape());
  CHECK(!feature->topologyLineage());
  CHECK(feature->shapeRevision() == lastRevision);
  std::string error;
  CHECK(!feature->topologyIndex(&error));
  CHECK(!error.empty());
  CHECK(feature->lastValidShape() == lastShape);
  CHECK(feature->lastValidTopologyLineage() == lastLineage);
  CHECK(feature->lastValidTopologyIndex(&error) == lastIndex);
  CHECK(error.empty());
  for (const auto& reference : references)
    checkReferenceResolves(*lastIndex, reference);

  feature->setAngleDeg(120.0);
  CHECK(document.recompute());
  CHECK(feature->shapeRevision() > lastRevision);
  checkExactLineage(*feature, body.id(), profile.id, geometryIds, true);
  const auto recoveredIndex = feature->topologyIndex();
  CHECK(recoveredIndex);
  for (const auto& reference : references)
    checkReferenceResolves(*recoveredIndex, reference);

  // An explicit picked-region override remains on the legacy NewBody path:
  // it is valid but publishes neither lineage nor v2 references.
  Document overrideDocument;
  auto& overrideProfile = overrideDocument.addSketch("Legacy override");
  overrideProfile.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
  auto& overrideBody = overrideDocument.addBody("Legacy override body");
  auto overrideRevolve = std::make_unique<RevolveFeature>(
      overrideProfile.id,
      AxisReference{AxisReferenceType::SketchHorizontalAxis,
                    overrideProfile.id, sketch::kInvalidGeometryId},
      120.0, "Legacy override", ExtrudeOperation::NewBody, false);
  auto* overrideFeature = overrideRevolve.get();
  overrideFeature->setProfileOverride(overrideProfile.geometry);
  overrideBody.addFeature(std::move(overrideRevolve));
  CHECK(overrideDocument.recompute());
  CHECK(overrideFeature->shape());
  CHECK(!overrideFeature->topologyLineage());
  const auto overrideIndex = overrideFeature->topologyIndex();
  CHECK(overrideIndex);
  CHECK(overrideIndex->rawFaceCount() > 0);
  const auto unavailable =
      overrideFeature->createFaceReferenceV2(overrideBody.id(), 0);
  CHECK(!unavailable);
  CHECK(unavailable.status == FaceReferenceV2CreationStatus::Missing);
  CHECK(!unavailable.reference.v2());

  return EXIT_SUCCESS;
}
