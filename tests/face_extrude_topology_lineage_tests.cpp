#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "TestGeometryUtils.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/FaceBoundaryEdgeProvenance.h"
#include "model/FaceExtrudeBuilder.h"
#include "model/MoveFeature.h"
#include "model/OcctFacePrismTopologyHistoryCapture.h"
#include "model/TopologyLineage.h"
#include "model/TopologyReferenceResolver.h"

namespace {

using namespace solidar;

std::size_t operationCount(const TopologyLineage& lineage,
                           TopologyProvenanceOperation operation) {
  return static_cast<std::size_t>(std::count_if(
      lineage.bindings().begin(), lineage.bindings().end(),
      [&](const auto& binding) {
        return binding.provenance.operation == operation;
      }));
}

std::size_t faceCount(const TopoDS_Shape& shape) {
  std::size_t count = 0;
  for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More(); faces.Next())
    ++count;
  return count;
}

std::optional<std::size_t> bridgeSideFaceIndex(const TopoDS_Shape& shape) {
  std::size_t index = 0;
  for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More();
       faces.Next(), ++index) {
    const TopoDS_Face face = TopoDS::Face(faces.Current());
    gp_Pln plane;
    gp_Dir normal;
    std::string error;
    if (!resolveFacePlaneAndNormal(face, &plane, &normal, &error)) continue;
    GProp_GProps properties;
    BRepGProp::SurfaceProperties(face, properties);
    const gp_Pnt center = properties.CentreOfMass();
    if (test::near(center.X(), 15.0) && test::near(center.Y(), 4.0) &&
        test::near(center.Z(), 2.5) && normal.Y() < -0.9)
      return index;
  }
  return std::nullopt;
}

std::vector<FaceReference> allFaceReferences(const Body& body,
                                             const ShapeFeature& feature) {
  const auto index = feature.topologyIndex();
  CHECK(index);
  std::vector<FaceReference> result;
  for (std::size_t raw = 0; raw < index->rawFaceCount(); ++raw) {
    const auto created = feature.createFaceReferenceV2(body.id(), raw);
    CHECK(created);
    CHECK(created.reference.featureId() == feature.id());
    result.push_back(created.reference);
  }
  return result;
}

void checkReferences(const ShapeFeature& feature,
                     const std::vector<FaceReference>& references) {
  const auto index = feature.topologyIndex();
  CHECK(index);
  CHECK(references.size() == index->faceCount());
  for (const auto& reference : references) {
    const auto resolved = index->resolveFace(reference);
    CHECK(resolved);
    CHECK(resolved.method == TopologyMatchMethod::OperationProvenance);
  }
}

enum class BoxFace : std::size_t {
  XMin,
  XMax,
  YMin,
  YMax,
  ZMin,
  ZMax,
  Count,
};

constexpr std::array<BoxFace, 4> kBoxSides{
    BoxFace::XMin, BoxFace::XMax, BoxFace::YMin, BoxFace::YMax};

std::size_t slot(BoxFace face) { return static_cast<std::size_t>(face); }

BoxFace classifyBoxFace(const TopoDS_Shape& shape,
                        const test::Bounds& bounds) {
  CHECK(!shape.IsNull());
  CHECK(shape.ShapeType() == TopAbs_FACE);
  GProp_GProps properties;
  BRepGProp::SurfaceProperties(TopoDS::Face(shape), properties);
  const gp_Pnt center = properties.CentreOfMass();
  if (test::near(center.X(), bounds.minX, 1e-5)) return BoxFace::XMin;
  if (test::near(center.X(), bounds.maxX, 1e-5)) return BoxFace::XMax;
  if (test::near(center.Y(), bounds.minY, 1e-5)) return BoxFace::YMin;
  if (test::near(center.Y(), bounds.maxY, 1e-5)) return BoxFace::YMax;
  if (test::near(center.Z(), bounds.minZ, 1e-5)) return BoxFace::ZMin;
  CHECK(test::near(center.Z(), bounds.maxZ, 1e-5));
  return BoxFace::ZMax;
}

using FaceKeyTable =
    std::array<std::optional<TopologyProvenanceKey>,
               static_cast<std::size_t>(BoxFace::Count)>;

FaceKeyTable faceKeysByPlane(const TopologyLineage& lineage,
                             const test::Bounds& bounds) {
  FaceKeyTable result;
  for (const auto& binding : lineage.bindings()) {
    if (binding.provenance.kind != TopologyKind::Face) continue;
    CHECK(binding.subshape);
    const auto face = classifyBoxFace(*binding.subshape, bounds);
    CHECK(!result[slot(face)]);
    result[slot(face)] = binding.provenance;
  }
  for (const auto& key : result) CHECK(key);
  return result;
}

bool sameOriginSet(std::span<const TopologyProvenanceOrigin> actual,
                   std::span<const TopologyProvenanceOrigin> expected) {
  if (actual.size() != expected.size()) return false;
  return std::all_of(expected.begin(), expected.end(), [&](const auto& item) {
    return std::count(actual.begin(), actual.end(), item) == 1;
  });
}

void checkBoundsAndVolume(const ShapeFeature& feature,
                          const test::Bounds& expected,
                          double expectedVolume) {
  CHECK(feature.shape());
  const auto actual = test::boundsOf(*feature.shape());
  CHECK(test::near(actual.minX, expected.minX, 1e-5));
  CHECK(test::near(actual.maxX, expected.maxX, 1e-5));
  CHECK(test::near(actual.minY, expected.minY, 1e-5));
  CHECK(test::near(actual.maxY, expected.maxY, 1e-5));
  CHECK(test::near(actual.minZ, expected.minZ, 1e-5));
  CHECK(test::near(actual.maxZ, expected.maxZ, 1e-5));
  CHECK(test::near(test::volumeOf(*feature.shape()), expectedVolume, 1e-4));
}

struct StrictFixture {
  Document document;
  Body* body{};
  DocumentSketch* sketch{};
  ExtrudeFeature* base{};
  MoveFeature* move{};
  ExtrudeFeature* faceExtrude{};
  FaceReference selected;
};

StrictFixture makeStrictFixture(ExtrudeOperation operation) {
  StrictFixture fixture;
  fixture.sketch = &fixture.document.addSketch("Face lineage base");
  fixture.sketch->geometry.addRectangle({0.0, 0.0}, {20.0, 15.0});
  fixture.body = &fixture.document.addBody("Face lineage body");

  auto base = std::make_unique<ExtrudeFeature>(
      fixture.sketch->id, 10.0, "Base", ExtrudeOperation::NewBody, false);
  fixture.base = base.get();
  fixture.body->addFeature(std::move(base));
  auto move = std::make_unique<MoveFeature>(
      fixture.base->id(), Vector3d{3.0, -2.0, 4.0}, "Move");
  fixture.move = move.get();
  fixture.body->addFeature(std::move(move));
  CHECK(fixture.document.recompute());
  CHECK(fixture.move->topologyLineage());

  const auto top = test::topPlanarFace(*fixture.move->shape(), 14.0);
  CHECK(top);
  const auto selected = fixture.document.createFaceReferenceV2(
      fixture.body->id(), fixture.move->id(), *top);
  CHECK(selected);
  fixture.selected = selected.reference;

  auto faceExtrude = std::make_unique<ExtrudeFeature>(
      fixture.selected, 3.0,
      operation == ExtrudeOperation::Join ? "Face Join" : "Face Cut",
      operation, operation == ExtrudeOperation::Cut);
  fixture.faceExtrude = faceExtrude.get();
  fixture.body->addFeature(std::move(faceExtrude));
  CHECK(fixture.document.recompute());
  return fixture;
}

struct StrictOriginOracle {
  FaceKeyTable object;
  FaceKeyTable prism;
};

bool edgeMatchesSide(const TopoDS_Edge& edge, BoxFace side,
                     const test::Bounds& sourceBounds) {
  const auto bounds = test::boundsOf(edge);
  switch (side) {
    case BoxFace::XMin:
      return test::near(bounds.minX, sourceBounds.minX, 1e-5) &&
             test::near(bounds.maxX, sourceBounds.minX, 1e-5);
    case BoxFace::XMax:
      return test::near(bounds.minX, sourceBounds.maxX, 1e-5) &&
             test::near(bounds.maxX, sourceBounds.maxX, 1e-5);
    case BoxFace::YMin:
      return test::near(bounds.minY, sourceBounds.minY, 1e-5) &&
             test::near(bounds.maxY, sourceBounds.minY, 1e-5);
    case BoxFace::YMax:
      return test::near(bounds.minY, sourceBounds.maxY, 1e-5) &&
             test::near(bounds.maxY, sourceBounds.maxY, 1e-5);
    case BoxFace::ZMin:
    case BoxFace::ZMax:
    case BoxFace::Count:
      return false;
  }
  return false;
}

StrictOriginOracle deriveStrictOriginOracle(const StrictFixture& fixture,
                                            ExtrudeOperation operation) {
  CHECK(fixture.move->shape());
  CHECK(fixture.move->topologyLineage());
  const TopologySourceSnapshot source{
      fixture.move->id(), fixture.move->shape(),
      fixture.move->topologyLineage()};
  FaceBoundaryEdgeProvenanceFailure boundaryFailure;
  const auto boundary = captureUniqueManifoldFaceBoundary(
      source, fixture.selected, &boundaryFailure);
  CHECK(boundary);
  CHECK(boundaryFailure.code == FaceBoundaryEdgeProvenanceFailureCode::None);

  std::string error;
  const auto index = TopologyIndex::build(
      boundary->augmentedSource.shape, kInvalidShapeRevision,
      boundary->augmentedSource.lineage, &error);
  CHECK(index);
  const auto selected = index->resolveFace(fixture.selected);
  CHECK(selected);
  gp_Pln plane;
  gp_Dir normal;
  CHECK(resolveFacePlaneAndNormal(*selected.subshape, &plane, &normal, &error));
  const gp_Vec offset =
      gp_Vec(normal) * (operation == ExtrudeOperation::Cut ? -3.0 : 3.0);
  BRepPrimAPI_MakePrism prism(*selected.subshape, offset, false);
  prism.Build();
  CHECK(prism.IsDone());
  FacePrismTopologyHistoryFailure prismFailure;
  const auto prismBuilder = captureFacePrismTopologyHistory(
      prism, fixture.faceExtrude->id(), fixture.selected, *boundary,
      &prismFailure);
  CHECK(prismBuilder);
  CHECK(prismFailure.code == FacePrismTopologyHistoryFailureCode::None);
  const auto prismLineage = prismBuilder->build(&error);
  CHECK(prismLineage);

  const auto sourceBounds = test::boundsOf(*fixture.move->shape());
  const auto prismBounds = test::boundsOf(*prismBuilder->artifact().outputShape);
  StrictOriginOracle result{
      faceKeysByPlane(*fixture.move->topologyLineage(), sourceBounds),
      faceKeysByPlane(*prismLineage, prismBounds)};

  const auto selectedOrigin = fixture.selected.v2()->provenance.immediateOrigin();
  for (const BoxFace cap : {BoxFace::ZMin, BoxFace::ZMax}) {
    const auto& key = *result.prism[slot(cap)];
    CHECK(key.role == TopologySemanticRole::Cap);
    CHECK(key.operation == TopologyProvenanceOperation::Generated);
    CHECK(key.origins.size() == 1);
    CHECK(key.origins.front() == selectedOrigin);
  }
  for (const BoxFace side : kBoxSides) {
    const auto& key = *result.prism[slot(side)];
    CHECK(key.role == TopologySemanticRole::Side);
    CHECK(key.operation == TopologyProvenanceOperation::Generated);
    CHECK(key.origins.size() == 1);
    const auto matchingSeed = std::find_if(
        boundary->boundarySeeds.begin(), boundary->boundarySeeds.end(),
        [&](const auto& seed) {
          return seed.edge && edgeMatchesSide(*seed.edge, side, sourceBounds);
        });
    CHECK(matchingSeed != boundary->boundarySeeds.end());
    CHECK(std::count_if(boundary->boundarySeeds.begin(),
                        boundary->boundarySeeds.end(), [&](const auto& seed) {
                          return seed.edge &&
                                 edgeMatchesSide(*seed.edge, side, sourceBounds);
                        }) == 1);
    CHECK(key.origins.front() == matchingSeed->origin);
    CHECK(key.origins.front().kind == TopologyKind::Edge);
    CHECK(key.origins.front().featureId == fixture.move->id());
  }
  return result;
}

void checkStrictResult(const StrictFixture& fixture,
                       ExtrudeOperation operation) {
  CHECK(fixture.faceExtrude->isValid());
  CHECK(fixture.faceExtrude->shape());
  CHECK(fixture.faceExtrude->shape()->ShapeType() == TopAbs_SOLID);
  CHECK(faceCount(*fixture.faceExtrude->shape()) == 6);
  checkBoundsAndVolume(
      *fixture.faceExtrude,
      operation == ExtrudeOperation::Join
          ? test::Bounds{3.0, -2.0, 4.0, 23.0, 13.0, 17.0}
          : test::Bounds{3.0, -2.0, 4.0, 23.0, 13.0, 11.0},
      operation == ExtrudeOperation::Join ? 3900.0 : 2100.0);
  const auto lineage = fixture.faceExtrude->topologyLineage();
  CHECK(lineage);
  CHECK(lineage->coverage() == TopologyLineageCoverage::CompleteFaces);
  CHECK(lineage->bindings().size() == 6);
  CHECK(operationCount(*lineage, TopologyProvenanceOperation::Retained) == 2);
  CHECK(operationCount(
            *lineage, operation == ExtrudeOperation::Join
                          ? TopologyProvenanceOperation::Merge
                          : TopologyProvenanceOperation::Modified) == 4);

  bool sawObjectOrigin = false;
  bool sawToolOrigin = false;
  std::size_t objectOriginCount = 0;
  std::size_t toolOriginCount = 0;
  for (const auto& binding : lineage->bindings()) {
    CHECK(binding.provenance.featureId == fixture.faceExtrude->id());
    CHECK(binding.provenance.kind == TopologyKind::Face);
    CHECK(binding.provenance.role == TopologySemanticRole::BooleanResult);
    CHECK(binding.provenance.isCanonical());
    CHECK(binding.subshape);
    CHECK(!binding.subshape->IsNull());
    const std::size_t expectedOrigins =
        binding.provenance.operation == TopologyProvenanceOperation::Merge
            ? 2
            : 1;
    CHECK(binding.provenance.origins.size() == expectedOrigins);
    for (const auto& origin : binding.provenance.origins) {
      CHECK(origin.featureId == fixture.move->id() ||
            origin.featureId == fixture.faceExtrude->id());
      if (origin.featureId == fixture.move->id()) {
        sawObjectOrigin = true;
        ++objectOriginCount;
      } else {
        sawToolOrigin = true;
        ++toolOriginCount;
        CHECK(origin.operation == TopologyProvenanceOperation::Generated);
        CHECK(origin.role == TopologySemanticRole::Cap ||
              origin.role == TopologySemanticRole::Side);
        CHECK(origin.localName.starts_with("face-prism:v1;"));
      }
    }
  }
  CHECK(sawObjectOrigin);
  CHECK(sawToolOrigin);
  CHECK(objectOriginCount == 5);
  CHECK(toolOriginCount ==
        (operation == ExtrudeOperation::Join ? 5 : 1));

  const auto oracle = deriveStrictOriginOracle(fixture, operation);
  const auto outputKeys = faceKeysByPlane(
      *lineage, test::boundsOf(*fixture.faceExtrude->shape()));
  const auto checkOutput = [&](BoxFace face,
                               TopologyProvenanceOperation expectedOperation,
                               std::vector<TopologyProvenanceOrigin> origins) {
    const auto& key = *outputKeys[slot(face)];
    CHECK(key.operation == expectedOperation);
    CHECK(key.role == TopologySemanticRole::BooleanResult);
    CHECK(sameOriginSet(key.origins, origins));
  };
  checkOutput(
      BoxFace::ZMin, TopologyProvenanceOperation::Retained,
      {oracle.object[slot(BoxFace::ZMin)]->immediateOrigin()});
  checkOutput(
      BoxFace::ZMax, TopologyProvenanceOperation::Retained,
      {oracle.prism[slot(operation == ExtrudeOperation::Join
                             ? BoxFace::ZMax
                             : BoxFace::ZMin)]
           ->immediateOrigin()});
  for (const BoxFace side : kBoxSides) {
    std::vector<TopologyProvenanceOrigin> origins{
        oracle.object[slot(side)]->immediateOrigin()};
    if (operation == ExtrudeOperation::Join)
      origins.push_back(oracle.prism[slot(side)]->immediateOrigin());
    checkOutput(side,
                operation == ExtrudeOperation::Join
                    ? TopologyProvenanceOperation::Merge
                    : TopologyProvenanceOperation::Modified,
                std::move(origins));
  }

  const auto references =
      allFaceReferences(*fixture.body, *fixture.faceExtrude);
  CHECK(references.size() == 6);
  checkReferences(*fixture.faceExtrude, references);
}

void checkEditedFixture(const StrictFixture& fixture,
                        const std::vector<FaceReference>& saved,
                        const test::Bounds& expectedBounds,
                        double expectedVolume) {
  CHECK(fixture.faceExtrude->isValid());
  CHECK(fixture.faceExtrude->faceReference() == fixture.selected);
  const auto sourceIndex = fixture.move->topologyIndex();
  CHECK(sourceIndex);
  const auto selected = sourceIndex->resolveFace(fixture.selected);
  CHECK(selected);
  CHECK(selected.method == TopologyMatchMethod::OperationProvenance);
  checkReferences(*fixture.faceExtrude, saved);
  checkBoundsAndVolume(*fixture.faceExtrude, expectedBounds, expectedVolume);
}

void translateFixtureSketch(StrictFixture& fixture, double dx, double dy) {
  std::vector<sketch::GeometryId> lineIds;
  for (std::size_t index = 0; index < fixture.sketch->geometry.lines().size();
       ++index)
    lineIds.push_back(fixture.sketch->geometry.lineId(index));
  const auto beforeIds = lineIds;
  const auto translated =
      fixture.sketch->geometry.translateLinesByIds(lineIds, dx, dy);
  CHECK(translated.committed());
  for (std::size_t index = 0; index < beforeIds.size(); ++index)
    CHECK(fixture.sketch->geometry.lineId(index) == beforeIds[index]);
  CHECK(fixture.document.markSketchDirty(fixture.sketch->id));
  CHECK(fixture.document.recompute());
}

class InjectedBoxFeature final : public ShapeFeature {
 public:
  explicit InjectedBoxFeature(TopologyLineageCoverage coverage)
      : ShapeFeature("Injected box"), coverage_(coverage) {}

  std::unique_ptr<Feature> clone() const override {
    return std::make_unique<InjectedBoxFeature>(*this);
  }

 private:
  bool rebuildImpl(const RebuildContext&) override {
    auto shape = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape());
    if (coverage_ == TopologyLineageCoverage::None) {
      setShape(std::move(shape));
      markValid();
      return true;
    }
    TopExp_Explorer faces(*shape, TopAbs_FACE);
    CHECK(faces.More());
    const TopologyProvenanceKey key{
        id(), TopologyKind::Face, TopologyProvenanceOperation::Generated,
        TopologySemanticRole::Original, "injected-face"};
    std::string error;
    auto lineage = TopologyLineage::create(
        {{key, std::make_shared<TopoDS_Face>(TopoDS::Face(faces.Current()))}},
        coverage_, &error);
    CHECK(lineage);
    setShapeAndTopologyLineage(std::move(shape), std::move(lineage));
    markValid();
    return true;
  }

  TopologyLineageCoverage coverage_{};
};

void expectIncompleteLineageFailure(TopologyLineageCoverage coverage) {
  Document document;
  auto& body = document.addBody("Incomplete lineage");
  auto source = std::make_unique<InjectedBoxFeature>(coverage);
  const auto sourceId = source->id();
  body.addFeature(std::move(source));
  CHECK(document.recompute());
  const FaceReference reference = FaceReference::fromV2(
      {body.id(),
       {sourceId, TopologyKind::Face,
        TopologyProvenanceOperation::Generated,
        TopologySemanticRole::Original, "injected-face"}});
  auto faceExtrude = std::make_unique<ExtrudeFeature>(
      reference, 2.0, "Strict failure", ExtrudeOperation::Join, false);
  auto* faceExtrudePtr = faceExtrude.get();
  body.addFeature(std::move(faceExtrude));
  CHECK(!document.recompute());
  CHECK(faceExtrudePtr->isFailed());
  CHECK(faceExtrudePtr->error().find("lineage is incomplete") !=
        std::string::npos);
  CHECK(!faceExtrudePtr->shape());
  CHECK(!faceExtrudePtr->topologyLineage());
}

}  // namespace

int main() {
  // V2 Join and Cut both use the strict boundary -> live prism -> live
  // boolean history pipeline and publish exact, complete six-face snapshots.
  auto joined = makeStrictFixture(ExtrudeOperation::Join);
  checkStrictResult(joined, ExtrudeOperation::Join);
  auto cut = makeStrictFixture(ExtrudeOperation::Cut);
  checkStrictResult(cut, ExtrudeOperation::Cut);

  // Every authored output reference survives face length, base length, source
  // GeometryId-preserving translation, and Move parameter edits.
  const auto saved = allFaceReferences(*joined.body, *joined.faceExtrude);
  joined.faceExtrude->setLengthMm(5.0);
  CHECK(joined.document.recompute());
  checkEditedFixture(
      joined, saved, {3.0, -2.0, 4.0, 23.0, 13.0, 19.0}, 4500.0);
  joined.base->setLengthMm(12.0);
  CHECK(joined.document.recompute());
  checkEditedFixture(
      joined, saved, {3.0, -2.0, 4.0, 23.0, 13.0, 21.0}, 5100.0);
  joined.move->setOffsetMm({6.0, 1.0, -2.0});
  CHECK(joined.document.recompute());
  checkEditedFixture(
      joined, saved, {6.0, 1.0, -2.0, 26.0, 16.0, 15.0}, 5100.0);
  translateFixtureSketch(joined, 2.0, 3.0);
  checkEditedFixture(
      joined, saved, {8.0, 4.0, -2.0, 28.0, 19.0, 15.0}, 5100.0);

  // Cut has the same all-output persistence obligation. Keep its six original
  // references while changing depth, base height, placement and source
  // coordinates without replacing any GeometryId.
  const auto savedCut = allFaceReferences(*cut.body, *cut.faceExtrude);
  cut.faceExtrude->setLengthMm(2.0);
  CHECK(cut.document.recompute());
  checkEditedFixture(
      cut, savedCut, {3.0, -2.0, 4.0, 23.0, 13.0, 12.0}, 2400.0);
  cut.base->setLengthMm(12.0);
  CHECK(cut.document.recompute());
  checkEditedFixture(
      cut, savedCut, {3.0, -2.0, 4.0, 23.0, 13.0, 14.0}, 3000.0);
  cut.move->setOffsetMm({6.0, 1.0, -2.0});
  CHECK(cut.document.recompute());
  checkEditedFixture(
      cut, savedCut, {6.0, 1.0, -2.0, 26.0, 16.0, 8.0}, 3000.0);
  translateFixtureSketch(cut, 2.0, 3.0);
  checkEditedFixture(
      cut, savedCut, {8.0, 4.0, -2.0, 28.0, 19.0, 8.0}, 3000.0);

  // Failure after a committed result is atomic; the last exact shape and
  // lineage remain available, and a later valid edit recovers normally.
  const auto lastShape = joined.faceExtrude->lastValidShape();
  const auto lastLineage = joined.faceExtrude->lastValidTopologyLineage();
  joined.faceExtrude->setReversed(true);
  joined.faceExtrude->setLengthMm(1.0);
  CHECK(!joined.document.recompute());
  CHECK(joined.faceExtrude->isFailed());
  CHECK(joined.faceExtrude->lastValidShape() == lastShape);
  CHECK(joined.faceExtrude->lastValidTopologyLineage() == lastLineage);
  CHECK(!joined.faceExtrude->shape());
  CHECK(!joined.faceExtrude->topologyLineage());
  joined.faceExtrude->setReversed(false);
  joined.faceExtrude->setLengthMm(4.0);
  CHECK(joined.document.recompute());
  CHECK(joined.faceExtrude->isValid());
  CHECK(joined.faceExtrude->topologyLineage());
  checkBoundsAndVolume(
      *joined.faceExtrude, {8.0, 4.0, -2.0, 28.0, 19.0, 14.0}, 4800.0);

  // A lineage-capable base does not opt a V1 face into the strict path.
  {
    Document document;
    auto& sketch = document.addSketch("Legacy v1 source");
    sketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    auto& body = document.addBody();
    auto base = std::make_unique<ExtrudeFeature>(sketch.id, 8.0);
    auto* basePtr = base.get();
    body.addFeature(std::move(base));
    CHECK(document.recompute());
    CHECK(basePtr->topologyLineage());
    const auto top = test::topPlanarFace(*basePtr->shape(), 8.0);
    CHECK(top);
    const auto legacy = makeFaceReference(
        *basePtr->shape(), body.id(), basePtr->id(), *top);
    CHECK(legacy.isV1());
    auto faceExtrude = std::make_unique<ExtrudeFeature>(
        legacy, 2.0, "Legacy join", ExtrudeOperation::Join, false);
    auto* faceExtrudePtr = faceExtrude.get();
    body.addFeature(std::move(faceExtrude));
    CHECK(document.recompute());
    CHECK(faceExtrudePtr->isValid());
    CHECK(faceExtrudePtr->shape());
    CHECK(!faceExtrudePtr->topologyLineage());
  }

  expectIncompleteLineageFailure(TopologyLineageCoverage::None);
  expectIncompleteLineageFailure(TopologyLineageCoverage::PartialFaces);

  // A deeper Cut through the complete connector face would split the body.
  // The raw historical solid-count contract rejects it before capture,
  // without replacing the prior valid shallow-Cut snapshot; reducing the
  // depth recovers.
  {
    Document document;
    auto& sketch = document.addSketch("Dumbbell split source");
    const std::array<sketch::Point, 12> outline{{
        {0.0, 0.0}, {10.0, 0.0}, {10.0, 4.0}, {20.0, 4.0},
        {20.0, 0.0}, {30.0, 0.0}, {30.0, 10.0}, {20.0, 10.0},
        {20.0, 6.0}, {10.0, 6.0}, {10.0, 10.0}, {0.0, 10.0}}};
    for (std::size_t index = 0; index < outline.size(); ++index)
      sketch.geometry.addLine(outline[index],
                              outline[(index + 1) % outline.size()]);
    auto& body = document.addBody();
    auto base = std::make_unique<ExtrudeFeature>(sketch.id, 5.0);
    auto* basePtr = base.get();
    body.addFeature(std::move(base));
    auto move = std::make_unique<MoveFeature>(basePtr->id(), Vector3d{});
    auto* movePtr = move.get();
    body.addFeature(std::move(move));
    CHECK(document.recompute());
    const auto bridgeFace = bridgeSideFaceIndex(*movePtr->shape());
    CHECK(bridgeFace);
    const auto selected = document.createFaceReferenceV2(
        body.id(), movePtr->id(), *bridgeFace);
    CHECK(selected);
    auto faceExtrude = std::make_unique<ExtrudeFeature>(
        selected.reference, 1.0, "Connector operation",
        ExtrudeOperation::Cut, true);
    auto* faceExtrudePtr = faceExtrude.get();
    body.addFeature(std::move(faceExtrude));
    CHECK(document.recompute());
    const auto stableShape = faceExtrudePtr->lastValidShape();
    const auto stableLineage = faceExtrudePtr->lastValidTopologyLineage();
    CHECK(stableShape);
    CHECK(stableLineage);

    faceExtrudePtr->setLengthMm(3.0);
    CHECK(!document.recompute());
    CHECK(faceExtrudePtr->error().find("split") != std::string::npos ||
          faceExtrudePtr->error().find("multiple solids") !=
              std::string::npos);
    CHECK(faceExtrudePtr->lastValidShape() == stableShape);
    CHECK(faceExtrudePtr->lastValidTopologyLineage() == stableLineage);
    CHECK(!faceExtrudePtr->shape());
    CHECK(!faceExtrudePtr->topologyLineage());

    faceExtrudePtr->setLengthMm(1.0);
    CHECK(document.recompute());
    CHECK(faceExtrudePtr->topologyLineage());
  }

  // V2 owner validation is exact even when the same provenance can resolve.
  {
    auto fixture = makeStrictFixture(ExtrudeOperation::Join);
    const auto* selected = fixture.selected.v2();
    CHECK(selected);
    const FaceReference wrongBody = FaceReference::fromV2(
        {fixture.body->id() + 1000, selected->provenance});
    auto wrong = std::make_unique<ExtrudeFeature>(
        wrongBody, 2.0, "Wrong body", ExtrudeOperation::Join, false);
    auto* wrongPtr = wrong.get();
    fixture.body->addFeature(std::move(wrong));
    CHECK(!fixture.document.recompute());
    CHECK(wrongPtr->error().find("different Body") != std::string::npos);
  }

  {
    auto fixture = makeStrictFixture(ExtrudeOperation::Join);
    auto nonPredecessor = std::make_unique<ExtrudeFeature>(
        fixture.selected, 2.0, "Non-predecessor", ExtrudeOperation::Join,
        false);
    auto* nonPredecessorPtr = nonPredecessor.get();
    fixture.body->addFeature(std::move(nonPredecessor));
    CHECK(!fixture.document.recompute());
    CHECK(nonPredecessorPtr->error().find("immediate source Feature") !=
          std::string::npos);
    CHECK(!nonPredecessorPtr->shape());
    CHECK(!nonPredecessorPtr->topologyLineage());
  }

  // A forged context cannot substitute an equal copied shape for the exact
  // current predecessor snapshot.
  {
    auto fixture = makeStrictFixture(ExtrudeOperation::Join);
    const auto stableShape = fixture.faceExtrude->lastValidShape();
    const auto stableLineage = fixture.faceExtrude->lastValidTopologyLineage();
    const TopoDS_Shape copied = *fixture.move->shape();
    RebuildContext incoherent{fixture.document, fixture.body, &copied,
                              fixture.move};
    fixture.faceExtrude->setDirty();
    CHECK(!fixture.faceExtrude->rebuild(incoherent));
    CHECK(fixture.faceExtrude->error().find("immediate predecessor") !=
          std::string::npos);
    CHECK(fixture.faceExtrude->lastValidShape() == stableShape);
    CHECK(fixture.faceExtrude->lastValidTopologyLineage() == stableLineage);
    fixture.faceExtrude->setLengthMm(4.0);
    CHECK(fixture.document.recompute());
  }

  return EXIT_SUCCESS;
}
