#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "model/ImportedShapeFeature.h"
#include "model/IdGeneration.h"
#include "model/ProductDocument.h"
#include "model/TopologyLineage.h"
#include "model/TopologyReferenceResolver.h"
#include "presentation/ComponentPresentationBuilder.h"

namespace solidar {

class ComponentDrawingValidationTestAccess final {
 public:
  static void append(ComponentDefinition& definition,
                     ComponentDrawing drawing) {
    definition.drawings_.push_back(std::move(drawing));
  }
};

}  // namespace solidar

namespace {

using namespace solidar;

constexpr ProductPresentationEpoch kEpoch{8101};
constexpr AssemblyMeshProfile kNormal{AssemblyMeshQuality::Normal};

class ControlledShapeFeature final : public ShapeFeature {
 public:
  ControlledShapeFeature(
      FeatureId id, ShapePtr source, std::string name,
      std::shared_ptr<const TopologyLineage> lineage = {})
      : ShapeFeature(id, std::move(name)),
        source_(std::move(source)),
        lineage_(std::move(lineage)) {}

  [[nodiscard]] std::unique_ptr<Feature> clone() const override {
    return std::make_unique<ControlledShapeFeature>(*this);
  }

  void setFailure(bool fail) noexcept { fail_ = fail; }

 protected:
  bool rebuildImpl(const RebuildContext&) override {
    if (fail_) {
      clearShape();
      markError("controlled component presentation failure");
      return false;
    }
    if (lineage_)
      setShapeAndTopologyLineage(source_, lineage_);
    else
      setShape(source_);
    markValid();
    return true;
  }

 private:
  ShapePtr source_;
  std::shared_ptr<const TopologyLineage> lineage_;
  bool fail_{};
};

ShapeFeature::ShapePtr box(double x, double y, double z) {
  TopoDS_Shape shape = BRepPrimAPI_MakeBox(x, y, z).Shape();
  BRepTools::Clean(shape);
  return std::make_shared<const TopoDS_Shape>(std::move(shape));
}

std::shared_ptr<const TopologyLineage> singleFaceLineage(
    const ShapeFeature::ShapePtr& shape, FeatureId producer) {
  CHECK(shape);
  TopExp_Explorer explorer(*shape, TopAbs_FACE);
  CHECK(explorer.More());
  TopologyProvenanceKey provenance{
      producer, TopologyKind::Face,
      TopologyProvenanceOperation::Generated,
      TopologySemanticRole::Side, "presentation:retained"};
  auto lineage = TopologyLineage::create(
      {{std::move(provenance),
        std::make_shared<const TopoDS_Shape>(explorer.Current())}},
      TopologyLineageCoverage::PartialFaces);
  CHECK(lineage);
  return lineage;
}

std::unique_ptr<Document> bodyDocument(BodyId bodyId, FeatureId featureId,
                                       bool bodyVisible = true) {
  auto document = std::make_unique<Document>();
  Body& body = document->addBody(bodyId, "component body");
  body.setVisible(bodyVisible);
  body.addFeature(std::make_unique<ImportedShapeFeature>(
      featureId, box(2.0, 3.0, 4.0), "component feature"));
  CHECK(document->recompute());
  return document;
}

RigidTransform3d translation(double x, double y, double z) {
  const auto created = RigidTransform3d::create(
      {x, y, z}, {1.0, 0.0, 0.0, 0.0});
  CHECK(created);
  return *created.transform;
}

RigidTransform3d z90WithTranslation(double x, double y, double z) {
  const double halfSqrt = std::sqrt(0.5);
  const auto created = RigidTransform3d::create(
      {x, y, z}, {halfSqrt, 0.0, 0.0, halfSqrt});
  CHECK(created);
  return *created.transform;
}

std::optional<OccurrencePath> occurrencePath(
    std::initializer_list<std::uint64_t> values) {
  std::vector<OccurrenceId> entries;
  entries.reserve(values.size());
  for (const auto value : values) entries.emplace_back(value);
  auto created = OccurrencePath::create(std::move(entries));
  CHECK(created);
  return std::move(created.path);
}

std::unique_ptr<ProductDocument> componentProduct(
    std::vector<ComponentDefinition> definitions, DefinitionId rootId) {
  auto created = ProductDocument::createComponentGraph(
      std::move(definitions), rootId);
  CHECK(created);
  return std::move(created.product);
}

ProductIncarnationId nextIncarnationProbe() {
  DefinitionRegistry registry;
  CHECK(registry.insert(PartDefinition(DefinitionId{900001},
                                       std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(std::move(registry),
                                         DefinitionId{900001});
  CHECK(created);
  return created.product->incarnation();
}

ShapeRevision nextShapeRevisionProbe() {
  detail::ScopedExplicitIdReservationPause pause;
  Document document;
  Body& body = document.addBody(BodyId{900002}, "revision probe");
  auto& feature = body.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{900003}, box(1.0, 1.0, 1.0), "revision probe"));
  CHECK(document.recompute());
  return feature.shapeRevision();
}

struct GlobalIdCursorSample final {
  ProductIncarnationId incarnation;
  BodyId bodyId{};
  FeatureId featureId{};
  SketchId sketchId{};
  ShapeRevision shapeRevision{};
};

GlobalIdCursorSample sampleGlobalIdCursors() {
  const auto incarnation = nextIncarnationProbe();
  Body body("body cursor probe");
  ImportedShapeFeature feature(box(1.0, 1.0, 1.0),
                               "feature cursor probe");
  Document sketchDocument;
  const SketchId sketchId =
      sketchDocument.addSketch("sketch cursor probe").id;
  return {incarnation, body.id(), feature.id(), sketchId,
          nextShapeRevisionProbe()};
}

void checkExactlyOneProbeStep(const GlobalIdCursorSample& before,
                              const GlobalIdCursorSample& after) {
  CHECK(after.incarnation.value() == before.incarnation.value() + 1);
  CHECK(after.bodyId == before.bodyId + 1);
  CHECK(after.featureId == before.featureId + 1);
  CHECK(after.sketchId == before.sketchId + 1);
  CHECK(after.shapeRevision == before.shapeRevision + 1);
}

const ComponentPresentationInstance& findInstance(
    const ComponentPresentationSnapshot& snapshot, DefinitionId definitionId,
    const std::optional<OccurrencePath>& path, BodyId bodyId) {
  for (const auto& instance : snapshot.instances()) {
    const auto& address = instance.componentAddress();
    if (address.definitionId == definitionId &&
        address.occurrencePath == path &&
        instance.bodyIdentity().bodyId == bodyId)
      return instance;
  }
  CHECK(false);
  std::abort();
}

void verifyRootOnlyComponent() {
  std::vector<ComponentDefinition> definitions;
  definitions.emplace_back(DefinitionId{10},
                           bodyDocument(BodyId{100}, FeatureId{1000}));
  auto product = componentProduct(std::move(definitions), DefinitionId{10});

  const auto idsBefore = sampleGlobalIdCursors();
  const auto built =
      ComponentPresentationBuilder::build(*product, kEpoch, kNormal);
  const auto idsAfter = sampleGlobalIdCursors();
  checkExactlyOneProbeStep(idsBefore, idsAfter);
  CHECK(built);
  CHECK(built.snapshot->sources().size() == 1);
  CHECK(built.snapshot->instances().size() == 1);
  CHECK(built.snapshot->graphTraversal().visits.size() == 1);
  const auto& instance = built.snapshot->instances().front();
  CHECK(instance.componentAddress().rootInstance());
  CHECK(instance.componentAddress().rootDefinitionId == DefinitionId{10});
  CHECK(instance.componentAddress().definitionId == DefinitionId{10});
  CHECK(instance.worldTransform() == RigidTransform3d::identity());
  CHECK(instance.componentVisible());
  CHECK(!instance.suppressed());
  CHECK(!instance.grounded());
  CHECK(instance.effectivelyVisible());

  const auto repeated =
      ComponentPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(repeated);
  CHECK(built.snapshot->semanticallyEquivalent(*repeated.snapshot));
  const auto otherEpoch = ComponentPresentationBuilder::build(
      *product, ProductPresentationEpoch{8102}, kNormal);
  CHECK(otherEpoch);
  CHECK(!built.snapshot->semanticallyEquivalent(*otherEpoch.snapshot));

  TopoDS_Shape otherAddressToken;
  ComponentMeshSourceKey addressVariant = built.snapshot->sources()[0].key();
  addressVariant.shapeIdentity = &otherAddressToken;
  CHECK(addressVariant != built.snapshot->sources()[0].key());
  CHECK(addressVariant.semanticKey() ==
        built.snapshot->sources()[0].key().semanticKey());

  const auto high = ComponentPresentationBuilder::build(
      *product, kEpoch,
      AssemblyMeshProfile{AssemblyMeshQuality::High});
  CHECK(high);
  CHECK(high.snapshot->sources()[0].key().shapeIdentity ==
        built.snapshot->sources()[0].key().shapeIdentity);
  CHECK(high.snapshot->sources()[0].key().semanticKey() !=
        built.snapshot->sources()[0].key().semanticKey());
  CHECK(!built.snapshot->semanticallyEquivalent(*high.snapshot));
}

void verifyExpandedSharedGraph() {
  constexpr DefinitionId rootId{300};
  constexpr DefinitionId middleId{200};
  constexpr DefinitionId leafId{100};
  constexpr BodyId localBodyId{7};
  constexpr FeatureId localFeatureId{70};

  ComponentDefinition root(rootId,
                           bodyDocument(localBodyId, localFeatureId));
  ComponentDefinition middle(middleId,
                             bodyDocument(localBodyId, localFeatureId));
  ComponentDefinition leaf(leafId,
                           bodyDocument(localBodyId, localFeatureId));

  const auto rootPlacement = z90WithTranslation(10.0, 0.0, 0.0);
  const auto childPlacement = translation(2.0, 0.0, 0.0);
  CHECK(middle.insert(ComponentOccurrence(
            OccurrenceId{20}, leafId, childPlacement,
            {.visible = true, .suppressed = false, .grounded = true})) ==
        ComponentOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{10}, middleId, rootPlacement,
            {.visible = true, .suppressed = false, .grounded = false})) ==
        ComponentOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{11}, middleId, RigidTransform3d::identity(),
            {.visible = false, .suppressed = true, .grounded = false})) ==
        ComponentOccurrenceInsertStatus::Inserted);

  // Deliberately noncanonical authored vector order.  Published sources and
  // instances must depend only on stable IDs and paths.
  std::vector<ComponentDefinition> definitions;
  definitions.push_back(std::move(middle));
  definitions.push_back(std::move(root));
  definitions.push_back(std::move(leaf));
  auto product = componentProduct(std::move(definitions), rootId);

  const auto built =
      ComponentPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(built);
  CHECK(built.snapshot->sources().size() == 3);
  CHECK(built.snapshot->instances().size() == 5);
  CHECK(built.snapshot->graphTraversal().visits.size() == 5);
  CHECK(built.snapshot->instances().front().componentAddress().rootInstance());
  CHECK(built.snapshot->sources()[0].key().definitionId == leafId);
  CHECK(built.snapshot->sources()[1].key().definitionId == middleId);
  CHECK(built.snapshot->sources()[2].key().definitionId == rootId);
  CHECK(built.snapshot->sources()[0].key().bodyId == localBodyId);
  CHECK(built.snapshot->sources()[1].key().bodyId == localBodyId);
  CHECK(built.snapshot->sources()[0].key().featureId == localFeatureId);
  CHECK(built.snapshot->sources()[1].key().featureId == localFeatureId);
  CHECK(built.snapshot->sources()[0].key().semanticKey() !=
        built.snapshot->sources()[1].key().semanticKey());

  const auto firstMiddlePath = occurrencePath({10});
  const auto firstLeafPath = occurrencePath({10, 20});
  const auto hiddenMiddlePath = occurrencePath({11});
  const auto hiddenLeafPath = occurrencePath({11, 20});
  const auto& firstMiddle = findInstance(
      *built.snapshot, middleId, firstMiddlePath, localBodyId);
  const auto& firstLeaf =
      findInstance(*built.snapshot, leafId, firstLeafPath, localBodyId);
  const auto& hiddenMiddle = findInstance(
      *built.snapshot, middleId, hiddenMiddlePath, localBodyId);
  const auto& hiddenLeaf =
      findInstance(*built.snapshot, leafId, hiddenLeafPath, localBodyId);
  const auto composed =
      RigidTransform3d::compose(rootPlacement, childPlacement);
  const auto reversed =
      RigidTransform3d::compose(childPlacement, rootPlacement);
  CHECK(composed);
  CHECK(reversed);
  CHECK(*composed.transform != *reversed.transform);
  CHECK(std::abs(composed.transform->translationMm()[0] - 10.0) < 1e-12);
  CHECK(std::abs(composed.transform->translationMm()[1] - 2.0) < 1e-12);
  CHECK(std::abs(reversed.transform->translationMm()[0] - 12.0) < 1e-12);
  CHECK(std::abs(reversed.transform->translationMm()[1]) < 1e-12);
  CHECK(firstMiddle.worldTransform() == rootPlacement);
  CHECK(firstLeaf.worldTransform() == *composed.transform);
  CHECK(firstLeaf.componentVisible());
  CHECK(!firstLeaf.suppressed());
  CHECK(firstLeaf.grounded());
  CHECK(firstLeaf.effectivelyVisible());
  CHECK(!hiddenMiddle.componentVisible());
  CHECK(hiddenMiddle.suppressed());
  CHECK(!hiddenMiddle.effectivelyVisible());
  CHECK(!hiddenLeaf.componentVisible());
  CHECK(hiddenLeaf.suppressed());
  CHECK(hiddenLeaf.grounded());
  CHECK(!hiddenLeaf.effectivelyVisible());

  // Both shared instances use the same definition-owned mesh source.
  CHECK(firstMiddle.sourceKey() == hiddenMiddle.sourceKey());
  CHECK(firstLeaf.sourceKey() == hiddenLeaf.sourceKey());
}

void verifyRetainedLastValidShape() {
  auto document = std::make_unique<Document>();
  Body& body = document->addBody(BodyId{8}, "retained");
  const auto retainedShape = box(5.0, 5.0, 5.0);
  const auto lineage = singleFaceLineage(retainedShape, FeatureId{80});
  auto controlled = std::make_unique<ControlledShapeFeature>(
      FeatureId{80}, retainedShape, "retained feature", lineage);
  ControlledShapeFeature* feature = controlled.get();
  body.addFeature(std::move(controlled));
  CHECK(document->recompute());

  std::vector<ComponentDefinition> definitions;
  definitions.emplace_back(DefinitionId{1}, std::move(document));
  auto product = componentProduct(std::move(definitions), DefinitionId{1});
  const std::uint64_t topologyBuildsBefore =
      TopologyIndex::buildAttemptCount();
  const auto before =
      ComponentPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(before);
  CHECK(TopologyIndex::buildAttemptCount() == topologyBuildsBefore);
  const auto keyBefore = before.snapshot->sources().front().key();
  const auto* shapeBefore = before.snapshot->sources().front().shape().get();
  CHECK(feature->lastValidTopologyLineage() == lineage);
  CHECK(before.snapshot->sources().front().topologyLineage() == lineage);

  feature->setFailure(true);
  feature->setDirty();
  auto& live = const_cast<Document&>(
      product->rootComponentDefinition()->document());
  CHECK(!live.recompute());
  CHECK(!feature->shape());
  CHECK(feature->lastValidShape().get() == shapeBefore);
  const auto after =
      ComponentPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(after);
  CHECK(TopologyIndex::buildAttemptCount() == topologyBuildsBefore);
  CHECK(after.snapshot->sources().front().key() == keyBefore);
  CHECK(after.snapshot->sources().front().shape().get() == shapeBefore);
  CHECK(after.snapshot->sources().front().topologyLineage() == lineage);

  const auto retainedSnapshot = after.snapshot;
  product.reset();
  CHECK(retainedSnapshot->sources().front().shape());
  CHECK(retainedSnapshot->sources().front().shape().get() == shapeBefore);
  CHECK(!retainedSnapshot->sources().front().shape()->IsNull());
  CHECK(retainedSnapshot->sources().front().topologyLineage() == lineage);
}

void verifyTypedFailuresAndGraphLimits() {
  auto document = bodyDocument(BodyId{1}, FeatureId{1});
  DefinitionRegistry registry;
  CHECK(registry.insert(PartDefinition(DefinitionId{1},
                                       std::move(document))) ==
        DefinitionInsertStatus::Inserted);
  auto partCreated = ProductDocument::create(std::move(registry),
                                             DefinitionId{1});
  CHECK(partCreated);
  CHECK(ComponentPresentationBuilder::build(
            *partCreated.product, kEpoch, kNormal)
            .status == ComponentPresentationBuildStatus::RootIsNotComponent);
  CHECK(ComponentPresentationBuilder::build(
            *partCreated.product, kInvalidProductPresentationEpoch, kNormal)
            .status == ComponentPresentationBuildStatus::InvalidEpoch);

  ComponentDefinition root(DefinitionId{10}, std::make_unique<Document>());
  ComponentDefinition leaf(DefinitionId{20}, std::make_unique<Document>());
  std::vector<ComponentDefinition> components;
  components.push_back(std::move(root));
  components.push_back(std::move(leaf));
  auto product = componentProduct(std::move(components), DefinitionId{10});
  auto* mutableRoot = const_cast<ComponentDefinition*>(
      product->rootComponentDefinition());
  CHECK(mutableRoot != nullptr);
  for (std::uint64_t id = 1; id <= 4097; ++id) {
    CHECK(mutableRoot->insert(ComponentOccurrence(
              OccurrenceId{id}, DefinitionId{20},
              RigidTransform3d::identity())) ==
          ComponentOccurrenceInsertStatus::Inserted);
  }
  const auto limited =
      ComponentPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(!limited);
  CHECK(!limited.snapshot);
  CHECK(limited.status ==
        ComponentPresentationBuildStatus::ResourceLimitExceeded);
  CHECK(!limited.diagnostic().empty());
}

void verifyFailureAtomicityAndDuplicateIdentity() {
  auto lateInvalid = std::make_unique<Document>();
  Body& validBody = lateInvalid->addBody(BodyId{10}, "valid first");
  validBody.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{100}, box(2.0, 3.0, 4.0), "valid first"));
  CHECK(lateInvalid->recompute());
  Body& missingBody = lateInvalid->addBody(BodyId{20}, "invalid second");
  missingBody.addFeature(std::make_unique<ControlledShapeFeature>(
      FeatureId{200}, box(1.0, 1.0, 1.0), "never rebuilt"));
  std::vector<ComponentDefinition> lateDefinitions;
  lateDefinitions.emplace_back(DefinitionId{1}, std::move(lateInvalid));
  auto lateProduct =
      componentProduct(std::move(lateDefinitions), DefinitionId{1});

  const auto idsBefore = sampleGlobalIdCursors();
  const auto lateFailure =
      ComponentPresentationBuilder::build(*lateProduct, kEpoch, kNormal);
  const auto idsAfter = sampleGlobalIdCursors();
  checkExactlyOneProbeStep(idsBefore, idsAfter);
  CHECK(!lateFailure);
  CHECK(!lateFailure.snapshot);
  CHECK(lateFailure.status ==
        ComponentPresentationBuildStatus::MissingRetainedShape);

  Document seed;
  Body& seedBody = seed.addBody(BodyId{30}, "duplicate seed");
  seedBody.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{300}, box(2.0, 2.0, 2.0), "duplicate seed"));
  CHECK(seed.recompute());
  const Body duplicate(seed.bodies().front());
  auto duplicateDocument = std::make_unique<Document>();
  CHECK(duplicateDocument->applyBodySlice(
      0, std::optional<Body>(std::in_place, duplicate)));
  CHECK(duplicateDocument->applyBodySlice(
      1, std::optional<Body>(std::in_place, duplicate)));
  std::vector<ComponentDefinition> duplicateDefinitions;
  duplicateDefinitions.emplace_back(DefinitionId{2},
                                    std::move(duplicateDocument));
  auto duplicateProduct =
      componentProduct(std::move(duplicateDefinitions), DefinitionId{2});
  const auto duplicateFailure = ComponentPresentationBuilder::build(
      *duplicateProduct, kEpoch, kNormal);
  CHECK(!duplicateFailure);
  CHECK(!duplicateFailure.snapshot);
  CHECK(duplicateFailure.status ==
        ComponentPresentationBuildStatus::DuplicateStableIdentity);
}

void verifySourceLimit() {
  auto document = std::make_unique<Document>();
  const auto sharedShape = box(1.0, 1.0, 1.0);
  {
    detail::ScopedExplicitIdReservationPause pause;
    for (std::size_t index = 0;
         index <= ComponentPresentationLimits::kMaximumSources; ++index) {
      const auto offset = static_cast<std::uint64_t>(index);
      Body& body = document->addBody(BodyId{1000000 + offset},
                                     "source limit body");
      body.addFeature(std::make_unique<ImportedShapeFeature>(
          FeatureId{2000000 + offset}, sharedShape, "source limit feature"));
    }
  }
  CHECK(document->recompute());
  std::vector<ComponentDefinition> definitions;
  definitions.emplace_back(DefinitionId{3}, std::move(document));
  auto product = componentProduct(std::move(definitions), DefinitionId{3});
  const auto limited =
      ComponentPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(!limited);
  CHECK(!limited.snapshot);
  CHECK(limited.status ==
        ComponentPresentationBuildStatus::ResourceLimitExceeded);
}

void verifyBodyInstanceLimit() {
  constexpr DefinitionId rootId{40};
  constexpr DefinitionId middleId{41};
  constexpr DefinitionId leafId{42};
  auto leafDocument = std::make_unique<Document>();
  const auto sharedShape = box(1.0, 1.0, 1.0);
  for (std::uint64_t index = 1; index <= 17; ++index) {
    Body& body = leafDocument->addBody(BodyId{5000 + index}, "leaf body");
    body.addFeature(std::make_unique<ImportedShapeFeature>(
        FeatureId{6000 + index}, sharedShape, "leaf feature"));
  }
  CHECK(leafDocument->recompute());
  ComponentDefinition root(rootId, std::make_unique<Document>());
  ComponentDefinition middle(middleId, std::make_unique<Document>());
  ComponentDefinition leaf(leafId, std::move(leafDocument));
  for (std::uint64_t id = 1; id <= 255; ++id) {
    CHECK(root.insert(ComponentOccurrence(
              OccurrenceId{id}, middleId,
              RigidTransform3d::identity())) ==
          ComponentOccurrenceInsertStatus::Inserted);
    CHECK(middle.insert(ComponentOccurrence(
              OccurrenceId{id}, leafId,
              RigidTransform3d::identity())) ==
          ComponentOccurrenceInsertStatus::Inserted);
  }
  std::vector<ComponentDefinition> definitions;
  definitions.push_back(std::move(leaf));
  definitions.push_back(std::move(root));
  definitions.push_back(std::move(middle));
  auto product = componentProduct(std::move(definitions), rootId);
  const auto limited =
      ComponentPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(!limited);
  CHECK(!limited.snapshot);
  CHECK(limited.status ==
        ComponentPresentationBuildStatus::ResourceLimitExceeded);
}

void verifyDrawingProjectionResourceLimits() {
  {
    std::vector<ComponentDefinition> definitions;
    definitions.emplace_back(DefinitionId{1}, std::make_unique<Document>());
    auto product = componentProduct(std::move(definitions), DefinitionId{1});
    auto* root = const_cast<ComponentDefinition*>(
        product->rootComponentDefinition());
    CHECK(root);
    const std::string maximumName(
        kComponentDrawingMaximumNameUtf8Bytes, 'x');
    for (std::uint64_t id = 1;
         id <= kComponentGraphMaximumDrawings + 1; ++id) {
      ComponentDrawingValidationTestAccess::append(
          *root,
          ComponentDrawing(
              DrawingId{id}, maximumName, ComponentDrawingPaper::A4,
              ComponentDrawingOrientation::Portrait, 1.0, {}));
    }
    const auto rejected =
        ComponentPresentationBuilder::buildTreeProjection(*product);
    CHECK(!rejected);
    CHECK(rejected.status ==
          ComponentTreeProjectionStatus::ResourceLimitExceeded);
  }

  {
    auto document = std::make_unique<Document>();
    std::vector<BodyId> sourceBodyIds;
    sourceBodyIds.reserve(kComponentGraphMaximumDrawingBodyReferences);
    for (std::uint64_t id = 1;
         id <= kComponentGraphMaximumDrawingBodyReferences; ++id) {
      document->addBody(BodyId{id}, "drawing source");
      sourceBodyIds.push_back(BodyId{id});
    }
    std::vector<ComponentDefinition> definitions;
    definitions.emplace_back(DefinitionId{1}, std::move(document));
    auto product = componentProduct(std::move(definitions), DefinitionId{1});
    auto* root = const_cast<ComponentDefinition*>(
        product->rootComponentDefinition());
    CHECK(root);
    ComponentDrawingValidationTestAccess::append(
        *root,
        ComponentDrawing(
            DrawingId{1}, "maximum references", ComponentDrawingPaper::A4,
            ComponentDrawingOrientation::Portrait, 1.0,
            std::move(sourceBodyIds)));
    ComponentDrawingValidationTestAccess::append(
        *root,
        ComponentDrawing(
            DrawingId{2}, "one reference too many",
            ComponentDrawingPaper::A4,
            ComponentDrawingOrientation::Portrait, 1.0,
            std::vector<BodyId>{BodyId{1}}));
    const auto rejected =
        ComponentPresentationBuilder::buildTreeProjection(*product);
    CHECK(!rejected);
    CHECK(rejected.status ==
          ComponentTreeProjectionStatus::ResourceLimitExceeded);
  }
}

}  // namespace

int main() {
  verifyRootOnlyComponent();
  verifyExpandedSharedGraph();
  verifyRetainedLastValidShape();
  verifyTypedFailuresAndGraphLimits();
  verifyFailureAtomicityAndDuplicateIdentity();
  verifySourceLimit();
  verifyBodyInstanceLimit();
  verifyDrawingProjectionResourceLimits();
  return 0;
}
