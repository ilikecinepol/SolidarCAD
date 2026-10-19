#include "TestAssertions.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "model/OcctTopologyHistoryCapture.h"
#include "model/TopologyReferenceResolver.h"

namespace {

constexpr solidar::BodyId kBodyId = 7001;
constexpr solidar::SketchId kSketchId = 7101;
constexpr solidar::FeatureId kPrismFeatureId = 7201;

struct RectangleProfile {
  std::shared_ptr<const TopoDS_Face> face;
  std::vector<std::shared_ptr<const TopoDS_Edge>> edges;
  std::vector<solidar::PrismProfileEdgeSeed> seeds;
};

class ExactIdentityTransform final : public BRepBuilderAPI_Transform {
 public:
  explicit ExactIdentityTransform(const TopoDS_Shape& shape)
      : BRepBuilderAPI_Transform(gp_Trsf{}) {
    myShape = shape;
    Done();
  }

  TopoDS_Shape ModifiedShape(const TopoDS_Shape& source) const override {
    return source.Reversed();
  }
};

RectangleProfile rectangleProfile(double xOffset = 0.0) {
  const std::array<gp_Pnt, 4> points{
      gp_Pnt{xOffset, 0.0, 0.0}, gp_Pnt{xOffset + 8.0, 0.0, 0.0},
      gp_Pnt{xOffset + 8.0, 5.0, 0.0}, gp_Pnt{xOffset, 5.0, 0.0}};
  RectangleProfile result;
  BRepBuilderAPI_MakeWire wire;
  for (std::size_t index = 0; index < points.size(); ++index) {
    wire.Add(BRepBuilderAPI_MakeEdge(points[index],
                                    points[(index + 1) % points.size()])
                 .Edge());
  }
  result.face = std::make_shared<TopoDS_Face>(
      BRepBuilderAPI_MakeFace(wire.Wire()).Face());
  for (TopExp_Explorer explorer(*result.face, TopAbs_EDGE); explorer.More();
       explorer.Next()) {
    auto edge =
        std::make_shared<TopoDS_Edge>(TopoDS::Edge(explorer.Current()));
    TopoDS_Vertex first;
    TopoDS_Vertex last;
    TopExp::Vertices(*edge, first, last);
    const auto firstPoint = BRep_Tool::Pnt(first);
    const auto lastPoint = BRep_Tool::Pnt(last);
    solidar::sketch::GeometryId geometryId{};
    if (firstPoint.Y() == 0.0 && lastPoint.Y() == 0.0)
      geometryId = 101;
    else if (firstPoint.X() == xOffset + 8.0 &&
             lastPoint.X() == xOffset + 8.0)
      geometryId = 102;
    else if (firstPoint.Y() == 5.0 && lastPoint.Y() == 5.0)
      geometryId = 103;
    else if (firstPoint.X() == xOffset && lastPoint.X() == xOffset)
      geometryId = 104;
    CHECK(geometryId != solidar::sketch::kInvalidGeometryId);
    result.edges.push_back(edge);
    result.seeds.push_back({geometryId, edge});
  }
  CHECK(result.seeds.size() == 4);
  return result;
}

std::size_t faceCount(const TopoDS_Shape& shape) {
  std::vector<TopoDS_Shape> faces;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next()) {
    if (std::none_of(faces.begin(), faces.end(), [&](const auto& existing) {
          return existing.IsSame(explorer.Current());
        }))
      faces.push_back(explorer.Current());
  }
  return faces.size();
}

const solidar::HistoryFact* factFor(
    const solidar::ShapeHistoryArtifact& artifact,
    const solidar::TopologyProvenanceKey& key) {
  const auto found = std::find_if(
      artifact.facts.begin(), artifact.facts.end(), [&](const auto& fact) {
        return fact.outputProvenance && *fact.outputProvenance == key;
      });
  return found == artifact.facts.end() ? nullptr : &*found;
}

const solidar::HistoryFact* factForOrigin(
    const solidar::ShapeHistoryArtifact& artifact,
    const solidar::TopologyProvenanceOrigin& origin) {
  const auto found = std::find_if(
      artifact.facts.begin(), artifact.facts.end(), [&](const auto& fact) {
        return fact.source && *fact.source == origin;
      });
  return found == artifact.facts.end() ? nullptr : &*found;
}

const solidar::TransformFaceSeed* seedForOrigin(
    std::span<const solidar::TransformFaceSeed> seeds,
    const solidar::TopologyProvenanceOrigin& origin) {
  const auto found =
      std::find_if(seeds.begin(), seeds.end(), [&](const auto& seed) {
        return seed.sourceOrigin == origin;
      });
  return found == seeds.end() ? nullptr : &*found;
}

void checkEmittedSeedKeys(
    const solidar::ShapeHistoryArtifact& artifact,
    std::span<const solidar::TransformFaceSeed> seeds) {
  CHECK(artifact.facts.size() == seeds.size());
  for (const auto& fact : artifact.facts) {
    CHECK(fact.source);
    CHECK(fact.outputProvenance);
    const auto* seed = seedForOrigin(seeds, *fact.source);
    CHECK(seed);
    CHECK(*fact.outputProvenance == seed->outputProvenance);
  }
}

std::vector<solidar::TransformFaceSeed> transformSeeds(
    const solidar::TopologyLineage& lineage,
    solidar::FeatureId producerFeatureId,
    solidar::TopologyProvenanceOperation operation,
    solidar::TopologySemanticRole role, std::uint64_t slot) {
  std::vector<solidar::TransformFaceSeed> seeds;
  seeds.reserve(lineage.bindings().size());
  for (const auto& binding : lineage.bindings()) {
    const auto origin = binding.provenance.immediateOrigin();
    seeds.push_back(
        {origin,
         {producerFeatureId, solidar::TopologyKind::Face, operation, role,
          "transform:" + std::to_string(origin.featureId) + ":" +
              origin.localName,
          {origin}, solidar::TopologyOccurrenceKey{producerFeatureId, slot}}});
  }
  return seeds;
}

std::shared_ptr<const solidar::TopologyLineage> buildLineage(
    const solidar::TopologyLineageBuilder& builder) {
  std::string error;
  auto lineage = builder.build(&error);
  CHECK(lineage);
  CHECK(error.empty());
  return lineage;
}

}  // namespace

int main() {
  const auto profile = rectangleProfile();

  // Live prism history is copied before OCCT scratch lists or the maker die.
  // Identity comes only from typed sketch/geometry tokens, never ordinals.
  std::optional<solidar::TopologyLineageBuilder> prismBuilder;
  std::vector<std::shared_ptr<const TopoDS_Shape>> expectedSides;
  std::shared_ptr<const TopoDS_Shape> expectedStartCap;
  std::shared_ptr<const TopoDS_Shape> expectedEndCap;
  {
    BRepPrimAPI_MakePrism maker(*profile.face, gp_Vec(0.0, 0.0, 12.0));
    expectedStartCap =
        std::make_shared<TopoDS_Shape>(maker.FirstShape(*profile.face));
    expectedEndCap =
        std::make_shared<TopoDS_Shape>(maker.LastShape(*profile.face));
    std::string error = "stale";
    prismBuilder = solidar::capturePrismFaceHistory(
        maker, kPrismFeatureId, kSketchId, profile.face, profile.seeds,
        &error);
    CHECK(prismBuilder);
    CHECK(error.empty());
    CHECK(prismBuilder->artifact().coverage ==
          solidar::TopologyLineageCoverage::CompleteFaces);
    CHECK(prismBuilder->artifact().facts.size() == 6);
    CHECK(faceCount(*prismBuilder->artifact().outputShape) == 6);

    // Repeated sequential Generated() calls reuse maker-owned scratch state;
    // copied artifact faces must remain exact after later calls.
    for (const auto& seed : profile.seeds) {
      const auto& generated = maker.Generated(*seed.edge);
      CHECK(generated.Size() == 1);
      auto copied = std::make_shared<TopoDS_Shape>(generated.First());
      expectedSides.push_back(copied);
      const auto key = solidar::projectAuthoredTopologyToken(
          {solidar::AuthoredTopologySourceKind::GeometrySide, kSketchId,
           seed.geometryId},
          kPrismFeatureId, &error);
      CHECK(key);
      const auto* fact = factFor(prismBuilder->artifact(), *key);
      CHECK(fact);
      CHECK(fact->outputSubshape->IsSame(*copied));
    }
  }

  CHECK(prismBuilder);
  std::string error;
  CHECK(solidar::validateShapeHistoryArtifact(prismBuilder->artifact(),
                                               &error));
  const auto prismLineage = buildLineage(*prismBuilder);
  const auto prismIndex = solidar::TopologyIndex::build(
      prismBuilder->artifact().outputShape, 8001, prismLineage, &error);
  CHECK(prismIndex);
  CHECK(error.empty());
  for (std::size_t index = 0; index < profile.seeds.size(); ++index) {
    const auto key = solidar::projectAuthoredTopologyToken(
        {solidar::AuthoredTopologySourceKind::GeometrySide, kSketchId,
         profile.seeds[index].geometryId},
        kPrismFeatureId, &error);
    CHECK(key);
    const auto resolved = prismIndex->resolveFace({kBodyId, *key});
    CHECK(resolved);
    CHECK(resolved.subshape->IsSame(*expectedSides[index]));
  }
  const auto startCapKey = solidar::projectAuthoredTopologyToken(
      {solidar::AuthoredTopologySourceKind::ProfileStartCap, kSketchId,
       solidar::sketch::kInvalidGeometryId},
      kPrismFeatureId, &error);
  const auto endCapKey = solidar::projectAuthoredTopologyToken(
      {solidar::AuthoredTopologySourceKind::ProfileEndCap, kSketchId,
       solidar::sketch::kInvalidGeometryId},
      kPrismFeatureId, &error);
  CHECK(startCapKey);
  CHECK(endCapKey);
  const auto startCap = prismIndex->resolveFace({kBodyId, *startCapKey});
  const auto endCap = prismIndex->resolveFace({kBodyId, *endCapKey});
  CHECK(startCap);
  CHECK(endCap);
  CHECK(startCap.subshape->IsSame(*expectedStartCap));
  CHECK(endCap.subshape->IsSame(*expectedEndCap));
  CHECK(!startCap.subshape->IsSame(*endCap.subshape));

  // Sweep direction, mapping order and edge orientation do not affect the
  // authored identities.
  {
    auto reversedSeeds = profile.seeds;
    std::reverse(reversedSeeds.begin(), reversedSeeds.end());
    auto reversedEdge = std::make_shared<TopoDS_Edge>(
        TopoDS::Edge(reversedSeeds.front().edge->Reversed()));
    reversedSeeds.front().edge = reversedEdge;
    std::optional<solidar::TopologyLineageBuilder> captured;
    std::shared_ptr<const TopoDS_Shape> expectedStart;
    std::shared_ptr<const TopoDS_Shape> expectedEnd;
    std::vector<std::pair<solidar::sketch::GeometryId,
                          std::shared_ptr<const TopoDS_Shape>>>
        expected;
    {
      BRepPrimAPI_MakePrism maker(*profile.face, gp_Vec(0.0, 0.0, -7.0));
      expectedStart =
          std::make_shared<TopoDS_Shape>(maker.FirstShape(*profile.face));
      expectedEnd =
          std::make_shared<TopoDS_Shape>(maker.LastShape(*profile.face));
      for (const auto& seed : reversedSeeds) {
        const auto& generated = maker.Generated(*seed.edge);
        CHECK(generated.Size() == 1);
        expected.emplace_back(
            seed.geometryId,
            std::make_shared<TopoDS_Shape>(generated.First()));
      }
      captured = solidar::capturePrismFaceHistory(
          maker, kPrismFeatureId + 1, kSketchId, profile.face, reversedSeeds,
          &error);
    }
    CHECK(captured);
    CHECK(error.empty());
    const auto lineage = buildLineage(*captured);
    const auto index = solidar::TopologyIndex::build(
        captured->artifact().outputShape, 8002, lineage, &error);
    CHECK(index);
    for (const auto& [geometryId, expectedSide] : expected) {
      const auto key = solidar::projectAuthoredTopologyToken(
          {solidar::AuthoredTopologySourceKind::GeometrySide, kSketchId,
           geometryId},
          kPrismFeatureId + 1, &error);
      CHECK(key);
      const auto resolved = index->resolveFace({kBodyId, *key});
      CHECK(resolved);
      CHECK(resolved.subshape->IsSame(*expectedSide));
    }
    const auto startKey = solidar::projectAuthoredTopologyToken(
        {solidar::AuthoredTopologySourceKind::ProfileStartCap, kSketchId,
         solidar::sketch::kInvalidGeometryId},
        kPrismFeatureId + 1, &error);
    const auto endKey = solidar::projectAuthoredTopologyToken(
        {solidar::AuthoredTopologySourceKind::ProfileEndCap, kSketchId,
         solidar::sketch::kInvalidGeometryId},
        kPrismFeatureId + 1, &error);
    CHECK(startKey);
    CHECK(endKey);
    const auto resolvedStart = index->resolveFace({kBodyId, *startKey});
    const auto resolvedEnd = index->resolveFace({kBodyId, *endKey});
    CHECK(resolvedStart);
    CHECK(resolvedEnd);
    CHECK(resolvedStart.subshape->IsSame(*expectedStart));
    CHECK(resolvedEnd.subshape->IsSame(*expectedEnd));
  }

  // Copy=true uses the same synchronous history contract.
  {
    std::optional<solidar::TopologyLineageBuilder> captured;
    std::shared_ptr<const TopoDS_Shape> expectedStart;
    std::shared_ptr<const TopoDS_Shape> expectedEnd;
    std::vector<std::pair<solidar::sketch::GeometryId,
                          std::shared_ptr<const TopoDS_Shape>>>
        expected;
    {
      BRepPrimAPI_MakePrism maker(*profile.face, gp_Vec(0.0, 0.0, 2.0),
                                  true);
      expectedStart =
          std::make_shared<TopoDS_Shape>(maker.FirstShape(*profile.face));
      expectedEnd =
          std::make_shared<TopoDS_Shape>(maker.LastShape(*profile.face));
      for (const auto& seed : profile.seeds) {
        const auto& generated = maker.Generated(*seed.edge);
        CHECK(generated.Size() == 1);
        expected.emplace_back(
            seed.geometryId,
            std::make_shared<TopoDS_Shape>(generated.First()));
      }
      captured = solidar::capturePrismFaceHistory(
          maker, kPrismFeatureId + 3, kSketchId, profile.face, profile.seeds,
          &error);
    }
    CHECK(captured);
    const auto lineage = buildLineage(*captured);
    const auto index = solidar::TopologyIndex::build(
        captured->artifact().outputShape, 8006, lineage, &error);
    CHECK(index);
    for (const auto& [geometryId, expectedSide] : expected) {
      const auto key = solidar::projectAuthoredTopologyToken(
          {solidar::AuthoredTopologySourceKind::GeometrySide, kSketchId,
           geometryId},
          kPrismFeatureId + 3, &error);
      CHECK(key);
      const auto resolved = index->resolveFace({kBodyId, *key});
      CHECK(resolved);
      CHECK(resolved.subshape->IsSame(*expectedSide));
    }
    const auto startKey = solidar::projectAuthoredTopologyToken(
        {solidar::AuthoredTopologySourceKind::ProfileStartCap, kSketchId,
         solidar::sketch::kInvalidGeometryId},
        kPrismFeatureId + 3, &error);
    const auto endKey = solidar::projectAuthoredTopologyToken(
        {solidar::AuthoredTopologySourceKind::ProfileEndCap, kSketchId,
         solidar::sketch::kInvalidGeometryId},
        kPrismFeatureId + 3, &error);
    CHECK(startKey);
    CHECK(endKey);
    const auto resolvedStart = index->resolveFace({kBodyId, *startKey});
    const auto resolvedEnd = index->resolveFace({kBodyId, *endKey});
    CHECK(resolvedStart);
    CHECK(resolvedEnd);
    CHECK(resolvedStart.subshape->IsSame(*expectedStart));
    CHECK(resolvedEnd.subshape->IsSame(*expectedEnd));
  }

  // Prism mapping failures are closed before a partial artifact can escape.
  {
    auto runFailure = [&](std::vector<solidar::PrismProfileEdgeSeed> seeds) {
      BRepPrimAPI_MakePrism maker(*profile.face, gp_Vec(0.0, 0.0, 3.0));
      error = "stale";
      const auto captured = solidar::capturePrismFaceHistory(
          maker, kPrismFeatureId + 2, kSketchId, profile.face, seeds, &error);
      CHECK(!captured);
      CHECK(!error.empty());
      CHECK(error != "stale");
    };
    auto missing = profile.seeds;
    missing.pop_back();
    runFailure(missing);
    auto duplicateId = profile.seeds;
    duplicateId[1].geometryId = duplicateId[0].geometryId;
    runFailure(duplicateId);
    auto duplicateEdge = profile.seeds;
    duplicateEdge[1].edge = duplicateEdge[0].edge;
    runFailure(duplicateEdge);
    auto missingId = profile.seeds;
    missingId[0].geometryId = solidar::sketch::kInvalidGeometryId;
    runFailure(missingId);
    auto foreign = profile.seeds;
    const auto foreignProfile = rectangleProfile(100.0);
    foreign[0].edge = foreignProfile.edges[0];
    runFailure(foreign);
    auto nullEdge = profile.seeds;
    nullEdge[0].edge.reset();
    BRepPrimAPI_MakePrism maker(*profile.face, gp_Vec(0.0, 0.0, 3.0));
    error = "stale";
    CHECK(!solidar::capturePrismFaceHistory(
        maker, kPrismFeatureId + 2, kSketchId, profile.face, nullEdge,
        &error));
    CHECK(!error.empty());
    CHECK(error != "stale");
  }

  const solidar::TopologySourceSnapshot sourceSnapshot{
      kPrismFeatureId, prismBuilder->artifact().outputShape, prismLineage};

  // Translation copy=true emits one Modified fact per source face. Reordered
  // source bindings and seeds produce the exact same origin-to-output map.
  std::optional<solidar::TopologyLineageBuilder> translatedBuilder;
  std::optional<solidar::TopologyLineageBuilder> permutedBuilder;
  const auto translatedSeeds = transformSeeds(
      *prismLineage, 7301, solidar::TopologyProvenanceOperation::Modified,
      solidar::TopologySemanticRole::PatternInstance, 1);
  std::vector<std::pair<solidar::TopologyProvenanceOrigin,
                        std::shared_ptr<const TopoDS_Shape>>>
      expectedTranslated;
  {
    gp_Trsf translation;
    translation.SetTranslation(gp_Vec(20.0, 4.0, 2.0));
    BRepBuilderAPI_Transform maker(*sourceSnapshot.shape, translation, true);
    for (const auto& binding : prismLineage->bindings()) {
      expectedTranslated.emplace_back(
          binding.provenance.immediateOrigin(),
          std::make_shared<TopoDS_Shape>(
              maker.ModifiedShape(*binding.subshape)));
    }
    error = "stale";
    translatedBuilder = solidar::captureTransformFaceHistory(
        maker, 7301, sourceSnapshot.shape, sourceSnapshot, translatedSeeds,
        &error);
    CHECK(translatedBuilder);
    CHECK(error.empty());
    checkEmittedSeedKeys(translatedBuilder->artifact(), translatedSeeds);
    CHECK(translatedBuilder->artifact().facts.size() == 6);
    for (const auto& fact : translatedBuilder->artifact().facts) {
      CHECK(fact.kind == solidar::ShapeHistoryFactKind::Modified);
      CHECK(!fact.outputSubshape->IsSame(*fact.sourceSubshape));
      CHECK(fact.source);
      const auto* seed = seedForOrigin(translatedSeeds, *fact.source);
      CHECK(seed);
      CHECK(fact.outputProvenance == seed->outputProvenance);
    }

    auto reversedBindings = prismLineage->bindings();
    std::reverse(reversedBindings.begin(), reversedBindings.end());
    const auto reversedLineage = solidar::TopologyLineage::create(
        std::move(reversedBindings),
        solidar::TopologyLineageCoverage::CompleteFaces, &error);
    CHECK(reversedLineage);
    const solidar::TopologySourceSnapshot reversedSource{
        kPrismFeatureId, sourceSnapshot.shape, reversedLineage};
    auto reversedSeeds = translatedSeeds;
    std::reverse(reversedSeeds.begin(), reversedSeeds.end());
    permutedBuilder = solidar::captureTransformFaceHistory(
        maker, 7301, sourceSnapshot.shape, reversedSource, reversedSeeds,
        &error);
    CHECK(permutedBuilder);
    checkEmittedSeedKeys(permutedBuilder->artifact(), reversedSeeds);
    CHECK(permutedBuilder->artifact().facts.size() ==
          translatedBuilder->artifact().facts.size());
    for (const auto& [origin, expectedOutput] : expectedTranslated) {
      const auto* forward = factForOrigin(translatedBuilder->artifact(), origin);
      const auto* reverse = factForOrigin(permutedBuilder->artifact(), origin);
      const auto* seed = seedForOrigin(translatedSeeds, origin);
      CHECK(forward);
      CHECK(reverse);
      CHECK(seed);
      CHECK(forward->outputProvenance == seed->outputProvenance);
      CHECK(reverse->outputProvenance == seed->outputProvenance);
      CHECK(forward->outputSubshape->IsSame(*expectedOutput));
      CHECK(reverse->outputSubshape->IsSame(*expectedOutput));
      CHECK(forward->outputSubshape->IsSame(*reverse->outputSubshape));
    }
  }

  CHECK(translatedBuilder);
  const auto translatedLineage = buildLineage(*translatedBuilder);
  const auto translatedIndex = solidar::TopologyIndex::build(
      translatedBuilder->artifact().outputShape, 8003, translatedLineage,
      &error);
  CHECK(translatedIndex);
  for (const auto& [origin, expectedOutput] : expectedTranslated) {
    const auto* fact = factForOrigin(translatedBuilder->artifact(), origin);
    const auto* seed = seedForOrigin(translatedSeeds, origin);
    CHECK(fact);
    CHECK(seed);
    CHECK(fact->outputProvenance == seed->outputProvenance);
    const auto resolved =
        translatedIndex->resolveFace({kBodyId, seed->outputProvenance});
    CHECK(resolved);
    CHECK(resolved.subshape->IsSame(*expectedOutput));
  }
  CHECK(permutedBuilder);
  const auto permutedLineage = buildLineage(*permutedBuilder);
  const auto permutedIndex = solidar::TopologyIndex::build(
      permutedBuilder->artifact().outputShape, 8009, permutedLineage, &error);
  CHECK(permutedIndex);
  for (const auto& [origin, expectedOutput] : expectedTranslated) {
    const auto* seed = seedForOrigin(translatedSeeds, origin);
    CHECK(seed);
    const auto resolved =
        permutedIndex->resolveFace({kBodyId, seed->outputProvenance});
    CHECK(resolved);
    CHECK(resolved.subshape->IsSame(*expectedOutput));
  }

  // Caller-authored occurrence slots distinguish two transform instances
  // owned by the same producer. Reusing a slot yields an exact-key collision
  // that the combined index reports deterministically as ambiguous.
  {
    constexpr solidar::FeatureId producer = 7310;
    const auto slotOne = transformSeeds(
        *prismLineage, producer,
        solidar::TopologyProvenanceOperation::Modified,
        solidar::TopologySemanticRole::PatternInstance, 1);
    const auto slotTwo = transformSeeds(
        *prismLineage, producer,
        solidar::TopologyProvenanceOperation::Modified,
        solidar::TopologySemanticRole::PatternInstance, 2);
    CHECK(slotOne.front().outputProvenance !=
          slotTwo.front().outputProvenance);
    CHECK(slotOne.front().outputProvenance.role ==
          solidar::TopologySemanticRole::PatternInstance);
    CHECK(slotTwo.front().outputProvenance.role ==
          solidar::TopologySemanticRole::PatternInstance);
    CHECK(slotOne.front().outputProvenance.localName ==
          slotTwo.front().outputProvenance.localName);
    CHECK(slotOne.front().outputProvenance.origins ==
          slotTwo.front().outputProvenance.origins);
    CHECK(slotOne.front().outputProvenance.occurrence);
    CHECK(slotTwo.front().outputProvenance.occurrence);
    CHECK(slotOne.front().outputProvenance.occurrence->slot == 1);
    CHECK(slotTwo.front().outputProvenance.occurrence->slot == 2);

    std::optional<solidar::TopologyLineageBuilder> first;
    std::optional<solidar::TopologyLineageBuilder> second;
    std::optional<solidar::TopologyLineageBuilder> repeatedSlot;
    std::vector<std::pair<solidar::TopologyProvenanceOrigin,
                          std::shared_ptr<const TopoDS_Shape>>>
        expectedFirst;
    std::vector<std::pair<solidar::TopologyProvenanceOrigin,
                          std::shared_ptr<const TopoDS_Shape>>>
        expectedSecond;
    {
      gp_Trsf firstTranslation;
      firstTranslation.SetTranslation(gp_Vec(30.0, 0.0, 0.0));
      gp_Trsf secondTranslation;
      secondTranslation.SetTranslation(gp_Vec(-30.0, 0.0, 0.0));
      BRepBuilderAPI_Transform firstMaker(*sourceSnapshot.shape,
                                          firstTranslation, true);
      BRepBuilderAPI_Transform secondMaker(*sourceSnapshot.shape,
                                           secondTranslation, true);
      for (const auto& binding : prismLineage->bindings()) {
        expectedFirst.emplace_back(
            binding.provenance.immediateOrigin(),
            std::make_shared<TopoDS_Shape>(
                firstMaker.ModifiedShape(*binding.subshape)));
        expectedSecond.emplace_back(
            binding.provenance.immediateOrigin(),
            std::make_shared<TopoDS_Shape>(
                secondMaker.ModifiedShape(*binding.subshape)));
      }
      first = solidar::captureTransformFaceHistory(
          firstMaker, producer, sourceSnapshot.shape, sourceSnapshot, slotOne,
          &error);
      second = solidar::captureTransformFaceHistory(
          secondMaker, producer, sourceSnapshot.shape, sourceSnapshot, slotTwo,
          &error);
      repeatedSlot = solidar::captureTransformFaceHistory(
          secondMaker, producer, sourceSnapshot.shape, sourceSnapshot, slotOne,
          &error);
    }
    CHECK(first);
    CHECK(second);
    CHECK(repeatedSlot);
    checkEmittedSeedKeys(first->artifact(), slotOne);
    checkEmittedSeedKeys(second->artifact(), slotTwo);
    checkEmittedSeedKeys(repeatedSlot->artifact(), slotOne);
    for (const auto& [origin, expectedOutput] : expectedFirst) {
      const auto* seed = seedForOrigin(slotOne, origin);
      const auto* fact = factForOrigin(first->artifact(), origin);
      CHECK(seed);
      CHECK(fact);
      CHECK(fact->outputProvenance == seed->outputProvenance);
      CHECK(fact->outputSubshape->IsSame(*expectedOutput));
    }
    for (const auto& [origin, expectedOutput] : expectedSecond) {
      const auto* seed = seedForOrigin(slotTwo, origin);
      const auto* fact = factForOrigin(second->artifact(), origin);
      CHECK(seed);
      CHECK(fact);
      CHECK(fact->outputProvenance == seed->outputProvenance);
      CHECK(fact->outputSubshape->IsSame(*expectedOutput));
    }
    for (const auto& [origin, expectedOutput] : expectedSecond) {
      const auto* seed = seedForOrigin(slotOne, origin);
      const auto* fact = factForOrigin(repeatedSlot->artifact(), origin);
      CHECK(seed);
      CHECK(fact);
      CHECK(fact->outputProvenance == seed->outputProvenance);
      CHECK(fact->outputSubshape->IsSame(*expectedOutput));
    }

    TopoDS_Compound uniqueCompound;
    BRep_Builder uniqueCompoundBuilder;
    uniqueCompoundBuilder.MakeCompound(uniqueCompound);
    uniqueCompoundBuilder.Add(uniqueCompound, *first->artifact().outputShape);
    uniqueCompoundBuilder.Add(uniqueCompound,
                              *second->artifact().outputShape);
    solidar::ShapeHistoryArtifact uniqueArtifact;
    uniqueArtifact.producerFeatureId = producer;
    uniqueArtifact.outputShape =
        std::make_shared<TopoDS_Shape>(uniqueCompound);
    uniqueArtifact.coverage =
        solidar::TopologyLineageCoverage::CompleteFaces;
    uniqueArtifact.sources.push_back(sourceSnapshot);
    uniqueArtifact.facts = first->artifact().facts;
    uniqueArtifact.facts.insert(uniqueArtifact.facts.end(),
                                second->artifact().facts.begin(),
                                second->artifact().facts.end());
    const auto uniqueBuilder = solidar::TopologyLineageBuilder::create(
        std::move(uniqueArtifact), &error);
    CHECK(uniqueBuilder);
    const auto uniqueLineage = buildLineage(*uniqueBuilder);
    const auto uniqueIndex = solidar::TopologyIndex::build(
        uniqueBuilder->artifact().outputShape, 8010, uniqueLineage, &error);
    CHECK(uniqueIndex);
    for (const auto& [origin, expectedOutput] : expectedFirst) {
      const auto* seed = seedForOrigin(slotOne, origin);
      CHECK(seed);
      const auto resolved =
          uniqueIndex->resolveFace({kBodyId, seed->outputProvenance});
      CHECK(resolved);
      CHECK(resolved.subshape->IsSame(*expectedOutput));
    }
    for (const auto& [origin, expectedOutput] : expectedSecond) {
      const auto* seed = seedForOrigin(slotTwo, origin);
      CHECK(seed);
      const auto resolved =
          uniqueIndex->resolveFace({kBodyId, seed->outputProvenance});
      CHECK(resolved);
      CHECK(resolved.subshape->IsSame(*expectedOutput));
    }

    TopoDS_Compound compound;
    BRep_Builder compoundBuilder;
    compoundBuilder.MakeCompound(compound);
    compoundBuilder.Add(compound, *first->artifact().outputShape);
    compoundBuilder.Add(compound, *repeatedSlot->artifact().outputShape);
    solidar::ShapeHistoryArtifact collisionArtifact;
    collisionArtifact.producerFeatureId = producer;
    collisionArtifact.outputShape = std::make_shared<TopoDS_Shape>(compound);
    collisionArtifact.coverage =
        solidar::TopologyLineageCoverage::CompleteFaces;
    collisionArtifact.sources.push_back(sourceSnapshot);
    collisionArtifact.facts = first->artifact().facts;
    collisionArtifact.facts.insert(collisionArtifact.facts.end(),
                                   repeatedSlot->artifact().facts.begin(),
                                   repeatedSlot->artifact().facts.end());
    error = "stale";
    const auto collisionBuilder = solidar::TopologyLineageBuilder::create(
        std::move(collisionArtifact), &error);
    CHECK(!collisionBuilder);
    CHECK(!error.empty());
    CHECK(error != "stale");
  }

  // An exact identity result (including an orientation alias) emits Retained
  // and survives the full builder/index/resolve path.
  {
    const auto seeds = transformSeeds(
        *prismLineage, 7302, solidar::TopologyProvenanceOperation::Retained,
        solidar::TopologySemanticRole::Original, 1);
    std::optional<solidar::TopologyLineageBuilder> retained;
    std::vector<std::pair<solidar::TopologyProvenanceOrigin,
                          std::shared_ptr<const TopoDS_Shape>>>
        expected;
    {
      ExactIdentityTransform maker(*sourceSnapshot.shape);
      for (const auto& binding : prismLineage->bindings()) {
        expected.emplace_back(
            binding.provenance.immediateOrigin(),
            std::make_shared<TopoDS_Shape>(
                maker.ModifiedShape(*binding.subshape)));
      }
      retained = solidar::captureTransformFaceHistory(
          maker, 7302, sourceSnapshot.shape, sourceSnapshot, seeds, &error);
    }
    CHECK(retained);
    checkEmittedSeedKeys(retained->artifact(), seeds);
    const auto lineage = buildLineage(*retained);
    const auto index = solidar::TopologyIndex::build(
        retained->artifact().outputShape, 8004, lineage, &error);
    CHECK(index);
    for (const auto& [origin, expectedOutput] : expected) {
      const auto* fact = factForOrigin(retained->artifact(), origin);
      const auto* seed = seedForOrigin(seeds, origin);
      CHECK(fact);
      CHECK(seed);
      CHECK(fact->outputProvenance == seed->outputProvenance);
      CHECK(fact->kind == solidar::ShapeHistoryFactKind::Retained);
      CHECK(fact->outputSubshape->IsSame(*fact->sourceSubshape));
      const auto resolved =
          index->resolveFace({kBodyId, seed->outputProvenance});
      CHECK(resolved);
      CHECK(resolved.subshape->IsSame(*expectedOutput));
    }
  }

  // The pinned OCCT 8.0.1 default identity transform with copyGeom=false
  // creates a new TShape, so its live Retained path is unavailable. The
  // synthetic adapter above remains the Retained-classification oracle.
  {
    const auto seeds = transformSeeds(
        *prismLineage, 7305, solidar::TopologyProvenanceOperation::Modified,
        solidar::TopologySemanticRole::Original, 1);
    std::optional<solidar::TopologyLineageBuilder> identityCapture;
    std::vector<std::pair<solidar::TopologyProvenanceOrigin,
                          std::shared_ptr<const TopoDS_Shape>>>
        expected;
    {
      gp_Trsf identity;
      BRepBuilderAPI_Transform maker(*sourceSnapshot.shape, identity, false);
      CHECK(!maker.Shape().IsSame(*sourceSnapshot.shape));
      for (const auto& binding : prismLineage->bindings()) {
        expected.emplace_back(
            binding.provenance.immediateOrigin(),
            std::make_shared<TopoDS_Shape>(
                maker.ModifiedShape(*binding.subshape)));
      }
      identityCapture = solidar::captureTransformFaceHistory(
          maker, 7305, sourceSnapshot.shape, sourceSnapshot, seeds, &error);
    }
    CHECK(identityCapture);
    checkEmittedSeedKeys(identityCapture->artifact(), seeds);
    const auto lineage = buildLineage(*identityCapture);
    const auto index = solidar::TopologyIndex::build(
        identityCapture->artifact().outputShape, 8005, lineage, &error);
    CHECK(index);
    for (const auto& [origin, expectedOutput] : expected) {
      const auto* fact = factForOrigin(identityCapture->artifact(), origin);
      const auto* seed = seedForOrigin(seeds, origin);
      CHECK(fact);
      CHECK(seed);
      CHECK(fact->outputProvenance == seed->outputProvenance);
      CHECK(fact->kind == solidar::ShapeHistoryFactKind::Modified);
      const auto resolved =
          index->resolveFace({kBodyId, seed->outputProvenance});
      CHECK(resolved);
      CHECK(resolved.subshape->IsSame(*expectedOutput));
    }
  }
  {
    const auto seeds = transformSeeds(
        *prismLineage, 7303, solidar::TopologyProvenanceOperation::Modified,
        solidar::TopologySemanticRole::PatternInstance, 1);
    std::optional<solidar::TopologyLineageBuilder> captured;
    std::vector<std::pair<solidar::TopologyProvenanceOrigin,
                          std::shared_ptr<const TopoDS_Shape>>>
        expected;
    {
      gp_Trsf translation;
      translation.SetTranslation(gp_Vec(-3.0, 6.0, 1.0));
      BRepBuilderAPI_Transform maker(*sourceSnapshot.shape, translation, false);
      for (const auto& binding : prismLineage->bindings())
        expected.emplace_back(
            binding.provenance.immediateOrigin(),
            std::make_shared<TopoDS_Shape>(
                maker.ModifiedShape(*binding.subshape)));
      captured = solidar::captureTransformFaceHistory(
          maker, 7303, sourceSnapshot.shape, sourceSnapshot, seeds, &error);
    }
    CHECK(captured);
    checkEmittedSeedKeys(captured->artifact(), seeds);
    const auto lineage = buildLineage(*captured);
    const auto index = solidar::TopologyIndex::build(
        captured->artifact().outputShape, 8007, lineage, &error);
    CHECK(index);
    for (const auto& [origin, expectedOutput] : expected) {
      const auto* fact = factForOrigin(captured->artifact(), origin);
      const auto* seed = seedForOrigin(seeds, origin);
      CHECK(fact);
      CHECK(seed);
      CHECK(fact->outputProvenance == seed->outputProvenance);
      CHECK(fact->kind == solidar::ShapeHistoryFactKind::Modified);
      const auto resolved =
          index->resolveFace({kBodyId, seed->outputProvenance});
      CHECK(resolved);
      CHECK(resolved.subshape->IsSame(*expectedOutput));
    }
  }

  // Reflection is deterministic and remains a one-to-one Modified mapping.
  {
    const auto seeds = transformSeeds(
        *prismLineage, 7304, solidar::TopologyProvenanceOperation::Modified,
        solidar::TopologySemanticRole::Reflected, 1);
    std::optional<solidar::TopologyLineageBuilder> captured;
    std::vector<std::pair<solidar::TopologyProvenanceOrigin,
                          std::shared_ptr<const TopoDS_Shape>>>
        expected;
    {
      gp_Trsf mirror;
      mirror.SetMirror(gp_Ax2(gp_Pnt(0.0, 0.0, 0.0),
                              gp_Dir(1.0, 0.0, 0.0)));
      BRepBuilderAPI_Transform maker(*sourceSnapshot.shape, mirror, true);
      for (const auto& binding : prismLineage->bindings())
        expected.emplace_back(
            binding.provenance.immediateOrigin(),
            std::make_shared<TopoDS_Shape>(
                maker.ModifiedShape(*binding.subshape)));
      captured = solidar::captureTransformFaceHistory(
          maker, 7304, sourceSnapshot.shape, sourceSnapshot, seeds, &error);
    }
    CHECK(captured);
    checkEmittedSeedKeys(captured->artifact(), seeds);
    const auto lineage = buildLineage(*captured);
    const auto index = solidar::TopologyIndex::build(
        captured->artifact().outputShape, 8008, lineage, &error);
    CHECK(index);
    for (const auto& [origin, expectedOutput] : expected) {
      const auto* fact = factForOrigin(captured->artifact(), origin);
      const auto* seed = seedForOrigin(seeds, origin);
      CHECK(fact);
      CHECK(seed);
      CHECK(fact->outputProvenance == seed->outputProvenance);
      CHECK(fact->kind == solidar::ShapeHistoryFactKind::Modified);
      const auto resolved =
          index->resolveFace({kBodyId, seed->outputProvenance});
      CHECK(resolved);
      CHECK(resolved.subshape->IsSame(*expectedOutput));
    }
  }

  // Foreign/incomplete/duplicate source evidence and a maker built from a
  // different input all fail without publishing partial history.
  {
    gp_Trsf translation;
    translation.SetTranslation(gp_Vec(1.0, 2.0, 3.0));
    BRepBuilderAPI_Transform maker(*sourceSnapshot.shape, translation, false);
    const auto validSeeds = transformSeeds(
        *prismLineage, 7410, solidar::TopologyProvenanceOperation::Modified,
        solidar::TopologySemanticRole::PatternInstance, 3);
    auto runSeedFailure =
        [&](std::vector<solidar::TransformFaceSeed> seeds) {
          error = "stale";
          CHECK(!solidar::captureTransformFaceHistory(
              maker, 7410, sourceSnapshot.shape, sourceSnapshot, seeds,
              &error));
          CHECK(!error.empty());
          CHECK(error != "stale");
        };
    auto missingSeed = validSeeds;
    missingSeed.pop_back();
    runSeedFailure(missingSeed);
    auto duplicateSeed = validSeeds;
    duplicateSeed[1] = duplicateSeed[0];
    runSeedFailure(duplicateSeed);
    auto wrongProducer = validSeeds;
    wrongProducer[0].outputProvenance.featureId = 9999;
    runSeedFailure(wrongProducer);
    auto wrongKind = validSeeds;
    wrongKind[0].outputProvenance.kind = solidar::TopologyKind::Edge;
    runSeedFailure(wrongKind);
    auto wrongOperation = validSeeds;
    wrongOperation[0].outputProvenance.operation =
        solidar::TopologyProvenanceOperation::Retained;
    runSeedFailure(wrongOperation);
    auto extraOrigin = validSeeds;
    extraOrigin[0].outputProvenance.origins.push_back(
        validSeeds[1].sourceOrigin);
    extraOrigin[0].outputProvenance.canonicalizeOrigins();
    runSeedFailure(extraOrigin);
    auto wrongOccurrenceOwner = validSeeds;
    wrongOccurrenceOwner[0].outputProvenance.occurrence =
        solidar::TopologyOccurrenceKey{9999, 3};
    runSeedFailure(wrongOccurrenceOwner);
    auto unknownRole = validSeeds;
    unknownRole[0].outputProvenance.role =
        static_cast<solidar::TopologySemanticRole>(255);
    runSeedFailure(unknownRole);

    const auto foreignProfile = rectangleProfile(200.0);
    BRepPrimAPI_MakePrism foreignPrism(*foreignProfile.face,
                                       gp_Vec(0.0, 0.0, 4.0));
    const auto foreignBuilder = solidar::capturePrismFaceHistory(
        foreignPrism, 7401, kSketchId + 1, foreignProfile.face,
        foreignProfile.seeds, &error);
    CHECK(foreignBuilder);
    const auto foreignLineage = buildLineage(*foreignBuilder);
    const solidar::TopologySourceSnapshot foreignSource{
        7401, foreignBuilder->artifact().outputShape, foreignLineage};
    const auto foreignSeeds = transformSeeds(
        *foreignLineage, 7402, solidar::TopologyProvenanceOperation::Modified,
        solidar::TopologySemanticRole::PatternInstance, 1);
    error = "stale";
    CHECK(!solidar::captureTransformFaceHistory(
        maker, 7402, sourceSnapshot.shape, foreignSource, foreignSeeds,
        &error));
    CHECK(!error.empty());
    CHECK(error != "stale");

    auto partialBindings = prismLineage->bindings();
    partialBindings.pop_back();
    const auto partialLineage = solidar::TopologyLineage::create(
        std::move(partialBindings),
        solidar::TopologyLineageCoverage::PartialFaces, &error);
    CHECK(partialLineage);
    const auto partialSeeds = transformSeeds(
        *partialLineage, 7403, solidar::TopologyProvenanceOperation::Modified,
        solidar::TopologySemanticRole::PatternInstance, 1);
    error = "stale";
    CHECK(!solidar::captureTransformFaceHistory(
        maker, 7403, sourceSnapshot.shape,
        {kPrismFeatureId, sourceSnapshot.shape, partialLineage}, partialSeeds,
        &error));
    CHECK(!error.empty());
    CHECK(error != "stale");

    auto duplicateBindings = prismLineage->bindings();
    duplicateBindings.push_back(duplicateBindings.front());
    const auto duplicateLineage = solidar::TopologyLineage::create(
        std::move(duplicateBindings),
        solidar::TopologyLineageCoverage::CompleteFaces, &error);
    CHECK(duplicateLineage);
    const auto duplicateSeeds = transformSeeds(
        *duplicateLineage, 7404,
        solidar::TopologyProvenanceOperation::Modified,
        solidar::TopologySemanticRole::PatternInstance, 1);
    error = "stale";
    CHECK(!solidar::captureTransformFaceHistory(
        maker, 7404, sourceSnapshot.shape,
        {kPrismFeatureId, sourceSnapshot.shape, duplicateLineage},
        duplicateSeeds, &error));
    CHECK(!error.empty());
    CHECK(error != "stale");

    BRepBuilderAPI_Transform foreignMaker(
        *foreignSource.shape, translation, false);
    const auto inputSeeds = transformSeeds(
        *prismLineage, 7405, solidar::TopologyProvenanceOperation::Modified,
        solidar::TopologySemanticRole::PatternInstance, 1);
    error = "stale";
    CHECK(!solidar::captureTransformFaceHistory(
        foreignMaker, 7405, sourceSnapshot.shape, sourceSnapshot, inputSeeds,
        &error));
    CHECK(!error.empty());
    CHECK(error != "stale");
  }

  return EXIT_SUCCESS;
}
