#include "model/OcctTopologyHistoryCapture.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepCheck_Analyzer.hxx>
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

void setGeometryFailure(const GeometryFailure& failure, std::string* error,
                        const char* context) {
  if (!error) return;
  *error = context;
  if (!failure.detail.empty()) {
    *error += ": ";
    *error += failure.detail;
  }
}

bool fail(std::string* error, const char* diagnostic) {
  if (error) *error = diagnostic;
  return false;
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
    const auto& candidate = explorer.Current();
    if (std::none_of(result.begin(), result.end(), [&](const auto& existing) {
          return existing.IsSame(candidate);
        }))
      result.push_back(candidate);
  }
  return result;
}

bool validateProfileMapping(
    const TopoDS_Face& profileFace,
    std::span<const PrismProfileEdgeSeed> profileEdges,
    std::string* error) {
  if (profileFace.IsNull() || profileEdges.empty())
    return fail(error, "Prism profile mapping is empty");
  const auto canonicalEdges = canonicalSubshapes(profileFace, TopAbs_EDGE);
  if (canonicalEdges.empty() || canonicalEdges.size() != profileEdges.size())
    return fail(error, "Prism profile edge coverage is incomplete");

  for (std::size_t index = 0; index < profileEdges.size(); ++index) {
    const auto& mapping = profileEdges[index];
    if (mapping.geometryId == sketch::kInvalidGeometryId)
      return fail(error, "Prism profile geometry identity is invalid");
    if (!mapping.edge || mapping.edge->IsNull() ||
        mapping.edge->ShapeType() != TopAbs_EDGE)
      return fail(error, "Prism profile edge shape is invalid");
    if (!contains(profileFace, *mapping.edge, TopAbs_EDGE))
      return fail(error, "Prism profile edge is foreign");
    for (std::size_t other = 0; other < index; ++other) {
      if (mapping.geometryId == profileEdges[other].geometryId)
        return fail(error, "Prism profile geometry identity is duplicated");
      if (mapping.edge->IsSame(*profileEdges[other].edge))
        return fail(error, "Prism profile edge identity is duplicated");
    }
  }
  for (const auto& edge : canonicalEdges) {
    const auto count = std::count_if(
        profileEdges.begin(), profileEdges.end(), [&](const auto& mapping) {
          return mapping.edge->IsSame(edge);
        });
    if (count != 1)
      return fail(error, "Prism profile edge coverage is not injective");
  }
  return true;
}

bool appendDistinctFace(std::vector<std::shared_ptr<const TopoDS_Shape>>& faces,
                        const TopoDS_Shape& candidate,
                        std::string* error) {
  if (candidate.IsNull() || candidate.ShapeType() != TopAbs_FACE)
    return fail(error, "OCCT history did not return a face");
  if (std::any_of(faces.begin(), faces.end(), [&](const auto& existing) {
        return existing->IsSame(candidate);
      }))
    return fail(error, "OCCT history returned a duplicate face");
  faces.push_back(std::make_shared<TopoDS_Shape>(candidate));
  return true;
}

std::optional<TopologyLineageBuilder> capturePrismFaceHistoryImpl(
    BRepPrimAPI_MakePrism& maker, FeatureId producerFeatureId,
    SketchId sketchId, std::shared_ptr<const TopoDS_Face> profileFace,
    std::span<const PrismProfileEdgeSeed> profileEdges,
    std::string* error) {
  if (producerFeatureId == kInvalidFeatureId ||
      sketchId == kInvalidSketchId || !profileFace) {
    fail(error, "Prism capture owner identity is invalid");
    return std::nullopt;
  }
  if (!validateProfileMapping(*profileFace, profileEdges, error))
    return std::nullopt;
  if (!maker.IsDone()) {
    fail(error, "Prism builder is not done");
    return std::nullopt;
  }

  const auto outputShape =
      std::make_shared<TopoDS_Shape>(maker.Shape());
  const TopoDS_Shape first = maker.FirstShape(*profileFace);
  const TopoDS_Shape last = maker.LastShape(*profileFace);
  if (outputShape->IsNull() || !BRepCheck_Analyzer(*outputShape).IsValid() ||
      first.IsNull() || last.IsNull() ||
      first.ShapeType() != TopAbs_FACE || last.ShapeType() != TopAbs_FACE ||
      first.IsSame(last) || !contains(*outputShape, first, TopAbs_FACE) ||
      !contains(*outputShape, last, TopAbs_FACE)) {
    fail(error, "Prism cap history is invalid");
    return std::nullopt;
  }

  std::vector<std::shared_ptr<const TopoDS_Shape>> capturedFaces;
  capturedFaces.reserve(profileEdges.size() + 2);
  if (!appendDistinctFace(capturedFaces, first, error) ||
      !appendDistinctFace(capturedFaces, last, error))
    return std::nullopt;

  ShapeHistoryArtifact artifact;
  artifact.producerFeatureId = producerFeatureId;
  artifact.outputShape = outputShape;
  artifact.coverage = TopologyLineageCoverage::CompleteFaces;
  artifact.facts.push_back(
      {ShapeHistoryFactKind::Seed,
       AuthoredTopologyToken{AuthoredTopologySourceKind::ProfileStartCap,
                             sketchId, sketch::kInvalidGeometryId},
       std::nullopt, {}, std::nullopt, capturedFaces[0]});
  artifact.facts.push_back(
      {ShapeHistoryFactKind::Seed,
       AuthoredTopologyToken{AuthoredTopologySourceKind::ProfileEndCap,
                             sketchId, sketch::kInvalidGeometryId},
       std::nullopt, {}, std::nullopt, capturedFaces[1]});

  for (const auto& mapping : profileEdges) {
    const auto& generated = maker.Generated(*mapping.edge);
    if (generated.Size() != 1) {
      fail(error, "Prism edge history is not exactly one-to-one");
      return std::nullopt;
    }
    const auto& generatedFace = generated.First();
    if (generatedFace.IsNull() || generatedFace.ShapeType() != TopAbs_FACE) {
      fail(error, "Prism edge history contains a non-face result");
      return std::nullopt;
    }
    auto side = std::make_shared<TopoDS_Shape>(generatedFace);
    if (!contains(*outputShape, *side, TopAbs_FACE) ||
        !appendDistinctFace(capturedFaces, *side, error))
      return std::nullopt;
    artifact.facts.push_back(
        {ShapeHistoryFactKind::Seed,
         AuthoredTopologyToken{AuthoredTopologySourceKind::GeometrySide,
                               sketchId, mapping.geometryId},
         std::nullopt, {}, std::nullopt, capturedFaces.back()});
  }

  if (canonicalSubshapes(*outputShape, TopAbs_FACE).size() !=
      capturedFaces.size()) {
    fail(error, "Prism history does not cover every output face");
    return std::nullopt;
  }
  return TopologyLineageBuilder::create(std::move(artifact), error);
}

std::optional<TopologyLineageBuilder> captureTransformFaceHistoryImpl(
    BRepBuilderAPI_Transform& maker, FeatureId producerFeatureId,
    std::shared_ptr<const TopoDS_Shape> makerInputShape,
    TopologySourceSnapshot source,
    std::span<const TransformFaceSeed> faceSeeds, std::string* error) {
  if (producerFeatureId == kInvalidFeatureId || !makerInputShape ||
      makerInputShape->IsNull() || !source.shape || source.shape->IsNull() ||
      !makerInputShape->IsSame(*source.shape)) {
    fail(error, "Transform source does not match maker input");
    return std::nullopt;
  }
  if (source.lineage == nullptr ||
      source.lineage->coverage() != TopologyLineageCoverage::CompleteFaces) {
    fail(error, "Transform source does not have complete face lineage");
    return std::nullopt;
  }
  if (!validateTopologySourceSnapshot(source, error))
    return std::nullopt;
  if (!maker.IsDone()) {
    fail(error, "Transform builder is not done");
    return std::nullopt;
  }

  const auto sourceFaces = canonicalSubshapes(*source.shape, TopAbs_FACE);
  if (sourceFaces.empty() || faceSeeds.size() != sourceFaces.size()) {
    fail(error, "Transform face seed coverage is incomplete");
    return std::nullopt;
  }
  struct SourceFace {
    TopologyProvenanceOrigin origin;
    std::shared_ptr<const TopoDS_Shape> face;
    TopologyProvenanceKey outputProvenance;
  };
  std::vector<SourceFace> mappedSources;
  mappedSources.reserve(sourceFaces.size());
  for (const auto& sourceFace : sourceFaces) {
    const TopologyLineageBinding* matched = nullptr;
    for (const auto& binding : source.lineage->bindings()) {
      if (binding.provenance.kind != TopologyKind::Face ||
          !binding.subshape->IsSame(sourceFace))
        continue;
      if (matched != nullptr) {
        fail(error, "Transform source face has multiple identities");
        return std::nullopt;
      }
      matched = &binding;
    }
    if (matched == nullptr) {
      fail(error, "Transform source face identity is missing");
      return std::nullopt;
    }
    const auto origin = matched->provenance.immediateOrigin();
    const TransformFaceSeed* matchedSeed = nullptr;
    for (const auto& seed : faceSeeds) {
      if (seed.sourceOrigin != origin) continue;
      if (matchedSeed != nullptr) {
        fail(error, "Transform source identity has duplicate seeds");
        return std::nullopt;
      }
      matchedSeed = &seed;
    }
    if (matchedSeed == nullptr) {
      fail(error, "Transform source identity seed is missing");
      return std::nullopt;
    }
    std::string diagnostic;
    if (!matchedSeed->sourceOrigin.isValid(&diagnostic) ||
        matchedSeed->sourceOrigin.featureId != source.featureId ||
        matchedSeed->sourceOrigin.kind != TopologyKind::Face ||
        !matchedSeed->outputProvenance.isCanonical(&diagnostic) ||
        matchedSeed->outputProvenance.featureId != producerFeatureId ||
        matchedSeed->outputProvenance.kind != TopologyKind::Face ||
        (matchedSeed->outputProvenance.operation !=
             TopologyProvenanceOperation::Modified &&
         matchedSeed->outputProvenance.operation !=
             TopologyProvenanceOperation::Retained) ||
        matchedSeed->outputProvenance.origins.size() != 1 ||
        matchedSeed->outputProvenance.origins.front() != origin) {
      fail(error, "Transform face seed provenance is invalid");
      return std::nullopt;
    }
    for (const auto& mapped : mappedSources) {
      if (mapped.outputProvenance == matchedSeed->outputProvenance) {
        fail(error, "Transform output provenance is duplicated");
        return std::nullopt;
      }
      if (mapped.outputProvenance.immediateOrigin() ==
              matchedSeed->outputProvenance.immediateOrigin() &&
          mapped.outputProvenance != matchedSeed->outputProvenance) {
        fail(error,
             "Transform output identity has conflicting source ancestry");
        return std::nullopt;
      }
    }
    mappedSources.push_back(
        {origin, std::make_shared<TopoDS_Shape>(sourceFace),
         matchedSeed->outputProvenance});
  }

  const auto outputShape =
      std::make_shared<TopoDS_Shape>(maker.Shape());
  if (outputShape->IsNull() || !BRepCheck_Analyzer(*outputShape).IsValid()) {
    fail(error, "Transform output shape is invalid");
    return std::nullopt;
  }

  ShapeHistoryArtifact artifact;
  artifact.producerFeatureId = producerFeatureId;
  artifact.outputShape = outputShape;
  artifact.coverage = TopologyLineageCoverage::CompleteFaces;
  artifact.sources.push_back(std::move(source));
  std::vector<std::shared_ptr<const TopoDS_Shape>> mappedOutputs;
  mappedOutputs.reserve(mappedSources.size());
  for (const auto& mappedSource : mappedSources) {
    const TopoDS_Shape modified = maker.ModifiedShape(*mappedSource.face);
    if (modified.IsNull() || modified.ShapeType() != TopAbs_FACE ||
        !contains(*outputShape, modified, TopAbs_FACE) ||
        std::any_of(mappedOutputs.begin(), mappedOutputs.end(),
                    [&](const auto& existing) {
                      return existing->IsSame(modified);
                    })) {
      fail(error, "Transform face history is not one-to-one");
      return std::nullopt;
    }
    auto outputFace = std::make_shared<TopoDS_Shape>(modified);
    const bool retained = outputFace->IsSame(*mappedSource.face);
    const auto operation = retained ? TopologyProvenanceOperation::Retained
                                    : TopologyProvenanceOperation::Modified;
    if (mappedSource.outputProvenance.operation != operation) {
      fail(error, "Transform face seed operation disagrees with OCCT history");
      return std::nullopt;
    }
    artifact.facts.push_back(
        {retained ? ShapeHistoryFactKind::Retained
                  : ShapeHistoryFactKind::Modified,
         std::nullopt, mappedSource.origin, mappedSource.face,
         mappedSource.outputProvenance,
         outputFace});
    mappedOutputs.push_back(std::move(outputFace));
  }

  if (canonicalSubshapes(*outputShape, TopAbs_FACE).size() !=
      mappedOutputs.size()) {
    fail(error, "Transform history does not cover every output face");
    return std::nullopt;
  }
  return TopologyLineageBuilder::create(std::move(artifact), error);
}

}  // namespace

std::optional<TopologyLineageBuilder> capturePrismFaceHistory(
    BRepPrimAPI_MakePrism& maker, FeatureId producerFeatureId,
    SketchId sketchId, std::shared_ptr<const TopoDS_Face> profileFace,
    std::span<const PrismProfileEdgeSeed> profileEdges,
    std::string* error) {
  if (error) error->clear();
  std::optional<TopologyLineageBuilder> result;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&] {
        result = capturePrismFaceHistoryImpl(
            maker, producerFeatureId, sketchId, std::move(profileFace),
            profileEdges, error);
      },
      &failure);
  if (!completed)
    setGeometryFailure(failure, error,
                       "Prism history capture raised a geometry exception");
  if (result && error) error->clear();
  return result;
}

std::optional<TopologyLineageBuilder> captureTransformFaceHistory(
    BRepBuilderAPI_Transform& maker, FeatureId producerFeatureId,
    std::shared_ptr<const TopoDS_Shape> makerInputShape,
    TopologySourceSnapshot source,
    std::span<const TransformFaceSeed> faceSeeds, std::string* error) {
  if (error) error->clear();
  std::optional<TopologyLineageBuilder> result;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&] {
        result = captureTransformFaceHistoryImpl(
            maker, producerFeatureId, std::move(makerInputShape),
            std::move(source), faceSeeds, error);
      },
      &failure);
  if (!completed)
    setGeometryFailure(failure, error,
                       "Transform history capture raised a geometry exception");
  if (result && error) error->clear();
  return result;
}

}  // namespace solidar
