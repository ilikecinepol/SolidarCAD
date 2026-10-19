#include "model/OcctBooleanTopologyHistoryCapture.h"

#include <BOPAlgo_Operation.hxx>
#include <BRepAlgoAPI_BooleanOperation.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <NCollection_List.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <utility>

#include "model/GeometryOperation.h"

namespace solidar {
namespace {

constexpr std::size_t kMaximumLocalNameLength = 16384;

enum class RelationKind : std::uint8_t {
  Retained,
  Modified,
};

struct SourceFace {
  BooleanOperandRole role;
  TopologyProvenanceOrigin origin;
  std::shared_ptr<const TopoDS_Shape> face;
};

struct OutputRelation {
  std::size_t sourceIndex{};
  RelationKind kind{RelationKind::Modified};
};

struct OutputFace {
  std::shared_ptr<const TopoDS_Shape> face;
  std::vector<OutputRelation> relations;
};

bool originLess(const TopologyProvenanceOrigin& left,
                const TopologyProvenanceOrigin& right) {
  const auto occurrence = [](const TopologyProvenanceOrigin& origin) {
    return std::tuple{origin.occurrence.has_value(),
                      origin.occurrence ? origin.occurrence->featureId : 0,
                      origin.occurrence ? origin.occurrence->slot : 0};
  };
  const auto leftIdentity = std::tie(left.featureId, left.kind, left.operation,
                                     left.role, left.localName);
  const auto rightIdentity = std::tie(
      right.featureId, right.kind, right.operation, right.role,
      right.localName);
  if (leftIdentity != rightIdentity) return leftIdentity < rightIdentity;
  return occurrence(left) < occurrence(right);
}

void clearFailure(BooleanTopologyCaptureFailure* failure) {
  if (failure) *failure = {};
}

template <class Result = bool>
Result fail(BooleanTopologyCaptureFailure* failure,
            BooleanTopologyCaptureFailureCode code, std::string detail) {
  if (failure) {
    failure->code = code;
    failure->detail = std::move(detail);
  }
  return Result{};
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

bool containsFace(const TopoDS_Shape& owner, const TopoDS_Shape& face) {
  const auto faces = canonicalFaces(owner);
  return std::any_of(faces.begin(), faces.end(),
                     [&](const auto& item) { return item.IsSame(face); });
}

std::vector<TopoDS_Shape> copyList(
    const NCollection_List<TopoDS_Shape>& list) {
  std::vector<TopoDS_Shape> result;
  result.reserve(static_cast<std::size_t>(list.Size()));
  for (NCollection_List<TopoDS_Shape>::Iterator iterator(list);
       iterator.More(); iterator.Next())
    result.push_back(iterator.Value());
  return result;
}

bool sameShapeSet(const NCollection_List<TopoDS_Shape>& actual,
                  std::span<const TopologySourceSnapshot* const> expected) {
  if (actual.Size() != static_cast<int>(expected.size()))
    return false;
  std::vector<bool> matched(expected.size(), false);
  for (NCollection_List<TopoDS_Shape>::Iterator iterator(actual);
       iterator.More(); iterator.Next()) {
    std::size_t match = expected.size();
    for (std::size_t index = 0; index < expected.size(); ++index) {
      if (!matched[index] &&
          iterator.Value().IsSame(*expected[index]->shape)) {
        match = index;
        break;
      }
    }
    if (match == expected.size()) return false;
    matched[match] = true;
  }
  return std::all_of(matched.begin(), matched.end(),
                     [](bool value) { return value; });
}

bool canonicalOrigins(std::vector<TopologyProvenanceOrigin>& origins) {
  for (const auto& origin : origins)
    if (!origin.isValid()) return false;
  std::sort(origins.begin(), origins.end(), originLess);
  return std::adjacent_find(origins.begin(), origins.end()) == origins.end();
}

bool validOperandRole(BooleanOperandRole role) noexcept {
  switch (role) {
    case BooleanOperandRole::Object:
    case BooleanOperandRole::Tool:
      return true;
  }
  return false;
}

void appendField(std::string& result, std::string_view value) {
  result += std::to_string(value.size());
  result.push_back(':');
  result.append(value);
  result.push_back(';');
}

void appendOrigin(std::string& result, const TopologyProvenanceOrigin& origin,
                  std::optional<BooleanOperandRole> cutRole) {
  appendField(result, std::to_string(origin.featureId));
  appendField(result, std::to_string(static_cast<unsigned>(origin.kind)));
  appendField(result, std::to_string(static_cast<unsigned>(origin.operation)));
  appendField(result, std::to_string(static_cast<unsigned>(origin.role)));
  appendField(result, origin.localName);
  appendField(result, origin.occurrence ? "1" : "0");
  if (origin.occurrence) {
    appendField(result, std::to_string(origin.occurrence->featureId));
    appendField(result, std::to_string(origin.occurrence->slot));
  }
  if (cutRole)
    appendField(result, *cutRole == BooleanOperandRole::Object ? "object"
                                                               : "tool");
}

std::optional<std::string> makeLocalName(
    BOPAlgo_Operation operation, TopologyProvenanceOperation provenance,
    const std::vector<TopologyProvenanceOrigin>& origins,
    const std::vector<SourceFace>& sourceFaces,
    std::optional<std::string_view> branch,
    BooleanTopologyCaptureFailure* failure) {
  std::string name = "boolean-face:v1;";
  appendField(name, operation == BOPAlgo_CUT ? "cut" : "fuse");
  appendField(name, std::to_string(static_cast<unsigned>(provenance)));
  appendField(name, std::to_string(origins.size()));
  for (const auto& origin : origins) {
    std::optional<BooleanOperandRole> role;
    if (operation == BOPAlgo_CUT) {
      const auto found = std::find_if(
          sourceFaces.begin(), sourceFaces.end(), [&](const auto& source) {
            return source.origin == origin;
          });
      if (found == sourceFaces.end())
        return fail<std::optional<std::string>>(
            failure, BooleanTopologyCaptureFailureCode::InvalidHistory,
            "Boolean output origin is not one of the captured sources");
      role = found->role;
    }
    appendOrigin(name, origin, role);
  }
  if (branch) {
    appendField(name, "branch");
    appendField(name, *branch);
  }
  if (name.size() > kMaximumLocalNameLength)
    return fail<std::optional<std::string>>(
        failure, BooleanTopologyCaptureFailureCode::LocalNameTooLong,
        "Boolean face local name exceeds 16384 bytes");
  return name;
}

ShapeHistoryFactKind factKind(RelationKind kind) {
  switch (kind) {
    case RelationKind::Retained:
      return ShapeHistoryFactKind::Retained;
    case RelationKind::Modified:
      return ShapeHistoryFactKind::Modified;
  }
  return ShapeHistoryFactKind::Modified;
}

TopologyProvenanceOperation provenanceOperation(RelationKind kind) {
  switch (kind) {
    case RelationKind::Retained:
      return TopologyProvenanceOperation::Retained;
    case RelationKind::Modified:
      return TopologyProvenanceOperation::Modified;
  }
  return TopologyProvenanceOperation::Modified;
}

bool addRelation(std::vector<OutputFace>& outputs,
                 const TopoDS_Shape& candidate, std::size_t sourceIndex,
                 RelationKind kind, BooleanTopologyCaptureFailure* failure) {
  if (candidate.IsNull() || candidate.ShapeType() != TopAbs_FACE)
    return fail(failure,
                BooleanTopologyCaptureFailureCode::InvalidHistory,
                "Boolean face history contains an invalid face");
  auto output = std::find_if(outputs.begin(), outputs.end(), [&](auto& item) {
    return item.face->IsSame(candidate);
  });
  if (output == outputs.end())
    return fail(failure,
                BooleanTopologyCaptureFailureCode::InvalidHistory,
                "Boolean history face is not part of the final result");
  const auto existing = std::find_if(
      output->relations.begin(), output->relations.end(),
      [&](const auto& relation) { return relation.sourceIndex == sourceIndex; });
  if (existing == output->relations.end()) {
    output->relations.push_back({sourceIndex, kind});
    return true;
  }
  if (existing->kind == kind || kind == RelationKind::Retained) {
    existing->kind = kind;
    return true;
  }
  if (existing->kind == RelationKind::Retained) return true;
  return fail(failure, BooleanTopologyCaptureFailureCode::InvalidHistory,
              "Boolean history assigns conflicting relations to one face");
}

std::optional<BooleanTopologyCaptureResult> captureImpl(
    BRepAlgoAPI_BooleanOperation& maker, BOPAlgo_Operation expectedOperation,
    FeatureId producerFeatureId,
    std::span<const BooleanTopologySource> sources,
    std::span<const BooleanFaceBranchSeed> splitSeeds,
    BooleanTopologyCaptureOptions options,
    BooleanTopologyCaptureFailure* failure) {
  if (producerFeatureId == kInvalidFeatureId)
    return fail<std::optional<BooleanTopologyCaptureResult>>(
        failure, BooleanTopologyCaptureFailureCode::InvalidProducer,
        "Boolean history producer identity is invalid");
  if (sources.empty())
    return fail<std::optional<BooleanTopologyCaptureResult>>(
        failure, BooleanTopologyCaptureFailureCode::InvalidSource,
        "Boolean history has no sources");
  if (maker.Operation() != expectedOperation)
    return fail<std::optional<BooleanTopologyCaptureResult>>(
        failure, BooleanTopologyCaptureFailureCode::SourceMismatch,
        "Boolean maker operation does not match capture overload");
  if (!maker.IsDone())
    return fail<std::optional<BooleanTopologyCaptureResult>>(
        failure, BooleanTopologyCaptureFailureCode::BuilderNotDone,
        "Boolean maker is not done");
  if (!maker.HasHistory())
    return fail<std::optional<BooleanTopologyCaptureResult>>(
        failure, BooleanTopologyCaptureFailureCode::HistoryUnavailable,
        "Boolean maker history collection is disabled");

  std::vector<const TopologySourceSnapshot*> objects;
  std::vector<const TopologySourceSnapshot*> tools;
  for (std::size_t index = 0; index < sources.size(); ++index) {
    const auto& source = sources[index];
    if (!validOperandRole(source.role))
      return fail<std::optional<BooleanTopologyCaptureResult>>(
          failure, BooleanTopologyCaptureFailureCode::InvalidSource,
          "Boolean source operand role is invalid");
    for (std::size_t other = 0; other < index; ++other) {
      if (sources[other].snapshot.featureId == source.snapshot.featureId)
        return fail<std::optional<BooleanTopologyCaptureResult>>(
            failure,
            BooleanTopologyCaptureFailureCode::DuplicateSourceFeature,
            "Boolean source feature identity is duplicated");
    }
    if (source.snapshot.featureId == kInvalidFeatureId ||
        !source.snapshot.shape || source.snapshot.shape->IsNull() ||
        !source.snapshot.lineage)
      return fail<std::optional<BooleanTopologyCaptureResult>>(
          failure, BooleanTopologyCaptureFailureCode::InvalidSource,
          "Boolean source snapshot header is invalid");
    for (const auto& face : canonicalFaces(*source.snapshot.shape)) {
      const auto identityCount = static_cast<std::size_t>(std::count_if(
          source.snapshot.lineage->bindings().begin(),
          source.snapshot.lineage->bindings().end(),
          [&](const auto& binding) {
            return binding.provenance.kind == TopologyKind::Face &&
                   binding.subshape && !binding.subshape->IsNull() &&
                   binding.subshape->IsSame(face);
          }));
      if (identityCount == 0)
        return fail<std::optional<BooleanTopologyCaptureResult>>(
            failure,
            BooleanTopologyCaptureFailureCode::MissingSourceIdentity,
            "Boolean source face identity is missing");
      if (identityCount > 1)
        return fail<std::optional<BooleanTopologyCaptureResult>>(
            failure,
            BooleanTopologyCaptureFailureCode::DuplicateSourceIdentity,
            "Boolean source face has multiple identities");
    }
    if (!validateTopologySourceSnapshot(source.snapshot))
      return fail<std::optional<BooleanTopologyCaptureResult>>(
          failure, BooleanTopologyCaptureFailureCode::InvalidSource,
          "Boolean source snapshot is invalid or incomplete");
    if (source.snapshot.lineage->coverage() !=
        TopologyLineageCoverage::CompleteFaces)
      return fail<std::optional<BooleanTopologyCaptureResult>>(
          failure, BooleanTopologyCaptureFailureCode::InvalidSource,
          "Boolean source does not have complete face lineage");
    (source.role == BooleanOperandRole::Object ? objects : tools)
        .push_back(&source.snapshot);
  }
  if (objects.empty() || tools.empty() ||
      !sameShapeSet(maker.Arguments(), objects) ||
      !sameShapeSet(maker.Tools(), tools))
    return fail<std::optional<BooleanTopologyCaptureResult>>(
        failure, BooleanTopologyCaptureFailureCode::SourceMismatch,
        "Boolean sources do not exactly match maker Arguments/Tools");

  if (options.simplifyResult) {
    maker.SimplifyResult(options.unifyEdges, options.unifyFaces);
    if (!maker.IsDone())
      return fail<std::optional<BooleanTopologyCaptureResult>>(
          failure, BooleanTopologyCaptureFailureCode::BuilderNotDone,
          "Boolean maker failed while simplifying its result");
  }

  auto outputShape = std::make_shared<TopoDS_Shape>(maker.Shape());
  if (outputShape->IsNull() ||
      !BRepCheck_Analyzer(*outputShape).IsValid())
    return fail<std::optional<BooleanTopologyCaptureResult>>(
        failure, BooleanTopologyCaptureFailureCode::InvalidOutput,
        "Boolean output shape is null or invalid");
  std::vector<OutputFace> outputs;
  for (const auto& face : canonicalFaces(*outputShape))
    outputs.push_back({std::make_shared<TopoDS_Shape>(face), {}});
  if (outputs.empty())
    return fail<std::optional<BooleanTopologyCaptureResult>>(
        failure, BooleanTopologyCaptureFailureCode::InvalidOutput,
        "Boolean output has no faces");

  std::vector<SourceFace> sourceFaces;
  for (const auto& source : sources) {
    for (const auto& face : canonicalFaces(*source.snapshot.shape)) {
      const TopologyLineageBinding* binding = nullptr;
      for (const auto& candidate : source.snapshot.lineage->bindings()) {
        if (candidate.provenance.kind != TopologyKind::Face ||
            !candidate.subshape->IsSame(face))
          continue;
        if (binding)
          return fail<std::optional<BooleanTopologyCaptureResult>>(
              failure,
              BooleanTopologyCaptureFailureCode::DuplicateSourceIdentity,
              "Boolean source face has multiple identities");
        binding = &candidate;
      }
      if (!binding)
        return fail<std::optional<BooleanTopologyCaptureResult>>(
            failure,
            BooleanTopologyCaptureFailureCode::MissingSourceIdentity,
            "Boolean source face identity is missing");
      sourceFaces.push_back(
          {source.role, binding->provenance.immediateOrigin(),
           std::make_shared<TopoDS_Shape>(face)});
    }
  }

  std::vector<bool> deleted(sourceFaces.size(), false);
  for (std::size_t index = 0; index < sourceFaces.size(); ++index) {
    const auto& source = sourceFaces[index];
    if (containsFace(*outputShape, *source.face) &&
        !addRelation(outputs, *source.face, index, RelationKind::Retained,
                     failure))
      return std::nullopt;
    const auto modified = copyList(maker.Modified(*source.face));
    const auto generated = copyList(maker.Generated(*source.face));
    for (const auto& candidate : modified) {
      if (candidate.IsNull() || candidate.ShapeType() != TopAbs_FACE)
        return fail<std::optional<BooleanTopologyCaptureResult>>(
            failure, BooleanTopologyCaptureFailureCode::InvalidHistory,
            "Boolean Modified history contains a non-face result");
      if (!addRelation(outputs, candidate, index, RelationKind::Modified,
                       failure))
        return std::nullopt;
    }
    for (const auto& candidate : generated) {
      if (candidate.IsNull())
        return fail<std::optional<BooleanTopologyCaptureResult>>(
            failure, BooleanTopologyCaptureFailureCode::InvalidHistory,
            "Boolean Generated history contains a null shape");
      // OCCT boolean face history generates intersection edges/vertices, not
      // faces. Accepting a face here would freeze malformed/scripted history
      // as a persistent identity. Generic RT0 Generated algebra remains valid
      // for producers whose API explicitly defines face generation.
      if (candidate.ShapeType() == TopAbs_FACE)
        return fail<std::optional<BooleanTopologyCaptureResult>>(
            failure, BooleanTopologyCaptureFailureCode::InvalidHistory,
            "Boolean face source unexpectedly generated a face");
    }
    const bool hasLive = std::any_of(
        outputs.begin(), outputs.end(), [&](const auto& output) {
          return std::any_of(output.relations.begin(), output.relations.end(),
                             [&](const auto& relation) {
                               return relation.sourceIndex == index;
                             });
        });
    deleted[index] = maker.IsDeleted(*source.face);
    if (hasLive && deleted[index])
      return fail<std::optional<BooleanTopologyCaptureResult>>(
          failure, BooleanTopologyCaptureFailureCode::InvalidHistory,
          "Boolean source face is both live and deleted");
    if (!hasLive && !deleted[index])
      return fail<std::optional<BooleanTopologyCaptureResult>>(
          failure, BooleanTopologyCaptureFailureCode::MissingHistory,
          "Boolean source face has neither output nor deletion evidence");
  }

  for (const auto& output : outputs)
    if (output.relations.empty())
      return fail<std::optional<BooleanTopologyCaptureResult>>(
          failure, BooleanTopologyCaptureFailureCode::UncoveredOutput,
          "Boolean history does not cover every final face");

  std::vector<std::vector<TopologyProvenanceOrigin>> outputOrigins;
  outputOrigins.reserve(outputs.size());
  std::vector<std::size_t> sourceOutputCounts(sourceFaces.size(), 0);
  for (const auto& output : outputs) {
    std::vector<TopologyProvenanceOrigin> origins;
    for (const auto& relation : output.relations) {
      origins.push_back(sourceFaces[relation.sourceIndex].origin);
      ++sourceOutputCounts[relation.sourceIndex];
    }
    if (origins.size() > kMaximumTopologyImmediateOrigins)
      return fail<std::optional<BooleanTopologyCaptureResult>>(
          failure, BooleanTopologyCaptureFailureCode::ResourceLimit,
          "Boolean output exceeds the 256 immediate-origin limit");
    if (!canonicalOrigins(origins))
      return fail<std::optional<BooleanTopologyCaptureResult>>(
          failure, BooleanTopologyCaptureFailureCode::InvalidHistory,
          "Boolean output origins are invalid or duplicated");
    outputOrigins.push_back(std::move(origins));
  }
  std::vector<bool> ambiguous(outputs.size(), false);
  for (std::size_t index = 0; index < outputs.size(); ++index) {
    ambiguous[index] = std::any_of(
        outputs[index].relations.begin(), outputs[index].relations.end(),
        [&](const auto& relation) {
          return sourceOutputCounts[relation.sourceIndex] > 1;
        });
    for (std::size_t other = 0; other < index; ++other)
      if (outputOrigins[index] == outputOrigins[other])
        ambiguous[index] = ambiguous[other] = true;
  }

  std::vector<const BooleanFaceBranchSeed*> matchedSeeds(outputs.size(),
                                                          nullptr);
  std::vector<bool> consumedSeeds(splitSeeds.size(), false);
  for (std::size_t index = 0; index < splitSeeds.size(); ++index) {
    const auto& seed = splitSeeds[index];
    auto seedOrigins = seed.origins;
    if (!canonicalOrigins(seedOrigins) || seedOrigins != seed.origins ||
        seed.stableBranchId.empty() || !seed.outputFace ||
        seed.outputFace->IsNull() ||
        seed.outputFace->ShapeType() != TopAbs_FACE)
      return fail<std::optional<BooleanTopologyCaptureResult>>(
          failure, BooleanTopologyCaptureFailureCode::InvalidSplitSeed,
          "Boolean split seed is invalid or non-canonical");
    for (std::size_t other = 0; other < index; ++other)
      if (splitSeeds[other].stableBranchId == seed.stableBranchId)
        return fail<std::optional<BooleanTopologyCaptureResult>>(
            failure, BooleanTopologyCaptureFailureCode::SplitSeedCollision,
            "Boolean split seed branch identity is duplicated");
    for (std::size_t outputIndex = 0; outputIndex < outputs.size();
         ++outputIndex) {
      if (!outputs[outputIndex].face->IsSame(*seed.outputFace)) continue;
      if (!ambiguous[outputIndex] ||
          outputOrigins[outputIndex] != seed.origins ||
          matchedSeeds[outputIndex])
        return fail<std::optional<BooleanTopologyCaptureResult>>(
            failure, BooleanTopologyCaptureFailureCode::InvalidSplitSeed,
            "Boolean split seed does not uniquely match ambiguous history");
      matchedSeeds[outputIndex] = &seed;
      consumedSeeds[index] = true;
    }
    if (!consumedSeeds[index])
      return fail<std::optional<BooleanTopologyCaptureResult>>(
          failure, BooleanTopologyCaptureFailureCode::InvalidSplitSeed,
          "Boolean split seed output is not part of ambiguous history");
  }
  for (std::size_t index = 0; index < outputs.size(); ++index)
    if (ambiguous[index] && !matchedSeeds[index])
      return fail<std::optional<BooleanTopologyCaptureResult>>(
          failure, BooleanTopologyCaptureFailureCode::SplitSeedRequired,
          "Boolean split history requires a stable authored seed");

  ShapeHistoryArtifact artifact;
  artifact.producerFeatureId = producerFeatureId;
  artifact.outputShape = outputShape;
  artifact.coverage = TopologyLineageCoverage::CompleteFaces;
  for (const auto& source : sources)
    artifact.sources.push_back(source.snapshot);
  for (std::size_t index = 0; index < sourceFaces.size(); ++index) {
    if (!deleted[index]) continue;
    artifact.facts.push_back(
        {ShapeHistoryFactKind::Deleted, std::nullopt,
         sourceFaces[index].origin, sourceFaces[index].face, std::nullopt,
         {}});
  }
  std::vector<TopologyProvenanceKey> emittedKeys;
  for (std::size_t outputIndex = 0; outputIndex < outputs.size();
       ++outputIndex) {
    const auto& output = outputs[outputIndex];
    const auto operation =
        output.relations.size() > 1
            ? TopologyProvenanceOperation::Merge
            : provenanceOperation(output.relations.front().kind);
    const auto branch = matchedSeeds[outputIndex]
                            ? std::optional<std::string_view>(
                                  matchedSeeds[outputIndex]->stableBranchId)
                            : std::nullopt;
    auto localName = makeLocalName(expectedOperation, operation,
                                   outputOrigins[outputIndex], sourceFaces,
                                   branch, failure);
    if (!localName) return std::nullopt;
    TopologyProvenanceKey key{
        producerFeatureId, TopologyKind::Face, operation,
        TopologySemanticRole::BooleanResult, std::move(*localName),
        outputOrigins[outputIndex]};
    if (std::find(emittedKeys.begin(), emittedKeys.end(), key) !=
        emittedKeys.end())
      return fail<std::optional<BooleanTopologyCaptureResult>>(
          failure, BooleanTopologyCaptureFailureCode::SplitSeedCollision,
          "Boolean history produced a duplicate output identity");
    emittedKeys.push_back(key);
    for (const auto& relation : output.relations) {
      artifact.facts.push_back(
          {factKind(relation.kind), std::nullopt,
           sourceFaces[relation.sourceIndex].origin,
           sourceFaces[relation.sourceIndex].face, key, output.face});
    }
  }

  std::string error;
  auto builder = TopologyLineageBuilder::create(std::move(artifact), &error);
  if (!builder)
    return fail<std::optional<BooleanTopologyCaptureResult>>(
        failure, BooleanTopologyCaptureFailureCode::InvalidHistory,
        "Boolean history artifact is invalid: " + error);
  return BooleanTopologyCaptureResult{std::move(*builder),
                                      std::move(outputShape)};
}

template <class Maker>
std::optional<BooleanTopologyCaptureResult> captureGuarded(
    Maker& maker, BOPAlgo_Operation operation, FeatureId producerFeatureId,
    std::span<const BooleanTopologySource> sources,
    std::span<const BooleanFaceBranchSeed> splitSeeds,
    BooleanTopologyCaptureOptions options,
    BooleanTopologyCaptureFailure* failure) {
  clearFailure(failure);
  std::optional<BooleanTopologyCaptureResult> result;
  GeometryFailure geometryFailure;
  const bool completed = runGeometryOperation(
      [&] {
        result = captureImpl(maker, operation, producerFeatureId, sources,
                             splitSeeds, options, failure);
      },
      &geometryFailure);
  if (!completed)
    return fail<std::optional<BooleanTopologyCaptureResult>>(
        failure, BooleanTopologyCaptureFailureCode::GeometryException,
        geometryFailure.detail.empty()
            ? "Boolean history capture raised a geometry exception"
            : "Boolean history capture raised a geometry exception: " +
                  geometryFailure.detail);
  if (result) clearFailure(failure);
  return result;
}

}  // namespace

std::optional<BooleanTopologyCaptureResult> captureBooleanFaceHistory(
    BRepAlgoAPI_Fuse& maker, FeatureId producerFeatureId,
    std::span<const BooleanTopologySource> sources,
    std::span<const BooleanFaceBranchSeed> splitSeeds,
    BooleanTopologyCaptureOptions options,
    BooleanTopologyCaptureFailure* failure) {
  return captureGuarded(maker, BOPAlgo_FUSE, producerFeatureId, sources,
                        splitSeeds, options, failure);
}

std::optional<BooleanTopologyCaptureResult> captureBooleanFaceHistory(
    BRepAlgoAPI_Cut& maker, FeatureId producerFeatureId,
    std::span<const BooleanTopologySource> sources,
    std::span<const BooleanFaceBranchSeed> splitSeeds,
    BooleanTopologyCaptureOptions options,
    BooleanTopologyCaptureFailure* failure) {
  return captureGuarded(maker, BOPAlgo_CUT, producerFeatureId, sources,
                        splitSeeds, options, failure);
}

}  // namespace solidar
