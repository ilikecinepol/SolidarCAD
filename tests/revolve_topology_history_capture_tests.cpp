#include "TestAssertions.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <NCollection_List.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "model/OcctRevolveTopologyHistoryCapture.h"

namespace {

constexpr solidar::BodyId kBodyId = 8101;
constexpr solidar::SketchId kSketchId = 8201;
constexpr solidar::FeatureId kFeatureId = 8301;
const gp_Ax1 kYAxis{gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 1.0, 0.0)};

struct Profile {
  std::shared_ptr<const TopoDS_Face> face;
  std::vector<std::shared_ptr<const TopoDS_Edge>> edges;
  std::vector<solidar::RevolveProfileEdgeSeed> seeds;
};

Profile rectangleProfile(double xMin = 2.0, double xMax = 7.0,
                         double yMin = 1.0, double yMax = 5.0) {
  const std::array<gp_Pnt, 4> points{
      gp_Pnt{xMin, yMin, 0.0}, gp_Pnt{xMax, yMin, 0.0},
      gp_Pnt{xMax, yMax, 0.0}, gp_Pnt{xMin, yMax, 0.0}};
  BRepBuilderAPI_MakeWire wire;
  Profile result;
  for (std::size_t index = 0; index < points.size(); ++index) {
    const TopoDS_Edge edge =
        BRepBuilderAPI_MakeEdge(points[index],
                               points[(index + 1) % points.size()])
            .Edge();
    wire.Add(edge);
  }
  result.face = std::make_shared<TopoDS_Face>(
      BRepBuilderAPI_MakeFace(wire.Wire()).Face());
  for (TopExp_Explorer explorer(*result.face, TopAbs_EDGE); explorer.More();
       explorer.Next()) {
    auto owned = std::make_shared<TopoDS_Edge>(
        TopoDS::Edge(explorer.Current()));
    TopoDS_Vertex first;
    TopoDS_Vertex last;
    TopExp::Vertices(*owned, first, last);
    const gp_Pnt firstPoint = BRep_Tool::Pnt(first);
    const gp_Pnt lastPoint = BRep_Tool::Pnt(last);
    solidar::sketch::GeometryId geometryId =
        solidar::sketch::kInvalidGeometryId;
    if (std::abs(firstPoint.Y() - yMin) < 1.0e-9 &&
        std::abs(lastPoint.Y() - yMin) < 1.0e-9)
      geometryId = 101;
    else if (std::abs(firstPoint.X() - xMax) < 1.0e-9 &&
             std::abs(lastPoint.X() - xMax) < 1.0e-9)
      geometryId = 102;
    else if (std::abs(firstPoint.Y() - yMax) < 1.0e-9 &&
             std::abs(lastPoint.Y() - yMax) < 1.0e-9)
      geometryId = 103;
    else if (std::abs(firstPoint.X() - xMin) < 1.0e-9 &&
             std::abs(lastPoint.X() - xMin) < 1.0e-9)
      geometryId = 104;
    CHECK(geometryId != solidar::sketch::kInvalidGeometryId);
    result.edges.push_back(owned);
    result.seeds.push_back({geometryId, owned});
  }
  CHECK(result.seeds.size() == 4);
  return result;
}

Profile circleProfile() {
  const gp_Circ circle{gp_Ax2(gp_Pnt(5.0, 3.0, 0.0),
                              gp_Dir(0.0, 0.0, 1.0)),
                       1.0};
  BRepBuilderAPI_MakeWire wire;
  wire.Add(BRepBuilderAPI_MakeEdge(circle).Edge());
  Profile result;
  result.face = std::make_shared<TopoDS_Face>(
      BRepBuilderAPI_MakeFace(wire.Wire()).Face());
  for (TopExp_Explorer explorer(*result.face, TopAbs_EDGE); explorer.More();
       explorer.Next()) {
    auto edge = std::make_shared<TopoDS_Edge>(
        TopoDS::Edge(explorer.Current()));
    result.edges.push_back(edge);
    result.seeds.push_back({201, edge});
  }
  CHECK(result.seeds.size() == 1);
  return result;
}

std::vector<TopoDS_Shape> canonicalFaces(const TopoDS_Shape& shape) {
  std::vector<TopoDS_Shape> result;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next()) {
    if (std::none_of(result.begin(), result.end(), [&](const auto& existing) {
          return existing.IsSame(explorer.Current());
        }))
      result.push_back(explorer.Current());
  }
  return result;
}

bool containsFace(const TopoDS_Shape& owner, const TopoDS_Shape& candidate) {
  const auto faces = canonicalFaces(owner);
  return std::any_of(faces.begin(), faces.end(),
                     [&](const auto& face) { return face.IsSame(candidate); });
}

const solidar::HistoryFact* factFor(
    const solidar::ShapeHistoryArtifact& artifact,
    solidar::AuthoredTopologySourceKind kind,
    solidar::sketch::GeometryId geometryId =
        solidar::sketch::kInvalidGeometryId) {
  const auto found = std::find_if(
      artifact.facts.begin(), artifact.facts.end(), [&](const auto& fact) {
        return fact.authoredSource && fact.authoredSource->kind == kind &&
               fact.authoredSource->geometryId == geometryId;
      });
  return found == artifact.facts.end() ? nullptr : &*found;
}

void checkAuthoredFact(
    const solidar::ShapeHistoryArtifact& artifact,
    solidar::FeatureId producerFeatureId,
    solidar::AuthoredTopologySourceKind kind,
    solidar::sketch::GeometryId geometryId,
    const TopoDS_Shape& expectedOutput) {
  const auto* fact = factFor(artifact, kind, geometryId);
  CHECK(fact);
  CHECK(fact->kind == solidar::ShapeHistoryFactKind::Seed);
  CHECK(fact->authoredSource);
  const solidar::AuthoredTopologyToken expectedToken{kind, kSketchId,
                                                      geometryId};
  CHECK(*fact->authoredSource == expectedToken);
  CHECK(!fact->source);
  CHECK(!fact->sourceSubshape);
  CHECK(fact->outputProvenance);
  std::string error;
  const auto expectedKey = solidar::projectAuthoredTopologyToken(
      expectedToken, producerFeatureId, &error);
  CHECK(expectedKey);
  CHECK(error.empty());
  CHECK(*fact->outputProvenance == *expectedKey);
  CHECK(fact->outputSubshape);
  CHECK(fact->outputSubshape->IsSame(expectedOutput));
}

std::set<std::string> localNames(
    const solidar::TopologyLineageBuilder& builder) {
  std::string error;
  const auto lineage = builder.build(&error);
  CHECK(lineage);
  CHECK(error.empty());
  std::set<std::string> result;
  for (const auto& binding : lineage->bindings())
    result.insert(binding.provenance.localName);
  return result;
}

void checkAuthoredContract(const solidar::TopologyLineageBuilder& builder,
                           std::size_t expectedFaces) {
  CHECK(builder.artifact().coverage ==
        solidar::TopologyLineageCoverage::CompleteFaces);
  CHECK(builder.artifact().facts.size() == expectedFaces);
  CHECK(canonicalFaces(*builder.artifact().outputShape).size() ==
        expectedFaces);
  std::string error;
  CHECK(solidar::validateShapeHistoryArtifact(builder.artifact(), &error));
  CHECK(error.empty());
  const auto lineage = builder.build(&error);
  CHECK(lineage);
  CHECK(error.empty());
  for (const auto& fact : builder.artifact().facts) {
    CHECK(fact.kind == solidar::ShapeHistoryFactKind::Seed);
    CHECK(fact.authoredSource);
    CHECK(!fact.source);
    CHECK(!fact.sourceSubshape);
    CHECK(fact.outputProvenance);
    CHECK(fact.outputProvenance->operation ==
          solidar::TopologyProvenanceOperation::Seed);
    CHECK(fact.outputProvenance->origins.empty());
    CHECK(fact.outputSubshape);
  }
  for (const auto& binding : lineage->bindings()) {
    CHECK(binding.provenance.featureId == kFeatureId ||
          binding.provenance.featureId > kFeatureId);
    CHECK(binding.provenance.kind == solidar::TopologyKind::Face);
    CHECK(binding.provenance.operation ==
          solidar::TopologyProvenanceOperation::Seed);
    CHECK(binding.provenance.origins.empty());
    CHECK(binding.provenance.role == solidar::TopologySemanticRole::Cap ||
          binding.provenance.role == solidar::TopologySemanticRole::Side);
  }
}

class ScriptedRevol final : public BRepPrimAPI_MakeRevol {
 public:
  enum class Script { Real, OneCap, DuplicateCaps, EmptyGenerated,
                      DuplicateGenerated, MultipleGenerated,
                      NonFaceGenerated, ForeignGenerated };

  ScriptedRevol(const Profile& profile, double angle)
      : BRepPrimAPI_MakeRevol(*profile.face, kYAxis, angle, false),
        profile_(profile) {
    Build();
    for (const auto& seed : profile_.seeds) {
      std::vector<TopoDS_Shape> values;
      const auto& generated = BRepPrimAPI_MakeRevol::Generated(*seed.edge);
      for (NCollection_List<TopoDS_Shape>::Iterator iterator(generated);
           iterator.More(); iterator.Next())
        values.push_back(iterator.Value());
      realGenerated_.push_back(std::move(values));
    }
    realFirst_ = BRepPrimAPI_MakeRevol::FirstShape();
    realLast_ = BRepPrimAPI_MakeRevol::LastShape();
  }

  void setScript(Script script) { script_ = script; }
  void setEmptyGeneratedGeometry(solidar::sketch::GeometryId geometryId) {
    script_ = Script::EmptyGenerated;
    emptyGeneratedGeometry_ = geometryId;
  }
  [[nodiscard]] std::size_t generatedCallCount() const noexcept {
    return generatedCallCount_;
  }
  void invalidate() { NotDone(); }
  void replaceOutput(const TopoDS_Shape& shape) {
    myShape = shape;
    Done();
  }

  TopoDS_Shape FirstShape() override { return realFirst_; }

  TopoDS_Shape LastShape() override {
    if (script_ == Script::OneCap) {
      return foreignFace();
    }
    if (script_ == Script::DuplicateCaps) return realFirst_;
    return realLast_;
  }

  const NCollection_List<TopoDS_Shape>& Generated(
      const TopoDS_Shape& source) override {
    ++generatedCallCount_;
    scratch_.Clear();
    std::size_t index = profile_.seeds.size();
    for (std::size_t candidate = 0; candidate < profile_.seeds.size();
         ++candidate) {
      if (profile_.seeds[candidate].edge->IsSame(source)) {
        index = candidate;
        break;
      }
    }
    if (index == profile_.seeds.size()) return scratch_;
    if (script_ == Script::EmptyGenerated &&
        (!emptyGeneratedGeometry_ ||
         profile_.seeds[index].geometryId == *emptyGeneratedGeometry_))
      return scratch_;
    if (script_ == Script::DuplicateGenerated && index == 1) {
      if (!realGenerated_.front().empty())
        scratch_.Append(realGenerated_.front().front());
      return scratch_;
    }
    if (script_ == Script::MultipleGenerated && index == 0) {
      if (!realGenerated_[0].empty())
        scratch_.Append(realGenerated_[0].front());
      if (!realGenerated_[1].empty())
        scratch_.Append(realGenerated_[1].front());
      return scratch_;
    }
    if (script_ == Script::NonFaceGenerated && index == 0) {
      scratch_.Append(*profile_.seeds[0].edge);
      return scratch_;
    }
    if (script_ == Script::ForeignGenerated && index == 0) {
      scratch_.Append(foreignFace());
      return scratch_;
    }
    for (const auto& item : realGenerated_[index]) scratch_.Append(item);
    return scratch_;
  }

 private:
  static TopoDS_Face foreignFace() {
    return BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(100.0, 0.0, 0.0),
                                          gp_Dir(1.0, 0.0, 0.0)),
                                   0.0, 1.0, 0.0, 1.0)
        .Face();
  }

  Profile profile_;
  Script script_{Script::Real};
  std::optional<solidar::sketch::GeometryId> emptyGeneratedGeometry_;
  std::size_t generatedCallCount_{0};
  TopoDS_Shape realFirst_;
  TopoDS_Shape realLast_;
  std::vector<std::vector<TopoDS_Shape>> realGenerated_;
  NCollection_List<TopoDS_Shape> scratch_;
};

void expectFailure(BRepPrimAPI_MakeRevol& maker,
                   solidar::FeatureId producer,
                   solidar::SketchId sketchId,
                   std::shared_ptr<const TopoDS_Face> profileFace,
                   std::span<const solidar::RevolveProfileEdgeSeed> seeds,
                   solidar::RevolveTopologyHistoryFailureCode expected) {
  solidar::RevolveTopologyHistoryFailure failure;
  const auto captured = solidar::captureRevolveTopologyHistory(
      maker, producer, sketchId, std::move(profileFace), seeds, &failure);
  CHECK(!captured);
  CHECK(failure.code == expected);
  CHECK(!failure.detail.empty());
}

}  // namespace

int main() {
  const auto profile = rectangleProfile();

  // A real partial revolution publishes two authored caps and one stable side
  // for every profile edge. All shapes survive maker destruction.
  std::optional<solidar::TopologyLineageBuilder> partial;
  std::vector<TopoDS_Shape> expectedSides;
  TopoDS_Shape expectedStart;
  TopoDS_Shape expectedEnd;
  {
    BRepPrimAPI_MakeRevol maker(*profile.face, kYAxis,
                                2.0 * std::numbers::pi / 3.0, false);
    maker.Build();
    expectedStart = maker.FirstShape();
    expectedEnd = maker.LastShape();
    for (const auto& seed : profile.seeds) {
      const auto& generated = maker.Generated(*seed.edge);
      CHECK(generated.Size() == 1);
      expectedSides.push_back(generated.First());
    }
    solidar::RevolveTopologyHistoryFailure failure;
    partial = solidar::captureRevolveTopologyHistory(
        maker, kFeatureId, kSketchId, profile.face, profile.seeds, &failure);
    CHECK(partial);
    CHECK(failure.code ==
          solidar::RevolveTopologyHistoryFailureCode::None);
    CHECK(failure.detail.empty());
  }
  CHECK(partial);
  checkAuthoredContract(*partial, 6);
  const auto* start = factFor(partial->artifact(),
                              solidar::AuthoredTopologySourceKind::ProfileStartCap);
  const auto* end = factFor(partial->artifact(),
                            solidar::AuthoredTopologySourceKind::ProfileEndCap);
  CHECK(start);
  CHECK(end);
  CHECK(start->outputSubshape->IsSame(expectedStart));
  CHECK(end->outputSubshape->IsSame(expectedEnd));
  for (std::size_t index = 0; index < profile.seeds.size(); ++index) {
    const auto* side = factFor(
        partial->artifact(), solidar::AuthoredTopologySourceKind::GeometrySide,
        profile.seeds[index].geometryId);
    CHECK(side);
    CHECK(side->outputSubshape->IsSame(expectedSides[index]));
  }

  // Full revolution has no contained caps; generated sides alone provide
  // complete coverage. The cap identity lifecycle is explicit.
  std::optional<solidar::TopologyLineageBuilder> full;
  {
    BRepPrimAPI_MakeRevol maker(*profile.face, kYAxis, false);
    maker.Build();
    full = solidar::captureRevolveTopologyHistory(
        maker, kFeatureId + 1, kSketchId, profile.face, profile.seeds);
  }
  CHECK(full);
  checkAuthoredContract(*full, 4);
  CHECK(!factFor(full->artifact(),
                 solidar::AuthoredTopologySourceKind::ProfileStartCap));
  CHECK(!factFor(full->artifact(),
                 solidar::AuthoredTopologySourceKind::ProfileEndCap));

  // OCCT 8 may omit Generated(edge) for a perpendicular edge in a closed
  // revolution even though its internal sweep map owns the output face. Keep
  // an explicit witness for the full-only recovery and prove exact key-to-face
  // identity with reversed seed order and one reversed edge orientation.
  solidar::sketch::GeometryId recoveredGeometryId =
      solidar::sketch::kInvalidGeometryId;
  TopoDS_Shape recoveredFace;
  {
    auto reordered = profile.seeds;
    std::reverse(reordered.begin(), reordered.end());
    reordered.front().edge = std::make_shared<TopoDS_Edge>(
        TopoDS::Edge(reordered.front().edge->Reversed()));
    BRepPrimAPI_MakeRevol maker(*profile.face, kYAxis, false);
    maker.Build();
    std::vector<std::pair<solidar::sketch::GeometryId, TopoDS_Shape>>
        expectedSides;
    for (const auto& seed : reordered) {
      const auto& generated = maker.Generated(*seed.edge);
      CHECK(generated.Size() <= 1);
      if (generated.Size() == 1) {
        expectedSides.emplace_back(seed.geometryId, generated.First());
        continue;
      }
      const TopoDS_Shape closedSide =
          const_cast<BRepSweep_Revol&>(maker.Revol()).Shape(*seed.edge);
      CHECK(!closedSide.IsNull());
      CHECK(closedSide.ShapeType() == TopAbs_FACE);
      CHECK(containsFace(maker.Shape(), closedSide));
      expectedSides.emplace_back(seed.geometryId, closedSide);
      if (recoveredGeometryId == solidar::sketch::kInvalidGeometryId) {
        recoveredGeometryId = seed.geometryId;
        recoveredFace = closedSide;
      }
    }
    CHECK(recoveredGeometryId != solidar::sketch::kInvalidGeometryId);
    CHECK(!recoveredFace.IsNull());
    const auto producerFeatureId = kFeatureId + 30;
    const auto captured = solidar::captureRevolveTopologyHistory(
        maker, producerFeatureId, kSketchId, profile.face, reordered);
    CHECK(captured);
    CHECK(captured->artifact().facts.size() == expectedSides.size());
    CHECK(!factFor(captured->artifact(),
                   solidar::AuthoredTopologySourceKind::ProfileStartCap));
    CHECK(!factFor(captured->artifact(),
                   solidar::AuthoredTopologySourceKind::ProfileEndCap));
    for (const auto& [geometryId, expectedSide] : expectedSides)
      checkAuthoredFact(
          captured->artifact(), producerFeatureId,
          solidar::AuthoredTopologySourceKind::GeometrySide, geometryId,
          expectedSide);
    checkAuthoredFact(
        captured->artifact(), producerFeatureId,
        solidar::AuthoredTopologySourceKind::GeometrySide,
        recoveredGeometryId, recoveredFace);
  }
  {
    ScriptedRevol maker(profile, std::numbers::pi / 2.0);
    maker.setEmptyGeneratedGeometry(recoveredGeometryId);
    expectFailure(
        maker, kFeatureId + 31, kSketchId, profile.face, profile.seeds,
        solidar::RevolveTopologyHistoryFailureCode::IncompleteOutputCoverage);
  }

  // Copy=true breaks the exact input identity contract and is rejected before
  // querying per-edge history.
  {
    BRepPrimAPI_MakeRevol maker(*profile.face, kYAxis,
                                std::numbers::pi / 2.0, true);
    maker.Build();
    expectFailure(
        maker, kFeatureId + 32, kSketchId, profile.face, profile.seeds,
        solidar::RevolveTopologyHistoryFailureCode::MakerInputMismatch);
  }

  // Direction, near-full angle, seed traversal order and edge orientation do
  // not choose semantic identities. Classification is by result membership.
  const std::array<double, 2> directionalAngles{
      -std::numbers::pi / 2.0,
      2.0 * std::numbers::pi - 1.0e-7};
  for (std::size_t caseIndex = 0; caseIndex < directionalAngles.size();
       ++caseIndex) {
    const double angle = directionalAngles[caseIndex];
    auto reordered = profile.seeds;
    std::reverse(reordered.begin(), reordered.end());
    reordered.front().edge = std::make_shared<TopoDS_Edge>(
        TopoDS::Edge(reordered.front().edge->Reversed()));
    BRepPrimAPI_MakeRevol maker(*profile.face, kYAxis, angle, false);
    maker.Build();
    const TopoDS_Shape caseStart = maker.FirstShape();
    const TopoDS_Shape caseEnd = maker.LastShape();
    std::vector<std::pair<solidar::sketch::GeometryId, TopoDS_Shape>>
        caseSides;
    for (const auto& seed : reordered) {
      const auto& generated = maker.Generated(*seed.edge);
      CHECK(generated.Size() == 1);
      caseSides.emplace_back(seed.geometryId, generated.First());
    }
    const auto producerFeatureId = kFeatureId + 20 + caseIndex;
    const auto captured = solidar::captureRevolveTopologyHistory(
        maker, producerFeatureId, kSketchId, profile.face, reordered);
    CHECK(captured);
    CHECK(captured->artifact().facts.size() == 6);
    checkAuthoredFact(
        captured->artifact(), producerFeatureId,
        solidar::AuthoredTopologySourceKind::ProfileStartCap,
        solidar::sketch::kInvalidGeometryId, caseStart);
    checkAuthoredFact(
        captured->artifact(), producerFeatureId,
        solidar::AuthoredTopologySourceKind::ProfileEndCap,
        solidar::sketch::kInvalidGeometryId, caseEnd);
    for (const auto& [geometryId, expectedSide] : caseSides)
      checkAuthoredFact(
          captured->artifact(), producerFeatureId,
          solidar::AuthoredTopologySourceKind::GeometrySide, geometryId,
          expectedSide);
  }

  // Dimension and angle edits preserve authored local names for the same
  // SketchId/GeometryIds even though every OCCT side is rebuilt.
  const auto editedProfile = rectangleProfile(3.0, 11.0, 1.0, 8.0);
  BRepPrimAPI_MakeRevol editedMaker(*editedProfile.face, kYAxis,
                                    std::numbers::pi / 3.0, false);
  editedMaker.Build();
  const auto edited = solidar::captureRevolveTopologyHistory(
      editedMaker, kFeatureId, kSketchId, editedProfile.face,
      editedProfile.seeds);
  CHECK(edited);
  CHECK(localNames(*edited) == localNames(*partial));

  // An edge on the axis legitimately generates no face. The other sides and
  // the two caps still form CompleteFaces coverage.
  const auto axisBound = rectangleProfile(0.0, 5.0, 1.0, 6.0);
  BRepPrimAPI_MakeRevol axisMaker(*axisBound.face, kYAxis,
                                  std::numbers::pi / 2.0, false);
  axisMaker.Build();
  const auto axisCaptured = solidar::captureRevolveTopologyHistory(
      axisMaker, kFeatureId + 3, kSketchId, axisBound.face, axisBound.seeds);
  CHECK(axisCaptured);
  CHECK(axisCaptured->artifact().facts.size() == 5);
  CHECK(!factFor(axisCaptured->artifact(),
                 solidar::AuthoredTopologySourceKind::GeometrySide, 104));
  BRepPrimAPI_MakeRevol axisFullMaker(*axisBound.face, kYAxis, false);
  axisFullMaker.Build();
  const auto axisFullCaptured = solidar::captureRevolveTopologyHistory(
      axisFullMaker, kFeatureId + 4, kSketchId, axisBound.face,
      axisBound.seeds);
  CHECK(axisFullCaptured);
  CHECK(axisFullCaptured->artifact().facts.size() == 3);
  CHECK(!factFor(axisFullCaptured->artifact(),
                 solidar::AuthoredTopologySourceKind::GeometrySide, 104));

  // A single closed curved boundary follows the same authored contract.
  // This guards the non-polygon path used by circles in sketch profiles.
  {
    const auto circle = circleProfile();
    BRepPrimAPI_MakeRevol partialMaker(*circle.face, kYAxis,
                                       std::numbers::pi / 2.0, false);
    partialMaker.Build();
    const auto circlePartial = solidar::captureRevolveTopologyHistory(
        partialMaker, kFeatureId + 5, kSketchId, circle.face, circle.seeds);
    CHECK(circlePartial);
    CHECK(circlePartial->artifact().facts.size() == 3);
    CHECK(factFor(circlePartial->artifact(),
                  solidar::AuthoredTopologySourceKind::GeometrySide, 201));

    BRepPrimAPI_MakeRevol fullMaker(*circle.face, kYAxis, false);
    fullMaker.Build();
    const auto circleFull = solidar::captureRevolveTopologyHistory(
        fullMaker, kFeatureId + 6, kSketchId, circle.face, circle.seeds);
    CHECK(circleFull);
    CHECK(circleFull->artifact().facts.size() == 1);
    CHECK(factFor(circleFull->artifact(),
                  solidar::AuthoredTopologySourceKind::GeometrySide, 201));
  }

  // Invalid owner/profile evidence is rejected before unsafe OCCT history
  // queries and no partial builder escapes.
  {
    BRepPrimAPI_MakeRevol maker(*profile.face, kYAxis,
                                std::numbers::pi / 2.0, false);
    maker.Build();
    expectFailure(maker, solidar::kInvalidFeatureId, kSketchId, profile.face,
                  profile.seeds,
                  solidar::RevolveTopologyHistoryFailureCode::InvalidProducer);
    expectFailure(maker, kFeatureId, solidar::kInvalidSketchId, profile.face,
                  profile.seeds,
                  solidar::RevolveTopologyHistoryFailureCode::InvalidSketch);
    expectFailure(maker, kFeatureId, kSketchId, {}, profile.seeds,
                  solidar::RevolveTopologyHistoryFailureCode::InvalidProfile);

    auto missing = profile.seeds;
    missing.pop_back();
    expectFailure(maker, kFeatureId, kSketchId, profile.face, missing,
                  solidar::RevolveTopologyHistoryFailureCode::MissingProfileSeed);
    auto invalid = profile.seeds;
    invalid.front().geometryId = solidar::sketch::kInvalidGeometryId;
    expectFailure(maker, kFeatureId, kSketchId, profile.face, invalid,
                  solidar::RevolveTopologyHistoryFailureCode::InvalidProfileSeed);
    auto duplicateId = profile.seeds;
    duplicateId[1].geometryId = duplicateId[0].geometryId;
    expectFailure(maker, kFeatureId, kSketchId, profile.face, duplicateId,
                  solidar::RevolveTopologyHistoryFailureCode::AmbiguousProfileSeed);
    auto duplicateEdge = profile.seeds;
    duplicateEdge[1].edge = duplicateEdge[0].edge;
    expectFailure(maker, kFeatureId, kSketchId, profile.face, duplicateEdge,
                  solidar::RevolveTopologyHistoryFailureCode::AmbiguousProfileSeed);
    auto foreign = profile.seeds;
    foreign[0].edge = rectangleProfile(20.0, 25.0).edges[0];
    expectFailure(maker, kFeatureId, kSketchId, profile.face, foreign,
                  solidar::RevolveTopologyHistoryFailureCode::ForeignProfileSeed);

    std::vector<solidar::RevolveProfileEdgeSeed> excessive(4097);
    expectFailure(maker, kFeatureId, kSketchId, profile.face, excessive,
                  solidar::RevolveTopologyHistoryFailureCode::ResourceLimit);
  }

  // The no-argument first-shape probe rejects a maker built from another face
  // without ever calling Generated() on a foreign subshape. The spy override
  // makes the call-order safety property executable rather than inferential.
  {
    const auto other = rectangleProfile(20.0, 25.0);
    ScriptedRevol maker(other, std::numbers::pi / 2.0);
    CHECK(maker.generatedCallCount() == 0);
    expectFailure(maker, kFeatureId, kSketchId, profile.face, profile.seeds,
                  solidar::RevolveTopologyHistoryFailureCode::MakerInputMismatch);
    CHECK(maker.generatedCallCount() == 0);
  }

  // A narrow scripted history seam exercises closed failure states that real
  // OCCT builders do not intentionally produce.
  {
    ScriptedRevol maker(profile, std::numbers::pi / 2.0);
    maker.invalidate();
    expectFailure(maker, kFeatureId, kSketchId, profile.face, profile.seeds,
                  solidar::RevolveTopologyHistoryFailureCode::InvalidMaker);
  }
  for (const auto [script, expected] : {
           std::pair{ScriptedRevol::Script::OneCap,
                     solidar::RevolveTopologyHistoryFailureCode::InvalidCapHistory},
           std::pair{ScriptedRevol::Script::DuplicateCaps,
                     solidar::RevolveTopologyHistoryFailureCode::InvalidCapHistory},
           std::pair{ScriptedRevol::Script::EmptyGenerated,
                     solidar::RevolveTopologyHistoryFailureCode::IncompleteOutputCoverage},
           std::pair{ScriptedRevol::Script::DuplicateGenerated,
                     solidar::RevolveTopologyHistoryFailureCode::IdentityCollision},
           std::pair{ScriptedRevol::Script::MultipleGenerated,
                     solidar::RevolveTopologyHistoryFailureCode::InvalidSideHistory},
           std::pair{ScriptedRevol::Script::NonFaceGenerated,
                     solidar::RevolveTopologyHistoryFailureCode::InvalidSideHistory},
           std::pair{ScriptedRevol::Script::ForeignGenerated,
                     solidar::RevolveTopologyHistoryFailureCode::InvalidSideHistory},
       }) {
    ScriptedRevol maker(profile, std::numbers::pi / 2.0);
    maker.setScript(script);
    expectFailure(maker, kFeatureId, kSketchId, profile.face, profile.seeds,
                  expected);
  }

  // A done maker whose result is not a valid one-solid B-Rep is rejected.
  {
    ScriptedRevol maker(profile, std::numbers::pi / 2.0);
    TopoDS_Compound empty;
    BRep_Builder builder;
    builder.MakeCompound(empty);
    maker.replaceOutput(empty);
    expectFailure(maker, kFeatureId, kSketchId, profile.face, profile.seeds,
                  solidar::RevolveTopologyHistoryFailureCode::InvalidOutput);
  }

  return EXIT_SUCCESS;
}
