#include "TestAssertions.h"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/ImportedShapeFeature.h"
#include "model/JoinBodiesFeature.h"
#include "model/JoinBodiesToolSession.h"
#include "model/MoveFeature.h"
#include "model/TopologyReferenceResolver.h"
#include "project/ProjectFile.h"

namespace {

double volumeOf(const TopoDS_Shape& shape) {
  GProp_GProps properties;
  BRepGProp::VolumeProperties(shape, properties);
  return properties.Mass();
}

solidar::ShapeFeature::ShapePtr boxAt(double x) {
  gp_Trsf translation;
  translation.SetTranslation(gp_Vec(x, 0.0, 0.0));
  return std::make_shared<TopoDS_Shape>(BRepBuilderAPI_Transform(
      BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape(), translation, true)
                                            .Shape());
}

struct ExtrudedSource {
  solidar::BodyId bodyId{};
  solidar::FeatureId featureId{};
  solidar::SketchId sketchId{};
  solidar::ExtrudeFeature* feature{};
};

ExtrudedSource addExtrudedRectangle(solidar::Document& document,
                                    std::string name, double x0, double y0,
                                    double x1, double y1) {
  auto& sketch = document.addSketch(name + " sketch");
  sketch.geometry.addRectangle({x0, y0}, {x1, y1});
  const auto sketchId = sketch.id;
  auto& body = document.addBody(name + " body");
  auto extrusion =
      std::make_unique<solidar::ExtrudeFeature>(sketchId, 10.0, name);
  auto* feature = extrusion.get();
  const auto featureId = feature->id();
  const auto bodyId = body.id();
  body.addFeature(std::move(extrusion));
  return {bodyId, featureId, sketchId, feature};
}

class PartialLineageFeature final : public solidar::ShapeFeature {
 public:
  explicit PartialLineageFeature(ShapePtr source)
      : ShapeFeature("Partial lineage"), source_(std::move(source)) {}

  [[nodiscard]] std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<PartialLineageFeature>(*this);
  }

 protected:
  bool rebuildImpl(const solidar::RebuildContext&) override {
    if (!source_ || source_->IsNull()) {
      clearShape();
      markError("Partial-lineage test source is missing");
      return false;
    }
    TopExp_Explorer faces(*source_, TopAbs_FACE);
    if (!faces.More()) {
      clearShape();
      markError("Partial-lineage test source has no faces");
      return false;
    }
    solidar::TopologyProvenanceKey key{
        id(), solidar::TopologyKind::Face,
        solidar::TopologyProvenanceOperation::Seed,
        solidar::TopologySemanticRole::Custom, "partial:test"};
    std::string error;
    auto lineage = solidar::TopologyLineage::create(
        {{std::move(key),
          std::make_shared<TopoDS_Shape>(faces.Current())}},
        solidar::TopologyLineageCoverage::PartialFaces, &error);
    if (!lineage) {
      clearShape();
      markError(std::move(error));
      return false;
    }
    setShapeAndTopologyLineage(source_, std::move(lineage));
    markValid();
    return true;
  }

 private:
  ShapePtr source_;
};

const solidar::TopologyLineageBinding* bindingWithOperation(
    const solidar::TopologyLineage& lineage,
    solidar::TopologyProvenanceOperation operation) {
  const auto found = std::find_if(
      lineage.bindings().begin(), lineage.bindings().end(),
      [operation](const auto& binding) {
        return binding.provenance.kind == solidar::TopologyKind::Face &&
               binding.provenance.operation == operation;
      });
  return found == lineage.bindings().end() ? nullptr : &*found;
}

std::size_t bindingCountWithOperation(
    const solidar::TopologyLineage& lineage,
    solidar::TopologyProvenanceOperation operation) {
  return static_cast<std::size_t>(std::count_if(
      lineage.bindings().begin(), lineage.bindings().end(),
      [operation](const auto& binding) {
        return binding.provenance.kind == solidar::TopologyKind::Face &&
               binding.provenance.operation == operation;
      }));
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);

  // Session preview accepts a real union and keeps invalid/disconnected input
  // non-committable without modifying either source.
  solidar::JoinBodiesToolSession session;
  session.begin();
  CHECK(session.lifecycle() == solidar::ToolLifecycle::SelectingInput);
  session.setBodies({{1, 11, boxAt(0.0)}, {2, 22, boxAt(5.0)}});
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  CHECK(session.previewShape());
  CHECK(std::abs(volumeOf(*session.previewShape()) - 1500.0) < 1e-5);
  session.setBodies({{1, 11, boxAt(0.0)}, {2, 22, boxAt(20.0)}});
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
  CHECK(!session.previewShape());
  CHECK(!session.error().empty());
  CHECK(session.errorCode() ==
        solidar::OperationFailureCode::BodiesDoNotTouch);
  session.cancel();
  CHECK(session.lifecycle() == solidar::ToolLifecycle::Inactive);

  // A JoinBodies feature is a cross-Body dependency. The result rebuilds when
  // either source Feature changes and stores no raw B-Rep in the document.
  solidar::Document document;
  auto& firstBody = document.addBody("First");
  auto& firstFeature = firstBody.addFeature(
      std::make_unique<solidar::ImportedShapeFeature>(boxAt(0.0), "Box A"));
  const auto firstBodyId = firstBody.id();
  const auto firstFeatureId = firstFeature.id();

  auto& secondBody = document.addBody("Second");
  auto& secondBase = secondBody.addFeature(
      std::make_unique<solidar::ImportedShapeFeature>(boxAt(0.0), "Box B"));
  auto& moved = secondBody.addFeature(std::make_unique<solidar::MoveFeature>(
      secondBase.id(), solidar::Vector3d{5.0, 0.0, 0.0}, "Move B"));
  const auto secondBodyId = secondBody.id();
  const auto movedId = moved.id();

  auto& resultBody = document.addBody("Joined");
  auto& joined = resultBody.addFeature(
      std::make_unique<solidar::JoinBodiesFeature>(
          firstBodyId, firstFeatureId, secondBodyId, movedId, "Join"));
  const auto resultBodyId = resultBody.id();
  const auto joinedId = joined.id();
  document.findBody(firstBodyId)->setVisible(false);
  document.findBody(secondBodyId)->setVisible(false);
  CHECK(document.recompute());
  CHECK(joined.isValid() && joined.hasShape());
  CHECK(std::abs(volumeOf(*joined.shape()) - 1500.0) < 1e-5);

  auto* move = dynamic_cast<solidar::MoveFeature*>(
      document.findBody(secondBodyId)->features().back().get());
  CHECK(move);
  move->setOffsetMm({4.0, 0.0, 0.0});
  CHECK(document.recomputeFrom(move->id()));
  const auto* rebuilt = document.findBody(resultBodyId)->activeFeature();
  CHECK(rebuilt && rebuilt->id() == joinedId && rebuilt->isValid());
  CHECK(std::abs(volumeOf(*rebuilt->shape()) - 1400.0) < 1e-5);

  // Save/load preserves dependency IDs and consumed-body visibility.
  QTemporaryDir directory(
      QDir::current().filePath(QStringLiteral("join-bodies-tests-XXXXXX")));
  CHECK(directory.isValid());
  const QString path = directory.filePath(QStringLiteral("joined.solidar"));
  QString error;
  CHECK(solidar::project::ProjectFile::saveDocument(path, document, &error));
  solidar::Document restored;
  CHECK(solidar::project::ProjectFile::loadDocument(path, &restored, &error));
  CHECK(restored.bodies().size() == 3);
  CHECK(!restored.findBody(firstBodyId)->visible());
  CHECK(!restored.findBody(secondBodyId)->visible());
  CHECK(restored.findBody(resultBodyId)->visible());
  const auto* restoredJoin = dynamic_cast<const solidar::JoinBodiesFeature*>(
      restored.findBody(resultBodyId)->activeFeature());
  CHECK(restoredJoin && restoredJoin->id() == joinedId);
  CHECK(restoredJoin->firstBodyId() == firstBodyId);
  CHECK(restoredJoin->secondBodyId() == secondBodyId);
  CHECK(restoredJoin->firstFeatureId() == firstFeatureId);
  CHECK(restoredJoin->secondFeatureId() == movedId);
  CHECK(restoredJoin->isValid() && restoredJoin->hasShape());
  CHECK(std::abs(volumeOf(*restoredJoin->shape()) - 1400.0) < 1e-5);

  // Two complete face-lineage sources take the strict boolean-history path.
  // The result owns exact V2 identities for retained and merged faces.
  {
    solidar::Document topologyDocument;
    const auto first = addExtrudedRectangle(
        topologyDocument, "Topology A", 0.0, 0.0, 10.0, 10.0);
    const auto secondBase = addExtrudedRectangle(
        topologyDocument, "Topology B", 0.0, 0.0, 10.0, 10.0);
    auto& secondBody = *topologyDocument.findBody(secondBase.bodyId);
    auto move = std::make_unique<solidar::MoveFeature>(
        secondBase.featureId, solidar::Vector3d{5.0, 0.0, 0.0},
        "Move topology B");
    auto* moveFeature = move.get();
    const auto moveId = moveFeature->id();
    secondBody.addFeature(std::move(move));

    auto& resultBody = topologyDocument.addBody("Topology join result");
    const auto resultBodyId = resultBody.id();
    auto join = std::make_unique<solidar::JoinBodiesFeature>(
        first.bodyId, first.featureId, secondBase.bodyId, moveId,
        "Topology join");
    auto* joinFeature = join.get();
    const auto joinId = joinFeature->id();
    resultBody.addFeature(std::move(join));

    CHECK(topologyDocument.recompute());
    CHECK(joinFeature->isValid());
    const auto lineage = joinFeature->topologyLineage();
    CHECK(lineage);
    CHECK(lineage->coverage() ==
          solidar::TopologyLineageCoverage::CompleteFaces);
    CHECK(static_cast<std::size_t>(std::count_if(
              lineage->bindings().begin(), lineage->bindings().end(),
              [](const auto& binding) {
                return binding.provenance.kind ==
                       solidar::TopologyKind::Face;
              })) == 6);
    CHECK(bindingCountWithOperation(
              *lineage, solidar::TopologyProvenanceOperation::Retained) == 2);
    CHECK(bindingCountWithOperation(
              *lineage, solidar::TopologyProvenanceOperation::Merge) == 4);
    const auto* retained = bindingWithOperation(
        *lineage, solidar::TopologyProvenanceOperation::Retained);
    const auto* merged = bindingWithOperation(
        *lineage, solidar::TopologyProvenanceOperation::Merge);
    CHECK(retained);
    CHECK(merged);

    const auto retainedReference = solidar::FaceReference::fromV2(
        {resultBodyId, retained->provenance});
    const auto mergedReference = solidar::FaceReference::fromV2(
        {resultBodyId, merged->provenance});
    auto index = joinFeature->topologyIndex();
    CHECK(index);
    CHECK(index->resolveFace(retainedReference));
    CHECK(index->resolveFace(mergedReference));

    std::vector<solidar::FaceReference> savedReferences;
    savedReferences.reserve(lineage->bindings().size());
    for (const auto& binding : lineage->bindings()) {
      if (binding.provenance.kind != solidar::TopologyKind::Face) continue;
      const auto exact = solidar::FaceReference::fromV2(
          {resultBodyId, binding.provenance});
      const auto resolved = index->resolveFace(exact);
      CHECK(resolved);
      const auto factoryReference = topologyDocument.createFaceReferenceV2(
          resultBodyId, joinId, resolved.index);
      CHECK(factoryReference);
      CHECK(factoryReference.reference == exact);
      savedReferences.push_back(exact);
    }

    const auto created = topologyDocument.createFaceReferenceV2(
        resultBodyId, joinId, index->resolveFace(mergedReference).index);
    CHECK(created);
    CHECK(created.reference.bodyId() == resultBodyId);
    CHECK(created.reference.featureId() == joinId);
    CHECK(created.reference.v2());
    CHECK(created.reference.v2()->provenance == merged->provenance);
    const auto wrongOwner = topologyDocument.createFaceReferenceV2(
        first.bodyId, joinId, 0);
    CHECK(!wrongOwner);
    CHECK(wrongOwner.status ==
          solidar::FaceReferenceV2CreationStatus::InvalidReference);

    moveFeature->setOffsetMm({4.0, 0.0, 0.0});
    CHECK(topologyDocument.recomputeFrom(moveId));
    index = joinFeature->topologyIndex();
    CHECK(index);
    for (const auto& reference : savedReferences)
      CHECK(index->resolveFace(reference));

    joinFeature->setInputs(secondBase.bodyId, moveId, first.bodyId,
                           first.featureId);
    CHECK(topologyDocument.recomputeFrom(joinId));
    index = joinFeature->topologyIndex();
    CHECK(index);
    for (const auto& reference : savedReferences)
      CHECK(index->resolveFace(reference));

    // A real split-history ambiguity is a hard failure. It must not fall back
    // to the legacy fuse or expose a partially committed result.
    const auto lastShape = joinFeature->shape();
    const auto lastLineage = joinFeature->topologyLineage();
    const auto lastIndex = joinFeature->topologyIndex();
    const auto lastRevision = joinFeature->shapeRevision();
    solidar::sketch::Sketch crossing;
    crossing.addRectangle({0.0, -5.0}, {2.0, 15.0});
    CHECK(topologyDocument.replaceSketchGeometry(secondBase.sketchId,
                                                  crossing));
    moveFeature->setOffsetMm({4.0, 0.0, 0.0});
    CHECK(!topologyDocument.recompute());
    CHECK(joinFeature->isFailed());
    CHECK(!joinFeature->shape());
    CHECK(!joinFeature->topologyLineage());
    CHECK(joinFeature->shapeRevision() == lastRevision);
    CHECK(joinFeature->lastValidShape() == lastShape);
    CHECK(joinFeature->lastValidTopologyLineage() == lastLineage);
    CHECK(joinFeature->lastValidTopologyIndex() == lastIndex);

    solidar::sketch::Sketch recovered;
    recovered.addRectangle({0.0, 0.0}, {10.0, 10.0});
    CHECK(topologyDocument.replaceSketchGeometry(secondBase.sketchId,
                                                  recovered));
    moveFeature->setOffsetMm({5.0, 0.0, 0.0});
    CHECK(topologyDocument.recompute());
    CHECK(joinFeature->isValid());
    CHECK(joinFeature->shapeRevision() > lastRevision);
    for (const auto& reference : savedReferences)
      CHECK(joinFeature->topologyIndex()->resolveFace(reference));
  }

  // A present but incomplete lineage is never eligible for legacy fallback.
  {
    solidar::Document partialDocument;
    auto& partialBody = partialDocument.addBody("Partial source");
    auto partial =
        std::make_unique<PartialLineageFeature>(boxAt(0.0));
    const auto partialBodyId = partialBody.id();
    const auto partialId = partial->id();
    partialBody.addFeature(std::move(partial));
    auto& legacyBody = partialDocument.addBody("Legacy source");
    auto& legacy = legacyBody.addFeature(
        std::make_unique<solidar::ImportedShapeFeature>(boxAt(5.0)));
    const auto legacyBodyId = legacyBody.id();
    const auto legacyId = legacy.id();
    auto& result = partialDocument.addBody("Rejected join");
    auto& rejected = result.addFeature(
        std::make_unique<solidar::JoinBodiesFeature>(
            partialBodyId, partialId, legacyBodyId, legacyId));
    CHECK(!partialDocument.recompute());
    CHECK(rejected.isFailed());
    CHECK(!rejected.hasShape());
    CHECK(!rejected.topologyLineage());
  }

  // One missing lineage keeps staged compatibility and produces no lineage.
  {
    solidar::Document mixedDocument;
    const auto lineaged = addExtrudedRectangle(
        mixedDocument, "Lineaged", 0.0, 0.0, 10.0, 10.0);
    auto& legacyBody = mixedDocument.addBody("Legacy");
    auto& legacy = legacyBody.addFeature(
        std::make_unique<solidar::ImportedShapeFeature>(boxAt(5.0)));
    const auto legacyBodyId = legacyBody.id();
    const auto legacyId = legacy.id();
    auto& result = mixedDocument.addBody("Mixed result");
    auto& mixed = result.addFeature(
        std::make_unique<solidar::JoinBodiesFeature>(
            lineaged.bodyId, lineaged.featureId, legacyBodyId, legacyId));
    CHECK(mixedDocument.recompute());
    CHECK(mixed.isValid());
    CHECK(mixed.hasShape());
    CHECK(!mixed.topologyLineage());
    const auto mixedIndex = mixed.topologyIndex();
    CHECK(mixedIndex);
    CHECK(mixedIndex->rawEdgeCount() > 0);
    const auto unavailable =
        mixed.createEdgeReferenceV2(result.id(), 0);
    CHECK(!unavailable);
    CHECK(unavailable.status ==
          solidar::EdgeReferenceV2CreationStatus::Missing);
    CHECK(!unavailable.reference.v2());
  }

  // Self-joining one Body is invalid even if two Feature ids are supplied.
  {
    solidar::Document sameBodyDocument;
    auto& source = sameBodyDocument.addBody("Same source");
    auto& first = source.addFeature(
        std::make_unique<solidar::ImportedShapeFeature>(boxAt(0.0)));
    auto& second = source.addFeature(
        std::make_unique<solidar::ImportedShapeFeature>(boxAt(5.0)));
    const auto sourceBodyId = source.id();
    const auto firstId = first.id();
    const auto secondId = second.id();
    auto& result = sameBodyDocument.addBody("Same body result");
    auto& rejected = result.addFeature(
        std::make_unique<solidar::JoinBodiesFeature>(
            sourceBodyId, firstId, sourceBodyId, secondId));
    CHECK(!sameBodyDocument.recompute());
    CHECK(rejected.isFailed());
    CHECK(!rejected.hasShape());
  }

  // The result Feature cannot live in either source Body: that would make a
  // Part Design dependency look like an in-place assembly operation.
  {
    solidar::Document aliasDocument;
    auto& firstBody = aliasDocument.addBody("Aliased result");
    auto& first = firstBody.addFeature(
        std::make_unique<solidar::ImportedShapeFeature>(boxAt(0.0)));
    const auto firstBodyId = firstBody.id();
    const auto firstId = first.id();
    auto& secondBody = aliasDocument.addBody("Other source");
    auto& second = secondBody.addFeature(
        std::make_unique<solidar::ImportedShapeFeature>(boxAt(5.0)));
    const auto secondBodyId = secondBody.id();
    const auto secondId = second.id();
    auto& aliasedBody = *aliasDocument.findBody(firstBodyId);
    auto& rejected = aliasedBody.addFeature(
        std::make_unique<solidar::JoinBodiesFeature>(
            firstBodyId, firstId, secondBodyId, secondId));
    CHECK(!aliasDocument.recompute());
    CHECK(rejected.isFailed());
    CHECK(!rejected.hasShape());
  }

  return EXIT_SUCCESS;
}
