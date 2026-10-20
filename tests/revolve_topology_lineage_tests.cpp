#include "TestAssertions.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "model/Document.h"
#include "model/ImportedShapeFeature.h"
#include "model/OcctBooleanTopologyHistoryCapture.h"
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

  CHECK(static_cast<std::size_t>(std::count_if(
            lineage->bindings().begin(), lineage->bindings().end(),
            [](const auto& binding) {
              return binding.provenance.kind == solidar::TopologyKind::Face;
            })) == expected.size());
  for (const auto& binding : lineage->bindings()) {
    if (binding.provenance.kind != solidar::TopologyKind::Face) continue;
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

solidar::TopologyProvenanceOrigin authoredOrigin(
    solidar::AuthoredTopologySourceKind kind, solidar::SketchId sketchId,
    solidar::sketch::GeometryId geometryId,
    solidar::FeatureId featureId) {
  return authoredKey(kind, sketchId, geometryId, featureId).immediateOrigin();
}

enum class BooleanOracleOperation { Fuse, Cut };
enum class GeometryOracleKind { CylinderRadius, PlaneX, FaceCenter };

struct FaceGeometryOracle {
  GeomAbs_SurfaceType surfaceType;
  GeometryOracleKind kind;
  std::array<double, 3> measure;
};

FaceGeometryOracle cylinder(double radius) {
  return {GeomAbs_Cylinder, GeometryOracleKind::CylinderRadius,
          {radius, 0.0, 0.0}};
}

FaceGeometryOracle planeX(double x) {
  return {GeomAbs_Plane, GeometryOracleKind::PlaneX, {x, 0.0, 0.0}};
}

FaceGeometryOracle planeCenter(double x, double y, double z) {
  return {GeomAbs_Plane, GeometryOracleKind::FaceCenter, {x, y, z}};
}

void checkFaceGeometry(const TopoDS_Shape& shape,
                       const FaceGeometryOracle& expected) {
  CHECK(shape.ShapeType() == TopAbs_FACE);
  const auto face = TopoDS::Face(shape);
  BRepAdaptor_Surface surface(face);
  CHECK(surface.GetType() == expected.surfaceType);
  if (expected.kind == GeometryOracleKind::CylinderRadius) {
    CHECK(near(surface.Cylinder().Radius(), expected.measure[0]));
  } else if (expected.kind == GeometryOracleKind::PlaneX) {
    CHECK(near(surface.Plane().Location().X(), expected.measure[0]));
  } else {
    GProp_GProps properties;
    BRepGProp::SurfaceProperties(face, properties);
    const auto center = properties.CentreOfMass();
    CHECK(near(center.X(), expected.measure[0]));
    CHECK(near(center.Y(), expected.measure[1]));
    CHECK(near(center.Z(), expected.measure[2]));
  }
}

struct ExpectedBooleanOrigin {
  solidar::TopologyProvenanceOrigin origin;
  solidar::BooleanOperandRole operand;
};

struct ExpectedBooleanFace {
  solidar::TopologyProvenanceOperation operation;
  std::vector<ExpectedBooleanOrigin> origins;
  FaceGeometryOracle geometry;
};

ExpectedBooleanOrigin objectOrigin(
    solidar::TopologyProvenanceOrigin origin) {
  return {std::move(origin), solidar::BooleanOperandRole::Object};
}

ExpectedBooleanOrigin toolOrigin(solidar::TopologyProvenanceOrigin origin) {
  return {std::move(origin), solidar::BooleanOperandRole::Tool};
}

bool oracleOriginLess(const ExpectedBooleanOrigin& left,
                      const ExpectedBooleanOrigin& right) {
  const auto occurrence = [](const auto& value) {
    return std::tuple{value.occurrence.has_value(),
                      value.occurrence ? value.occurrence->featureId : 0,
                      value.occurrence ? value.occurrence->slot : 0};
  };
  const auto& lhs = left.origin;
  const auto& rhs = right.origin;
  const auto leftIdentity =
      std::tie(lhs.featureId, lhs.kind, lhs.operation, lhs.role, lhs.localName);
  const auto rightIdentity =
      std::tie(rhs.featureId, rhs.kind, rhs.operation, rhs.role, rhs.localName);
  if (leftIdentity != rightIdentity) return leftIdentity < rightIdentity;
  return occurrence(lhs) < occurrence(rhs);
}

void appendOracleField(std::string* result, std::string_view value) {
  *result += std::to_string(value.size());
  result->push_back(':');
  result->append(value);
  result->push_back(';');
}

void appendOracleOrigin(std::string* result,
                        const ExpectedBooleanOrigin& expected,
                        BooleanOracleOperation operation) {
  const auto& origin = expected.origin;
  appendOracleField(result, std::to_string(origin.featureId));
  appendOracleField(result,
                    std::to_string(static_cast<unsigned>(origin.kind)));
  appendOracleField(result,
                    std::to_string(static_cast<unsigned>(origin.operation)));
  appendOracleField(result,
                    std::to_string(static_cast<unsigned>(origin.role)));
  appendOracleField(result, origin.localName);
  appendOracleField(result, origin.occurrence ? "1" : "0");
  if (origin.occurrence) {
    appendOracleField(result,
                      std::to_string(origin.occurrence->featureId));
    appendOracleField(result, std::to_string(origin.occurrence->slot));
  }
  if (operation == BooleanOracleOperation::Cut)
    appendOracleField(
        result, expected.operand == solidar::BooleanOperandRole::Object
                    ? "object"
                    : "tool");
}

solidar::TopologyProvenanceKey expectedBooleanKey(
    solidar::FeatureId producerFeatureId, BooleanOracleOperation operation,
    const ExpectedBooleanFace& expected) {
  auto canonical = expected.origins;
  std::sort(canonical.begin(), canonical.end(), oracleOriginLess);
  std::vector<solidar::TopologyProvenanceOrigin> origins;
  origins.reserve(canonical.size());
  std::string localName = "boolean-face:v1;";
  appendOracleField(&localName,
                    operation == BooleanOracleOperation::Cut ? "cut"
                                                             : "fuse");
  appendOracleField(
      &localName,
      std::to_string(static_cast<unsigned>(expected.operation)));
  appendOracleField(&localName, std::to_string(canonical.size()));
  for (const auto& origin : canonical) {
    origins.push_back(origin.origin);
    appendOracleOrigin(&localName, origin, operation);
  }
  return {producerFeatureId, solidar::TopologyKind::Face,
          expected.operation, solidar::TopologySemanticRole::BooleanResult,
          std::move(localName), std::move(origins)};
}

std::vector<TopoDS_Face> rawFaces(const TopoDS_Shape& shape) {
  std::vector<TopoDS_Face> result;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next())
    result.push_back(TopoDS::Face(explorer.Current()));
  return result;
}

std::vector<solidar::FaceReference> checkExactBooleanLineage(
    const solidar::RevolveFeature& feature, solidar::BodyId bodyId,
    BooleanOracleOperation operation,
    const std::vector<ExpectedBooleanFace>& expected) {
  std::vector<solidar::TopologyProvenanceKey> expectedKeys;
  expectedKeys.reserve(expected.size());
  for (const auto& item : expected)
    expectedKeys.push_back(expectedBooleanKey(feature.id(), operation, item));
  for (std::size_t index = 0; index < expectedKeys.size(); ++index)
    CHECK(std::count(expectedKeys.begin(), expectedKeys.end(),
                     expectedKeys[index]) == 1);

  const auto lineage = feature.topologyLineage();
  CHECK(lineage);
  CHECK(lineage->coverage() ==
        solidar::TopologyLineageCoverage::CompleteFaces);
  CHECK(lineage->bindings().size() == expected.size());
  const auto index = feature.topologyIndex();
  CHECK(index);
  CHECK(index->faceCount() == expected.size());
  CHECK(index->rawFaceCount() == expected.size());
  const auto outputFaces = canonicalFaces(*feature.shape());
  CHECK(outputFaces.size() == expected.size());
  const auto traversalFaces = rawFaces(*feature.shape());
  CHECK(traversalFaces.size() == expected.size());

  std::vector<bool> consumedBindings(expected.size(), false);
  for (const auto& binding : lineage->bindings()) {
    const auto match = std::find(expectedKeys.begin(), expectedKeys.end(),
                                 binding.provenance);
    CHECK(match != expectedKeys.end());
    const auto expectedIndex =
        static_cast<std::size_t>(match - expectedKeys.begin());
    CHECK(!consumedBindings[expectedIndex]);
    consumedBindings[expectedIndex] = true;
    CHECK(binding.subshape);
    CHECK(std::any_of(outputFaces.begin(), outputFaces.end(),
                      [&](const auto& face) {
                        return face.IsSame(*binding.subshape);
                      }));
    checkFaceGeometry(*binding.subshape, expected[expectedIndex].geometry);
  }
  CHECK(std::all_of(consumedBindings.begin(), consumedBindings.end(),
                    [](bool value) { return value; }));

  std::vector<solidar::FaceReference> references;
  references.reserve(index->rawFaceCount());
  std::vector<bool> consumedRaw(expected.size(), false);
  for (std::size_t raw = 0; raw < index->rawFaceCount(); ++raw) {
    auto created = feature.createFaceReferenceV2(bodyId, raw);
    CHECK(created);
    CHECK(created.status ==
          solidar::FaceReferenceV2CreationStatus::Created);
    const auto* v2 = created.reference.v2();
    CHECK(v2);
    CHECK(v2->bodyId == bodyId);
    const auto expectedKey =
        std::find(expectedKeys.begin(), expectedKeys.end(), v2->provenance);
    CHECK(expectedKey != expectedKeys.end());
    const auto expectedIndex =
        static_cast<std::size_t>(expectedKey - expectedKeys.begin());
    CHECK(!consumedRaw[expectedIndex]);
    consumedRaw[expectedIndex] = true;
    const auto resolved = index->resolveFace(created.reference);
    CHECK(resolved);
    CHECK(resolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    CHECK(resolved.subshape->IsSame(traversalFaces[raw]));
    checkFaceGeometry(traversalFaces[raw], expected[expectedIndex].geometry);
    references.push_back(std::move(created.reference));
  }
  CHECK(std::all_of(consumedRaw.begin(), consumedRaw.end(),
                    [](bool value) { return value; }));
  return references;
}

void checkReferencesResolve(
    const solidar::RevolveFeature& feature,
    solidar::BodyId bodyId, BooleanOracleOperation operation,
    const std::vector<ExpectedBooleanFace>& expected,
    const std::vector<solidar::FaceReference>& references) {
  const auto index = feature.topologyIndex();
  CHECK(index);
  std::vector<solidar::TopologyProvenanceKey> expectedKeys;
  for (const auto& item : expected)
    expectedKeys.push_back(expectedBooleanKey(feature.id(), operation, item));
  std::vector<bool> consumed(expected.size(), false);
  for (const auto& reference : references) {
    const auto* v2 = reference.v2();
    CHECK(v2);
    CHECK(v2->bodyId == bodyId);
    const auto match = std::find(expectedKeys.begin(), expectedKeys.end(),
                                 v2->provenance);
    CHECK(match != expectedKeys.end());
    const auto expectedIndex =
        static_cast<std::size_t>(match - expectedKeys.begin());
    CHECK(!consumed[expectedIndex]);
    consumed[expectedIndex] = true;
    const auto resolved = index->resolveFace(reference);
    CHECK(resolved);
    CHECK(resolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    checkFaceGeometry(*resolved.subshape, expected[expectedIndex].geometry);
  }
}

void checkLegacyLineageUnavailable(const solidar::RevolveFeature& feature,
                                   solidar::BodyId bodyId) {
  CHECK(feature.shape());
  CHECK(!feature.topologyLineage());
  const auto index = feature.topologyIndex();
  CHECK(index);
  CHECK(index->rawFaceCount() > 0);
  const auto created = feature.createFaceReferenceV2(bodyId, 0);
  CHECK(!created);
  CHECK(created.status == solidar::FaceReferenceV2CreationStatus::Missing);
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
        BRepPrimAPI_MakeBox(40.0, 40.0, 40.0).Shape());
    TopExp_Explorer faces(*shape, TopAbs_FACE);
    CHECK(faces.More());
    solidar::TopologyProvenanceKey key{
        id(), solidar::TopologyKind::Face,
        solidar::TopologyProvenanceOperation::Seed,
        solidar::TopologySemanticRole::Custom, "partial-face"};
    std::string error;
    auto lineage = solidar::TopologyLineage::create(
        {{std::move(key), std::make_shared<TopoDS_Shape>(faces.Current())}},
        solidar::TopologyLineageCoverage::PartialFaces, &error);
    CHECK(lineage);
    setShapeAndTopologyLineage(std::move(shape), std::move(lineage));
    markValid();
    return true;
  }
};

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
  CHECK(overrideIndex->rawEdgeCount() > 0);
  const auto unavailableEdge =
      overrideFeature->createEdgeReferenceV2(overrideBody.id(), 0);
  CHECK(!unavailableEdge);
  CHECK(unavailableEdge.status == EdgeReferenceV2CreationStatus::Missing);
  CHECK(!unavailableEdge.reference.v2());

  // Full coaxial Join removes the shared cylindrical interface and publishes
  // exactly the two retained cylinders plus the two merged end planes.
  {
    Document oracle;
    auto& baseProfile = oracle.addSketch("Boolean Join base");
    baseProfile.geometry.addRectangle({10.0, 5.0}, {30.0, 10.0});
    const auto baseSketchId = baseProfile.id;
    const std::array baseIds{baseProfile.geometry.lineId(0),
                             baseProfile.geometry.lineId(1),
                             baseProfile.geometry.lineId(2),
                             baseProfile.geometry.lineId(3)};
    auto& oracleBody = oracle.addBody();
    auto base = std::make_unique<RevolveFeature>(
        baseProfile.id,
        AxisReference{AxisReferenceType::SketchHorizontalAxis,
                      baseProfile.id, sketch::kInvalidGeometryId},
        360.0, "Base", ExtrudeOperation::NewBody, false);
    auto* basePtr = base.get();
    oracleBody.addFeature(std::move(base));
    auto& toolProfile = oracle.addSketch("Boolean Join tool");
    toolProfile.geometry.addRectangle({10.0, 10.0}, {30.0, 15.0});
    const auto toolSketchId = toolProfile.id;
    const std::array toolIds{toolProfile.geometry.lineId(0),
                             toolProfile.geometry.lineId(1),
                             toolProfile.geometry.lineId(2),
                             toolProfile.geometry.lineId(3)};
    auto join = std::make_unique<RevolveFeature>(
        toolProfile.id,
        AxisReference{AxisReferenceType::SketchHorizontalAxis,
                      toolProfile.id, sketch::kInvalidGeometryId},
        360.0, "Join", ExtrudeOperation::Join, false);
    auto* joinPtr = join.get();
    oracleBody.addFeature(std::move(join));
    CHECK(oracle.recompute());
    const auto baseSide = [&](std::size_t index) {
      return authoredOrigin(AuthoredTopologySourceKind::GeometrySide,
                            baseSketchId, baseIds[index], basePtr->id());
    };
    const auto toolSide = [&](std::size_t index) {
      return authoredOrigin(AuthoredTopologySourceKind::GeometrySide,
                            toolSketchId, toolIds[index], joinPtr->id());
    };
    const auto fullExpected = [&](double outerRadius) {
      return std::vector<ExpectedBooleanFace>{
          {TopologyProvenanceOperation::Retained,
           {objectOrigin(baseSide(0))}, cylinder(5.0)},
          {TopologyProvenanceOperation::Merge,
           {objectOrigin(baseSide(3)), toolOrigin(toolSide(3))},
           planeX(10.0)},
          {TopologyProvenanceOperation::Merge,
           {objectOrigin(baseSide(1)), toolOrigin(toolSide(1))},
           planeX(30.0)},
          {TopologyProvenanceOperation::Retained,
           {toolOrigin(toolSide(2))}, cylinder(outerRadius)},
      };
    };
    const auto fullReferences = checkExactBooleanLineage(
        *joinPtr, oracleBody.id(), BooleanOracleOperation::Fuse,
        fullExpected(15.0));
    CHECK(fullReferences.size() == 4);

    basePtr->setAngleDeg(120.0);
    joinPtr->setAngleDeg(120.0);
    CHECK(oracle.recompute());
    const auto baseCap = [&](AuthoredTopologySourceKind kind) {
      return authoredOrigin(kind, baseSketchId, sketch::kInvalidGeometryId,
                            basePtr->id());
    };
    const auto toolCap = [&](AuthoredTopologySourceKind kind) {
      return authoredOrigin(kind, toolSketchId, sketch::kInvalidGeometryId,
                            joinPtr->id());
    };
    const auto partialExpected = [&](bool reversed) {
      const double endZ = reversed ? -8.660254037844386 : 8.660254037844386;
      return std::vector<ExpectedBooleanFace>{
          {TopologyProvenanceOperation::Retained,
           {objectOrigin(baseSide(0))}, cylinder(5.0)},
          {TopologyProvenanceOperation::Merge,
           {objectOrigin(
                baseCap(AuthoredTopologySourceKind::ProfileEndCap)),
            toolOrigin(toolCap(AuthoredTopologySourceKind::ProfileEndCap))},
           planeCenter(20.0, -5.0, endZ)},
          {TopologyProvenanceOperation::Retained,
           {toolOrigin(toolSide(2))}, cylinder(15.0)},
          {TopologyProvenanceOperation::Merge,
           {objectOrigin(
                baseCap(AuthoredTopologySourceKind::ProfileStartCap)),
            toolOrigin(toolCap(AuthoredTopologySourceKind::ProfileStartCap))},
           planeCenter(20.0, 10.0, 0.0)},
          {TopologyProvenanceOperation::Merge,
           {objectOrigin(baseSide(1)), toolOrigin(toolSide(1))},
           planeX(30.0)},
          {TopologyProvenanceOperation::Merge,
           {objectOrigin(baseSide(3)), toolOrigin(toolSide(3))},
           planeX(10.0)},
      };
    };
    const auto partialReferences = checkExactBooleanLineage(
        *joinPtr, oracleBody.id(), BooleanOracleOperation::Fuse,
        partialExpected(false));
    checkReferencesResolve(*joinPtr, oracleBody.id(),
                           BooleanOracleOperation::Fuse,
                           partialExpected(false), fullReferences);
    basePtr->setReversed(true);
    joinPtr->setReversed(true);
    CHECK(oracle.recompute());
    checkExactBooleanLineage(*joinPtr, oracleBody.id(),
                             BooleanOracleOperation::Fuse,
                             partialExpected(true));
    checkReferencesResolve(*joinPtr, oracleBody.id(),
                           BooleanOracleOperation::Fuse,
                           partialExpected(true), partialReferences);

    // A contained Join is a semantic no-op. Failure preserves the exact last
    // valid shape/lineage/index/revision, and restoring the authored IDs
    // recovers all stable full-sweep references.
    basePtr->setAngleDeg(360.0);
    joinPtr->setAngleDeg(360.0);
    basePtr->setReversed(false);
    joinPtr->setReversed(false);
    CHECK(oracle.recompute());
    checkExactBooleanLineage(*joinPtr, oracleBody.id(),
                             BooleanOracleOperation::Fuse,
                             fullExpected(15.0));
    const auto lastShape = joinPtr->shape();
    const auto lastLineage = joinPtr->topologyLineage();
    const auto lastIndex = joinPtr->topologyIndex();
    const auto lastRevision = joinPtr->shapeRevision();
    sketch::Sketch contained;
    contained.addRectangle({10.0, 6.0}, {30.0, 9.0});
    CHECK(contained.restoreGeometryIds(
        {toolIds[0], toolIds[1], toolIds[2], toolIds[3]}, {}, {}, {}));
    CHECK(oracle.replaceSketchGeometry(toolSketchId, std::move(contained)));
    CHECK(!oracle.recompute());
    CHECK(joinPtr->error() == "Revolve Join does not intersect the body");
    CHECK(!joinPtr->shape());
    CHECK(!joinPtr->topologyLineage());
    CHECK(joinPtr->lastValidShape() == lastShape);
    CHECK(joinPtr->lastValidTopologyLineage() == lastLineage);
    CHECK(joinPtr->lastValidTopologyIndex() == lastIndex);
    CHECK(joinPtr->shapeRevision() == lastRevision);
    sketch::Sketch restored;
    restored.addLine({10.0, 17.0}, {30.0, 17.0});
    restored.addLine({30.0, 10.0}, {10.0, 10.0});
    restored.addLine({10.0, 10.0}, {10.0, 17.0});
    restored.addLine({30.0, 17.0}, {30.0, 10.0});
    CHECK(restored.restoreGeometryIds(
        {toolIds[2], toolIds[0], toolIds[3], toolIds[1]}, {}, {}, {}));
    CHECK(oracle.replaceSketchGeometry(toolSketchId, std::move(restored)));
    CHECK(oracle.recompute());
    checkExactBooleanLineage(*joinPtr, oracleBody.id(),
                             BooleanOracleOperation::Fuse,
                             fullExpected(17.0));
    checkReferencesResolve(*joinPtr, oracleBody.id(),
                           BooleanOracleOperation::Fuse,
                           fullExpected(17.0), fullReferences);
  }

  // Full and partial coaxial Cut have fixed live OCCT history semantics.
  // Shared outer/interface faces are excluded; the new outer cylinder comes
  // from the tool while the end planes are Modified object faces.
  {
    Document cutOracle;
    auto& cutBaseProfile = cutOracle.addSketch("Cut base");
    cutBaseProfile.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
    const auto baseSketchId = cutBaseProfile.id;
    const std::array baseIds{cutBaseProfile.geometry.lineId(0),
                             cutBaseProfile.geometry.lineId(1),
                             cutBaseProfile.geometry.lineId(2),
                             cutBaseProfile.geometry.lineId(3)};
    auto& cutBody = cutOracle.addBody();
    auto cutBase = std::make_unique<RevolveFeature>(
        cutBaseProfile.id,
        AxisReference{AxisReferenceType::SketchHorizontalAxis,
                      cutBaseProfile.id, sketch::kInvalidGeometryId},
        360.0, "Cut base", ExtrudeOperation::NewBody, false);
    auto* cutBasePtr = cutBase.get();
    cutBody.addFeature(std::move(cutBase));
    auto& cutToolProfile = cutOracle.addSketch("Cut tool");
    cutToolProfile.geometry.addRectangle({10.0, 10.0}, {30.0, 15.0});
    const auto toolSketchId = cutToolProfile.id;
    const std::array toolIds{cutToolProfile.geometry.lineId(0),
                             cutToolProfile.geometry.lineId(1),
                             cutToolProfile.geometry.lineId(2),
                             cutToolProfile.geometry.lineId(3)};
    auto cut = std::make_unique<RevolveFeature>(
        cutToolProfile.id,
        AxisReference{AxisReferenceType::SketchHorizontalAxis,
                      cutToolProfile.id, sketch::kInvalidGeometryId},
        360.0, "Cut", ExtrudeOperation::Cut, false);
    auto* cutPtr = cut.get();
    cutBody.addFeature(std::move(cut));
    CHECK(cutOracle.recompute());
    const auto baseSide = [&](std::size_t index) {
      return authoredOrigin(AuthoredTopologySourceKind::GeometrySide,
                            baseSketchId, baseIds[index], cutBasePtr->id());
    };
    const auto toolSide = [&](std::size_t index) {
      return authoredOrigin(AuthoredTopologySourceKind::GeometrySide,
                            toolSketchId, toolIds[index], cutPtr->id());
    };
    const std::vector<ExpectedBooleanFace> fullExpected{
        {TopologyProvenanceOperation::Retained,
         {objectOrigin(baseSide(0))}, cylinder(5.0)},
        {TopologyProvenanceOperation::Modified,
         {objectOrigin(baseSide(3))}, planeX(10.0)},
        {TopologyProvenanceOperation::Modified,
         {objectOrigin(baseSide(1))}, planeX(30.0)},
        {TopologyProvenanceOperation::Retained,
         {toolOrigin(toolSide(0))}, cylinder(10.0)},
    };
    const auto fullReferences = checkExactBooleanLineage(
        *cutPtr, cutBody.id(), BooleanOracleOperation::Cut, fullExpected);
    cutBasePtr->setAngleDeg(120.0);
    cutPtr->setAngleDeg(120.0);
    CHECK(cutOracle.recompute());
    const auto baseCap = [&](AuthoredTopologySourceKind kind) {
      return authoredOrigin(kind, baseSketchId,
                            sketch::kInvalidGeometryId, cutBasePtr->id());
    };
    const auto partialExpected = [&](bool reversed) {
      const double endZ = reversed ? -6.49519052838329 : 6.49519052838329;
      return std::vector<ExpectedBooleanFace>{
          {TopologyProvenanceOperation::Retained,
           {objectOrigin(baseSide(0))}, cylinder(5.0)},
          {TopologyProvenanceOperation::Modified,
           {objectOrigin(baseSide(3))}, planeX(10.0)},
          {TopologyProvenanceOperation::Modified,
           {objectOrigin(baseSide(1))}, planeX(30.0)},
          {TopologyProvenanceOperation::Modified,
           {objectOrigin(
               baseCap(AuthoredTopologySourceKind::ProfileStartCap))},
           planeCenter(20.0, 7.5, 0.0)},
          {TopologyProvenanceOperation::Modified,
           {objectOrigin(baseCap(AuthoredTopologySourceKind::ProfileEndCap))},
           planeCenter(20.0, -3.75, endZ)},
          {TopologyProvenanceOperation::Retained,
           {toolOrigin(toolSide(0))}, cylinder(10.0)},
      };
    };
    const auto partialReferences = checkExactBooleanLineage(
        *cutPtr, cutBody.id(), BooleanOracleOperation::Cut,
        partialExpected(false));
    checkReferencesResolve(*cutPtr, cutBody.id(),
                           BooleanOracleOperation::Cut,
                           partialExpected(false), fullReferences);
    cutBasePtr->setReversed(true);
    cutPtr->setReversed(true);
    CHECK(cutOracle.recompute());
    checkExactBooleanLineage(*cutPtr, cutBody.id(),
                             BooleanOracleOperation::Cut,
                             partialExpected(true));
    checkReferencesResolve(*cutPtr, cutBody.id(),
                           BooleanOracleOperation::Cut,
                           partialExpected(true), partialReferences);

    // A disjoint Cut is a no-op and a radial slice across the whole section
    // would split the body. Both fail atomically and recover.
    cutBasePtr->setAngleDeg(360.0);
    cutPtr->setAngleDeg(360.0);
    cutBasePtr->setReversed(false);
    cutPtr->setReversed(false);
    CHECK(cutOracle.recompute());
    checkExactBooleanLineage(*cutPtr, cutBody.id(),
                             BooleanOracleOperation::Cut, fullExpected);
    const auto lastShape = cutPtr->shape();
    const auto lastLineage = cutPtr->topologyLineage();
    const auto lastIndex = cutPtr->topologyIndex();
    const auto lastRevision = cutPtr->shapeRevision();
    sketch::Sketch disjoint;
    disjoint.addRectangle({10.0, 20.0}, {30.0, 25.0});
    CHECK(disjoint.restoreGeometryIds(
        {toolIds[0], toolIds[1], toolIds[2], toolIds[3]}, {}, {}, {}));
    CHECK(cutOracle.replaceSketchGeometry(toolSketchId, std::move(disjoint)));
    CHECK(!cutOracle.recompute());
    CHECK(cutPtr->error() == "Revolve Cut does not intersect the body");
    CHECK(!cutPtr->shape());
    CHECK(!cutPtr->topologyLineage());
    CHECK(cutPtr->lastValidShape() == lastShape);
    CHECK(cutPtr->lastValidTopologyLineage() == lastLineage);
    CHECK(cutPtr->lastValidTopologyIndex() == lastIndex);
    CHECK(cutPtr->shapeRevision() == lastRevision);

    sketch::Sketch splitting;
    splitting.addRectangle({19.0, 5.0}, {21.0, 15.0});
    CHECK(splitting.restoreGeometryIds(
        {toolIds[0], toolIds[1], toolIds[2], toolIds[3]}, {}, {}, {}));
    CHECK(cutOracle.replaceSketchGeometry(toolSketchId, std::move(splitting)));
    CHECK(!cutOracle.recompute());
    CHECK(cutPtr->error() == "Revolve Cut would split the body");
    CHECK(!cutPtr->shape());
    CHECK(!cutPtr->topologyLineage());
    CHECK(cutPtr->lastValidShape() == lastShape);
    CHECK(cutPtr->lastValidTopologyLineage() == lastLineage);
    CHECK(cutPtr->lastValidTopologyIndex() == lastIndex);
    CHECK(cutPtr->shapeRevision() == lastRevision);

    sketch::Sketch restored;
    restored.addRectangle({10.0, 10.0}, {30.0, 15.0});
    CHECK(restored.restoreGeometryIds(
        {toolIds[0], toolIds[1], toolIds[2], toolIds[3]}, {}, {}, {}));
    CHECK(cutOracle.replaceSketchGeometry(toolSketchId, std::move(restored)));
    CHECK(cutOracle.recompute());
    checkExactBooleanLineage(*cutPtr, cutBody.id(),
                             BooleanOracleOperation::Cut, fullExpected);
    checkReferencesResolve(*cutPtr, cutBody.id(),
                           BooleanOracleOperation::Cut, fullExpected,
                           fullReferences);
  }

  // Eligibility is deliberately strict but bounded: absent lineage and an
  // explicit profile override preserve legacy behavior, while advertised
  // partial lineage fails closed. Missing-base diagnostics and unregistered
  // preview behavior also remain unchanged.
  {
    Document importedDocument;
    auto& importedBody = importedDocument.addBody();
    importedBody.addFeature(std::make_unique<ImportedShapeFeature>(
        std::make_shared<TopoDS_Shape>(
            BRepPrimAPI_MakeBox(40.0, 40.0, 40.0).Shape())));
    auto& importedProfile = importedDocument.addSketch("Imported legacy");
    importedProfile.geometry.addRectangle({10.0, 10.0}, {30.0, 15.0});
    auto importedJoin = std::make_unique<RevolveFeature>(
        importedProfile.id,
        AxisReference{AxisReferenceType::SketchHorizontalAxis,
                      importedProfile.id, sketch::kInvalidGeometryId},
        120.0, "Imported legacy", ExtrudeOperation::Join, false);
    auto* importedJoinPtr = importedJoin.get();
    importedBody.addFeature(std::move(importedJoin));
    CHECK(importedDocument.recompute());
    checkLegacyLineageUnavailable(*importedJoinPtr, importedBody.id());

    Document partialDocument;
    auto& partialBody = partialDocument.addBody();
    partialBody.addFeature(std::make_unique<PartialLineageBoxFeature>());
    auto& partialProfile = partialDocument.addSketch("Partial rejected");
    partialProfile.geometry.addRectangle({10.0, 10.0}, {30.0, 15.0});
    auto partialJoin = std::make_unique<RevolveFeature>(
        partialProfile.id,
        AxisReference{AxisReferenceType::SketchHorizontalAxis,
                      partialProfile.id, sketch::kInvalidGeometryId},
        120.0, "Partial rejected", ExtrudeOperation::Join, false);
    auto* partialJoinPtr = partialJoin.get();
    partialBody.addFeature(std::move(partialJoin));
    CHECK(!partialDocument.recompute());
    CHECK(partialJoinPtr->error() ==
          "Revolve base face lineage is incomplete");
    CHECK(!partialJoinPtr->shape());
    CHECK(!partialJoinPtr->topologyLineage());

    Document overrideBooleanDocument;
    auto& overrideBaseProfile =
        overrideBooleanDocument.addSketch("Override base");
    overrideBaseProfile.geometry.addRectangle({10.0, 5.0}, {30.0, 10.0});
    auto& overrideBooleanBody = overrideBooleanDocument.addBody();
    overrideBooleanBody.addFeature(std::make_unique<RevolveFeature>(
        overrideBaseProfile.id,
        AxisReference{AxisReferenceType::SketchHorizontalAxis,
                      overrideBaseProfile.id, sketch::kInvalidGeometryId},
        360.0, "Override base", ExtrudeOperation::NewBody, false));
    auto& overrideToolProfile =
        overrideBooleanDocument.addSketch("Override tool");
    overrideToolProfile.geometry.addRectangle({10.0, 10.0}, {30.0, 15.0});
    auto overrideJoin = std::make_unique<RevolveFeature>(
        overrideToolProfile.id,
        AxisReference{AxisReferenceType::SketchHorizontalAxis,
                      overrideToolProfile.id, sketch::kInvalidGeometryId},
        360.0, "Override tool", ExtrudeOperation::Join, false);
    overrideJoin->setProfileOverride(overrideToolProfile.geometry);
    auto* overrideJoinPtr = overrideJoin.get();
    overrideBooleanBody.addFeature(std::move(overrideJoin));
    CHECK(overrideBooleanDocument.recompute());
    checkLegacyLineageUnavailable(*overrideJoinPtr,
                                  overrideBooleanBody.id());

    Document multiDocument;
    auto& multiBaseProfile = multiDocument.addSketch("Multi base");
    multiBaseProfile.geometry.addRectangle({10.0, 5.0}, {30.0, 20.0});
    auto& multiBody = multiDocument.addBody();
    multiBody.addFeature(std::make_unique<RevolveFeature>(
        multiBaseProfile.id,
        AxisReference{AxisReferenceType::SketchHorizontalAxis,
                      multiBaseProfile.id, sketch::kInvalidGeometryId},
        360.0, "Multi base", ExtrudeOperation::NewBody, false));
    auto& multiToolProfile = multiDocument.addSketch("Multi tool");
    multiToolProfile.geometry.addRectangle({10.0, 8.0}, {30.0, 10.0});
    multiToolProfile.geometry.addRectangle({10.0, 14.0}, {30.0, 16.0});
    auto multiJoin = std::make_unique<RevolveFeature>(
        multiToolProfile.id,
        AxisReference{AxisReferenceType::SketchHorizontalAxis,
                      multiToolProfile.id, sketch::kInvalidGeometryId},
        360.0, "Multi legacy", ExtrudeOperation::Join, false);
    auto* multiJoinPtr = multiJoin.get();
    multiBody.addFeature(std::move(multiJoin));
    CHECK(multiDocument.recompute());
    checkLegacyLineageUnavailable(*multiJoinPtr, multiBody.id());

    Document previewDocument;
    auto& previewBaseProfile = previewDocument.addSketch("Preview base");
    previewBaseProfile.geometry.addRectangle({10.0, 5.0}, {30.0, 10.0});
    auto& previewBody = previewDocument.addBody();
    auto previewBase = std::make_unique<RevolveFeature>(
        previewBaseProfile.id,
        AxisReference{AxisReferenceType::SketchHorizontalAxis,
                      previewBaseProfile.id, sketch::kInvalidGeometryId},
        360.0, "Preview base", ExtrudeOperation::NewBody, false);
    auto* previewBasePtr = previewBase.get();
    previewBody.addFeature(std::move(previewBase));
    auto& previewToolProfile = previewDocument.addSketch("Preview tool");
    previewToolProfile.geometry.addRectangle({10.0, 10.0}, {30.0, 15.0});
    CHECK(previewDocument.recompute());
    RevolveFeature preview(
        previewToolProfile.id,
        AxisReference{AxisReferenceType::SketchHorizontalAxis,
                      previewToolProfile.id, sketch::kInvalidGeometryId},
        360.0, "Transient preview", ExtrudeOperation::Join, false);
    CHECK(preview.rebuild({previewDocument, &previewBody,
                           previewBasePtr->shape().get(), previewBasePtr}));
    CHECK(preview.shape());
    CHECK(!preview.topologyLineage());

    auto registeredPreview = std::make_unique<RevolveFeature>(
        previewToolProfile.id,
        AxisReference{AxisReferenceType::SketchHorizontalAxis,
                      previewToolProfile.id, sketch::kInvalidGeometryId},
        360.0, "Registered preview source", ExtrudeOperation::Join, false);
    auto* registeredPreviewPtr = registeredPreview.get();
    previewBody.addFeature(std::move(registeredPreview));
    CHECK(previewDocument.recompute());
    auto collidingClone = registeredPreviewPtr->clone();
    auto* collidingPreview =
        dynamic_cast<RevolveFeature*>(collidingClone.get());
    CHECK(collidingPreview);
    collidingPreview->setDirty();
    CHECK(collidingPreview->rebuild({previewDocument, &previewBody,
                                     previewBasePtr->shape().get(),
                                     previewBasePtr}));
    CHECK(collidingPreview->shape());
    CHECK(!collidingPreview->topologyLineage());

    RevolveFeature missingBase(
        previewToolProfile.id,
        AxisReference{AxisReferenceType::SketchHorizontalAxis,
                      previewToolProfile.id, sketch::kInvalidGeometryId},
        360.0, "Missing base", ExtrudeOperation::Join, false);
    CHECK(!missingBase.rebuild({previewDocument, nullptr, nullptr, nullptr}));
    CHECK(missingBase.error() == "Revolve Join base shape is missing");
    RevolveFeature missingCutBase(
        previewToolProfile.id,
        AxisReference{AxisReferenceType::SketchHorizontalAxis,
                      previewToolProfile.id, sketch::kInvalidGeometryId},
        360.0, "Missing Cut base", ExtrudeOperation::Cut, false);
    CHECK(!missingCutBase.rebuild(
        {previewDocument, nullptr, nullptr, nullptr}));
    CHECK(missingCutBase.error() == "Revolve Cut base shape is missing");

    Document firstJoinDocument;
    auto& firstJoinProfile = firstJoinDocument.addSketch("First Join");
    firstJoinProfile.geometry.addRectangle({10.0, 10.0}, {30.0, 15.0});
    auto& firstJoinBody = firstJoinDocument.addBody();
    auto firstJoin = std::make_unique<RevolveFeature>(
        firstJoinProfile.id,
        AxisReference{AxisReferenceType::SketchHorizontalAxis,
                      firstJoinProfile.id, sketch::kInvalidGeometryId},
        360.0, "First Join", ExtrudeOperation::Join, false);
    auto* firstJoinPtr = firstJoin.get();
    firstJoinBody.addFeature(std::move(firstJoin));
    CHECK(!firstJoinDocument.recompute());
    CHECK(firstJoinPtr->error() == "Revolve Join base shape is missing");
    firstJoinPtr->setDirty();
    CHECK(!firstJoinPtr->rebuild(
        {firstJoinDocument, &firstJoinBody, previewBasePtr->shape().get(),
         previewBasePtr}));
    CHECK(firstJoinPtr->error() ==
          "Revolve base Feature is not the immediate predecessor");

    Document firstCutDocument;
    auto& firstCutProfile = firstCutDocument.addSketch("First Cut");
    firstCutProfile.geometry.addRectangle({10.0, 10.0}, {30.0, 15.0});
    auto& firstCutBody = firstCutDocument.addBody();
    auto firstCut = std::make_unique<RevolveFeature>(
        firstCutProfile.id,
        AxisReference{AxisReferenceType::SketchHorizontalAxis,
                      firstCutProfile.id, sketch::kInvalidGeometryId},
        360.0, "First Cut", ExtrudeOperation::Cut, false);
    auto* firstCutPtr = firstCut.get();
    firstCutBody.addFeature(std::move(firstCut));
    CHECK(!firstCutDocument.recompute());
    CHECK(firstCutPtr->error() == "Revolve Cut base shape is missing");
  }

  // A registered strict consumer accepts only its exact current same-Body
  // predecessor and exact shared shape pointer. Foreign, earlier, copied and
  // missing-predecessor contexts all fail atomically; the correct context
  // recovers after every rejection.
  {
    Document contextDocument;
    auto addProfile = [&](const char* name, double inner, double outer) {
      auto& sketch = contextDocument.addSketch(name);
      sketch.geometry.addRectangle({10.0, inner}, {30.0, outer});
      return sketch.id;
    };
    const auto baseSketch = addProfile("Context base", 5.0, 10.0);
    const auto firstSketch = addProfile("Context first", 10.0, 15.0);
    const auto targetSketch = addProfile("Context target", 15.0, 20.0);
    const auto foreignSketch = addProfile("Context foreign", 5.0, 10.0);
    auto axisFor = [](SketchId sketchId) {
      return AxisReference{AxisReferenceType::SketchHorizontalAxis, sketchId,
                           sketch::kInvalidGeometryId};
    };

    auto& targetBody = contextDocument.addBody("Context target body");
    const auto targetBodyId = targetBody.id();
    auto base = std::make_unique<RevolveFeature>(
        baseSketch, axisFor(baseSketch), 360.0, "Context base",
        ExtrudeOperation::NewBody, false);
    auto* basePtr = base.get();
    targetBody.addFeature(std::move(base));
    auto first = std::make_unique<RevolveFeature>(
        firstSketch, axisFor(firstSketch), 360.0, "Context first",
        ExtrudeOperation::Join, false);
    auto* firstPtr = first.get();
    targetBody.addFeature(std::move(first));
    auto target = std::make_unique<RevolveFeature>(
        targetSketch, axisFor(targetSketch), 360.0, "Context target",
        ExtrudeOperation::Join, false);
    auto* targetPtr = target.get();
    targetBody.addFeature(std::move(target));

    auto& foreignBody = contextDocument.addBody("Context foreign body");
    auto foreign = std::make_unique<RevolveFeature>(
        foreignSketch, axisFor(foreignSketch), 360.0, "Context foreign",
        ExtrudeOperation::NewBody, false);
    auto* foreignPtr = foreign.get();
    foreignBody.addFeature(std::move(foreign));
    CHECK(contextDocument.recompute());
    const auto* targetBodyPtr = contextDocument.findBody(targetBodyId);
    CHECK(targetBodyPtr);

    const RebuildContext validContext{contextDocument, targetBodyPtr,
                                      firstPtr->shape().get(), firstPtr};
    const auto reject = [&](const RebuildContext& forged) {
      const auto validShape = targetPtr->shape();
      const auto validLineage = targetPtr->topologyLineage();
      const auto validIndex = targetPtr->topologyIndex();
      const auto validRevision = targetPtr->shapeRevision();
      targetPtr->setDirty();
      CHECK(!targetPtr->rebuild(forged));
      CHECK(targetPtr->error() ==
            "Revolve base Feature is not the immediate predecessor");
      CHECK(!targetPtr->shape());
      CHECK(!targetPtr->topologyLineage());
      CHECK(targetPtr->lastValidShape() == validShape);
      CHECK(targetPtr->lastValidTopologyLineage() == validLineage);
      CHECK(targetPtr->lastValidTopologyIndex() == validIndex);
      CHECK(targetPtr->shapeRevision() == validRevision);
      targetPtr->setDirty();
      CHECK(targetPtr->rebuild(validContext));
      CHECK(targetPtr->topologyLineage());
    };

    reject({contextDocument, targetBodyPtr, foreignPtr->shape().get(),
            foreignPtr});
    reject({contextDocument, targetBodyPtr, basePtr->shape().get(), basePtr});
    TopoDS_Shape copied = *firstPtr->shape();
    reject({contextDocument, targetBodyPtr, &copied, firstPtr});
    reject({contextDocument, targetBodyPtr, firstPtr->shape().get(), nullptr});
  }

  return EXIT_SUCCESS;
}
