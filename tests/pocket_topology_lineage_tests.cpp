#include "TestAssertions.h"

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/ImportedShapeFeature.h"
#include "model/MoveFeature.h"
#include "model/PocketFeature.h"
#include "model/TopologyLineageBuilder.h"
#include "model/TopologyReferenceResolver.h"

namespace {

bool near(double actual, double expected, double tolerance = 1e-5) {
  return std::abs(actual - expected) <= tolerance;
}

double volumeOf(const TopoDS_Shape& shape) {
  GProp_GProps properties;
  BRepGProp::VolumeProperties(shape, properties);
  return properties.Mass();
}

solidar::sketch::Point toLocal(const solidar::SketchPlacement& placement,
                               solidar::Point3d world) {
  const solidar::Vector3d delta{world.x - placement.origin.x,
                                world.y - placement.origin.y,
                                world.z - placement.origin.z};
  return {delta.x * placement.xDirection.x +
              delta.y * placement.xDirection.y +
              delta.z * placement.xDirection.z,
          delta.x * placement.yDirection.x +
              delta.y * placement.yDirection.y +
              delta.z * placement.yDirection.z};
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

solidar::TopologyProvenanceKey descendantKey(
    const solidar::ShapeFeature& feature,
    const solidar::TopologyProvenanceOrigin& origin) {
  const auto lineage = feature.topologyLineage();
  CHECK(lineage);
  const auto found = std::find_if(
      lineage->bindings().begin(), lineage->bindings().end(),
      [&](const auto& binding) {
        return binding.provenance.origins.size() == 1 &&
               binding.provenance.origins.front() == origin;
      });
  CHECK(found != lineage->bindings().end());
  return found->provenance;
}

std::vector<solidar::TopologyProvenanceKey> keysOf(
    const solidar::ShapeFeature& feature) {
  const auto lineage = feature.topologyLineage();
  CHECK(lineage);
  std::vector<solidar::TopologyProvenanceKey> keys;
  keys.reserve(lineage->bindings().size());
  for (const auto& binding : lineage->bindings())
    keys.push_back(binding.provenance);
  return keys;
}

void checkKeysResolve(const solidar::ShapeFeature& feature,
                      solidar::BodyId bodyId,
                      const std::vector<solidar::TopologyProvenanceKey>& keys) {
  const auto index = feature.topologyIndex();
  CHECK(index);
  for (const auto& key : keys) {
    const auto resolved = index->resolveFace({bodyId, key});
    CHECK(resolved);
    CHECK(resolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
  }
}

void checkEveryFaceHasV2FactoryReference(
    const solidar::ShapeFeature& feature, solidar::BodyId bodyId) {
  const auto index = feature.topologyIndex();
  CHECK(index);
  std::vector<solidar::TopologyProvenanceKey> created;
  for (std::size_t raw = 0; raw < index->rawFaceCount(); ++raw) {
    const auto reference = feature.createFaceReferenceV2(bodyId, raw);
    CHECK(reference);
    CHECK(reference.status == solidar::FaceReferenceV2CreationStatus::Created);
    const auto* v2 = reference.reference.v2();
    CHECK(v2);
    const auto resolved = index->resolveFace(reference.reference);
    CHECK(resolved);
    CHECK(resolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    created.push_back(v2->provenance);
  }
  CHECK(created.size() == feature.topologyLineage()->bindings().size());
  for (const auto& binding : feature.topologyLineage()->bindings())
    CHECK(std::count(created.begin(), created.end(), binding.provenance) == 1);
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

enum class InvalidProfileKind { OpenLine, DegenerateCircle };

struct PocketFailureOutcome {
  bool rebuilt{};
  solidar::FeatureState state{solidar::FeatureState::Dirty};
  std::string error;
  bool hasShape{};
  bool hasLineage{};
};

PocketFailureOutcome invalidProfileOutcome(bool completeBaseLineage,
                                           InvalidProfileKind profileKind) {
  solidar::Document document;
  auto& body = document.addBody();
  if (completeBaseLineage) {
    auto& base = document.addSketch();
    base.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
    body.addFeature(
        std::make_unique<solidar::ExtrudeFeature>(base.id, 10.0));
  } else {
    body.addFeature(std::make_unique<solidar::ImportedShapeFeature>(
        std::make_shared<TopoDS_Shape>(
            BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape())));
  }
  auto& profile = document.addSketch();
  profile.placement.origin.z = 10.0;
  if (profileKind == InvalidProfileKind::OpenLine)
    profile.geometry.addLine({2.0, 2.0}, {8.0, 2.0});
  else
    profile.geometry.addCircle({10.0, 10.0}, 0.0);
  auto pocket = std::make_unique<solidar::PocketFeature>(profile.id, 3.0);
  auto* pocketPtr = pocket.get();
  body.addFeature(std::move(pocket));
  const bool rebuilt = document.rebuild();
  return {rebuilt, pocketPtr->state(), pocketPtr->error(),
          static_cast<bool>(pocketPtr->shape()),
          static_cast<bool>(pocketPtr->topologyLineage())};
}

}  // namespace

int main() {
  using namespace solidar;

  // Deterministic strict path: authored Extrude identity survives Move and is
  // consumed through a v2 face-supported circle Pocket.
  Document document;
  auto& baseSketch = document.addSketch("B2 base");
  baseSketch.geometry.addRectangle({0.0, 0.0}, {40.0, 30.0});
  auto& body = document.addBody("B2 body");
  auto extrusion = std::make_unique<ExtrudeFeature>(baseSketch.id, 20.0);
  auto* extrusionPtr = extrusion.get();
  body.addFeature(std::move(extrusion));
  auto move = std::make_unique<MoveFeature>(extrusionPtr->id(),
                                            Vector3d{5.0, 7.0, 3.0});
  auto* movePtr = move.get();
  body.addFeature(std::move(move));
  CHECK(document.rebuild());

  const auto baseTop = authoredKey(AuthoredTopologySourceKind::ProfileEndCap,
                                   baseSketch.id,
                                   sketch::kInvalidGeometryId,
                                   extrusionPtr->id());
  const auto movedTop = descendantKey(*movePtr, baseTop.immediateOrigin());
  auto& pocketSketch = document.addSketch("B2 pocket");
  CHECK(document.attachSketchToFace(
      pocketSketch.id,
      FaceReference::fromV2({body.id(), movedTop})));
  const auto circleCenter =
      toLocal(pocketSketch.placement, {25.0, 22.0, 23.0});
  pocketSketch.geometry.addCircle(circleCenter, 4.0);
  const auto circleId = pocketSketch.geometry.circleId(0);
  auto pocket = std::make_unique<PocketFeature>(pocketSketch.id, 8.0);
  auto* pocketPtr = pocket.get();
  body.addFeature(std::move(pocket));
  CHECK(document.rebuild());
  CHECK(pocketPtr->topologyLineage());
  CHECK(pocketPtr->topologyLineage()->coverage() ==
        TopologyLineageCoverage::CompleteFaces);
  CHECK(pocketPtr->topologyLineage()->bindings().size() == 8);
  CHECK(pocketPtr->shape()->ShapeType() == TopAbs_SOLID);
  CHECK(near(volumeOf(*pocketPtr->shape()),
             40.0 * 30.0 * 20.0 - std::numbers::pi * 16.0 * 8.0,
             1e-3));

  std::size_t retained = 0;
  std::size_t modified = 0;
  std::size_t merged = 0;
  std::size_t objectOrigins = 0;
  std::size_t toolOrigins = 0;
  std::optional<TopologyProvenanceKey> blindFloor;
  for (const auto& binding : pocketPtr->topologyLineage()->bindings()) {
    CHECK(binding.provenance.featureId == pocketPtr->id());
    CHECK(binding.provenance.kind == TopologyKind::Face);
    CHECK(binding.provenance.role == TopologySemanticRole::BooleanResult);
    CHECK(binding.provenance.origins.size() == 1);
    retained += binding.provenance.operation ==
                TopologyProvenanceOperation::Retained;
    modified += binding.provenance.operation ==
                TopologyProvenanceOperation::Modified;
    merged += binding.provenance.operation ==
              TopologyProvenanceOperation::Merge;
    const auto& origin = binding.provenance.origins.front();
    objectOrigins += origin.featureId == movePtr->id();
    toolOrigins += origin.featureId == pocketPtr->id();
    if (origin.featureId == pocketPtr->id() &&
        origin.role == TopologySemanticRole::Cap &&
        origin.localName.find("profile-end-cap") != std::string::npos)
      blindFloor = binding.provenance;
  }
  CHECK(retained == 7);
  CHECK(modified == 1);
  CHECK(merged == 0);
  CHECK(objectOrigins == 6);
  CHECK(toolOrigins == 2);
  CHECK(blindFloor);
  checkEveryFaceHasV2FactoryReference(*pocketPtr, body.id());

  const auto stableBlindKeys = keysOf(*pocketPtr);
  pocketPtr->setDepthMm(10.0);
  CHECK(document.rebuild());
  checkKeysResolve(*pocketPtr, body.id(), stableBlindKeys);

  sketch::Sketch resizedCircle;
  resizedCircle.addCircle(circleCenter, 5.0);
  CHECK(resizedCircle.restoreGeometryIds({}, {circleId}, {}, {}));
  CHECK(document.replaceSketchGeometry(pocketSketch.id,
                                       std::move(resizedCircle)));
  CHECK(document.rebuild());
  checkKeysResolve(*pocketPtr, body.id(), stableBlindKeys);

  extrusionPtr->setLengthMm(25.0);
  movePtr->setOffsetMm({-2.0, 4.0, 6.0});
  CHECK(document.rebuild());
  checkKeysResolve(*pocketPtr, body.id(), stableBlindKeys);

  // A through cut deliberately drops the blind-floor identity, and changing
  // back to a blind cut recovers the exact authored key.
  pocketPtr->setDepthMm(40.0);
  CHECK(document.rebuild());
  const auto missingFloor =
      pocketPtr->topologyIndex()->resolveFace({body.id(), *blindFloor});
  CHECK(!missingFloor);
  CHECK(missingFloor.failure == TopologyResolutionFailure::Missing);
  pocketPtr->setDepthMm(10.0);
  CHECK(document.rebuild());
  CHECK(pocketPtr->topologyIndex()->resolveFace({body.id(), *blindFloor}));

  // A blind strip splits an object face into ambiguous branches. No synthetic
  // seed is invented: the feature fails atomically and recovers in place.
  const auto lastShape = pocketPtr->shape();
  const auto lastLineage = pocketPtr->topologyLineage();
  const auto lastRevision = pocketPtr->shapeRevision();
  const auto lastIndex = pocketPtr->topologyIndex();
  sketch::Sketch strip;
  strip.addRectangle(toLocal(pocketSketch.placement, {-7.0, 14.0, 31.0}),
                     toLocal(pocketSketch.placement, {43.0, 20.0, 31.0}));
  CHECK(document.replaceSketchGeometry(pocketSketch.id, std::move(strip)));
  CHECK(!document.rebuild());
  CHECK(pocketPtr->error().find("split") != std::string::npos);
  CHECK(!pocketPtr->shape());
  CHECK(!pocketPtr->topologyLineage());
  CHECK(pocketPtr->shapeRevision() == lastRevision);
  CHECK(pocketPtr->lastValidShape() == lastShape);
  CHECK(pocketPtr->lastValidTopologyLineage() == lastLineage);
  CHECK(pocketPtr->lastValidTopologyIndex() == lastIndex);
  sketch::Sketch recoveredCircle;
  recoveredCircle.addCircle(circleCenter, 5.0);
  CHECK(recoveredCircle.restoreGeometryIds({}, {circleId}, {}, {}));
  CHECK(document.replaceSketchGeometry(pocketSketch.id,
                                       std::move(recoveredCircle)));
  CHECK(document.rebuild());
  checkKeysResolve(*pocketPtr, body.id(), stableBlindKeys);

  // A present but partial source lineage is a hard compatibility boundary.
  {
    Document partialDocument;
    auto& partialBody = partialDocument.addBody();
    partialBody.addFeature(std::make_unique<PartialLineageBoxFeature>());
    auto& profile = partialDocument.addSketch();
    profile.placement.origin.z = 10.0;
    profile.geometry.addCircle({10.0, 10.0}, 2.0);
    auto partialPocket = std::make_unique<PocketFeature>(profile.id, 3.0);
    auto* partialPocketPtr = partialPocket.get();
    partialBody.addFeature(std::move(partialPocket));
    CHECK(!partialDocument.rebuild());
    CHECK(partialPocketPtr->error() ==
          "Pocket base face lineage is incomplete");
    CHECK(!partialPocketPtr->shape());
  }

  // Imported/no-lineage input retains the established legacy Pocket path.
  {
    Document importedDocument;
    auto& importedBody = importedDocument.addBody();
    importedBody.addFeature(std::make_unique<ImportedShapeFeature>(
        std::make_shared<TopoDS_Shape>(
            BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape())));
    auto& profile = importedDocument.addSketch();
    profile.placement.origin.z = 10.0;
    profile.geometry.addCircle({10.0, 10.0}, 2.0);
    auto legacyPocket = std::make_unique<PocketFeature>(profile.id, 3.0);
    auto* legacyPocketPtr = legacyPocket.get();
    importedBody.addFeature(std::move(legacyPocket));
    CHECK(importedDocument.rebuild());
    CHECK(legacyPocketPtr->shape());
    CHECK(!legacyPocketPtr->topologyLineage());
  }

  // A valid annular profile lies outside the simple strict subset. Complete
  // lineage delegates to the exact legacy Pocket builder, preserving its
  // historical two-loop rejection and diagnostic rather than expanding
  // feature behavior through the generalized Extrude Cut path.
  {
    constexpr const char* kLegacyAnnularDiagnostic =
        "Pocket Extrude 2.0 supports one profile at a time";
    Document annularDocument;
    auto& base = annularDocument.addSketch();
    base.geometry.addRectangle({0.0, 0.0}, {30.0, 30.0});
    auto& annularBody = annularDocument.addBody();
    annularBody.addFeature(
        std::make_unique<ExtrudeFeature>(base.id, 10.0));
    auto& profile = annularDocument.addSketch();
    profile.placement.origin.z = 10.0;
    profile.geometry.addCircle({15.0, 15.0}, 6.0);
    profile.geometry.addCircle({15.0, 15.0}, 3.0);
    auto annularPocket =
        std::make_unique<PocketFeature>(profile.id, 4.0);
    auto* annularPocketPtr = annularPocket.get();
    annularBody.addFeature(std::move(annularPocket));
    CHECK(!annularDocument.rebuild());
    CHECK(annularPocketPtr->error() == kLegacyAnnularDiagnostic);
    CHECK(!annularPocketPtr->shape());
    CHECK(!annularPocketPtr->topologyLineage());

    Document legacyDocument;
    auto& legacyBody = legacyDocument.addBody();
    legacyBody.addFeature(std::make_unique<ImportedShapeFeature>(
        std::make_shared<TopoDS_Shape>(
            BRepPrimAPI_MakeBox(30.0, 30.0, 10.0).Shape())));
    auto& legacyProfile = legacyDocument.addSketch();
    legacyProfile.placement.origin.z = 10.0;
    legacyProfile.geometry.addCircle({15.0, 15.0}, 6.0);
    legacyProfile.geometry.addCircle({15.0, 15.0}, 3.0);
    auto legacyAnnular =
        std::make_unique<PocketFeature>(legacyProfile.id, 4.0);
    auto* legacyAnnularPtr = legacyAnnular.get();
    legacyBody.addFeature(std::move(legacyAnnular));
    CHECK(!legacyDocument.rebuild());
    CHECK(legacyAnnularPtr->error() == kLegacyAnnularDiagnostic);
    CHECK(!legacyAnnularPtr->shape());
    CHECK(!legacyAnnularPtr->topologyLineage());
  }

  // Strict eligibility is selection-only. Invalid profiles still reach the
  // historical builder and therefore produce byte-for-byte identical errors
  // and state with and without a CompleteFaces base.
  {
    const std::array cases{
        std::pair{InvalidProfileKind::OpenLine,
                  std::string{"Pocket Profile has insufficient geometry"}},
        std::pair{
            InvalidProfileKind::DegenerateCircle,
            std::string{"Pocket Profile has insufficient geometry"}},
    };
    for (const auto& [profileKind, expectedError] : cases) {
      const auto legacy = invalidProfileOutcome(false, profileKind);
      const auto complete = invalidProfileOutcome(true, profileKind);
      CHECK(!legacy.rebuilt);
      CHECK(!complete.rebuilt);
      CHECK(legacy.state == FeatureState::Error);
      CHECK(complete.state == FeatureState::Error);
      CHECK(!legacy.hasShape);
      CHECK(!complete.hasShape);
      CHECK(!legacy.hasLineage);
      CHECK(!complete.hasLineage);
      CHECK(legacy.error == expectedError);
      CHECK(complete.error == legacy.error);
    }
  }

  return EXIT_SUCCESS;
}
