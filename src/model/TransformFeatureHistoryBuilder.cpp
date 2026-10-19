#include "model/TransformFeatureHistoryBuilder.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>

#include <utility>
#include <vector>

#include "model/GeometryOperation.h"
#include "model/OcctTopologyHistoryCapture.h"

namespace solidar {
namespace {

bool validRelation(TransformFeatureInstanceRelation relation) noexcept {
  switch (relation) {
    case TransformFeatureInstanceRelation::RetainedSource:
    case TransformFeatureInstanceRelation::ModifiedCopy:
      return true;
  }
  return false;
}

void setGeometryFailure(const GeometryFailure& failure, std::string* error) {
  if (!error) return;
  *error = "Transform feature history raised a geometry exception";
  if (!failure.detail.empty()) {
    *error += ": ";
    *error += failure.detail;
  }
}

std::optional<std::string> transformDerivedLocalNameImpl(
    const TopologyProvenanceOrigin& source, std::string* error) {
  std::string diagnostic;
  if (!source.isValid(&diagnostic)) {
    if (error) *error = std::move(diagnostic);
    return std::nullopt;
  }
  std::string result =
      "transform-source:v1;feature=" + std::to_string(source.featureId) +
      ";kind=" + std::to_string(static_cast<unsigned>(source.kind)) +
      ";operation=" +
      std::to_string(static_cast<unsigned>(source.operation)) +
      ";role=" + std::to_string(static_cast<unsigned>(source.role)) +
      ";name-length=" + std::to_string(source.localName.size()) + ":" +
      source.localName + ";occurrence=";
  if (source.occurrence) {
    result += "present;owner=" +
              std::to_string(source.occurrence->featureId) +
              ";slot=" + std::to_string(source.occurrence->slot);
  } else {
    result += "absent";
  }
  return result;
}

std::optional<TopologyProvenanceKey> deriveTransformFaceProvenanceImpl(
    const TopologyProvenanceOrigin& source, FeatureId producerFeatureId,
    TopologyProvenanceOperation operation, TopologySemanticRole role,
    std::optional<TopologyOccurrenceKey> occurrence, std::string* error) {
  if (producerFeatureId == kInvalidFeatureId || source.kind != TopologyKind::Face ||
      (operation != TopologyProvenanceOperation::Modified &&
       operation != TopologyProvenanceOperation::Retained) ||
      (occurrence && occurrence->featureId != producerFeatureId)) {
    if (error) *error = "Transform output provenance request is invalid";
    return std::nullopt;
  }
  const auto localName = transformDerivedLocalNameImpl(source, error);
  if (!localName) return std::nullopt;
  TopologyProvenanceKey result{producerFeatureId, TopologyKind::Face,
                               operation, role, *localName, {source},
                               occurrence};
  std::string diagnostic;
  if (!result.isCanonical(&diagnostic)) {
    if (error) *error = std::move(diagnostic);
    return std::nullopt;
  }
  return result;
}

bool buildTransformFeatureHistoryImpl(
    TopologySourceSnapshot source, FeatureId producerFeatureId,
    std::span<const TransformFeatureInstanceRequest> instances,
    bool directSingleOutput, TransformFeatureHistoryBuild* result,
    std::string* error) {
  const auto fail = [&](std::string diagnostic) {
    if (error) *error = std::move(diagnostic);
    return false;
  };
  if (!result || producerFeatureId == kInvalidFeatureId || instances.empty())
    return fail("Transform feature history input is invalid");
  if (!source.lineage ||
      source.lineage->coverage() != TopologyLineageCoverage::CompleteFaces)
    return fail("Transform source does not have complete face lineage");
  if (!validateTopologySourceSnapshot(source, error)) return false;
  if (directSingleOutput &&
      (instances.size() != 1 ||
       instances.front().relation !=
           TransformFeatureInstanceRelation::ModifiedCopy))
    return fail("Direct transform output requires one modified copy");

  std::vector<std::shared_ptr<const TopoDS_Shape>> instanceShapes;
  std::vector<HistoryFact> facts;
  instanceShapes.reserve(instances.size());
  facts.reserve(source.lineage->bindings().size() * instances.size());

  for (const auto& instance : instances) {
    if (!validRelation(instance.relation) ||
        (instance.occurrence &&
         instance.occurrence->featureId != producerFeatureId))
      return fail("Transform instance identity is invalid");
    const auto operation =
        instance.relation == TransformFeatureInstanceRelation::RetainedSource
            ? TopologyProvenanceOperation::Retained
            : TopologyProvenanceOperation::Modified;

    if (instance.relation ==
        TransformFeatureInstanceRelation::RetainedSource) {
      instanceShapes.push_back(source.shape);
      for (const auto& binding : source.lineage->bindings()) {
        if (binding.provenance.kind != TopologyKind::Face) continue;
        const auto origin = binding.provenance.immediateOrigin();
        const auto output = deriveTransformFaceProvenanceImpl(
            origin, producerFeatureId, operation, instance.role,
            instance.occurrence, error);
        if (!output) return false;
        facts.push_back({ShapeHistoryFactKind::Retained, std::nullopt, origin,
                         binding.subshape, *output, binding.subshape});
      }
      continue;
    }

    std::vector<TransformFaceSeed> seeds;
    seeds.reserve(source.lineage->bindings().size());
    for (const auto& binding : source.lineage->bindings()) {
      if (binding.provenance.kind != TopologyKind::Face) continue;
      const auto origin = binding.provenance.immediateOrigin();
      const auto output = deriveTransformFaceProvenanceImpl(
          origin, producerFeatureId, operation, instance.role,
          instance.occurrence, error);
      if (!output) return false;
      seeds.push_back({origin, *output});
    }

    BRepBuilderAPI_Transform maker(*source.shape, instance.transform, true);
    if (!maker.IsDone() || maker.Shape().IsNull())
      return fail("Transform instance construction failed");
    const auto captured = captureTransformFaceHistory(
        maker, producerFeatureId, source.shape, source, seeds, error);
    if (!captured) return false;
    instanceShapes.push_back(captured->artifact().outputShape);
    facts.insert(facts.end(), captured->artifact().facts.begin(),
                 captured->artifact().facts.end());
  }

  std::shared_ptr<const TopoDS_Shape> outputShape;
  if (directSingleOutput) {
    outputShape = instanceShapes.front();
  } else {
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const auto& instanceShape : instanceShapes) {
      if (!instanceShape || instanceShape->IsNull())
        return fail("Transform instance output is invalid");
      builder.Add(compound, *instanceShape);
    }
    outputShape = std::make_shared<TopoDS_Shape>(compound);
  }
  if (!outputShape || outputShape->IsNull() ||
      !BRepCheck_Analyzer(*outputShape).IsValid())
    return fail("Transform feature output is invalid");

  ShapeHistoryArtifact artifact;
  artifact.producerFeatureId = producerFeatureId;
  artifact.outputShape = outputShape;
  artifact.coverage = TopologyLineageCoverage::CompleteFaces;
  artifact.sources.push_back(std::move(source));
  artifact.facts = std::move(facts);
  const auto builder = TopologyLineageBuilder::create(std::move(artifact), error);
  if (!builder) return false;
  const auto lineage = builder->build(error);
  if (!lineage ||
      lineage->coverage() != TopologyLineageCoverage::CompleteFaces)
    return fail(error && !error->empty() ? *error
                                         : "Transform face lineage is incomplete");

  TransformFeatureHistoryBuild built{outputShape, lineage};
  *result = std::move(built);
  if (error) error->clear();
  return true;
}

}  // namespace

std::optional<std::string> transformDerivedLocalName(
    const TopologyProvenanceOrigin& source, std::string* error) {
  if (error) error->clear();
  std::optional<std::string> result;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&] { result = transformDerivedLocalNameImpl(source, error); }, &failure);
  if (!completed) setGeometryFailure(failure, error);
  if (result && error) error->clear();
  return result;
}

std::optional<TopologyProvenanceKey> deriveTransformFaceProvenance(
    const TopologyProvenanceOrigin& source, FeatureId producerFeatureId,
    TopologyProvenanceOperation operation, TopologySemanticRole role,
    std::optional<TopologyOccurrenceKey> occurrence, std::string* error) {
  if (error) error->clear();
  std::optional<TopologyProvenanceKey> result;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&] {
        result = deriveTransformFaceProvenanceImpl(
            source, producerFeatureId, operation, role, occurrence, error);
      },
      &failure);
  if (!completed) setGeometryFailure(failure, error);
  if (result && error) error->clear();
  return result;
}

bool buildTransformFeatureHistory(
    TopologySourceSnapshot source, FeatureId producerFeatureId,
    std::span<const TransformFeatureInstanceRequest> instances,
    bool directSingleOutput, TransformFeatureHistoryBuild* result,
    std::string* error) {
  if (error) error->clear();
  bool built = false;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&] {
        built = buildTransformFeatureHistoryImpl(
            std::move(source), producerFeatureId, instances,
            directSingleOutput, result, error);
      },
      &failure);
  if (!completed) setGeometryFailure(failure, error);
  if (completed && built && error) error->clear();
  return completed && built;
}

}  // namespace solidar
