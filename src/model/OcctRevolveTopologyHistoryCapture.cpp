#include "model/OcctRevolveTopologyHistoryCapture.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <NCollection_List.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <utility>
#include <vector>

#include "model/GeometryOperation.h"

namespace solidar {
namespace {

constexpr std::size_t kMaximumProfileEdges = 4096;

void setFailure(RevolveTopologyHistoryFailure* failure,
                RevolveTopologyHistoryFailureCode code,
                std::string detail) {
  if (!failure) return;
  failure->code = code;
  failure->detail = std::move(detail);
}

bool contains(const TopoDS_Shape& owner, const TopoDS_Shape& candidate,
              TopAbs_ShapeEnum kind) {
  for (TopExp_Explorer explorer(owner, kind); explorer.More();
       explorer.Next()) {
    if (explorer.Current().IsSame(candidate)) return true;
  }
  return false;
}

std::vector<TopoDS_Shape> canonicalSubshapes(const TopoDS_Shape& shape,
                                              TopAbs_ShapeEnum kind) {
  std::vector<TopoDS_Shape> result;
  for (TopExp_Explorer explorer(shape, kind); explorer.More();
       explorer.Next()) {
    const TopoDS_Shape candidate = explorer.Current();
    if (std::none_of(result.begin(), result.end(), [&](const auto& existing) {
          return existing.IsSame(candidate);
        }))
      result.push_back(candidate);
  }
  return result;
}

std::optional<TopologyLineageBuilder> captureImpl(
    BRepPrimAPI_MakeRevol& maker, FeatureId producerFeatureId,
    SketchId sketchId, std::shared_ptr<const TopoDS_Face> profileFace,
    std::span<const RevolveProfileEdgeSeed> profileEdges,
    RevolveTopologyHistoryFailure* failure) {
  const auto fail = [&](RevolveTopologyHistoryFailureCode code,
                        std::string detail)
      -> std::optional<TopologyLineageBuilder> {
    setFailure(failure, code, std::move(detail));
    return std::nullopt;
  };

  if (producerFeatureId == kInvalidFeatureId)
    return fail(RevolveTopologyHistoryFailureCode::InvalidProducer,
                "Revolve producer identity is invalid");
  if (sketchId == kInvalidSketchId)
    return fail(RevolveTopologyHistoryFailureCode::InvalidSketch,
                "Revolve sketch identity is invalid");
  if (!profileFace || profileFace->IsNull() ||
      profileFace->ShapeType() != TopAbs_FACE)
    return fail(RevolveTopologyHistoryFailureCode::InvalidProfile,
                "Revolve profile face is invalid");
  if (profileEdges.size() > kMaximumProfileEdges)
    return fail(RevolveTopologyHistoryFailureCode::ResourceLimit,
                "Revolve profile exceeds the supported edge count");

  const auto canonicalEdges =
      canonicalSubshapes(*profileFace, TopAbs_EDGE);
  if (canonicalEdges.empty())
    return fail(RevolveTopologyHistoryFailureCode::InvalidProfile,
                "Revolve profile has no canonical edges");
  if (canonicalEdges.size() != profileEdges.size())
    return fail(RevolveTopologyHistoryFailureCode::MissingProfileSeed,
                "Revolve profile edge seed coverage is incomplete");

  for (std::size_t index = 0; index < profileEdges.size(); ++index) {
    const auto& seed = profileEdges[index];
    if (seed.geometryId == sketch::kInvalidGeometryId || !seed.edge ||
        seed.edge->IsNull() || seed.edge->ShapeType() != TopAbs_EDGE)
      return fail(RevolveTopologyHistoryFailureCode::InvalidProfileSeed,
                  "Revolve profile edge seed is invalid");
    if (!contains(*profileFace, *seed.edge, TopAbs_EDGE))
      return fail(RevolveTopologyHistoryFailureCode::ForeignProfileSeed,
                  "Revolve profile edge seed is foreign");
    for (std::size_t other = 0; other < index; ++other) {
      if (seed.geometryId == profileEdges[other].geometryId ||
          seed.edge->IsSame(*profileEdges[other].edge))
        return fail(RevolveTopologyHistoryFailureCode::AmbiguousProfileSeed,
                    "Revolve profile edge seed identity is duplicated");
    }
  }
  for (const auto& edge : canonicalEdges) {
    if (std::count_if(profileEdges.begin(), profileEdges.end(),
                      [&](const auto& seed) {
                        return seed.edge && seed.edge->IsSame(edge);
                      }) != 1)
      return fail(RevolveTopologyHistoryFailureCode::MissingProfileSeed,
                  "A canonical revolve profile edge lacks one exact seed");
  }

  if (!maker.IsDone())
    return fail(RevolveTopologyHistoryFailureCode::InvalidMaker,
                "Revolve builder is not done");

  // Partial sweeps expose the exact Copy=false input through FirstShape(). A
  // closed full sweep may report no first shape, so use the sweep's read-only
  // IsUsed membership predicate as that narrow fallback. Unlike
  // Generated(foreign), IsUsed is a non-producing lookup and is safe for
  // rejecting a mismatched input.
  const TopoDS_Shape first = maker.FirstShape();
  const TopoDS_Shape last = maker.LastShape();
  const bool firstMatchesInput =
      (!first.IsNull() && first.ShapeType() == TopAbs_FACE &&
       first.IsSame(*profileFace)) ||
      (first.IsNull() && maker.Revol().IsUsed(*profileFace));
  if (!firstMatchesInput)
    return fail(RevolveTopologyHistoryFailureCode::MakerInputMismatch,
                "Revolve maker input does not match the supplied profile");

  const auto outputShape = std::make_shared<TopoDS_Shape>(maker.Shape());
  if (outputShape->IsNull() || !BRepCheck_Analyzer(*outputShape).IsValid())
    return fail(RevolveTopologyHistoryFailureCode::InvalidOutput,
                "Revolve output B-Rep is invalid");
  const auto outputSolids = canonicalSubshapes(*outputShape, TopAbs_SOLID);
  if (outputSolids.size() != 1)
    return fail(RevolveTopologyHistoryFailureCode::InvalidOutput,
                "Revolve output must contain exactly one solid");

  const bool firstContained = contains(*outputShape, first, TopAbs_FACE);
  const bool lastIsFace = !last.IsNull() && last.ShapeType() == TopAbs_FACE;
  const bool lastContained =
      lastIsFace && contains(*outputShape, last, TopAbs_FACE);
  const bool partial = firstContained && lastContained &&
                       !first.IsSame(last);
  const bool full = !firstContained && !lastContained;
  if (!partial && !full)
    return fail(RevolveTopologyHistoryFailureCode::InvalidCapHistory,
                "Revolve cap history is neither a partial nor full sweep");

  // Copy all scratch-list values before any subsequent history query.
  std::vector<std::vector<TopoDS_Shape>> generatedBySeed;
  generatedBySeed.reserve(profileEdges.size());
  for (const auto& seed : profileEdges) {
    std::vector<TopoDS_Shape> copied;
    const auto& generated = maker.Generated(*seed.edge);
    copied.reserve(static_cast<std::size_t>(generated.Size()));
    for (NCollection_List<TopoDS_Shape>::Iterator iterator(generated);
         iterator.More(); iterator.Next())
      copied.push_back(iterator.Value());
    // OCCT 8 closes a full revolution by marking some perpendicular profile
    // edges as not "used", so Generated(edge) is empty even though the sweep
    // owns a face for that exact edge. Recover only that full-sweep case from
    // the already validated internal revolution map. Axis edges still yield
    // no contained face and remain a legitimate zero-result history entry.
    if (full && copied.empty()) {
      const TopoDS_Shape closedSide =
          const_cast<BRepSweep_Revol&>(maker.Revol()).Shape(*seed.edge);
      if (!closedSide.IsNull() && closedSide.ShapeType() == TopAbs_FACE &&
          contains(*outputShape, closedSide, TopAbs_FACE))
        copied.push_back(closedSide);
    }
    generatedBySeed.push_back(std::move(copied));
  }

  ShapeHistoryArtifact artifact;
  artifact.producerFeatureId = producerFeatureId;
  artifact.outputShape = outputShape;
  artifact.coverage = TopologyLineageCoverage::CompleteFaces;
  std::vector<std::shared_ptr<const TopoDS_Shape>> namedFaces;
  namedFaces.reserve(profileEdges.size() + (partial ? 2U : 0U));
  const auto appendSeed = [&](const TopoDS_Shape& output,
                              AuthoredTopologyToken token) -> bool {
    if (output.IsNull() || output.ShapeType() != TopAbs_FACE ||
        !contains(*outputShape, output, TopAbs_FACE) ||
        std::any_of(namedFaces.begin(), namedFaces.end(),
                    [&](const auto& item) { return item->IsSame(output); }))
      return false;
    auto ownedOutput = std::make_shared<TopoDS_Shape>(output);
    namedFaces.push_back(ownedOutput);
    artifact.facts.push_back({ShapeHistoryFactKind::Seed, token,
                              std::nullopt, {}, std::nullopt,
                              std::move(ownedOutput)});
    return true;
  };

  if (partial) {
    if (!appendSeed(first,
                    {AuthoredTopologySourceKind::ProfileStartCap, sketchId,
                     sketch::kInvalidGeometryId}) ||
        !appendSeed(last,
                    {AuthoredTopologySourceKind::ProfileEndCap, sketchId,
                     sketch::kInvalidGeometryId}))
      return fail(RevolveTopologyHistoryFailureCode::IdentityCollision,
                  "Revolve cap output collides with another face");
  }

  for (std::size_t index = 0; index < profileEdges.size(); ++index) {
    const auto& generated = generatedBySeed[index];
    if (generated.size() > 1)
      return fail(RevolveTopologyHistoryFailureCode::InvalidSideHistory,
                  "Revolve edge history generated multiple results");
    if (generated.empty()) continue;
    const auto& side = generated.front();
    if (side.IsNull() || side.ShapeType() != TopAbs_FACE ||
        !contains(*outputShape, side, TopAbs_FACE))
      return fail(RevolveTopologyHistoryFailureCode::InvalidSideHistory,
                  "Revolve edge history did not generate one contained face");
    if (!appendSeed(side,
                    {AuthoredTopologySourceKind::GeometrySide, sketchId,
                     profileEdges[index].geometryId}))
      return fail(RevolveTopologyHistoryFailureCode::IdentityCollision,
                  "Revolve side output is not globally unique");
  }

  const auto outputFaces = canonicalSubshapes(*outputShape, TopAbs_FACE);
  if (outputFaces.size() != namedFaces.size())
    return fail(RevolveTopologyHistoryFailureCode::IncompleteOutputCoverage,
                "Revolve history does not cover every canonical output face");
  for (const auto& face : outputFaces) {
    if (std::count_if(namedFaces.begin(), namedFaces.end(),
                      [&](const auto& item) { return item->IsSame(face); }) != 1)
      return fail(RevolveTopologyHistoryFailureCode::IncompleteOutputCoverage,
                  "Revolve canonical face coverage is not bijective");
  }

  std::string diagnostic;
  auto builder =
      TopologyLineageBuilder::create(std::move(artifact), &diagnostic);
  if (!builder)
    return fail(RevolveTopologyHistoryFailureCode::HistoryBuildFailure,
                diagnostic.empty() ? "Revolve history is invalid"
                                   : std::move(diagnostic));
  return builder;
}

}  // namespace

std::optional<TopologyLineageBuilder> captureRevolveTopologyHistory(
    BRepPrimAPI_MakeRevol& maker, FeatureId producerFeatureId,
    SketchId sketchId, std::shared_ptr<const TopoDS_Face> profileFace,
    std::span<const RevolveProfileEdgeSeed> profileEdges,
    RevolveTopologyHistoryFailure* failure) {
  if (failure) *failure = {};
  std::optional<TopologyLineageBuilder> result;
  GeometryFailure geometryFailure;
  const bool completed = runGeometryOperation(
      [&] {
        result = captureImpl(maker, producerFeatureId, sketchId,
                             std::move(profileFace), profileEdges, failure);
      },
      &geometryFailure);
  if (!completed) {
    std::string detail =
        "Revolve topology capture raised a geometry exception";
    if (!geometryFailure.detail.empty())
      detail += ": " + geometryFailure.detail;
    setFailure(failure,
               RevolveTopologyHistoryFailureCode::GeometryException,
               std::move(detail));
    return std::nullopt;
  }
  return result;
}

}  // namespace solidar
