#include "TestAssertions.h"

#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "model/Document.h"
#include "model/ShapeFeature.h"
#include "model/TopologyLineage.h"
#include "model/TopologyReferenceResolver.h"

namespace {

solidar::TopologyProvenanceKey makeKey(
    solidar::FeatureId featureId, solidar::TopologyKind kind,
    std::string localName) {
  return {featureId,
          kind,
          solidar::TopologyProvenanceOperation::Generated,
          kind == solidar::TopologyKind::Face
              ? solidar::TopologySemanticRole::Side
              : solidar::TopologySemanticRole::Custom,
          std::move(localName),
          {{11, kind, "profile:a"}, {12, kind, "profile:b"}}};
}

std::vector<std::shared_ptr<const TopoDS_Shape>> subshapes(
    const TopoDS_Shape& shape, TopAbs_ShapeEnum kind, std::size_t count) {
  std::vector<std::shared_ptr<const TopoDS_Shape>> result;
  for (TopExp_Explorer explorer(shape, kind);
       explorer.More() && result.size() < count; explorer.Next())
    result.push_back(
        std::make_shared<TopoDS_Shape>(explorer.Current()));
  return result;
}

std::shared_ptr<TopoDS_Shape> faceCompound(
    const std::shared_ptr<const TopoDS_Shape>& first,
    const std::shared_ptr<const TopoDS_Shape>& second) {
  TopoDS_Compound compound;
  BRep_Builder builder;
  builder.MakeCompound(compound);
  builder.Add(compound, *first);
  builder.Add(compound, *second);
  return std::make_shared<TopoDS_Shape>(compound);
}

std::shared_ptr<const solidar::TopologyLineage> lineageFor(
    const TopoDS_Shape& shape, const solidar::TopologyProvenanceKey& key,
    std::string* error = nullptr) {
  const auto faces = subshapes(shape, TopAbs_FACE, 1);
  if (faces.empty()) return {};
  return solidar::TopologyLineage::create({{key, faces.front()}}, error);
}

enum class SyntheticMode {
  ValidFirst,
  ValidNext,
  LegacyNoLineage,
  FailAfterStaging,
  ThrowAfterStaging,
  ForeignBinding,
  MalformedLineage,
};

class SyntheticLineageFeature final : public solidar::ShapeFeature {
 public:
  explicit SyntheticLineageFeature(solidar::BodyId bodyId)
      : ShapeFeature("Synthetic lineage"), bodyId_(bodyId) {}

  [[nodiscard]] std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<SyntheticLineageFeature>(*this);
  }

  [[nodiscard]] solidar::TopologyReferenceV2 reference(
      std::string localName) const {
    return {bodyId_, makeKey(id(), solidar::TopologyKind::Face,
                             std::move(localName))};
  }

  SyntheticMode mode{SyntheticMode::ValidFirst};

 protected:
  bool rebuildImpl(const solidar::RebuildContext&) override {
    const bool next = mode != SyntheticMode::ValidFirst;
    auto shape = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(next ? 20.0 : 10.0, 10.0, 10.0).Shape());
    if (mode == SyntheticMode::LegacyNoLineage) {
      setShape(std::move(shape));
      markValid();
      return true;
    }
    auto key = makeKey(id(), solidar::TopologyKind::Face,
                       next ? "side:next" : "side:first");
    if (mode == SyntheticMode::MalformedLineage)
      key.operation = static_cast<solidar::TopologyProvenanceOperation>(255);
    auto lineage = lineageFor(*shape, key);
    if (mode == SyntheticMode::ForeignBinding) {
      const TopoDS_Shape foreign =
          BRepPrimAPI_MakeBox(gp_Pnt(100.0, 0.0, 0.0), 2.0, 2.0, 2.0)
              .Shape();
      const auto faces = subshapes(foreign, TopAbs_FACE, 1);
      lineage = solidar::TopologyLineage::create({{key, faces.front()}});
    }

    setShapeAndTopologyLineage(std::move(shape), std::move(lineage));
    if (mode == SyntheticMode::ThrowAfterStaging)
      throw std::runtime_error("synthetic staged failure");
    if (mode == SyntheticMode::FailAfterStaging) {
      markError("synthetic staged failure");
      return false;
    }
    markValid();
    return true;
  }

 private:
  solidar::BodyId bodyId_;
};

}  // namespace

int main() {
  constexpr solidar::BodyId bodyId = 101;
  constexpr solidar::FeatureId featureId = 202;

  // Set-like origins are deterministically sorted and exactly deduplicated by
  // construction. Authored occurrence identity remains untouched.
  {
    const std::vector<solidar::TopologyProvenanceOrigin> forward{
        {11, solidar::TopologyKind::Face, "profile:a"},
        {12, solidar::TopologyKind::Face, "profile:b"},
        {12, solidar::TopologyKind::Face, "profile:b"}};
    const std::vector<solidar::TopologyProvenanceOrigin> reversed{
        forward[2], forward[1], forward[0]};
    const solidar::TopologyOccurrenceKey occurrence{featureId, 7};
    const solidar::TopologyProvenanceKey forwardKey{
        featureId, solidar::TopologyKind::Face,
        solidar::TopologyProvenanceOperation::Generated,
        solidar::TopologySemanticRole::PatternInstance, "side:0", forward,
        occurrence};
    const solidar::TopologyProvenanceKey reversedKey{
        featureId, solidar::TopologyKind::Face,
        solidar::TopologyProvenanceOperation::Generated,
        solidar::TopologySemanticRole::PatternInstance, "side:0", reversed,
        occurrence};
    std::string error;
    CHECK(forwardKey.isCanonical(&error));
    CHECK(error.empty());
    CHECK(forwardKey == reversedKey);
    CHECK(forwardKey.origins.size() == 2);
    CHECK(forwardKey.occurrence == occurrence);

    auto rawPermutation = forwardKey;
    std::swap(rawPermutation.origins[0], rawPermutation.origins[1]);
    const auto canonicalizedLineage = solidar::TopologyLineage::create(
        {{rawPermutation, std::make_shared<TopoDS_Shape>(
                               BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape())}},
        &error);
    CHECK(canonicalizedLineage);
    CHECK(canonicalizedLineage->bindings().front().provenance == forwardKey);
  }

  const auto shape = std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape());
  const auto faces = subshapes(*shape, TopAbs_FACE, 2);
  const auto edges = subshapes(*shape, TopAbs_EDGE, 1);
  CHECK(faces.size() == 2);
  CHECK(edges.size() == 1);

  // A unique v2 key resolves exactly. V2 cannot carry v1 evidence by type;
  // missing provenance therefore returns None/Missing. Carrier-backed v1
  // legacy, semantic-tag and geometric-signature behavior stays unchanged.
  {
    const auto faceKey =
        makeKey(featureId, solidar::TopologyKind::Face, "side:unique");
    const auto edgeKey =
        makeKey(featureId, solidar::TopologyKind::Edge, "edge:unique");
    std::string error;
    const auto lineage = solidar::TopologyLineage::create(
        {{faceKey, faces[0]}, {edgeKey, edges[0]}}, &error);
    CHECK(lineage);
    CHECK(error.empty());
    const auto index =
        solidar::TopologyIndex::build(shape, 700, lineage, &error);
    CHECK(index);
    CHECK(error.empty());
    CHECK(index->lineage() == lineage);
    const auto baselineIndex =
        solidar::TopologyIndex::build(shape, 700, &error);
    CHECK(baselineIndex);
    CHECK(error.empty());
    CHECK(!baselineIndex->lineage());

    const solidar::TopologyReferenceV2 faceReference{bodyId, faceKey};
    const auto resolvedFace = index->resolveFace(faceReference);
    CHECK(resolvedFace);
    CHECK(resolvedFace.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    CHECK(resolvedFace.subshape && resolvedFace.subshape->IsSame(*faces[0]));

    const solidar::TopologyReferenceV2 edgeReference{bodyId, edgeKey};
    const auto resolvedEdge = index->resolveEdge(edgeReference);
    CHECK(resolvedEdge);
    CHECK(resolvedEdge.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    CHECK(resolvedEdge.subshape && resolvedEdge.subshape->IsSame(*edges[0]));

    auto absentKey = faceKey;
    absentKey.localName = "side:absent";
    const auto missing = index->resolveFace({bodyId, absentKey});
    CHECK(!missing);
    CHECK(missing.method == solidar::TopologyMatchMethod::None);
    CHECK(missing.failure == solidar::TopologyResolutionFailure::Missing);
    CHECK(!missing.subshape);

    const auto invalidOwner = index->resolveFace(
        {solidar::kInvalidBodyId, faceKey});
    CHECK(!invalidOwner);
    CHECK(invalidOwner.failure ==
          solidar::TopologyResolutionFailure::InvalidReference);
    const auto wrongResolver = index->resolveEdge(faceReference);
    CHECK(!wrongResolver);
    CHECK(wrongResolver.failure ==
          solidar::TopologyResolutionFailure::InvalidReference);

    solidar::TopologyReference legacy;
    legacy.bodyId = bodyId;
    legacy.featureId = featureId;
    legacy.kind = solidar::TopologyKind::Face;
    legacy.legacyIndex = 0;
    const auto tagged =
        baselineIndex->createFaceReference(bodyId, featureId, 0);
    CHECK(tagged);
    auto signatureOnly = tagged.reference.topology();
    signatureOnly.persistentTag.clear();

    const auto compareV1 = [&](const solidar::TopologyReference& reference,
                               solidar::TopologyMatchMethod expectedMethod) {
      const auto baseline = baselineIndex->resolveFace(reference);
      const auto carrier = index->resolveFace(reference);
      CHECK(bool(baseline) == bool(carrier));
      CHECK(baseline.failure == carrier.failure);
      CHECK(baseline.method == carrier.method);
      CHECK(baseline.method == expectedMethod);
      CHECK(baseline.subshape.has_value() == carrier.subshape.has_value());
      CHECK(baseline.subshape);
      CHECK(carrier.subshape);
      CHECK(baseline.subshape->IsSame(*carrier.subshape));
    };
    compareV1(legacy, solidar::TopologyMatchMethod::LegacyIndex);
    compareV1(tagged.reference.topology(),
              solidar::TopologyMatchMethod::SemanticTag);
    compareV1(signatureOnly,
              solidar::TopologyMatchMethod::GeometricSignature);
  }

  // IsSame aliases are idempotent. Distinct keys may name the same canonical
  // subshape, while the same key on two distinct canonical subshapes remains
  // ambiguous.
  {
    const auto aliasKey =
        makeKey(featureId, solidar::TopologyKind::Face, "side:alias");
    const auto secondAliasKey =
        makeKey(featureId, solidar::TopologyKind::Face, "side:alias-two");
    const auto reversedAlias =
        std::make_shared<TopoDS_Shape>(faces[0]->Reversed());
    CHECK(faces[0]->IsSame(*reversedAlias));
    const auto aliasLineage = solidar::TopologyLineage::create(
        {{aliasKey, faces[0]},
         {aliasKey, reversedAlias},
         {secondAliasKey, reversedAlias}});
    CHECK(aliasLineage);
    const auto aliasIndex =
        solidar::TopologyIndex::build(shape, 701, aliasLineage);
    CHECK(aliasIndex);
    const auto firstAlias =
        aliasIndex->resolveFace({bodyId, aliasKey});
    const auto secondAlias =
        aliasIndex->resolveFace({bodyId, secondAliasKey});
    CHECK(firstAlias);
    CHECK(secondAlias);
    CHECK(firstAlias.subshape->IsSame(*secondAlias.subshape));

    const auto duplicateKey =
        makeKey(featureId, solidar::TopologyKind::Face, "side:duplicate");
    CHECK(!faces[0]->IsSame(*faces[1]));
    const auto lineage = solidar::TopologyLineage::create(
        {{duplicateKey, faces[0]}, {duplicateKey, faces[1]}});
    CHECK(lineage);
    const auto index = solidar::TopologyIndex::build(shape, 702, lineage);
    CHECK(index);
    const auto ambiguous =
        index->resolveFace(solidar::TopologyReferenceV2{bodyId, duplicateKey});
    CHECK(!ambiguous);
    CHECK(!ambiguous.subshape);
    CHECK(ambiguous.method == solidar::TopologyMatchMethod::None);
    CHECK(ambiguous.failure ==
          solidar::TopologyResolutionFailure::Ambiguous);
  }

  // Candidate traversal and binding insertion order do not change exact
  // Resolved/Missing/Ambiguous outcomes.
  {
    const auto forwardShape = faceCompound(faces[0], faces[1]);
    const auto reverseShape = faceCompound(faces[1], faces[0]);
    const auto uniqueKey =
        makeKey(featureId, solidar::TopologyKind::Face, "order:unique");
    const auto duplicateKey =
        makeKey(featureId, solidar::TopologyKind::Face, "order:duplicate");
    const auto missingKey =
        makeKey(featureId, solidar::TopologyKind::Face, "order:missing");
    const auto forwardLineage = solidar::TopologyLineage::create(
        {{uniqueKey, faces[0]},
         {duplicateKey, faces[0]},
         {duplicateKey, faces[1]}});
    const auto reverseLineage = solidar::TopologyLineage::create(
        {{duplicateKey, faces[1]},
         {duplicateKey, faces[0]},
         {uniqueKey, faces[0]}});
    CHECK(forwardLineage);
    CHECK(reverseLineage);
    const auto forwardIndex =
        solidar::TopologyIndex::build(forwardShape, 703, forwardLineage);
    const auto reverseIndex =
        solidar::TopologyIndex::build(reverseShape, 704, reverseLineage);
    CHECK(forwardIndex);
    CHECK(reverseIndex);
    const auto forwardResolved =
        forwardIndex->resolveFace({bodyId, uniqueKey});
    const auto reverseResolved =
        reverseIndex->resolveFace({bodyId, uniqueKey});
    CHECK(forwardResolved);
    CHECK(reverseResolved);
    CHECK(forwardResolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    CHECK(reverseResolved.method ==
          solidar::TopologyMatchMethod::OperationProvenance);
    CHECK(forwardResolved.subshape);
    CHECK(reverseResolved.subshape);
    CHECK(forwardResolved.subshape->IsSame(*faces[0]));
    CHECK(reverseResolved.subshape->IsSame(*faces[0]));
    CHECK(forwardResolved.subshape->IsSame(*reverseResolved.subshape));

    const auto forwardMissing =
        forwardIndex->resolveFace({bodyId, missingKey});
    const auto reverseMissing =
        reverseIndex->resolveFace({bodyId, missingKey});
    for (const auto* missing : {&forwardMissing, &reverseMissing}) {
      CHECK(!*missing);
      CHECK(!missing->subshape);
      CHECK(missing->method == solidar::TopologyMatchMethod::None);
      CHECK(missing->failure == solidar::TopologyResolutionFailure::Missing);
    }

    const auto forwardAmbiguous =
        forwardIndex->resolveFace({bodyId, duplicateKey});
    const auto reverseAmbiguous =
        reverseIndex->resolveFace({bodyId, duplicateKey});
    for (const auto* ambiguous : {&forwardAmbiguous, &reverseAmbiguous}) {
      CHECK(!*ambiguous);
      CHECK(!ambiguous->subshape);
      CHECK(ambiguous->method == solidar::TopologyMatchMethod::None);
      CHECK(ambiguous->failure ==
            solidar::TopologyResolutionFailure::Ambiguous);
    }
  }

  // Sidecar bindings are validated against the committed output and kind.
  {
    const auto faceKey =
        makeKey(featureId, solidar::TopologyKind::Face, "side:foreign");
    const TopoDS_Shape foreign =
        BRepPrimAPI_MakeBox(gp_Pnt(100.0, 0.0, 0.0), 1.0, 1.0, 1.0)
            .Shape();
    const auto foreignFace = subshapes(foreign, TopAbs_FACE, 1).front();
    std::string error;
    const auto foreignLineage =
        solidar::TopologyLineage::create({{faceKey, foreignFace}});
    CHECK(foreignLineage);
    CHECK(!solidar::TopologyIndex::build(shape, 705, foreignLineage, &error));
    CHECK(error.find("not part") != std::string::npos);

    const auto wrongKindLineage =
        solidar::TopologyLineage::create({{faceKey, edges.front()}});
    CHECK(wrongKindLineage);
    CHECK(!solidar::TopologyIndex::build(shape, 706, wrongKindLineage,
                                         &error));
    CHECK(error.find("non-face") != std::string::npos);

    for (int invalidField = 0; invalidField < 4; ++invalidField) {
      auto invalidKey = faceKey;
      if (invalidField == 0)
        invalidKey.kind = static_cast<solidar::TopologyKind>(255);
      else if (invalidField == 1)
        invalidKey.operation =
            static_cast<solidar::TopologyProvenanceOperation>(255);
      else if (invalidField == 2)
        invalidKey.role = static_cast<solidar::TopologySemanticRole>(255);
      else
        invalidKey.origins.front().kind =
            static_cast<solidar::TopologyKind>(255);
      CHECK(!solidar::TopologyLineage::create(
          {{invalidKey, faces.front()}}, &error));
      CHECK(!error.empty());
    }
  }

  // ShapeFeature commits B-Rep, lineage, revision and index together. Failed,
  // exceptional and invalid-lineage rebuilds retain the complete last-valid
  // snapshot; invalidation clears the complete snapshot.
  {
    solidar::Document document;
    SyntheticLineageFeature feature(bodyId);
    solidar::RebuildContext context{document, nullptr, nullptr, nullptr};
    CHECK(feature.rebuild(context));
    const auto firstShape = feature.shape();
    const auto firstLineage = feature.topologyLineage();
    const auto firstRevision = feature.shapeRevision();
    const auto firstIndex = feature.topologyIndex();
    CHECK(firstShape);
    CHECK(firstLineage);
    CHECK(firstRevision != solidar::kInvalidShapeRevision);
    CHECK(firstIndex);
    CHECK(firstIndex->revision() == firstRevision);
    CHECK(firstIndex->lineage() == firstLineage);
    CHECK(firstIndex->resolveFace(feature.reference("side:first")));

    for (const SyntheticMode failureMode : {
             SyntheticMode::FailAfterStaging,
             SyntheticMode::ThrowAfterStaging,
             SyntheticMode::ForeignBinding,
             SyntheticMode::MalformedLineage}) {
      feature.mode = failureMode;
      feature.setDirty();
      CHECK(!feature.rebuild(context));
      CHECK(feature.isFailed());
      CHECK(!feature.shape());
      CHECK(!feature.topologyLineage());
      std::string currentError;
      CHECK(!feature.topologyIndex(&currentError));
      CHECK(!currentError.empty());
      CHECK(feature.lastValidShape() == firstShape);
      CHECK(feature.lastValidTopologyLineage() == firstLineage);
      CHECK(feature.shapeRevision() == firstRevision);
      CHECK(feature.lastValidTopologyIndex() == firstIndex);
      CHECK(firstIndex->resolveFace(feature.reference("side:first")));
      CHECK(!firstIndex->resolveFace(feature.reference("side:next")));
    }

    // Recovery can publish the same key that was present in the failed
    // attempt, without retaining any half-staged state.
    feature.mode = SyntheticMode::ValidNext;
    feature.setDirty();
    CHECK(feature.rebuild(context));
    CHECK(feature.shape() != firstShape);
    CHECK(feature.topologyLineage() != firstLineage);
    CHECK(feature.shapeRevision() != firstRevision);
    const auto secondIndex = feature.topologyIndex();
    CHECK(secondIndex);
    CHECK(secondIndex->resolveFace(feature.reference("side:next")));
    CHECK(!secondIndex->resolveFace(feature.reference("side:first")));

    feature.prepareForHistory();
    CHECK(feature.isDirty());
    CHECK(!feature.lastValidShape());
    CHECK(!feature.lastValidTopologyLineage());
    CHECK(feature.rebuild(context));
    CHECK(feature.topologyIndex()->resolveFace(
        feature.reference("side:next")));

    feature.discardResult();
    CHECK(!feature.lastValidShape());
    CHECK(!feature.lastValidTopologyLineage());
    CHECK(feature.shapeRevision() == solidar::kInvalidShapeRevision);
    std::string error;
    CHECK(!feature.lastValidTopologyIndex(&error));
    CHECK(!error.empty());
    feature.setDirty();
    CHECK(feature.rebuild(context));
    CHECK(feature.topologyIndex()->resolveFace(
        feature.reference("side:next")));

    SyntheticLineageFeature legacyFeature(bodyId);
    legacyFeature.mode = SyntheticMode::LegacyNoLineage;
    CHECK(legacyFeature.rebuild(context));
    CHECK(legacyFeature.shape());
    CHECK(!legacyFeature.topologyLineage());
    CHECK(legacyFeature.topologyIndex());
  }

  // Standard_Failure and unknown-exception rollback remain covered by the
  // existing document_tests guarded ShapeFeature boundary matrix.

  return EXIT_SUCCESS;
}
