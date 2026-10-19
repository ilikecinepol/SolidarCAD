#include "TestAssertions.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <NCollection_IndexedMap.hxx>
#include <NCollection_List.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "model/FaceBoundaryEdgeProvenance.h"
#include "model/OcctFacePrismTopologyHistoryCapture.h"
#include "model/TopologyReferenceResolver.h"

namespace {

constexpr solidar::BodyId kBodyId = 5101;
constexpr solidar::FeatureId kSourceFeature = 5102;
constexpr solidar::FeatureId kPrismFeature = 5103;
using ShapeIndexedMap =
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;

class ScriptedGeneratedPrism final : public BRepPrimAPI_MakePrism {
 public:
  enum class Mode { Actual, Empty, Two, NonFace, Duplicate };

  ScriptedGeneratedPrism(const TopoDS_Face& face, const gp_Vec& vector,
                         Mode mode, TopoDS_Edge duplicateSource = {})
      : BRepPrimAPI_MakePrism(face, vector),
        mode_(mode),
        duplicateSource_(std::move(duplicateSource)) {}

  const NCollection_List<TopoDS_Shape>& Generated(
      const TopoDS_Shape& source) override {
    if (mode_ == Mode::Actual)
      return BRepPrimAPI_MakePrism::Generated(source);
    scripted_.Clear();
    if (mode_ == Mode::Empty) return scripted_;
    if (mode_ == Mode::NonFace) {
      scripted_.Append(source);
      return scripted_;
    }
    const TopoDS_Shape& queried =
        mode_ == Mode::Duplicate ? duplicateSource_ : source;
    const auto& actual = BRepPrimAPI_MakePrism::Generated(queried);
    if (!actual.IsEmpty()) {
      scripted_.Append(actual.First());
      if (mode_ == Mode::Two) scripted_.Append(actual.First());
    }
    return scripted_;
  }

  void forceNotDone() { NotDone(); }

 private:
  Mode mode_{Mode::Actual};
  TopoDS_Edge duplicateSource_;
  NCollection_List<TopoDS_Shape> scripted_;
};

bool contains(const TopoDS_Shape& owner, const TopoDS_Shape& candidate,
              TopAbs_ShapeEnum kind) {
  for (TopExp_Explorer explorer(owner, kind); explorer.More();
       explorer.Next())
    if (explorer.Current().IsSame(candidate)) return true;
  return false;
}

std::string faceName(const TopoDS_Face& face) {
  BRepAdaptor_Surface surface(face);
  if (surface.GetType() == GeomAbs_Cylinder) return "cylinder-side";
  CHECK(surface.GetType() == GeomAbs_Plane);
  gp_Dir direction = surface.Plane().Axis().Direction();
  if (face.Orientation() == TopAbs_REVERSED) direction.Reverse();
  const double x = direction.X();
  const double y = direction.Y();
  const double z = direction.Z();
  if (std::abs(x) >= std::abs(y) && std::abs(x) >= std::abs(z))
    return x >= 0.0 ? "x+" : "x-";
  if (std::abs(y) >= std::abs(z)) return y >= 0.0 ? "y+" : "y-";
  return z >= 0.0 ? "z+" : "z-";
}

solidar::TopologyProvenanceKey sourceFaceKey(std::string name) {
  return {kSourceFeature, solidar::TopologyKind::Face,
          solidar::TopologyProvenanceOperation::Generated,
          solidar::TopologySemanticRole::Original, std::move(name)};
}

solidar::TopologySourceSnapshot sourceFor(const TopoDS_Shape& shape) {
  auto owned = std::make_shared<TopoDS_Shape>(shape);
  ShapeIndexedMap faces;
  TopExp::MapShapes(*owned, TopAbs_FACE, faces);
  std::vector<solidar::TopologyLineageBinding> bindings;
  for (int index = 1; index <= faces.Extent(); ++index) {
    const TopoDS_Face face = TopoDS::Face(faces(index));
    bindings.push_back({sourceFaceKey(faceName(face)),
                        std::make_shared<TopoDS_Face>(face)});
  }
  std::string error;
  auto lineage = solidar::TopologyLineage::create(
      std::move(bindings), solidar::TopologyLineageCoverage::CompleteFaces,
      &error);
  CHECK(lineage);
  return {kSourceFeature, std::move(owned), std::move(lineage)};
}

solidar::FaceReference referenceFor(
    const solidar::TopologySourceSnapshot& source, std::string_view name) {
  const auto found = std::find_if(
      source.lineage->bindings().begin(), source.lineage->bindings().end(),
      [&](const auto& binding) {
        return binding.provenance.kind == solidar::TopologyKind::Face &&
               binding.provenance.localName == name;
      });
  CHECK(found != source.lineage->bindings().end());
  return solidar::FaceReference::fromV2({kBodyId, found->provenance});
}

solidar::FaceBoundaryEdgeProvenanceResult boundaryFor(
    const solidar::TopologySourceSnapshot& source,
    const solidar::FaceReference& selected) {
  solidar::FaceBoundaryEdgeProvenanceFailure failure;
  auto result = solidar::captureUniqueManifoldFaceBoundary(
      source, selected, &failure);
  if (!result)
    std::cerr << "boundary capture failed: " << failure.detail << '\n';
  CHECK(result);
  CHECK(failure.code ==
        solidar::FaceBoundaryEdgeProvenanceFailureCode::None);
  return std::move(*result);
}

TopoDS_Face resolvedFace(const solidar::FaceBoundaryEdgeProvenanceResult& b,
                         const solidar::FaceReference& selected) {
  std::string error;
  const auto index = solidar::TopologyIndex::build(
      b.augmentedSource.shape, 1, b.augmentedSource.lineage, &error);
  CHECK(index);
  const auto resolved = index->resolveFace(selected);
  CHECK(resolved);
  return *resolved.subshape;
}

void appendLengthPrefixed(std::string& output, std::string_view value) {
  output += std::to_string(value.size());
  output.push_back(':');
  output.append(value);
}

std::string encodeOrigin(const solidar::TopologyProvenanceOrigin& origin) {
  std::string result;
  appendLengthPrefixed(result, std::to_string(origin.featureId));
  appendLengthPrefixed(
      result, std::to_string(static_cast<unsigned>(origin.kind)));
  appendLengthPrefixed(
      result, std::to_string(static_cast<unsigned>(origin.operation)));
  appendLengthPrefixed(
      result, std::to_string(static_cast<unsigned>(origin.role)));
  appendLengthPrefixed(result, origin.localName);
  appendLengthPrefixed(result, origin.occurrence ? "1" : "0");
  if (origin.occurrence) {
    appendLengthPrefixed(result,
                         std::to_string(origin.occurrence->featureId));
    appendLengthPrefixed(result, std::to_string(origin.occurrence->slot));
  }
  return result;
}

std::string expectedName(
    std::string_view branch,
    const solidar::TopologyProvenanceOrigin& origin) {
  std::string result = "face-prism:v1;";
  appendLengthPrefixed(result, branch);
  appendLengthPrefixed(result, encodeOrigin(origin));
  return result;
}

struct ExpectedSideHistory {
  solidar::FaceBoundaryEdgeSeed seed;
  TopoDS_Shape output;
};

struct ExpectedPrismHistory {
  TopoDS_Shape first;
  TopoDS_Shape last;
  std::vector<ExpectedSideHistory> sides;
};

ExpectedPrismHistory copyExpectedHistory(
    BRepPrimAPI_MakePrism& maker,
    const solidar::FaceBoundaryEdgeProvenanceResult& boundary) {
  ExpectedPrismHistory expected{maker.FirstShape(), maker.LastShape(), {}};
  for (const auto& seed : boundary.boundarySeeds) {
    const auto& generated = maker.Generated(*seed.edge);
    CHECK(generated.Size() == 1);
    expected.sides.push_back({seed, generated.First()});
  }
  return expected;
}

const solidar::HistoryFact& factForKey(
    const solidar::ShapeHistoryArtifact& artifact,
    const solidar::TopologyProvenanceKey& key) {
  const auto found = std::find_if(
      artifact.facts.begin(), artifact.facts.end(), [&](const auto& fact) {
        return fact.outputProvenance && *fact.outputProvenance == key;
      });
  CHECK(found != artifact.facts.end());
  return *found;
}

void verifyCapturedHistory(
    const solidar::TopologyLineageBuilder& builder,
    const solidar::FaceBoundaryEdgeProvenanceResult& boundary,
    const solidar::FaceReference& selected,
    const ExpectedPrismHistory& expected) {
  const auto selectedOrigin = selected.v2()->provenance.immediateOrigin();
  const TopoDS_Face selectedShape = resolvedFace(boundary, selected);
  const auto verifyFact = [&](const solidar::TopologyProvenanceKey& key,
                              const solidar::TopologyProvenanceOrigin& origin,
                              const TopoDS_Shape& source,
                              const TopoDS_Shape& output) {
    const auto& fact = factForKey(builder.artifact(), key);
    CHECK(fact.kind == solidar::ShapeHistoryFactKind::Generated);
    CHECK(!fact.authoredSource);
    CHECK(fact.source == origin);
    CHECK(fact.sourceSubshape);
    CHECK(fact.sourceSubshape->IsSame(source));
    CHECK(fact.sourceSubshape->Orientation() == source.Orientation());
    CHECK(fact.outputSubshape);
    CHECK(fact.outputSubshape->IsSame(output));
    CHECK(fact.outputSubshape->Orientation() == output.Orientation());
  };

  const solidar::TopologyProvenanceKey startKey{
      kPrismFeature, solidar::TopologyKind::Face,
      solidar::TopologyProvenanceOperation::Generated,
      solidar::TopologySemanticRole::Cap,
      expectedName("start-cap", selectedOrigin), {selectedOrigin}};
  const solidar::TopologyProvenanceKey endKey{
      kPrismFeature, solidar::TopologyKind::Face,
      solidar::TopologyProvenanceOperation::Generated,
      solidar::TopologySemanticRole::Cap,
      expectedName("end-cap", selectedOrigin), {selectedOrigin}};
  verifyFact(startKey, selectedOrigin, selectedShape, expected.first);
  verifyFact(endKey, selectedOrigin, selectedShape, expected.last);

  CHECK(expected.sides.size() == boundary.boundarySeeds.size());
  for (const auto& side : expected.sides) {
    const solidar::TopologyProvenanceKey sideKey{
        kPrismFeature, solidar::TopologyKind::Face,
        solidar::TopologyProvenanceOperation::Generated,
        solidar::TopologySemanticRole::Side,
        expectedName("side", side.seed.origin), {side.seed.origin}};
    verifyFact(sideKey, side.seed.origin, *side.seed.edge, side.output);
  }
}

std::vector<solidar::TopologyProvenanceKey> sortedKeys(
    const solidar::TopologyLineage& lineage) {
  std::vector<solidar::TopologyProvenanceKey> keys;
  for (const auto& binding : lineage.bindings())
    keys.push_back(binding.provenance);
  std::sort(keys.begin(), keys.end(), [](const auto& left,
                                         const auto& right) {
    return left.localName < right.localName;
  });
  return keys;
}

std::shared_ptr<const solidar::TopologyLineage> captureLineage(
    const solidar::FaceBoundaryEdgeProvenanceResult& boundary,
    const solidar::FaceReference& selected, const gp_Vec& vector,
    solidar::FacePrismTopologyHistoryFailureCode expected =
        solidar::FacePrismTopologyHistoryFailureCode::None,
    solidar::FeatureId producer = kPrismFeature) {
  const TopoDS_Face face = resolvedFace(boundary, selected);
  ExpectedPrismHistory expectedHistory;
  solidar::FacePrismTopologyHistoryFailure failure;
  std::optional<solidar::TopologyLineageBuilder> captured;
  {
    BRepPrimAPI_MakePrism maker(face, vector);
    maker.Build();
    expectedHistory = copyExpectedHistory(maker, boundary);
    captured = solidar::captureFacePrismTopologyHistory(
        maker, producer, selected, boundary, &failure);
  }
  if (failure.code != expected)
    std::cerr << "capture failure mismatch expected="
              << static_cast<unsigned>(expected) << " actual="
              << static_cast<unsigned>(failure.code)
              << " detail=" << failure.detail << '\n';
  CHECK(failure.code == expected);
  if (expected != solidar::FacePrismTopologyHistoryFailureCode::None) {
    CHECK(!captured);
    CHECK(!failure.detail.empty());
    return {};
  }
  CHECK(captured);
  verifyCapturedHistory(*captured, boundary, selected, expectedHistory);
  std::string error;
  auto lineage = captured->build(&error);
  CHECK(lineage);
  CHECK(error.empty());
  CHECK(captured->artifact().outputShape);
  CHECK(lineage->coverage() ==
        solidar::TopologyLineageCoverage::CompleteFaces);
  return lineage;
}

void checkExactIdentities(
    const solidar::TopologyLineage& lineage,
    const solidar::FaceBoundaryEdgeProvenanceResult& boundary,
    const solidar::FaceReference& selected) {
  const auto selectedOrigin = selected.v2()->provenance.immediateOrigin();
  CHECK(lineage.bindings().size() == boundary.boundarySeeds.size() + 2);
  std::vector<solidar::TopologyProvenanceKey> expected{
      {kPrismFeature, solidar::TopologyKind::Face,
       solidar::TopologyProvenanceOperation::Generated,
       solidar::TopologySemanticRole::Cap,
       expectedName("start-cap", selectedOrigin), {selectedOrigin}},
      {kPrismFeature, solidar::TopologyKind::Face,
       solidar::TopologyProvenanceOperation::Generated,
       solidar::TopologySemanticRole::Cap,
       expectedName("end-cap", selectedOrigin), {selectedOrigin}}};
  for (const auto& seed : boundary.boundarySeeds)
    expected.emplace_back(
        kPrismFeature, solidar::TopologyKind::Face,
        solidar::TopologyProvenanceOperation::Generated,
        solidar::TopologySemanticRole::Side,
        expectedName("side", seed.origin),
        std::vector<solidar::TopologyProvenanceOrigin>{seed.origin});
  std::sort(expected.begin(), expected.end(), [](const auto& left,
                                                  const auto& right) {
    return left.localName < right.localName;
  });
  CHECK(sortedKeys(lineage) == expected);
  for (const auto& key : expected) {
    CHECK(!key.occurrence);
    CHECK(key.localName.size() <= 16384);
    CHECK(key.localName.find("index=") == std::string::npos);
    CHECK(key.localName.find("geometry=") == std::string::npos);
    CHECK(key.localName.find("orientation=") == std::string::npos);
    CHECK(key.localName.find("direction=") == std::string::npos);
    CHECK(key.localName.find("length=") == std::string::npos);
  }
}

}  // namespace

int main() {
  // Rectangular selected face: two caps plus four sides, all Generated from
  // exact immediate source identities and usable after maker destruction.
  const auto boxSource =
      sourceFor(BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape());
  const auto boxTop = referenceFor(boxSource, "z+");
  auto boxBoundary = boundaryFor(boxSource, boxTop);
  std::optional<solidar::TopologyLineageBuilder> surviving;
  std::shared_ptr<const TopoDS_Shape> capturedRoot;
  ExpectedPrismHistory baselineExpected;
  {
    const TopoDS_Face face = resolvedFace(boxBoundary, boxTop);
    BRepPrimAPI_MakePrism maker(face, gp_Vec(0.0, 0.0, 7.0));
    baselineExpected = copyExpectedHistory(maker, boxBoundary);
    solidar::FacePrismTopologyHistoryFailure failure;
    surviving = solidar::captureFacePrismTopologyHistory(
        maker, kPrismFeature, boxTop, boxBoundary, &failure);
    CHECK(surviving);
    CHECK(failure.code ==
          solidar::FacePrismTopologyHistoryFailureCode::None);
    capturedRoot = surviving->artifact().outputShape;
    CHECK(surviving->artifact().sources.size() == 1);
    CHECK(surviving->artifact().sources.front().featureId ==
          boxBoundary.augmentedSource.featureId);
    CHECK(surviving->artifact().sources.front().shape ==
          boxBoundary.augmentedSource.shape);
    CHECK(surviving->artifact().sources.front().lineage ==
          boxBoundary.augmentedSource.lineage);
    CHECK(surviving->artifact().facts.size() ==
          boxBoundary.boundarySeeds.size() + 2);
    for (const auto& fact : surviving->artifact().facts) {
      CHECK(fact.kind == solidar::ShapeHistoryFactKind::Generated);
      CHECK(!fact.authoredSource);
      CHECK(fact.source);
      CHECK(fact.sourceSubshape);
      CHECK(fact.outputProvenance);
      CHECK(fact.outputSubshape);
    }
  }
  verifyCapturedHistory(*surviving, boxBoundary, boxTop,
                        baselineExpected);
  std::string error;
  const auto boxLineage = surviving->build(&error);
  CHECK(boxLineage);
  CHECK(surviving->artifact().outputShape == capturedRoot);
  checkExactIdentities(*boxLineage, boxBoundary, boxTop);
  const auto boxIndex = solidar::TopologyIndex::build(
      capturedRoot, 1, boxLineage, &error);
  CHECK(boxIndex);
  for (const auto& binding : boxLineage->bindings()) {
    const auto resolved = boxIndex->resolveFace(
        solidar::TopologyReferenceV2{kBodyId, binding.provenance});
    CHECK(resolved);
    CHECK(resolved.subshape->IsSame(*binding.subshape));
  }

  // A cylindrical cap has one manifold boundary edge and therefore exactly
  // three canonical output faces.
  const auto cylinderSource =
      sourceFor(BRepPrimAPI_MakeCylinder(8.0, 12.0).Shape());
  const auto cylinderTop = referenceFor(cylinderSource, "z+");
  const auto cylinderBoundary = boundaryFor(cylinderSource, cylinderTop);
  CHECK(cylinderBoundary.boundarySeeds.size() == 1);
  const auto cylinderLineage = captureLineage(
      cylinderBoundary, cylinderTop, gp_Vec(0.0, 0.0, 4.0));
  CHECK(cylinderLineage->bindings().size() == 3);
  checkExactIdentities(*cylinderLineage, cylinderBoundary, cylinderTop);

  // Length, sweep direction, source-shape copy, source translation and seed
  // order never enter the authored identity. Every run also verifies exact
  // cap and semantic-edge-to-side mappings against live maker evidence.
  const auto forward = sortedKeys(*captureLineage(
      boxBoundary, boxTop, gp_Vec(0.0, 0.0, 3.0)));
  CHECK(sortedKeys(*captureLineage(
            boxBoundary, boxTop, gp_Vec(0.0, 0.0, 13.0))) == forward);
  CHECK(sortedKeys(*captureLineage(
            boxBoundary, boxTop, gp_Vec(0.0, 0.0, -3.0))) == forward);

  auto reordered = boxBoundary;
  std::reverse(reordered.boundarySeeds.begin(),
               reordered.boundarySeeds.end());
  CHECK(sortedKeys(*captureLineage(
            reordered, boxTop, gp_Vec(0.0, 0.0, 3.0))) == forward);

  BRepBuilderAPI_Copy copier(*boxSource.shape);
  const auto copiedSource = sourceFor(copier.Shape());
  const auto copiedTop = referenceFor(copiedSource, "z+");
  const auto copiedBoundary = boundaryFor(copiedSource, copiedTop);
  CHECK(sortedKeys(*captureLineage(
            copiedBoundary, copiedTop, gp_Vec(0.0, 0.0, 3.0))) == forward);

  gp_Trsf translation;
  translation.SetTranslation(gp_Vec(30.0, -7.0, 11.0));
  BRepBuilderAPI_Transform transform(*boxSource.shape, translation, true);
  const auto translatedSource = sourceFor(transform.Shape());
  const auto translatedTop = referenceFor(translatedSource, "z+");
  const auto translatedBoundary = boundaryFor(translatedSource, translatedTop);
  CHECK(sortedKeys(*captureLineage(
            translatedBoundary, translatedTop, gp_Vec(0.0, 0.0, 3.0))) ==
        forward);

  const TopoDS_Face boxFace = resolvedFace(boxBoundary, boxTop);
  BRepPrimAPI_MakePrism validMaker(boxFace, gp_Vec(0.0, 0.0, 2.0));
  auto expectFailure = [&](const solidar::FaceReference& selected,
                           const solidar::FaceBoundaryEdgeProvenanceResult& b,
                           solidar::FeatureId producer,
                           solidar::FacePrismTopologyHistoryFailureCode code) {
    solidar::FacePrismTopologyHistoryFailure failure;
    auto result = solidar::captureFacePrismTopologyHistory(
        validMaker, producer, selected, b, &failure);
    if (failure.code != code)
      std::cerr << "negative capture mismatch expected="
                << static_cast<unsigned>(code) << " actual="
                << static_cast<unsigned>(failure.code)
                << " detail=" << failure.detail << '\n';
    CHECK(!result);
    CHECK(failure.code == code);
    CHECK(!failure.detail.empty());
  };

  expectFailure(boxTop, boxBoundary, solidar::kInvalidFeatureId,
                solidar::FacePrismTopologyHistoryFailureCode::InvalidProducer);
  expectFailure(boxTop, boxBoundary, kSourceFeature,
                solidar::FacePrismTopologyHistoryFailureCode::ProducerMatchesSource);
  expectFailure(solidar::FaceReference{}, boxBoundary, kPrismFeature,
                solidar::FacePrismTopologyHistoryFailureCode::InvalidSelectedFace);
  auto foreignSelected = boxTop;
  auto foreignV2 = *foreignSelected.v2();
  foreignV2.provenance.featureId = 9999;
  foreignSelected = solidar::FaceReference::fromV2(std::move(foreignV2));
  expectFailure(foreignSelected, boxBoundary, kPrismFeature,
                solidar::FacePrismTopologyHistoryFailureCode::ForeignSelectedFace);
  auto missingSelected = boxTop;
  auto missingV2 = *missingSelected.v2();
  missingV2.provenance.localName = "missing-face";
  missingSelected = solidar::FaceReference::fromV2(std::move(missingV2));
  expectFailure(missingSelected, boxBoundary, kPrismFeature,
                solidar::FacePrismTopologyHistoryFailureCode::MissingSelectedFace);

  auto missingSeed = boxBoundary;
  missingSeed.boundarySeeds.pop_back();
  expectFailure(boxTop, missingSeed, kPrismFeature,
                solidar::FacePrismTopologyHistoryFailureCode::NonCanonicalBoundaryEvidence);
  auto duplicateSeed = boxBoundary;
  duplicateSeed.boundarySeeds[1] = duplicateSeed.boundarySeeds[0];
  expectFailure(boxTop, duplicateSeed, kPrismFeature,
                solidar::FacePrismTopologyHistoryFailureCode::NonCanonicalBoundaryEvidence);
  auto orientationAlias = boxBoundary;
  orientationAlias.boundarySeeds[0].edge = std::make_shared<TopoDS_Edge>(
      TopoDS::Edge(orientationAlias.boundarySeeds[0].edge->Reversed()));
  expectFailure(boxTop, orientationAlias, kPrismFeature,
                solidar::FacePrismTopologyHistoryFailureCode::NonCanonicalBoundaryEvidence);
  auto foreignSeed = boxBoundary;
  TopoDS_Edge nonBoundary;
  for (TopExp_Explorer explorer(*boxBoundary.augmentedSource.shape,
                                TopAbs_EDGE);
       explorer.More(); explorer.Next()) {
    if (!contains(boxFace, explorer.Current(), TopAbs_EDGE)) {
      nonBoundary = TopoDS::Edge(explorer.Current());
      break;
    }
  }
  CHECK(!nonBoundary.IsNull());
  foreignSeed.boundarySeeds[0].edge =
      std::make_shared<TopoDS_Edge>(nonBoundary);
  expectFailure(boxTop, foreignSeed, kPrismFeature,
                solidar::FacePrismTopologyHistoryFailureCode::NonCanonicalBoundaryEvidence);

  auto partial = boxBoundary;
  auto partialLineage = solidar::TopologyLineage::create(
      partial.augmentedSource.lineage->bindings(),
      solidar::TopologyLineageCoverage::PartialFaces, &error);
  CHECK(partialLineage);
  partial.augmentedSource.lineage = std::move(partialLineage);
  expectFailure(boxTop, partial, kPrismFeature,
                solidar::FacePrismTopologyHistoryFailureCode::IncompleteFaceCoverage);

  auto ambiguousFaceSource = boxBoundary;
  auto ambiguousFaceBindings =
      ambiguousFaceSource.augmentedSource.lineage->bindings();
  const auto selectedBinding = std::find_if(
      ambiguousFaceBindings.begin(), ambiguousFaceBindings.end(),
      [&](const auto& binding) {
        return binding.provenance == boxTop.v2()->provenance;
      });
  CHECK(selectedBinding != ambiguousFaceBindings.end());
  auto alternateFaceKey = selectedBinding->provenance;
  alternateFaceKey.localName += "-alternate";
  ambiguousFaceBindings.push_back(
      {std::move(alternateFaceKey), selectedBinding->subshape});
  auto ambiguousFaceLineage = solidar::TopologyLineage::create(
      std::move(ambiguousFaceBindings),
      solidar::TopologyLineageCoverage::CompleteFaces, &error);
  CHECK(ambiguousFaceLineage);
  ambiguousFaceSource.augmentedSource.lineage =
      std::move(ambiguousFaceLineage);
  expectFailure(boxTop, ambiguousFaceSource, kPrismFeature,
                solidar::FacePrismTopologyHistoryFailureCode::InvalidSource);

  auto ambiguousEdgeSource = boxBoundary;
  auto ambiguousEdgeBindings =
      ambiguousEdgeSource.augmentedSource.lineage->bindings();
  auto alternateEdgeKey =
      ambiguousEdgeSource.boundarySeeds.front().provenance;
  alternateEdgeKey.localName += "-alternate";
  ambiguousEdgeBindings.push_back(
      {std::move(alternateEdgeKey),
       ambiguousEdgeSource.boundarySeeds.front().edge});
  auto ambiguousEdgeLineage = solidar::TopologyLineage::create(
      std::move(ambiguousEdgeBindings),
      solidar::TopologyLineageCoverage::CompleteFaces, &error);
  CHECK(ambiguousEdgeLineage);
  ambiguousEdgeSource.augmentedSource.lineage =
      std::move(ambiguousEdgeLineage);
  expectFailure(
      boxTop, ambiguousEdgeSource, kPrismFeature,
      solidar::FacePrismTopologyHistoryFailureCode::NonCanonicalBoundaryEvidence);

  auto conflictingSeed = boxBoundary;
  conflictingSeed.boundarySeeds[0].origin.localName += "-conflict";
  expectFailure(boxTop, conflictingSeed, kPrismFeature,
                solidar::FacePrismTopologyHistoryFailureCode::NonCanonicalBoundaryEvidence);

  // Even a short, canonical, producer-owned key plus a matching current edge
  // binding is not trusted when it differs from fresh manifold derivation.
  auto tamperedCanonical = boxBoundary;
  auto attackerKey = tamperedCanonical.boundarySeeds.front().provenance;
  attackerKey.localName = "attacker-short-edge-key";
  attackerKey.origins.clear();
  tamperedCanonical.boundarySeeds.front().provenance = attackerKey;
  tamperedCanonical.boundarySeeds.front().origin =
      attackerKey.immediateOrigin();
  auto tamperedBindings =
      tamperedCanonical.augmentedSource.lineage->bindings();
  for (auto& binding : tamperedBindings) {
    if (binding.provenance.kind == solidar::TopologyKind::Edge &&
        binding.subshape->IsSame(
            *tamperedCanonical.boundarySeeds.front().edge)) {
      binding.provenance = attackerKey;
      break;
    }
  }
  auto tamperedLineage = solidar::TopologyLineage::create(
      std::move(tamperedBindings),
      solidar::TopologyLineageCoverage::CompleteFaces, &error);
  CHECK(tamperedLineage);
  tamperedCanonical.augmentedSource.lineage = std::move(tamperedLineage);
  expectFailure(
      boxTop, tamperedCanonical, kPrismFeature,
      solidar::FacePrismTopologyHistoryFailureCode::NonCanonicalBoundaryEvidence);

  // Find a canonical source identity near the boundary encoder's own size
  // limit: boundary capture still succeeds, while the additional face-prism
  // branch+full-origin envelope must fail closed at 16384 bytes.
  std::optional<solidar::FaceBoundaryEdgeProvenanceResult> longBoundary;
  solidar::FaceReference longSelected;
  for (std::size_t length = 16384; length > 15000 && !longBoundary;
       --length) {
    auto bindings = boxSource.lineage->bindings();
    auto selectedBinding = std::find_if(
        bindings.begin(), bindings.end(), [](const auto& binding) {
          return binding.provenance.localName == "z+";
        });
    CHECK(selectedBinding != bindings.end());
    selectedBinding->provenance.localName.assign(length, 'r');
    auto longLineage = solidar::TopologyLineage::create(
        std::move(bindings),
        solidar::TopologyLineageCoverage::CompleteFaces, &error);
    CHECK(longLineage);
    solidar::TopologySourceSnapshot longSource{
        kSourceFeature, boxSource.shape, std::move(longLineage)};
    const auto key = std::find_if(
        longSource.lineage->bindings().begin(),
        longSource.lineage->bindings().end(), [](const auto& binding) {
          return binding.provenance.localName.size() > 15000;
        });
    CHECK(key != longSource.lineage->bindings().end());
    const auto candidate =
        solidar::FaceReference::fromV2({kBodyId, key->provenance});
    solidar::FaceBoundaryEdgeProvenanceFailure boundaryFailure;
    auto captured = solidar::captureUniqueManifoldFaceBoundary(
        longSource, candidate, &boundaryFailure);
    if (!captured) continue;
    bool prismNameOversized =
        expectedName("start-cap", key->provenance.immediateOrigin()).size() >
        16384;
    for (const auto& seed : captured->boundarySeeds)
      prismNameOversized =
          prismNameOversized ||
          expectedName("side", seed.origin).size() > 16384;
    if (!prismNameOversized) continue;
    longSelected = candidate;
    longBoundary = std::move(captured);
  }
  CHECK(longBoundary);
  const TopoDS_Face longFace = resolvedFace(*longBoundary, longSelected);
  BRepPrimAPI_MakePrism longMaker(longFace, gp_Vec(0.0, 0.0, 2.0));
  solidar::FacePrismTopologyHistoryFailure longFailure;
  CHECK(!solidar::captureFacePrismTopologyHistory(
      longMaker, kPrismFeature, longSelected, *longBoundary, &longFailure));
  CHECK(longFailure.code ==
        solidar::FacePrismTopologyHistoryFailureCode::ResourceLimit);

  // A maker for a different exact source face cannot masquerade as the
  // selected-face prism: its cap/side history is absent or foreign.
  const auto boxBottom = referenceFor(boxSource, "z-");
  auto bottomBoundary = boundaryFor(boxSource, boxBottom);
  solidar::FacePrismTopologyHistoryFailure wrongMakerFailure;
  auto wrongMaker = solidar::captureFacePrismTopologyHistory(
      validMaker, kPrismFeature, boxBottom, bottomBoundary,
      &wrongMakerFailure);
  CHECK(!wrongMaker);
  CHECK(wrongMakerFailure.code ==
        solidar::FacePrismTopologyHistoryFailureCode::InvalidCapHistory);

  const auto expectMakerFailure = [&](BRepPrimAPI_MakePrism& maker,
                                      auto expectedCode) {
    solidar::FacePrismTopologyHistoryFailure failure;
    const auto result = solidar::captureFacePrismTopologyHistory(
        maker, kPrismFeature, boxTop, boxBoundary, &failure);
    if (failure.code != expectedCode)
      std::cerr << "maker failure mismatch expected="
                << static_cast<unsigned>(expectedCode) << " actual="
                << static_cast<unsigned>(failure.code)
                << " detail=" << failure.detail << '\n';
    CHECK(!result);
    CHECK(failure.code == expectedCode);
    CHECK(!failure.detail.empty());
  };

  // Exact source proof intentionally rejects Copy=true because the maker's
  // first cap is a copied TShape rather than the selected current face.
  BRepPrimAPI_MakePrism copiedInputMaker(
      boxFace, gp_Vec(0.0, 0.0, 2.0), true);
  expectMakerFailure(
      copiedInputMaker,
      solidar::FacePrismTopologyHistoryFailureCode::InvalidCapHistory);

  ScriptedGeneratedPrism notDoneMaker(
      boxFace, gp_Vec(0.0, 0.0, 2.0),
      ScriptedGeneratedPrism::Mode::Actual);
  notDoneMaker.forceNotDone();
  expectMakerFailure(
      notDoneMaker,
      solidar::FacePrismTopologyHistoryFailureCode::InvalidMaker);

  // OCCT's per-edge history is virtual even though MakePrism itself is a
  // concrete maker. A narrow test subclass exercises every malformed branch
  // without changing the public production API.
  for (const auto mode : {ScriptedGeneratedPrism::Mode::Empty,
                          ScriptedGeneratedPrism::Mode::Two,
                          ScriptedGeneratedPrism::Mode::NonFace}) {
    ScriptedGeneratedPrism malformed(boxFace, gp_Vec(0.0, 0.0, 2.0),
                                     mode);
    expectMakerFailure(
        malformed,
        solidar::FacePrismTopologyHistoryFailureCode::InvalidSideHistory);
  }
  ScriptedGeneratedPrism duplicateOutput(
      boxFace, gp_Vec(0.0, 0.0, 2.0),
      ScriptedGeneratedPrism::Mode::Duplicate,
      *boxBoundary.boundarySeeds.front().edge);
  expectMakerFailure(
      duplicateOutput,
      solidar::FacePrismTopologyHistoryFailureCode::IdentityCollision);

  return EXIT_SUCCESS;
}
