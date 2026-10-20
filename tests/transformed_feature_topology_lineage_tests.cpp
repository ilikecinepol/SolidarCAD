#include "TestAssertions.h"
#include "TestGeometryUtils.h"

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <utility>

#include "model/CircularPatternFeature.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/MirrorFeature.h"
#include "model/MoveFeature.h"
#include "model/PocketFeature.h"
#include "model/TopologyLineageBuilder.h"
#include "model/TopologyReferenceResolver.h"
#include "model/TransformFeatureHistoryBuilder.h"

namespace {

gp_Pnt faceCenter(const TopoDS_Shape& shape) {
  CHECK(shape.ShapeType() == TopAbs_FACE);
  GProp_GProps properties;
  BRepGProp::SurfaceProperties(TopoDS::Face(shape), properties);
  return properties.CentreOfMass();
}

double faceArea(const TopoDS_Shape& shape) {
  CHECK(shape.ShapeType() == TopAbs_FACE);
  GProp_GProps properties;
  BRepGProp::SurfaceProperties(TopoDS::Face(shape), properties);
  return properties.Mass();
}

gp_Pnt rotateZ(const gp_Pnt& point, double angleRad) {
  return {point.X() * std::cos(angleRad) - point.Y() * std::sin(angleRad),
          point.X() * std::sin(angleRad) + point.Y() * std::cos(angleRad),
          point.Z()};
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

const solidar::TopologyLineageBinding& bindingFor(
    const solidar::TopologyLineage& lineage,
    const solidar::TopologyProvenanceKey& key) {
  const auto found = std::find_if(
      lineage.bindings().begin(), lineage.bindings().end(),
      [&](const auto& binding) { return binding.provenance == key; });
  CHECK(found != lineage.bindings().end());
  return *found;
}

solidar::TopologyProvenanceKey transformedKey(
    const solidar::TopologyProvenanceKey& source,
    solidar::FeatureId producer,
    solidar::TopologyProvenanceOperation operation,
    solidar::TopologySemanticRole role,
    std::optional<std::uint64_t> slot = std::nullopt) {
  const auto origin = source.immediateOrigin();
  std::string localName =
      "transform-source:v1;feature=" + std::to_string(origin.featureId) +
      ";kind=" + std::to_string(static_cast<unsigned>(origin.kind)) +
      ";operation=" +
      std::to_string(static_cast<unsigned>(origin.operation)) +
      ";role=" + std::to_string(static_cast<unsigned>(origin.role)) +
      ";name-length=" + std::to_string(origin.localName.size()) + ":" +
      origin.localName + ";occurrence=";
  if (origin.occurrence) {
    localName += "present;owner=" +
                 std::to_string(origin.occurrence->featureId) +
                 ";slot=" + std::to_string(origin.occurrence->slot);
  } else {
    localName += "absent";
  }
  return {producer, solidar::TopologyKind::Face, operation, role,
          std::move(localName), {origin},
          slot ? std::optional<solidar::TopologyOccurrenceKey>(
                     solidar::TopologyOccurrenceKey{producer, *slot})
               : std::nullopt};
}

std::string independentlyEncodedName(
    const solidar::TopologyProvenanceOrigin& origin) {
  solidar::TopologyProvenanceKey source{
      origin.featureId, origin.kind, origin.operation, origin.role,
      origin.localName, {}, origin.occurrence};
  return transformedKey(source, 999991,
                        solidar::TopologyProvenanceOperation::Modified,
                        solidar::TopologySemanticRole::Original)
      .localName;
}

struct ExpectedBranch {
  solidar::TopologyProvenanceOperation operation;
  solidar::TopologySemanticRole role;
  std::optional<std::uint64_t> slot;
  bool retained{};
  std::function<gp_Pnt(const gp_Pnt&)> transform;
};

solidar::FaceResolution resolve(
    const solidar::ShapeFeature& feature, solidar::BodyId bodyId,
    const solidar::TopologyProvenanceKey& key);

void checkAllFaceBranches(
    const solidar::ShapeFeature& source,
    const solidar::ShapeFeature& output, solidar::BodyId bodyId,
    const std::vector<ExpectedBranch>& branches) {
  CHECK(source.topologyLineage());
  CHECK(output.topologyLineage());
  CHECK(source.topologyLineage()->coverage() ==
        solidar::TopologyLineageCoverage::CompleteFaces);
  CHECK(output.topologyLineage()->coverage() ==
        solidar::TopologyLineageCoverage::CompleteFaces);
  CHECK(static_cast<std::size_t>(std::count_if(
            source.topologyLineage()->bindings().begin(),
            source.topologyLineage()->bindings().end(),
            [](const auto& binding) {
              return binding.provenance.kind == solidar::TopologyKind::Face;
            })) == 6);
  CHECK(output.topologyLineage()->bindings().size() == 6 * branches.size());

  std::vector<solidar::TopologyProvenanceKey> expectedKeys;
  std::vector<TopoDS_Face> resolvedFaces;
  expectedKeys.reserve(6 * branches.size());
  resolvedFaces.reserve(6 * branches.size());
  for (const auto& sourceBinding : source.topologyLineage()->bindings()) {
    if (sourceBinding.provenance.kind != solidar::TopologyKind::Face) continue;
    const auto sourceCenter = faceCenter(*sourceBinding.subshape);
    const auto sourceArea = faceArea(*sourceBinding.subshape);
    for (const auto& branch : branches) {
      const auto key = transformedKey(
          sourceBinding.provenance, output.id(), branch.operation,
          branch.role, branch.slot);
      expectedKeys.push_back(key);
      const auto binding = std::find_if(
          output.topologyLineage()->bindings().begin(),
          output.topologyLineage()->bindings().end(),
          [&](const auto& candidate) { return candidate.provenance == key; });
      CHECK(binding != output.topologyLineage()->bindings().end());
      CHECK(binding->provenance.origins.size() == 1);
      CHECK(binding->provenance.origins.front() ==
            sourceBinding.provenance.immediateOrigin());
      const auto exact = resolve(output, bodyId, key);
      CHECK(exact);
      CHECK(exact.method == solidar::TopologyMatchMethod::OperationProvenance);
      CHECK(exact.subshape->IsSame(*binding->subshape));
      CHECK(branch.retained ==
            exact.subshape->IsSame(*sourceBinding.subshape));
      const auto expectedCenter = branch.transform(sourceCenter);
      const auto actualCenter = faceCenter(*exact.subshape);
      CHECK(solidar::test::near(actualCenter.X(), expectedCenter.X(), 1e-5));
      CHECK(solidar::test::near(actualCenter.Y(), expectedCenter.Y(), 1e-5));
      CHECK(solidar::test::near(actualCenter.Z(), expectedCenter.Z(), 1e-5));
      CHECK(solidar::test::near(faceArea(*exact.subshape), sourceArea, 1e-5));
      resolvedFaces.push_back(*exact.subshape);
    }
  }
  for (std::size_t first = 0; first < resolvedFaces.size(); ++first)
    for (std::size_t second = first + 1; second < resolvedFaces.size();
         ++second)
      CHECK(!resolvedFaces[first].IsSame(resolvedFaces[second]));

  for (const auto& actual : output.topologyLineage()->bindings()) {
    CHECK(std::count(expectedKeys.begin(), expectedKeys.end(),
                     actual.provenance) == 1);
  }
}

template <typename Invalidate, typename Recover>
void checkAtomicFailureRecovery(
    solidar::Document& document, solidar::Body& body,
    std::size_t featureIndex, solidar::ShapeFeature& feature,
    const solidar::TopologyProvenanceKey& savedKey,
    Invalidate invalidate, Recover recover) {
  const auto baselineShape = feature.shape();
  const auto baselineLineage = feature.topologyLineage();
  const auto baselineRevision = feature.shapeRevision();
  const auto baselineIndex = feature.topologyIndex();
  CHECK(baselineShape);
  CHECK(baselineLineage);
  CHECK(baselineIndex);
  const auto baselineFace = baselineIndex->resolveFace({body.id(), savedKey});
  CHECK(baselineFace);
  invalidate();
  body.markDirtyFrom(featureIndex);
  CHECK(!document.recompute());
  CHECK(!feature.shape());
  CHECK(!feature.topologyLineage());
  CHECK(!feature.topologyIndex());
  CHECK(feature.shapeRevision() == baselineRevision);
  CHECK(feature.lastValidShape() == baselineShape);
  CHECK(feature.lastValidTopologyLineage() == baselineLineage);
  const auto lastIndex = feature.lastValidTopologyIndex();
  CHECK(lastIndex == baselineIndex);
  const auto lastFace = lastIndex->resolveFace({body.id(), savedKey});
  CHECK(lastFace);
  CHECK(lastFace.subshape->IsSame(*baselineFace.subshape));
  recover();
  body.markDirtyFrom(featureIndex);
  CHECK(document.recompute());
  CHECK(feature.shapeRevision() > baselineRevision);
  CHECK(resolve(feature, body.id(), savedKey));
}

solidar::FaceResolution resolve(
    const solidar::ShapeFeature& feature, solidar::BodyId bodyId,
    const solidar::TopologyProvenanceKey& key) {
  std::string error;
  const auto index = feature.topologyIndex(&error);
  CHECK(index);
  CHECK(error.empty());
  return index->resolveFace({bodyId, key});
}

void checkMissing(const solidar::ShapeFeature& feature,
                  solidar::BodyId bodyId,
                  const solidar::TopologyProvenanceKey& key) {
  const auto missing = resolve(feature, bodyId, key);
  CHECK(!missing);
  CHECK(!missing.subshape);
  CHECK(missing.method == solidar::TopologyMatchMethod::None);
  CHECK(missing.failure == solidar::TopologyResolutionFailure::Missing);
}

void checkLegacyFeature(const solidar::ShapeFeature& feature,
                        solidar::BodyId bodyId) {
  CHECK(!feature.topologyLineage());
  CHECK(feature.shape());
  const auto index = feature.topologyIndex();
  CHECK(index);
  TopExp_Explorer faces(*feature.shape(), TopAbs_FACE);
  CHECK(faces.More());
  const solidar::TopologyReference legacy{
      bodyId, feature.id(), solidar::TopologyKind::Face, 0};
  const auto v1 = index->resolveFace(legacy);
  CHECK(v1);
  CHECK(v1.method == solidar::TopologyMatchMethod::LegacyIndex);
  CHECK(v1.subshape->IsSame(faces.Current()));
  const auto unavailable = authoredKey(
      solidar::AuthoredTopologySourceKind::ProfileStartCap,
      static_cast<solidar::SketchId>(1),
      solidar::sketch::kInvalidGeometryId, feature.id());
  checkMissing(feature, bodyId, unavailable);
}

class PartialFaceFeature final : public solidar::ShapeFeature {
 public:
  PartialFaceFeature() : ShapeFeature("Partial source") {}
  [[nodiscard]] std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<PartialFaceFeature>(*this);
  }

 protected:
  bool rebuildImpl(const solidar::RebuildContext&) override {
    clearShape();
    const auto shape = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(4.0, 3.0, 2.0).Shape());
    TopExp_Explorer faces(*shape, TopAbs_FACE);
    CHECK(faces.More());
    auto face = std::make_shared<TopoDS_Shape>(faces.Current());
    auto lineage = solidar::TopologyLineage::create(
        {{{id(), solidar::TopologyKind::Face,
           solidar::TopologyProvenanceOperation::Seed,
           solidar::TopologySemanticRole::Custom, "partial-face"},
          face}},
        solidar::TopologyLineageCoverage::PartialFaces);
    CHECK(lineage);
    setShapeAndTopologyLineage(shape, std::move(lineage));
    markValid();
    return true;
  }
};

enum class PartialConsumer {
  Move,
  Mirror,
  LinearJoin,
  CircularJoin,
  LinearNewBody,
  CircularNewBody,
};

void checkPartialConsumerFailsClosed(PartialConsumer kind) {
  using namespace solidar;
  Document document;
  auto& sourceBody = document.addBody("Partial source");
  auto partial = std::make_unique<PartialFaceFeature>();
  const auto partialId = partial->id();
  auto* partialPtr = partial.get();
  const auto sourceBodyId = sourceBody.id();
  sourceBody.addFeature(std::move(partial));

  Body* targetBody = &sourceBody;
  std::unique_ptr<ShapeFeature> consumer;
  switch (kind) {
    case PartialConsumer::Move:
      consumer = std::make_unique<MoveFeature>(
          partialId, Vector3d{1.0, 0.0, 0.0});
      break;
    case PartialConsumer::Mirror:
      consumer = std::make_unique<MirrorFeature>(partialId, MirrorPlane::YZ);
      break;
    case PartialConsumer::LinearJoin:
      consumer = std::make_unique<LinearPatternFeature>(
          partialId, PrincipalAxis::X, 2, 10.0);
      break;
    case PartialConsumer::CircularJoin:
      consumer = std::make_unique<CircularPatternFeature>(
          partialId, PrincipalAxis::Z, 2, 180.0);
      break;
    case PartialConsumer::LinearNewBody:
      targetBody = &document.addBody("Linear target");
      consumer = std::make_unique<LinearPatternFeature>(
          sourceBodyId, partialId, PrincipalAxis::X, 2, 10.0,
          PatternOperation::NewBody);
      break;
    case PartialConsumer::CircularNewBody:
      targetBody = &document.addBody("Circular target");
      consumer = std::make_unique<CircularPatternFeature>(
          sourceBodyId, partialId, PrincipalAxis::Z, 2, 180.0,
          PatternOperation::NewBody);
      break;
  }
  auto* consumerPtr = consumer.get();
  targetBody->addFeature(std::move(consumer));
  CHECK(!document.recompute());
  CHECK(partialPtr->isValid());
  CHECK(partialPtr->topologyLineage());
  CHECK(partialPtr->topologyLineage()->coverage() ==
        TopologyLineageCoverage::PartialFaces);
  CHECK(consumerPtr->isFailed());
  CHECK(!consumerPtr->shape());
  CHECK(!consumerPtr->topologyLineage());
  CHECK(consumerPtr->error().find("incomplete") != std::string::npos);
}

using namespace solidar;

struct FacePocketChain {
  FaceReference reference;
  Point2d circleCenter;
  double radiusMm{};
  double depthMm{};
  SketchId sketchId{kInvalidSketchId};
  FeatureId pocketId{kInvalidFeatureId};
  PocketFeature* pocket{};
};

FaceReference createExactFaceReferenceV2(
    Document& document, const Body& body, const ShapeFeature& owner,
    const TopologyProvenanceKey& key) {
  std::string error;
  const auto index = owner.topologyIndex(&error);
  CHECK(index);
  CHECK(error.empty());
  const auto resolved = index->resolveFace(TopologyReferenceV2{body.id(), key});
  CHECK(resolved);
  CHECK(resolved.method == TopologyMatchMethod::OperationProvenance);

  // The resolver supplies the canonical raw traversal ordinal. The test does
  // not couple persistent identity to a hard-coded OCCT face order.
  const auto created = document.createFaceReferenceV2(
      body.id(), owner.id(), resolved.index);
  CHECK(created);
  CHECK(created.error.empty());
  CHECK(created.reference.isV2());
  CHECK(created.reference.bodyId() == body.id());
  CHECK(created.reference.featureId() == owner.id());
  CHECK(created.reference.v2());
  CHECK(created.reference.v2()->provenance == key);

  const auto wrongProducer =
      index->createFaceReferenceV2(body.id(), owner.id() + 1000000,
                                   resolved.index);
  CHECK(!wrongProducer);
  CHECK(wrongProducer.status == FaceReferenceV2CreationStatus::InvalidReference);
  CHECK(!wrongProducer.reference.isV2());
  const auto wrongOwner = document.createFaceReferenceV2(
      kInvalidBodyId, owner.id(), resolved.index);
  CHECK(!wrongOwner);
  CHECK(wrongOwner.status == FaceReferenceV2CreationStatus::InvalidReference);
  return created.reference;
}

FacePocketChain addPocketOnExactFace(
    Document& document, Body& body, ShapeFeature& owner,
    const TopologyProvenanceKey& key, double radiusMm, double depthMm,
    std::string name) {
  const FaceReference reference =
      createExactFaceReferenceV2(document, body, owner, key);
  const auto resolved = owner.topologyIndex()->resolveFace(reference);
  CHECK(resolved);
  const gp_Pnt center = faceCenter(*resolved.subshape);

  auto& sketch = document.addSketch(name + " profile");
  CHECK(document.attachSketchToFace(sketch.id, reference));
  CHECK(sketch.support.type == SketchSupportType::Face);
  CHECK(sketch.support.face == reference);
  CHECK(sketch.supportResolved);
  const Point2d local = sketch.placement.toLocal(
      {center.X(), center.Y(), center.Z()});
  sketch.geometry.addCircle({local.x, local.y}, radiusMm);
  CHECK(sketch.geometry.circles().size() == 1);
  CHECK(test::near(sketch.geometry.circles().front().center.xMm, local.x));
  CHECK(test::near(sketch.geometry.circles().front().center.yMm, local.y));
  CHECK(test::near(sketch.geometry.circles().front().radiusMm, radiusMm));

  auto pocket = std::make_unique<PocketFeature>(sketch.id, depthMm,
                                                std::move(name));
  auto* pocketPtr = pocket.get();
  const FeatureId pocketId = pocketPtr->id();
  body.addFeature(std::move(pocket));
  return {reference, local, radiusMm, depthMm, sketch.id, pocketId,
          pocketPtr};
}

void checkSupportAtExactFace(const Document& document,
                             const DocumentSketch& sketch,
                             const ShapeFeature& owner,
                             const FaceReference& reference,
                             Point2d circleCenter,
                             const gp_Pnt& expectedCenter) {
  CHECK(sketch.support.type == SketchSupportType::Face);
  CHECK(sketch.support.face == reference);
  CHECK(sketch.supportResolved);
  CHECK(sketch.support.face.isV2());
  const auto resolved = owner.topologyIndex()->resolveFace(reference);
  CHECK(resolved);
  const auto actualCenter = faceCenter(*resolved.subshape);
  CHECK(test::near(actualCenter.X(), expectedCenter.X(), 1e-5));
  CHECK(test::near(actualCenter.Y(), expectedCenter.Y(), 1e-5));
  CHECK(test::near(actualCenter.Z(), expectedCenter.Z(), 1e-5));
  const Point3d reconstructed =
      sketch.placement.toWorld(circleCenter.x, circleCenter.y);
  CHECK(test::near(reconstructed.x, actualCenter.X(), 1e-5));
  CHECK(test::near(reconstructed.y, actualCenter.Y(), 1e-5));
  CHECK(test::near(reconstructed.z, actualCenter.Z(), 1e-5));
  CHECK(document.findSketch(sketch.id) == &sketch);
}

void checkSamePlacement(const SketchPlacement& actual,
                        const SketchPlacement& expected) {
  CHECK(test::near(actual.origin.x, expected.origin.x));
  CHECK(test::near(actual.origin.y, expected.origin.y));
  CHECK(test::near(actual.origin.z, expected.origin.z));
  CHECK(test::near(actual.xDirection.x, expected.xDirection.x));
  CHECK(test::near(actual.xDirection.y, expected.xDirection.y));
  CHECK(test::near(actual.xDirection.z, expected.xDirection.z));
  CHECK(test::near(actual.yDirection.x, expected.yDirection.x));
  CHECK(test::near(actual.yDirection.y, expected.yDirection.y));
  CHECK(test::near(actual.yDirection.z, expected.yDirection.z));
}

void checkPocketGeometry(const Body& body, const ShapeFeature& base,
                         const FacePocketChain& chain) {
  CHECK(base.shape());
  CHECK(chain.pocket);
  CHECK(chain.pocket->isValid());
  CHECK(chain.pocket->shape());
  CHECK(body.resultShape());
  CHECK(body.resultShape()->IsSame(*chain.pocket->shape()));
  CHECK(test::solidCount(*base.shape()) ==
        test::solidCount(*chain.pocket->shape()));
  const double removed = test::volumeOf(*base.shape()) -
                         test::volumeOf(*chain.pocket->shape());
  const double expected =
      std::numbers::pi * chain.radiusMm * chain.radiusMm * chain.depthMm;
  const double tolerance = std::max(1e-5, expected * 1e-5);
  CHECK(test::near(removed, expected, tolerance));
}

void checkFailedAttachIsAtomic(Document& document, Body& body,
                               DocumentSketch& sketch,
                               PocketFeature& pocket,
                               const FaceReference& invalidReplacement) {
  const FaceReference support = sketch.support.face;
  const SketchPlacement placement = sketch.placement;
  const std::uint64_t placementRevision = sketch.placementRevision;
  const bool supportResolved = sketch.supportResolved;
  const bool placementDirty = sketch.placementDirty;
  const SketchId sketchId = sketch.id;
  const FeatureId pocketId = pocket.id();
  const auto currentPocket = pocket.shape();
  const auto lastValidPocket = pocket.lastValidShape();
  const auto currentBody = body.resultShape();
  const auto lastValidBody = body.lastValidResultShape();
  CHECK(currentPocket);
  CHECK(lastValidPocket);
  CHECK(currentBody);
  CHECK(lastValidBody);

  CHECK(!document.attachSketchToFace(sketch.id, invalidReplacement));
  CHECK(sketch.id == sketchId);
  CHECK(sketch.support.face == support);
  CHECK(sketch.support.type == SketchSupportType::Face);
  CHECK(sketch.supportResolved == supportResolved);
  CHECK(sketch.placementDirty == placementDirty);
  checkSamePlacement(sketch.placement, placement);
  CHECK(sketch.placementRevision == placementRevision);
  CHECK(document.findSketch(sketchId) == &sketch);
  CHECK(pocket.id() == pocketId);
  CHECK(document.findFeature(body.id(), pocketId) == &pocket);
  CHECK(pocket.shape() == currentPocket);
  CHECK(pocket.lastValidShape() == lastValidPocket);
  CHECK(body.resultShape() == currentBody);
  CHECK(body.lastValidResultShape() == lastValidBody);
  CHECK(pocket.shape()->IsSame(*currentPocket));
  CHECK(pocket.lastValidShape()->IsSame(*lastValidPocket));
  CHECK(body.resultShape()->IsSame(*currentBody));
  CHECK(body.lastValidResultShape()->IsSame(*lastValidBody));
}

}  // namespace

int main() {
  using namespace solidar;

  // The derived name is injective over every immediate-origin field and is
  // length-safe even when a source local name resembles the encoding suffix.
  {
    const TopologyProvenanceOrigin plain{
        101, TopologyKind::Face, TopologyProvenanceOperation::Modified,
        TopologySemanticRole::Original,
        "x;occurrence=present;owner=101;slot=7"};
    const TopologyProvenanceOrigin occurred{
        101, TopologyKind::Face, TopologyProvenanceOperation::Modified,
        TopologySemanticRole::Original, "x", TopologyOccurrenceKey{101, 7}};
    const TopologyProvenanceOrigin changedRole{
        101, TopologyKind::Face, TopologyProvenanceOperation::Modified,
        TopologySemanticRole::Reflected,
        "x;occurrence=present;owner=101;slot=7"};
    const auto plainName = transformDerivedLocalName(plain);
    const auto occurredName = transformDerivedLocalName(occurred);
    const auto changedRoleName = transformDerivedLocalName(changedRole);
    CHECK(plainName);
    CHECK(occurredName);
    CHECK(changedRoleName);
    CHECK(*plainName == independentlyEncodedName(plain));
    CHECK(*occurredName == independentlyEncodedName(occurred));
    CHECK(*changedRoleName == independentlyEncodedName(changedRole));
    CHECK(*plainName != *occurredName);
    CHECK(*plainName != *changedRoleName);
  }

  // Move is a direct one-to-one Modified branch with no occurrence. Edits
  // preserve exact keys and independently check semantic centers and areas.
  {
    Document document;
    auto& sketch = document.addSketch("Move lineage");
    sketch.geometry.addRectangle({0.0, 0.0}, {10.0, 6.0});
    auto& body = document.addBody();
    auto extrusion = std::make_unique<ExtrudeFeature>(sketch.id, 4.0);
    auto* source = extrusion.get();
    const auto sourceId = source->id();
    body.addFeature(std::move(extrusion));
    auto move = std::make_unique<MoveFeature>(
        sourceId, Vector3d{12.0, -3.0, 7.0});
    auto* moved = move.get();
    body.addFeature(std::move(move));
    CHECK(document.recompute());
    CHECK(source->topologyLineage());
    CHECK(moved->topologyLineage());
    CHECK(moved->topologyLineage()->coverage() ==
          TopologyLineageCoverage::CompleteFaces);
    CHECK(moved->shape()->ShapeType() == TopAbs_SOLID);
    checkAllFaceBranches(
        *source, *moved, body.id(),
        {{TopologyProvenanceOperation::Modified,
          TopologySemanticRole::Original, std::nullopt, false,
          [](const gp_Pnt& point) {
            return gp_Pnt(point.X() + 12.0, point.Y() - 3.0,
                          point.Z() + 7.0);
          }}});

    const auto sourceCapKey = authoredKey(
        AuthoredTopologySourceKind::ProfileStartCap, sketch.id,
        sketch::kInvalidGeometryId, sourceId);
    const auto& sourceCap = bindingFor(*source->topologyLineage(), sourceCapKey);
    const auto movedCapKey = transformedKey(
        sourceCapKey, moved->id(), TopologyProvenanceOperation::Modified,
        TopologySemanticRole::Original);
    CHECK(!movedCapKey.occurrence);
    auto movedCap = resolve(*moved, body.id(), movedCapKey);
    CHECK(movedCap);
    CHECK(movedCap.method == TopologyMatchMethod::OperationProvenance);
    auto before = faceCenter(*sourceCap.subshape);
    auto after = faceCenter(*movedCap.subshape);
    CHECK(test::near(after.X(), before.X() + 12.0));
    CHECK(test::near(after.Y(), before.Y() - 3.0));
    CHECK(test::near(after.Z(), before.Z() + 7.0));
    CHECK(test::near(faceArea(*movedCap.subshape), faceArea(*sourceCap.subshape)));

    TransformFeatureHistoryBuild sentinel{moved->shape(),
                                           moved->topologyLineage()};
    const auto sentinelShape = sentinel.shape;
    const auto sentinelLineage = sentinel.lineage;
    const TransformFeatureInstanceRequest invalidInstance{
        TransformFeatureInstanceRelation::ModifiedCopy, gp_Trsf{},
        static_cast<TopologySemanticRole>(255), std::nullopt};
    std::string sentinelError = "stale";
    CHECK(!buildTransformFeatureHistory(
        {source->id(), source->shape(), source->topologyLineage()},
        moved->id(),
        std::span<const TransformFeatureInstanceRequest>(&invalidInstance, 1),
        true, &sentinel, &sentinelError));
    CHECK(!sentinelError.empty());
    CHECK(sentinel.shape == sentinelShape);
    CHECK(sentinel.lineage == sentinelLineage);

    moved->setOffsetMm({-5.0, 9.0, 2.0});
    body.markDirtyFrom(1);
    CHECK(document.recompute());
    movedCap = resolve(*moved, body.id(), movedCapKey);
    CHECK(movedCap);
    after = faceCenter(*movedCap.subshape);
    CHECK(test::near(after.X(), before.X() - 5.0));
    CHECK(test::near(after.Y(), before.Y() + 9.0));
    CHECK(test::near(after.Z(), before.Z() + 2.0));
    checkAllFaceBranches(
        *source, *moved, body.id(),
        {{TopologyProvenanceOperation::Modified,
          TopologySemanticRole::Original, std::nullopt, false,
          [](const gp_Pnt& point) {
            return gp_Pnt(point.X() - 5.0, point.Y() + 9.0,
                          point.Z() + 2.0);
          }}});
    checkAtomicFailureRecovery(
        document, body, 1, *moved, movedCapKey,
        [&] {
          moved->setOffsetMm(
              {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0});
        },
        [&] { moved->setOffsetMm({-5.0, 9.0, 2.0}); });
  }

  // Mirror retains exact raw source faces at Original slot 0 and captures the
  // reflected copy at slot 1. A v1 face plane is resolved via source index.
  {
    Document document;
    auto& sketch = document.addSketch("Mirror lineage");
    sketch.geometry.addRectangle({10.0, 1.0}, {20.0, 9.0});
    auto& body = document.addBody();
    auto extrusion = std::make_unique<ExtrudeFeature>(sketch.id, 5.0);
    auto* source = extrusion.get();
    const auto sourceId = source->id();
    body.addFeature(std::move(extrusion));
    auto mirror = std::make_unique<MirrorFeature>(sourceId, MirrorPlane::YZ);
    auto* mirrored = mirror.get();
    body.addFeature(std::move(mirror));
    CHECK(document.recompute());
    CHECK(test::solidCount(*mirrored->shape()) == 2);
    checkAllFaceBranches(
        *source, *mirrored, body.id(),
        {{TopologyProvenanceOperation::Retained,
          TopologySemanticRole::Original, 0, true,
          [](const gp_Pnt& point) { return point; }},
         {TopologyProvenanceOperation::Modified,
          TopologySemanticRole::Reflected, 1, false,
          [](const gp_Pnt& point) {
            return gp_Pnt(-point.X(), point.Y(), point.Z());
          }}});

    const auto sourceCapKey = authoredKey(
        AuthoredTopologySourceKind::ProfileEndCap, sketch.id,
        sketch::kInvalidGeometryId, sourceId);
    const auto& sourceCap = bindingFor(*source->topologyLineage(), sourceCapKey);
    const auto retainedKey = transformedKey(
        sourceCapKey, mirrored->id(), TopologyProvenanceOperation::Retained,
        TopologySemanticRole::Original, 0);
    const auto reflectedKey = transformedKey(
        sourceCapKey, mirrored->id(), TopologyProvenanceOperation::Modified,
        TopologySemanticRole::Reflected, 1);
    const auto retained = resolve(*mirrored, body.id(), retainedKey);
    auto reflected = resolve(*mirrored, body.id(), reflectedKey);
    CHECK(retained);
    CHECK(reflected);
    CHECK(retained.subshape->IsSame(*sourceCap.subshape));
    const auto sourceCenter = faceCenter(*sourceCap.subshape);
    auto reflectedCenter = faceCenter(*reflected.subshape);
    CHECK(test::near(reflectedCenter.X(), -sourceCenter.X()));
    CHECK(test::near(reflectedCenter.Y(), sourceCenter.Y()));
    CHECK(test::near(reflectedCenter.Z(), sourceCenter.Z()));

    const auto top = test::topPlanarFace(*source->shape(), 5.0);
    CHECK(top);
    const auto planeFace = makeFaceReference(
        *source->shape(), body.id(), sourceId, *top);
    CHECK(planeFace.signature());
    mirrored->setPlaneFace(planeFace);
    body.markDirtyFrom(1);
    CHECK(document.recompute());
    checkAllFaceBranches(
        *source, *mirrored, body.id(),
        {{TopologyProvenanceOperation::Retained,
          TopologySemanticRole::Original, 0, true,
          [](const gp_Pnt& point) { return point; }},
         {TopologyProvenanceOperation::Modified,
          TopologySemanticRole::Reflected, 1, false,
          [](const gp_Pnt& point) {
            return gp_Pnt(point.X(), point.Y(), 10.0 - point.Z());
          }}});
    reflected = resolve(*mirrored, body.id(), reflectedKey);
    CHECK(reflected);
    reflectedCenter = faceCenter(*reflected.subshape);
    CHECK(test::near(reflectedCenter.X(), sourceCenter.X()));
    CHECK(test::near(reflectedCenter.Y(), sourceCenter.Y()));
    CHECK(test::near(reflectedCenter.Z(), 10.0 - sourceCenter.Z()));
    checkAtomicFailureRecovery(
        document, body, 1, *mirrored, reflectedKey,
        [&] { mirrored->setPlane(static_cast<MirrorPlane>(255)); },
        [&] { mirrored->setPlaneFace(planeFace); });
  }

  // Linear Join preserves slot 0 and stable authored slots through
  // 3 -> 5 -> 3 -> 5, spacing edits, failure rollback and recovery.
  {
    Document document;
    auto& sketch = document.addSketch("Linear lineage");
    sketch.geometry.addRectangle({0.0, 0.0}, {8.0, 4.0});
    auto& body = document.addBody();
    auto extrusion = std::make_unique<ExtrudeFeature>(sketch.id, 3.0);
    auto* source = extrusion.get();
    const auto sourceId = source->id();
    body.addFeature(std::move(extrusion));
    auto pattern = std::make_unique<LinearPatternFeature>(
        sourceId, PrincipalAxis::X, 3, 20.0);
    auto* linear = pattern.get();
    body.addFeature(std::move(pattern));
    CHECK(document.recompute());
    checkAllFaceBranches(
        *source, *linear, body.id(),
        {{TopologyProvenanceOperation::Retained,
          TopologySemanticRole::PatternInstance, 0, true,
          [](const gp_Pnt& point) { return point; }},
         {TopologyProvenanceOperation::Modified,
          TopologySemanticRole::PatternInstance, 1, false,
          [](const gp_Pnt& point) {
            return gp_Pnt(point.X() + 20.0, point.Y(), point.Z());
          }},
         {TopologyProvenanceOperation::Modified,
          TopologySemanticRole::PatternInstance, 2, false,
          [](const gp_Pnt& point) {
            return gp_Pnt(point.X() + 40.0, point.Y(), point.Z());
          }}});

    const auto sourceCapKey = authoredKey(
        AuthoredTopologySourceKind::ProfileStartCap, sketch.id,
        sketch::kInvalidGeometryId, sourceId);
    const auto& sourceCap = bindingFor(*source->topologyLineage(), sourceCapKey);
    const auto slot0 = transformedKey(
        sourceCapKey, linear->id(), TopologyProvenanceOperation::Retained,
        TopologySemanticRole::PatternInstance, 0);
    const auto slot2 = transformedKey(
        sourceCapKey, linear->id(), TopologyProvenanceOperation::Modified,
        TopologySemanticRole::PatternInstance, 2);
    const auto slot4 = transformedKey(
        sourceCapKey, linear->id(), TopologyProvenanceOperation::Modified,
        TopologySemanticRole::PatternInstance, 4);
    auto original = resolve(*linear, body.id(), slot0);
    auto copy2 = resolve(*linear, body.id(), slot2);
    CHECK(original);
    CHECK(copy2);
    CHECK(original.subshape->IsSame(*sourceCap.subshape));
    const auto sourceCenter = faceCenter(*sourceCap.subshape);
    CHECK(test::near(faceCenter(*copy2.subshape).X(), sourceCenter.X() + 40.0));
    checkMissing(*linear, body.id(), slot4);

    linear->setCount(5);
    body.markDirtyFrom(1);
    CHECK(document.recompute());
    CHECK(resolve(*linear, body.id(), slot4));
    linear->setCount(3);
    body.markDirtyFrom(1);
    CHECK(document.recompute());
    checkMissing(*linear, body.id(), slot4);
    linear->setCount(5);
    linear->setSpacingMm(15.0);
    body.markDirtyFrom(1);
    CHECK(document.recompute());
    auto regrown = resolve(*linear, body.id(), slot4);
    CHECK(regrown);
    CHECK(test::near(faceCenter(*regrown.subshape).X(),
                     sourceCenter.X() + 60.0));
    std::vector<ExpectedBranch> fiveBranches{
        {TopologyProvenanceOperation::Retained,
         TopologySemanticRole::PatternInstance, 0, true,
         [](const gp_Pnt& point) { return point; }}};
    for (std::uint64_t slot = 1; slot < 5; ++slot) {
      fiveBranches.push_back(
          {TopologyProvenanceOperation::Modified,
           TopologySemanticRole::PatternInstance, slot, false,
           [slot](const gp_Pnt& point) {
             return gp_Pnt(point.X() + 15.0 * slot, point.Y(), point.Z());
           }});
    }
    checkAllFaceBranches(*source, *linear, body.id(), fiveBranches);
    checkAtomicFailureRecovery(
        document, body, 1, *linear, slot4,
        [&] { linear->setSpacingMm(0.0); },
        [&] { linear->setSpacingMm(12.0); });
    fiveBranches.clear();
    fiveBranches.push_back(
        {TopologyProvenanceOperation::Retained,
         TopologySemanticRole::PatternInstance, 0, true,
         [](const gp_Pnt& point) { return point; }});
    for (std::uint64_t slot = 1; slot < 5; ++slot) {
      fiveBranches.push_back(
          {TopologyProvenanceOperation::Modified,
           TopologySemanticRole::PatternInstance, slot, false,
           [slot](const gp_Pnt& point) {
             return gp_Pnt(point.X() + 12.0 * slot, point.Y(), point.Z());
           }});
    }
    checkAllFaceBranches(*source, *linear, body.id(), fiveBranches);
  }

  // Circular Join uses the existing full/partial angle formulas, and a
  // NewBody pattern created before its source Body resolves by exact Feature.
  {
    Document joinDocument;
    auto& sketch = joinDocument.addSketch("Circular lineage");
    sketch.geometry.addRectangle({10.0, 0.0}, {18.0, 4.0});
    auto& body = joinDocument.addBody();
    auto extrusion = std::make_unique<ExtrudeFeature>(sketch.id, 3.0);
    auto* source = extrusion.get();
    const auto sourceId = source->id();
    body.addFeature(std::move(extrusion));
    auto pattern = std::make_unique<CircularPatternFeature>(
        sourceId, PrincipalAxis::Z, 3, 360.0);
    auto* circular = pattern.get();
    body.addFeature(std::move(pattern));
    CHECK(joinDocument.recompute());
    checkAllFaceBranches(
        *source, *circular, body.id(),
        {{TopologyProvenanceOperation::Retained,
          TopologySemanticRole::PatternInstance, 0, true,
          [](const gp_Pnt& point) { return point; }},
         {TopologyProvenanceOperation::Modified,
          TopologySemanticRole::PatternInstance, 1, false,
          [](const gp_Pnt& point) {
            return rotateZ(point, 2.0 * std::numbers::pi / 3.0);
          }},
         {TopologyProvenanceOperation::Modified,
          TopologySemanticRole::PatternInstance, 2, false,
          [](const gp_Pnt& point) {
            return rotateZ(point, 4.0 * std::numbers::pi / 3.0);
          }}});

    const auto sourceCapKey = authoredKey(
        AuthoredTopologySourceKind::ProfileStartCap, sketch.id,
        sketch::kInvalidGeometryId, sourceId);
    const auto& sourceCap = bindingFor(*source->topologyLineage(), sourceCapKey);
    const auto slot0 = transformedKey(
        sourceCapKey, circular->id(), TopologyProvenanceOperation::Retained,
        TopologySemanticRole::PatternInstance, 0);
    const auto slot1 = transformedKey(
        sourceCapKey, circular->id(), TopologyProvenanceOperation::Modified,
        TopologySemanticRole::PatternInstance, 1);
    const auto slot4 = transformedKey(
        sourceCapKey, circular->id(), TopologyProvenanceOperation::Modified,
        TopologySemanticRole::PatternInstance, 4);
    CHECK(resolve(*circular, body.id(), slot0).subshape->IsSame(
        *sourceCap.subshape));
    auto rotated = resolve(*circular, body.id(), slot1);
    CHECK(rotated);
    const auto center = faceCenter(*sourceCap.subshape);
    auto rotatedCenter = faceCenter(*rotated.subshape);
    const double angle = 2.0 * std::numbers::pi / 3.0;
    CHECK(test::near(rotatedCenter.X(),
                     center.X() * std::cos(angle) -
                         center.Y() * std::sin(angle)));
    CHECK(test::near(rotatedCenter.Y(),
                     center.X() * std::sin(angle) +
                         center.Y() * std::cos(angle)));
    checkMissing(*circular, body.id(), slot4);
    circular->setCount(5);
    body.markDirtyFrom(1);
    CHECK(joinDocument.recompute());
    CHECK(resolve(*circular, body.id(), slot4));
    circular->setCount(3);
    body.markDirtyFrom(1);
    CHECK(joinDocument.recompute());
    checkMissing(*circular, body.id(), slot4);
    circular->setCount(5);
    body.markDirtyFrom(1);
    CHECK(joinDocument.recompute());
    CHECK(resolve(*circular, body.id(), slot4));
    circular->setCount(3);
    circular->setAngleDeg(180.0);
    body.markDirtyFrom(1);
    CHECK(joinDocument.recompute());
    rotated = resolve(*circular, body.id(), slot1);
    CHECK(rotated);
    rotatedCenter = faceCenter(*rotated.subshape);
    CHECK(test::near(rotatedCenter.X(), -center.Y()));
    CHECK(test::near(rotatedCenter.Y(), center.X()));
    checkAllFaceBranches(
        *source, *circular, body.id(),
        {{TopologyProvenanceOperation::Retained,
          TopologySemanticRole::PatternInstance, 0, true,
          [](const gp_Pnt& point) { return point; }},
         {TopologyProvenanceOperation::Modified,
          TopologySemanticRole::PatternInstance, 1, false,
          [](const gp_Pnt& point) {
            return rotateZ(point, std::numbers::pi / 2.0);
          }},
         {TopologyProvenanceOperation::Modified,
          TopologySemanticRole::PatternInstance, 2, false,
          [](const gp_Pnt& point) {
            return rotateZ(point, std::numbers::pi);
          }}});
    checkAtomicFailureRecovery(
        joinDocument, body, 1, *circular, slot1,
        [&] { circular->setAngleDeg(0.0); },
        [&] { circular->setAngleDeg(180.0); });

    constexpr SketchId sourceSketchId = 910001;
    constexpr BodyId targetBodyId = 910002;
    constexpr BodyId sourceBodyId = 910003;
    constexpr FeatureId patternId = 910004;
    constexpr FeatureId extrusionId = 910005;
    Document reversedBodies;
    sketch::Sketch sourceGeometry;
    sourceGeometry.addRectangle({5.0, 0.0}, {9.0, 3.0});
    reversedBodies.addSketch(sourceSketchId, "Late source",
                             std::move(sourceGeometry));
    auto& targetBody = reversedBodies.addBody(targetBodyId, "First target");
    auto newBodyPattern = std::make_unique<CircularPatternFeature>(
        patternId, sourceBodyId, extrusionId, PrincipalAxis::Z, 3, 360.0,
        PatternOperation::NewBody, "Copies");
    auto* newBody = newBodyPattern.get();
    targetBody.addFeature(std::move(newBodyPattern));
    auto& lateSourceBody =
        reversedBodies.addBody(sourceBodyId, "Second source");
    auto lateExtrusion = std::make_unique<ExtrudeFeature>(
        extrusionId, sourceSketchId, 2.0, "Source");
    auto* lateSource = lateExtrusion.get();
    lateSourceBody.addFeature(std::move(lateExtrusion));
    CHECK(reversedBodies.recompute());
    CHECK(newBody->topologyLineage());
    CHECK(test::solidCount(*newBody->shape()) == 2);
    const auto lateCapKey = authoredKey(
        AuthoredTopologySourceKind::ProfileEndCap, sourceSketchId,
        sketch::kInvalidGeometryId, extrusionId);
    const auto absentSlot0 = transformedKey(
        lateCapKey, patternId, TopologyProvenanceOperation::Retained,
        TopologySemanticRole::PatternInstance, 0);
    checkMissing(*newBody, targetBodyId, absentSlot0);
    for (std::uint64_t slot = 1; slot < 3; ++slot) {
      const auto key = transformedKey(
          lateCapKey, patternId, TopologyProvenanceOperation::Modified,
          TopologySemanticRole::PatternInstance, slot);
      CHECK(resolve(*newBody, targetBodyId, key));
    }
    CHECK(lateSource->topologyLineage());
    checkAllFaceBranches(
        *lateSource, *newBody, targetBodyId,
        {{TopologyProvenanceOperation::Modified,
          TopologySemanticRole::PatternInstance, 1, false,
          [](const gp_Pnt& point) {
            return rotateZ(point, 2.0 * std::numbers::pi / 3.0);
          }},
         {TopologyProvenanceOperation::Modified,
          TopologySemanticRole::PatternInstance, 2, false,
          [](const gp_Pnt& point) {
            return rotateZ(point, 4.0 * std::numbers::pi / 3.0);
          }}});
    const auto lateSlot4 = transformedKey(
        lateCapKey, patternId, TopologyProvenanceOperation::Modified,
        TopologySemanticRole::PatternInstance, 4);
    const TopologyReferenceV2 lateSlot4Reference{targetBodyId, lateSlot4};
    checkMissing(*newBody, targetBodyId, lateSlot4Reference.provenance);
    newBody->setCount(5);
    reversedBodies.findBody(targetBodyId)->markDirtyFrom(0);
    CHECK(reversedBodies.recompute());
    CHECK(resolve(*newBody, targetBodyId, lateSlot4Reference.provenance));
    newBody->setCount(3);
    reversedBodies.findBody(targetBodyId)->markDirtyFrom(0);
    CHECK(reversedBodies.recompute());
    checkMissing(*newBody, targetBodyId, lateSlot4Reference.provenance);
    newBody->setCount(5);
    newBody->setAngleDeg(180.0);
    reversedBodies.findBody(targetBodyId)->markDirtyFrom(0);
    CHECK(reversedBodies.recompute());
    auto regrownLate =
        resolve(*newBody, targetBodyId, lateSlot4Reference.provenance);
    CHECK(regrownLate);
    CHECK(test::near(faceCenter(*regrownLate.subshape).Z(), 2.0));
    std::vector<ExpectedBranch> circularNewBodyBranches;
    for (std::uint64_t slot = 1; slot < 5; ++slot) {
      circularNewBodyBranches.push_back(
          {TopologyProvenanceOperation::Modified,
           TopologySemanticRole::PatternInstance, slot, false,
           [slot](const gp_Pnt& point) {
             return rotateZ(point, static_cast<double>(slot) *
                                       std::numbers::pi / 4.0);
           }});
    }
    checkAllFaceBranches(*lateSource, *newBody, targetBodyId,
                         circularNewBodyBranches);
    lateSource->setLengthMm(4.0);
    lateSourceBody.markDirtyFrom(0);
    CHECK(reversedBodies.recompute());
    regrownLate = resolve(*newBody, targetBodyId,
                          lateSlot4Reference.provenance);
    CHECK(regrownLate);
    CHECK(test::near(faceCenter(*regrownLate.subshape).Z(), 4.0));
    checkAllFaceBranches(*lateSource, *newBody, targetBodyId,
                         circularNewBodyBranches);
  }

  // A second NewBody producer covers linear slots without renumbering.
  {
    Document document;
    auto& sketch = document.addSketch("Linear NewBody");
    sketch.geometry.addRectangle({0.0, 0.0}, {4.0, 3.0});
    auto& sourceBody = document.addBody("Source");
    auto extrusion = std::make_unique<ExtrudeFeature>(sketch.id, 2.0);
    auto* source = extrusion.get();
    const auto sourceId = source->id();
    const auto sourceBodyId = sourceBody.id();
    sourceBody.addFeature(std::move(extrusion));
    auto& targetBody = document.addBody("Copies");
    const auto targetBodyId = targetBody.id();
    auto pattern = std::make_unique<LinearPatternFeature>(
        sourceBodyId, sourceId, PrincipalAxis::Y, 3, 11.0,
        PatternOperation::NewBody);
    auto* linear = pattern.get();
    targetBody.addFeature(std::move(pattern));
    CHECK(document.recompute());
    CHECK(test::solidCount(*linear->shape()) == 2);
    const auto sourceCapKey = authoredKey(
        AuthoredTopologySourceKind::ProfileEndCap, sketch.id,
        sketch::kInvalidGeometryId, sourceId);
    const auto slot0 = transformedKey(
        sourceCapKey, linear->id(), TopologyProvenanceOperation::Retained,
        TopologySemanticRole::PatternInstance, 0);
    checkMissing(*linear, targetBodyId, slot0);
    const auto sourceCenter = faceCenter(
        *bindingFor(*source->topologyLineage(), sourceCapKey).subshape);
    for (std::uint64_t slot = 1; slot < 3; ++slot) {
      const auto key = transformedKey(
          sourceCapKey, linear->id(), TopologyProvenanceOperation::Modified,
          TopologySemanticRole::PatternInstance, slot);
      const auto copied = resolve(*linear, targetBodyId, key);
      CHECK(copied);
      CHECK(test::near(faceCenter(*copied.subshape).Y(),
                       sourceCenter.Y() + 11.0 * slot));
    }
    checkAllFaceBranches(
        *source, *linear, targetBodyId,
        {{TopologyProvenanceOperation::Modified,
          TopologySemanticRole::PatternInstance, 1, false,
          [](const gp_Pnt& point) {
            return gp_Pnt(point.X(), point.Y() + 11.0, point.Z());
          }},
         {TopologyProvenanceOperation::Modified,
          TopologySemanticRole::PatternInstance, 2, false,
          [](const gp_Pnt& point) {
            return gp_Pnt(point.X(), point.Y() + 22.0, point.Z());
          }}});
    const auto slot4 = transformedKey(
        sourceCapKey, linear->id(), TopologyProvenanceOperation::Modified,
        TopologySemanticRole::PatternInstance, 4);
    const TopologyReferenceV2 slot4Reference{targetBodyId, slot4};
    checkMissing(*linear, targetBodyId, slot4Reference.provenance);
    linear->setCount(5);
    targetBody.markDirtyFrom(0);
    CHECK(document.recompute());
    CHECK(resolve(*linear, targetBodyId, slot4Reference.provenance));
    linear->setCount(3);
    targetBody.markDirtyFrom(0);
    CHECK(document.recompute());
    checkMissing(*linear, targetBodyId, slot4Reference.provenance);
    linear->setCount(5);
    linear->setSpacingMm(7.0);
    targetBody.markDirtyFrom(0);
    CHECK(document.recompute());
    auto regrown = resolve(*linear, targetBodyId, slot4Reference.provenance);
    CHECK(regrown);
    CHECK(test::near(faceCenter(*regrown.subshape).Y(),
                     sourceCenter.Y() + 28.0));
    CHECK(test::near(faceCenter(*regrown.subshape).Z(), 2.0));
    std::vector<ExpectedBranch> linearNewBodyBranches;
    for (std::uint64_t slot = 1; slot < 5; ++slot) {
      linearNewBodyBranches.push_back(
          {TopologyProvenanceOperation::Modified,
           TopologySemanticRole::PatternInstance, slot, false,
           [slot](const gp_Pnt& point) {
             return gp_Pnt(point.X(), point.Y() + 7.0 * slot, point.Z());
           }});
    }
    checkAllFaceBranches(*source, *linear, targetBodyId,
                         linearNewBodyBranches);
    source->setLengthMm(4.0);
    document.findBody(sourceBodyId)->markDirtyFrom(0);
    CHECK(document.recompute());
    regrown = resolve(*linear, targetBodyId, slot4Reference.provenance);
    CHECK(regrown);
    CHECK(test::near(faceCenter(*regrown.subshape).Z(), 4.0));
    checkAllFaceBranches(*source, *linear, targetBodyId,
                         linearNewBodyBranches);
  }

  // A reflected v2 cap remains the authoritative support for a downstream
  // Sketch/Pocket chain when the source Extrude changes. The profile is
  // authored at the selected face's center of mass in face-local coordinates.
  {
    Document document;
    auto& sourceSketch = document.addSketch("Mirror pocket source");
    sourceSketch.geometry.addRectangle({10.0, 2.0}, {18.0, 10.0});
    auto& body = document.addBody("Mirror pocket body");
    const BodyId bodyId = body.id();
    auto extrusion = std::make_unique<ExtrudeFeature>(sourceSketch.id, 5.0);
    auto* source = extrusion.get();
    const auto sourceId = source->id();
    body.addFeature(std::move(extrusion));
    auto mirror = std::make_unique<MirrorFeature>(sourceId, MirrorPlane::YZ);
    auto* mirrored = mirror.get();
    body.addFeature(std::move(mirror));
    CHECK(document.recompute());

    const auto sourceCap = authoredKey(
        AuthoredTopologySourceKind::ProfileEndCap, sourceSketch.id,
        sketch::kInvalidGeometryId, sourceId);
    const auto reflectedCap = transformedKey(
        sourceCap, mirrored->id(), TopologyProvenanceOperation::Modified,
        TopologySemanticRole::Reflected, 1);
    const auto initialFace = resolve(*mirrored, body.id(), reflectedCap);
    CHECK(initialFace);
    const gp_Pnt initialCenter = faceCenter(*initialFace.subshape);
    const auto chain = addPocketOnExactFace(
        document, body, *mirrored, reflectedCap, 1.25, 2.0,
        "Reflected cap pocket");
    CHECK(document.recompute());
    CHECK(chain.pocket->id() == chain.pocketId);
    CHECK(chain.pocket->isValid());
    CHECK(chain.pocket->shape());
    checkPocketGeometry(body, *mirrored, chain);
    const auto* attached = document.findSketch(chain.sketchId);
    CHECK(attached);
    checkSupportAtExactFace(document, *attached, *mirrored, chain.reference,
                            chain.circleCenter, initialCenter);
    const double initialPlacementZ = attached->placement.origin.z;

    source->setLengthMm(8.0);
    CHECK(document.recompute());
    CHECK(chain.pocket->id() == chain.pocketId);
    CHECK(chain.pocket->isValid());
    CHECK(chain.pocket->shape());
    CHECK(attached->support.face == chain.reference);
    const auto movedFace =
        mirrored->topologyIndex()->resolveFace(chain.reference);
    CHECK(movedFace);
    const gp_Pnt movedCenter = faceCenter(*movedFace.subshape);
    CHECK(!test::near(movedCenter.Z(), initialCenter.Z()));
    CHECK(!test::near(attached->placement.origin.z, initialPlacementZ));
    checkSupportAtExactFace(document, *attached, *mirrored, chain.reference,
                            chain.circleCenter, movedCenter);
    checkPocketGeometry(body, *mirrored, chain);

    // Failed reattachment is transactional even when both IDs exist but do
    // not form the owning Body/Feature pair, and when the Feature is another
    // real v2 producer.
    auto& foreignSketch = document.addSketch("Foreign v2 producer source");
    foreignSketch.geometry.addRectangle({30.0, 2.0}, {36.0, 8.0});
    const SketchId foreignSketchId = foreignSketch.id;
    auto& foreignBody = document.addBody("Foreign v2 producer body");
    const BodyId foreignBodyId = foreignBody.id();
    auto foreignExtrusion =
        std::make_unique<ExtrudeFeature>(foreignSketchId, 4.0);
    auto* foreignProducer = foreignExtrusion.get();
    foreignBody.addFeature(std::move(foreignExtrusion));
    CHECK(document.recompute());
    const auto foreignCap = authoredKey(
        AuthoredTopologySourceKind::ProfileEndCap, foreignSketchId,
        sketch::kInvalidGeometryId, foreignProducer->id());
    const FaceReference foreignReference = createExactFaceReferenceV2(
        document, foreignBody, *foreignProducer, foreignCap);
    CHECK(foreignReference.v2());

    auto* primaryBody = document.findBody(bodyId);
    auto* currentAttached = document.findSketch(chain.sketchId);
    CHECK(primaryBody);
    CHECK(currentAttached);
    CHECK(document.findBody(foreignBodyId));
    CHECK(document.findFeature(bodyId, mirrored->id()) == mirrored);
    CHECK(document.findFeature(foreignBodyId, foreignProducer->id()) ==
          foreignProducer);
    const FaceReference mismatchedBody = FaceReference::fromV2(
        {foreignBodyId, chain.reference.v2()->provenance});
    CHECK(mismatchedBody.isV2());
    CHECK(!document.findFeature(mismatchedBody.bodyId(),
                                mismatchedBody.featureId()));
    checkFailedAttachIsAtomic(document, *primaryBody, *currentAttached,
                              *chain.pocket, mismatchedBody);
    const FaceReference mismatchedProducer = FaceReference::fromV2(
        {bodyId, foreignReference.v2()->provenance});
    CHECK(mismatchedProducer.isV2());
    CHECK(!document.findFeature(mismatchedProducer.bodyId(),
                                mismatchedProducer.featureId()));
    checkFailedAttachIsAtomic(document, *primaryBody, *currentAttached,
                              *chain.pocket, mismatchedProducer);
  }

  // Linear slot 4 can disappear and recover without weakening the saved v2
  // identity or silently selecting another face. A missing support blocks the
  // Pocket while retaining its last valid result and the last valid placement.
  {
    Document document;
    auto& sourceSketch = document.addSketch("Linear pocket source");
    sourceSketch.geometry.addRectangle({1.0, 2.0}, {7.0, 8.0});
    auto& body = document.addBody("Linear pocket body");
    auto extrusion = std::make_unique<ExtrudeFeature>(sourceSketch.id, 5.0);
    auto* source = extrusion.get();
    const auto sourceId = source->id();
    body.addFeature(std::move(extrusion));
    auto pattern = std::make_unique<LinearPatternFeature>(
        sourceId, PrincipalAxis::X, 5, 12.0);
    auto* linear = pattern.get();
    body.addFeature(std::move(pattern));
    CHECK(document.recompute());

    const auto sourceCap = authoredKey(
        AuthoredTopologySourceKind::ProfileEndCap, sourceSketch.id,
        sketch::kInvalidGeometryId, sourceId);
    const auto slot4 = transformedKey(
        sourceCap, linear->id(), TopologyProvenanceOperation::Modified,
        TopologySemanticRole::PatternInstance, 4);
    const gp_Pnt sourceCenter =
        faceCenter(*bindingFor(*source->topologyLineage(), sourceCap).subshape);
    const auto chain = addPocketOnExactFace(
        document, body, *linear, slot4, 1.0, 2.0, "Linear slot pocket");
    CHECK(document.recompute());
    CHECK(chain.pocket->isValid());
    CHECK(chain.pocket->id() == chain.pocketId);
    checkPocketGeometry(body, *linear, chain);
    auto* attached = document.findSketch(chain.sketchId);
    CHECK(attached);
    const auto initial = linear->topologyIndex()->resolveFace(chain.reference);
    CHECK(initial);
    const gp_Pnt initialCenter = faceCenter(*initial.subshape);
    CHECK(test::near(initialCenter.X(), sourceCenter.X() + 48.0, 1e-5));
    const gp_Pnt expectedInitial{sourceCenter.X() + 48.0, sourceCenter.Y(),
                                 sourceCenter.Z()};
    checkSupportAtExactFace(document, *attached, *linear, chain.reference,
                            chain.circleCenter, expectedInitial);

    linear->setSpacingMm(15.0);
    CHECK(document.recompute());
    CHECK(attached->support.face == chain.reference);
    CHECK(chain.pocket->id() == chain.pocketId);
    CHECK(chain.pocket->isValid());
    const auto spaced = linear->topologyIndex()->resolveFace(chain.reference);
    CHECK(spaced);
    const gp_Pnt spacedCenter = faceCenter(*spaced.subshape);
    CHECK(test::near(spacedCenter.X(), initialCenter.X() + 12.0, 1e-5));
    checkSupportAtExactFace(document, *attached, *linear, chain.reference,
                            chain.circleCenter, spacedCenter);
    checkPocketGeometry(body, *linear, chain);
    const auto lastValidPocket = chain.pocket->shape();
    CHECK(lastValidPocket);
    const SketchPlacement lastValidPlacement = attached->placement;

    linear->setCount(3);
    CHECK(!document.recompute());
    const auto missing = linear->topologyIndex()->resolveFace(chain.reference);
    CHECK(!missing);
    CHECK(missing.failure == TopologyResolutionFailure::Missing);
    CHECK(!attached->supportResolved);
    CHECK(attached->support.face == chain.reference);
    checkSamePlacement(attached->placement, lastValidPlacement);
    CHECK(chain.pocket->id() == chain.pocketId);
    CHECK(chain.pocket->state() == FeatureState::Error);
    CHECK(chain.pocket->error().find("Blocked") != std::string::npos);
    CHECK(!chain.pocket->shape());
    CHECK(chain.pocket->lastValidShape() == lastValidPocket);
    CHECK(!body.resultShape());
    CHECK(body.lastValidResultShape());
    CHECK(body.lastValidResultShape()->IsSame(*lastValidPocket));
    for (std::uint64_t remainingSlot = 0; remainingSlot < 3;
         ++remainingSlot) {
      const double remainingCenterX =
          sourceCenter.X() + 15.0 * remainingSlot;
      CHECK(std::abs(spacedCenter.X() - remainingCenterX) >
            3.0 + chain.radiusMm);
    }

    linear->setSpacingMm(18.0);
    linear->setCount(5);
    CHECK(document.recompute());
    CHECK(attached->supportResolved);
    CHECK(attached->support.face == chain.reference);
    CHECK(chain.pocket->id() == chain.pocketId);
    CHECK(chain.pocket->isValid());
    CHECK(chain.pocket->shape());
    const auto regrown = linear->topologyIndex()->resolveFace(chain.reference);
    CHECK(regrown);
    const gp_Pnt regrownCenter = faceCenter(*regrown.subshape);
    CHECK(test::near(regrownCenter.X(), initialCenter.X() + 24.0, 1e-5));
    checkSupportAtExactFace(document, *attached, *linear, chain.reference,
                            chain.circleCenter, regrownCenter);
    for (std::uint64_t otherSlot = 0; otherSlot < 4; ++otherSlot) {
      const double otherCenterX = sourceCenter.X() + 18.0 * otherSlot;
      CHECK(std::abs(spacedCenter.X() - otherCenterX) >
            3.0 + chain.radiusMm);
    }
    checkPocketGeometry(body, *linear, chain);
  }

  // Circular slot 4 likewise survives angular edits and a count-driven
  // disappearance/regrowth cycle with the exact same v2 carrier.
  {
    Document document;
    auto& sourceSketch = document.addSketch("Circular pocket source");
    sourceSketch.geometry.addRectangle({10.0, 2.0}, {16.0, 8.0});
    auto& body = document.addBody("Circular pocket body");
    auto extrusion = std::make_unique<ExtrudeFeature>(sourceSketch.id, 5.0);
    auto* source = extrusion.get();
    const auto sourceId = source->id();
    body.addFeature(std::move(extrusion));
    auto pattern = std::make_unique<CircularPatternFeature>(
        sourceId, PrincipalAxis::Z, 5, 360.0);
    auto* circular = pattern.get();
    body.addFeature(std::move(pattern));
    CHECK(document.recompute());

    const auto sourceCap = authoredKey(
        AuthoredTopologySourceKind::ProfileEndCap, sourceSketch.id,
        sketch::kInvalidGeometryId, sourceId);
    const auto slot4 = transformedKey(
        sourceCap, circular->id(), TopologyProvenanceOperation::Modified,
        TopologySemanticRole::PatternInstance, 4);
    const gp_Pnt sourceCenter =
        faceCenter(*bindingFor(*source->topologyLineage(), sourceCap).subshape);
    const auto chain = addPocketOnExactFace(
        document, body, *circular, slot4, 1.0, 2.0,
        "Circular slot pocket");
    CHECK(document.recompute());
    CHECK(chain.pocket->isValid());
    CHECK(chain.pocket->id() == chain.pocketId);
    checkPocketGeometry(body, *circular, chain);
    auto* attached = document.findSketch(chain.sketchId);
    CHECK(attached);
    const auto initial = circular->topologyIndex()->resolveFace(chain.reference);
    CHECK(initial);
    const gp_Pnt initialCenter = faceCenter(*initial.subshape);
    const gp_Pnt expectedInitial = rotateZ(
        sourceCenter, 8.0 * std::numbers::pi / 5.0);
    checkSupportAtExactFace(document, *attached, *circular, chain.reference,
                            chain.circleCenter, expectedInitial);

    circular->setAngleDeg(240.0);
    CHECK(document.recompute());
    CHECK(attached->support.face == chain.reference);
    CHECK(chain.pocket->id() == chain.pocketId);
    CHECK(chain.pocket->isValid());
    const auto angled = circular->topologyIndex()->resolveFace(chain.reference);
    CHECK(angled);
    const gp_Pnt angledCenter = faceCenter(*angled.subshape);
    const gp_Pnt expectedAngled =
        rotateZ(sourceCenter, 4.0 * std::numbers::pi / 3.0);
    CHECK(test::near(angledCenter.X(), expectedAngled.X(), 1e-5));
    CHECK(test::near(angledCenter.Y(), expectedAngled.Y(), 1e-5));
    CHECK(test::near(angledCenter.Z(), expectedAngled.Z(), 1e-5));
    CHECK(!test::near(angledCenter.X(), initialCenter.X()) ||
          !test::near(angledCenter.Y(), initialCenter.Y()));
    checkSupportAtExactFace(document, *attached, *circular, chain.reference,
                            chain.circleCenter, expectedAngled);
    checkPocketGeometry(body, *circular, chain);
    const auto lastValidPocket = chain.pocket->shape();
    CHECK(lastValidPocket);
    const SketchPlacement lastValidPlacement = attached->placement;

    circular->setCount(3);
    CHECK(!document.recompute());
    const auto missing = circular->topologyIndex()->resolveFace(chain.reference);
    CHECK(!missing);
    CHECK(missing.failure == TopologyResolutionFailure::Missing);
    CHECK(!attached->supportResolved);
    CHECK(attached->support.face == chain.reference);
    checkSamePlacement(attached->placement, lastValidPlacement);
    CHECK(chain.pocket->id() == chain.pocketId);
    CHECK(chain.pocket->state() == FeatureState::Error);
    CHECK(chain.pocket->error().find("Blocked") != std::string::npos);
    CHECK(!chain.pocket->shape());
    CHECK(chain.pocket->lastValidShape() == lastValidPocket);
    CHECK(!body.resultShape());
    CHECK(body.lastValidResultShape());
    CHECK(body.lastValidResultShape()->IsSame(*lastValidPocket));

    circular->setAngleDeg(300.0);
    circular->setCount(5);
    CHECK(document.recompute());
    CHECK(attached->supportResolved);
    CHECK(attached->support.face == chain.reference);
    CHECK(chain.pocket->id() == chain.pocketId);
    CHECK(chain.pocket->isValid());
    CHECK(chain.pocket->shape());
    const auto regrown = circular->topologyIndex()->resolveFace(chain.reference);
    CHECK(regrown);
    const gp_Pnt regrownCenter = faceCenter(*regrown.subshape);
    const gp_Pnt expectedRegrown = rotateZ(
        sourceCenter, 5.0 * std::numbers::pi / 3.0);
    CHECK(test::near(regrownCenter.X(), expectedRegrown.X(), 1e-5));
    CHECK(test::near(regrownCenter.Y(), expectedRegrown.Y(), 1e-5));
    CHECK(test::near(regrownCenter.Z(), expectedRegrown.Z(), 1e-5));
    checkSupportAtExactFace(document, *attached, *circular, chain.reference,
                            chain.circleCenter, expectedRegrown);
    checkPocketGeometry(body, *circular, chain);
  }

  // A full production chain retains only immediate origins while nested
  // length-safe names remain unique and exact-resolvable.
  {
    Document document;
    auto& sketch = document.addSketch("Lineage chain");
    sketch.geometry.addRectangle({3.0, 1.0}, {7.0, 5.0});
    const auto sideGeometryId = sketch.geometry.lineId(0);
    auto& body = document.addBody();
    auto extrusion = std::make_unique<ExtrudeFeature>(sketch.id, 2.0);
    auto* extrusionFeature = extrusion.get();
    const auto extrusionId = extrusion->id();
    body.addFeature(std::move(extrusion));
    auto move = std::make_unique<MoveFeature>(
        extrusionId, Vector3d{2.0, 0.0, 0.0});
    auto* moveFeature = move.get();
    const auto moveId = move->id();
    body.addFeature(std::move(move));
    auto mirror = std::make_unique<MirrorFeature>(moveId, MirrorPlane::YZ);
    auto* mirrorFeature = mirror.get();
    const auto mirrorId = mirror->id();
    body.addFeature(std::move(mirror));
    auto pattern = std::make_unique<LinearPatternFeature>(
        mirrorId, PrincipalAxis::Y, 2, 20.0);
    auto* finalPattern = pattern.get();
    body.addFeature(std::move(pattern));
    CHECK(document.recompute());
    CHECK(finalPattern->topologyLineage());
    for (const auto& binding : finalPattern->topologyLineage()->bindings()) {
      CHECK(binding.provenance.origins.size() == 1);
      CHECK(binding.provenance.origins.front().featureId == mirrorId);
      CHECK(binding.provenance.localName.find("name-length=") !=
            std::string::npos);
      const auto exact = resolve(*finalPattern, body.id(), binding.provenance);
      CHECK(exact);
      CHECK(exact.subshape->IsSame(*binding.subshape));
    }
    const std::array sourceKeys{
        authoredKey(AuthoredTopologySourceKind::ProfileStartCap, sketch.id,
                    sketch::kInvalidGeometryId, extrusionId),
        authoredKey(AuthoredTopologySourceKind::GeometrySide, sketch.id,
                    sideGeometryId, extrusionId)};
    for (const auto& sourceKey : sourceKeys) {
      const auto& sourceBinding =
          bindingFor(*extrusionFeature->topologyLineage(), sourceKey);
      const auto movedKey = transformedKey(
          sourceKey, moveId, TopologyProvenanceOperation::Modified,
          TopologySemanticRole::Original);
      CHECK(resolve(*moveFeature, body.id(), movedKey));
      const auto mirrorOriginalKey = transformedKey(
          movedKey, mirrorId, TopologyProvenanceOperation::Retained,
          TopologySemanticRole::Original, 0);
      const auto mirrorReflectedKey = transformedKey(
          movedKey, mirrorId, TopologyProvenanceOperation::Modified,
          TopologySemanticRole::Reflected, 1);
      CHECK(resolve(*mirrorFeature, body.id(), mirrorOriginalKey));
      CHECK(resolve(*mirrorFeature, body.id(), mirrorReflectedKey));
      const auto baseCenter = faceCenter(*sourceBinding.subshape);
      for (const auto& mirrorBranch :
           std::array{std::pair{mirrorOriginalKey, false},
                      std::pair{mirrorReflectedKey, true}}) {
        for (std::uint64_t patternSlot = 0; patternSlot < 2; ++patternSlot) {
          const auto finalKey = transformedKey(
              mirrorBranch.first, finalPattern->id(),
              patternSlot == 0 ? TopologyProvenanceOperation::Retained
                               : TopologyProvenanceOperation::Modified,
              TopologySemanticRole::PatternInstance, patternSlot);
          const auto finalFace = resolve(*finalPattern, body.id(), finalKey);
          CHECK(finalFace);
          const double movedX = baseCenter.X() + 2.0;
          const double expectedX = mirrorBranch.second ? -movedX : movedX;
          CHECK(test::near(faceCenter(*finalFace.subshape).X(), expectedX));
          CHECK(test::near(faceCenter(*finalFace.subshape).Y(),
                           baseCenter.Y() + 20.0 * patternSlot));
          CHECK(test::near(faceCenter(*finalFace.subshape).Z(),
                           baseCenter.Z()));
          CHECK(test::near(faceArea(*finalFace.subshape),
                           faceArea(*sourceBinding.subshape)));
          CHECK(finalKey.origins.size() == 1);
          CHECK(finalKey.origins.front() ==
                mirrorBranch.first.immediateOrigin());
        }
      }
    }
  }

  // Legacy multi-region inputs remain on v1 with no invented v2 lineage.
  {
    Document document;
    auto& sketch = document.addSketch("Legacy transform source");
    sketch.geometry.addRectangle({0.0, 0.0}, {4.0, 4.0});
    sketch.geometry.addRectangle({8.0, 0.0}, {12.0, 4.0});
    auto& body = document.addBody();
    auto extrusion = std::make_unique<ExtrudeFeature>(sketch.id, 2.0);
    const auto sourceId = extrusion->id();
    body.addFeature(std::move(extrusion));
    auto move = std::make_unique<MoveFeature>(
        sourceId, Vector3d{1.0, 2.0, 3.0});
    auto* moved = move.get();
    const auto moveId = moved->id();
    body.addFeature(std::move(move));
    auto mirror = std::make_unique<MirrorFeature>(moveId, MirrorPlane::YZ);
    auto* mirrored = mirror.get();
    const auto mirrorId = mirrored->id();
    body.addFeature(std::move(mirror));
    auto linearPattern = std::make_unique<LinearPatternFeature>(
        mirrorId, PrincipalAxis::Y, 2, 20.0);
    auto* linear = linearPattern.get();
    const auto linearId = linear->id();
    body.addFeature(std::move(linearPattern));
    auto circularPattern = std::make_unique<CircularPatternFeature>(
        linearId, PrincipalAxis::Z, 2, 180.0);
    auto* circular = circularPattern.get();
    body.addFeature(std::move(circularPattern));
    CHECK(document.recompute());
    checkLegacyFeature(*body.features().front(), body.id());
    checkLegacyFeature(*moved, body.id());
    checkLegacyFeature(*mirrored, body.id());
    checkLegacyFeature(*linear, body.id());
    checkLegacyFeature(*circular, body.id());
  }

  // Legacy Mirror face-plane still resolves its v1 reference through the
  // source Feature index, without manufacturing lineage.
  {
    Document document;
    auto& sketch = document.addSketch("Legacy face-plane mirror");
    sketch.geometry.addRectangle({0.0, 0.0}, {4.0, 4.0});
    sketch.geometry.addRectangle({8.0, 0.0}, {12.0, 4.0});
    auto& body = document.addBody();
    auto extrusion = std::make_unique<ExtrudeFeature>(sketch.id, 2.0);
    auto* source = extrusion.get();
    const auto sourceId = source->id();
    body.addFeature(std::move(extrusion));
    CHECK(document.recompute());
    CHECK(!source->topologyLineage());
    const auto top = test::topPlanarFace(*source->shape(), 2.0);
    CHECK(top);
    const auto plane =
        makeFaceReference(*source->shape(), body.id(), sourceId, *top);
    auto mirror = std::make_unique<MirrorFeature>(sourceId, plane);
    auto* mirrored = mirror.get();
    body.addFeature(std::move(mirror));
    CHECK(document.recompute());
    checkLegacyFeature(*mirrored, body.id());
  }

  // Cross-body legacy NewBody patterns preserve the old v1-only path for
  // both pattern producers.
  {
    Document document;
    auto& sketch = document.addSketch("Legacy pattern source");
    sketch.geometry.addRectangle({0.0, 0.0}, {4.0, 4.0});
    sketch.geometry.addRectangle({8.0, 0.0}, {12.0, 4.0});
    auto& sourceBody = document.addBody("Legacy source");
    auto extrusion = std::make_unique<ExtrudeFeature>(sketch.id, 2.0);
    const auto sourceId = extrusion->id();
    const auto sourceBodyId = sourceBody.id();
    sourceBody.addFeature(std::move(extrusion));
    auto& linearBody = document.addBody("Legacy linear copies");
    const auto linearBodyId = linearBody.id();
    auto linearPattern = std::make_unique<LinearPatternFeature>(
        sourceBodyId, sourceId, PrincipalAxis::X, 3, 20.0,
        PatternOperation::NewBody);
    auto* linear = linearPattern.get();
    linearBody.addFeature(std::move(linearPattern));
    auto& circularBody = document.addBody("Legacy circular copies");
    const auto circularBodyId = circularBody.id();
    auto circularPattern = std::make_unique<CircularPatternFeature>(
        sourceBodyId, sourceId, PrincipalAxis::Z, 3, 180.0,
        PatternOperation::NewBody);
    auto* circular = circularPattern.get();
    circularBody.addFeature(std::move(circularPattern));
    CHECK(document.recompute());
    const auto* currentSourceBody = document.findBody(sourceBodyId);
    CHECK(currentSourceBody);
    checkLegacyFeature(*currentSourceBody->features().front(), sourceBodyId);
    checkLegacyFeature(*linear, linearBodyId);
    checkLegacyFeature(*circular, circularBodyId);
  }

  // Non-null PartialFaces is malformed for RT2 consumers and cannot silently
  // retry through the legacy builder.
  for (const auto consumer :
       {PartialConsumer::Move, PartialConsumer::Mirror,
        PartialConsumer::LinearJoin, PartialConsumer::CircularJoin,
        PartialConsumer::LinearNewBody,
        PartialConsumer::CircularNewBody})
    checkPartialConsumerFailsClosed(consumer);

  return EXIT_SUCCESS;
}
