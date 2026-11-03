#include "TestAssertions.h"

#include <BRepTools.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "model/TopologyReferenceResolver.h"
#include "presentation/AssemblyPresentationBuilder.h"

namespace {

using namespace solidar;

constexpr DefinitionId kPartDefinition{10};
constexpr DefinitionId kSecondPartDefinition{20};
constexpr DefinitionId kOrphanDefinition{30};
constexpr DefinitionId kAssemblyDefinition{100};
constexpr OccurrenceId kFirstOccurrence{1001};
constexpr OccurrenceId kSecondOccurrence{1002};
constexpr ProductPresentationEpoch kEpoch{7001};
constexpr AssemblyMeshProfile kNormal{AssemblyMeshQuality::Normal};
constexpr AssemblyMeshProfile kHigh{AssemblyMeshQuality::High};

class ControlledShapeFeature final : public ShapeFeature {
 public:
  ControlledShapeFeature(FeatureId id, ShapePtr source, std::string name)
      : ShapeFeature(id, std::move(name)), source_(std::move(source)) {}

  [[nodiscard]] std::unique_ptr<Feature> clone() const override {
    return std::make_unique<ControlledShapeFeature>(*this);
  }

  void setFailure(bool fail) noexcept { fail_ = fail; }
  void seedRetainedWithoutRevision() noexcept { setShape(source_); }
  void corruptToMissingShape() noexcept { clearShape(); }
  void corruptToNullShape() {
    setShape(std::make_shared<const TopoDS_Shape>());
  }
  [[nodiscard]] int rebuildCalls() const noexcept { return rebuildCalls_; }

 protected:
  bool rebuildImpl(const RebuildContext&) override {
    ++rebuildCalls_;
    if (fail_) {
      clearShape();
      markError("controlled presentation failure");
      return false;
    }
    setShape(source_);
    markValid();
    return true;
  }

 private:
  ShapePtr source_;
  bool fail_{};
  int rebuildCalls_{};
};

ShapeFeature::ShapePtr box(double x, double y, double z) {
  TopoDS_Shape shape = BRepPrimAPI_MakeBox(x, y, z).Shape();
  BRepTools::Clean(shape);
  return std::make_shared<const TopoDS_Shape>(std::move(shape));
}

bool hasTriangulation(const TopoDS_Shape& shape) {
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next()) {
    TopLoc_Location location;
    if (!BRep_Tool::Triangulation(TopoDS::Face(explorer.Current()), location)
             .IsNull())
      return true;
  }
  return false;
}

RigidTransform3d z90WithTranslation() {
  const double halfSqrt = std::sqrt(0.5);
  const auto created = RigidTransform3d::create(
      {10.0, -20.0, 30.0}, {halfSqrt, 0.0, 0.0, halfSqrt});
  CHECK(created);
  return *created.transform;
}

OccurrencePath path(OccurrenceId id) {
  auto created = OccurrencePath::create({id});
  CHECK(created);
  return std::move(*created.path);
}

OccurrencePath path(std::initializer_list<std::uint64_t> ids) {
  std::vector<OccurrenceId> entries;
  entries.reserve(ids.size());
  for (const auto id : ids) entries.emplace_back(id);
  auto created = OccurrencePath::create(std::move(entries));
  CHECK(created);
  return std::move(*created.path);
}

struct PartInput {
  DefinitionId id;
  std::unique_ptr<Document> document;
};

struct OccurrenceInput {
  OccurrenceId id;
  DefinitionId target;
  RigidTransform3d placement{RigidTransform3d::identity()};
  OccurrenceFlags flags{};
};

template <typename... Rest>
std::vector<PartInput> partInputs(PartInput first, Rest... rest) {
  std::vector<PartInput> result;
  result.reserve(1 + sizeof...(rest));
  result.push_back(std::move(first));
  (result.push_back(std::move(rest)), ...);
  return result;
}

std::unique_ptr<ProductDocument> assemblyProduct(
    std::vector<PartInput> parts, std::vector<OccurrenceInput> occurrences,
    DefinitionId rootId = kAssemblyDefinition) {
  DefinitionRegistry registry;
  for (auto& part : parts) {
    CHECK(registry.insert(PartDefinition(part.id, std::move(part.document))) ==
          DefinitionInsertStatus::Inserted);
  }
  AssemblyDefinition root(rootId);
  for (const auto& occurrence : occurrences) {
    CHECK(root.insert(ComponentOccurrence(
              occurrence.id, occurrence.target, occurrence.placement,
              occurrence.flags)) == AssemblyOccurrenceInsertStatus::Inserted);
  }
  CHECK(registry.insert(std::move(root)) == DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(std::move(registry), rootId);
  CHECK(created);
  return std::move(created.product);
}

std::unique_ptr<ProductDocument> partRootProduct(
    std::unique_ptr<Document> document) {
  DefinitionRegistry registry;
  CHECK(registry.insert(PartDefinition(kPartDefinition, std::move(document))) ==
        DefinitionInsertStatus::Inserted);
  auto created =
      ProductDocument::create(std::move(registry), kPartDefinition);
  CHECK(created);
  return std::move(created.product);
}

Body& addImportedBody(Document& document, BodyId bodyId, FeatureId featureId,
                      ShapeFeature::ShapePtr shape, bool visible = true) {
  Body& body = document.addBody(bodyId, "body " + std::to_string(bodyId));
  body.setVisible(visible);
  body.addFeature(std::make_unique<ImportedShapeFeature>(
      featureId, std::move(shape), "feature " + std::to_string(featureId)));
  return body;
}

const AssemblyPresentationInstance& findInstance(
    const AssemblyPresentationSnapshot& snapshot, OccurrenceId occurrenceId,
    DefinitionId definitionId, BodyId bodyId) {
  for (const auto& instance : snapshot.instances()) {
    if (instance.occurrencePath().entries().front() == occurrenceId &&
        instance.sourceKey().definitionId == definitionId &&
        instance.sourceKey().bodyId == bodyId)
      return instance;
  }
  CHECK(false);
  std::abort();
}

const AssemblyPresentationInstance& findInstance(
    const AssemblyPresentationSnapshot& snapshot,
    const OccurrencePath& occurrencePath) {
  for (const auto& instance : snapshot.instances()) {
    if (instance.occurrencePath() == occurrencePath) return instance;
  }
  CHECK(false);
  std::abort();
}

void verifyRecursiveSharedPresentation() {
  auto document = std::make_unique<Document>();
  addImportedBody(*document, 10, 101, box(3.0, 4.0, 5.0));
  CHECK(document->recompute());

  const auto parent = z90WithTranslation();
  const auto localCreated =
      RigidTransform3d::create({2.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(localCreated);
  const auto expected =
      RigidTransform3d::compose(parent, *localCreated.transform);
  CHECK(expected);

  std::vector<PartDefinition> parts;
  parts.emplace_back(kPartDefinition, std::move(document));
  std::vector<AssemblyDefinition> assemblies;
  AssemblyDefinition child(DefinitionId{200});
  CHECK(child.insert(ComponentOccurrence(
            OccurrenceId{7}, kPartDefinition, *localCreated.transform,
            {.visible = true, .suppressed = false, .grounded = true},
            "общий лист")) == AssemblyOccurrenceInsertStatus::Inserted);
  AssemblyDefinition root(kAssemblyDefinition);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{1}, DefinitionId{200}, parent)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{2}, DefinitionId{200}, RigidTransform3d::identity(),
            {.visible = false, .suppressed = true, .grounded = false})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  assemblies.push_back(std::move(child));
  assemblies.push_back(std::move(root));
  auto product = ProductDocument::createNested(
      std::move(parts), std::move(assemblies), kAssemblyDefinition);
  CHECK(product);

  const auto built =
      AssemblyPresentationBuilder::build(*product.product, kEpoch, kNormal);
  CHECK(built);
  CHECK(built.snapshot->sources().size() == 1);
  CHECK(built.snapshot->instances().size() == 2);
  const auto firstPath = path({1, 7});
  const auto secondPath = path({2, 7});
  const auto& first = findInstance(*built.snapshot, firstPath);
  const auto& second = findInstance(*built.snapshot, secondPath);
  CHECK(first.sourceKey() == second.sourceKey());
  CHECK(first.worldTransform() == *expected.transform);
  CHECK(first.occurrenceVisible());
  CHECK(!first.suppressed());
  CHECK(first.grounded());
  CHECK(first.effectivelyVisible());
  CHECK(second.worldTransform() == *localCreated.transform);
  CHECK(!second.occurrenceVisible());
  CHECK(second.suppressed());
  CHECK(second.grounded());
  CHECK(!second.effectivelyVisible());
}

void verifySharedTwoBodyPresentation() {
  auto document = std::make_unique<Document>();
  const auto firstShape = box(10.0, 20.0, 30.0);
  const auto secondShape = box(5.0, 6.0, 7.0);
  addImportedBody(*document, 10, 101, firstShape, true);
  addImportedBody(*document, 20, 201, secondShape, false);
  CHECK(document->recompute());

  const RigidTransform3d placed = z90WithTranslation();
  auto product = assemblyProduct(
      partInputs(PartInput{kPartDefinition, std::move(document)}),
      {{kFirstOccurrence, kPartDefinition, RigidTransform3d::identity(), {}},
       {kSecondOccurrence, kPartDefinition, placed,
        OccurrenceFlags{false, true, true}}});

  const auto built =
      AssemblyPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(built);
  CHECK(built.diagnostic().empty());
  CHECK(built.snapshot->epoch() == kEpoch);
  CHECK(built.snapshot->meshProfile() == kNormal);
  CHECK(built.snapshot->sources().size() == 2);
  CHECK(built.snapshot->instances().size() == 4);

  CHECK(built.snapshot->sources()[0].key().bodyId == 10);
  CHECK(built.snapshot->sources()[1].key().bodyId == 20);
  CHECK(built.snapshot->sources()[0].shape().get() == firstShape.get());
  CHECK(built.snapshot->sources()[1].shape().get() == secondShape.get());
  CHECK(built.snapshot->sources()[0].key().shapeIdentity == firstShape.get());
  CHECK(built.snapshot->sources()[1].key().shapeIdentity == secondShape.get());

  const auto& firstFirst = findInstance(
      *built.snapshot, kFirstOccurrence, kPartDefinition, 10);
  const auto& firstSecond = findInstance(
      *built.snapshot, kFirstOccurrence, kPartDefinition, 20);
  const auto& secondFirst = findInstance(
      *built.snapshot, kSecondOccurrence, kPartDefinition, 10);
  const auto& secondSecond = findInstance(
      *built.snapshot, kSecondOccurrence, kPartDefinition, 20);
  const AssemblyOccurrenceIdentity expectedFirstIdentity{
      kEpoch, kAssemblyDefinition, path(kFirstOccurrence)};
  CHECK(firstFirst.occurrenceIdentity() == expectedFirstIdentity);
  CHECK(firstFirst.worldTransform() == RigidTransform3d::identity());
  CHECK(secondFirst.worldTransform() == placed);
  CHECK(firstFirst.sourceKey() == built.snapshot->sources()[0].key());
  CHECK(firstSecond.sourceKey() == built.snapshot->sources()[1].key());
  CHECK(secondFirst.sourceKey() == built.snapshot->sources()[0].key());
  CHECK(secondSecond.sourceKey() == built.snapshot->sources()[1].key());

  CHECK(firstFirst.occurrenceVisible());
  CHECK(!firstFirst.suppressed());
  CHECK(!firstFirst.grounded());
  CHECK(firstFirst.bodyVisible());
  CHECK(firstFirst.effectivelyVisible());
  CHECK(!firstSecond.bodyVisible());
  CHECK(!firstSecond.effectivelyVisible());
  CHECK(!secondFirst.occurrenceVisible());
  CHECK(secondFirst.suppressed());
  CHECK(secondFirst.grounded());
  CHECK(secondFirst.bodyVisible());
  CHECK(!secondFirst.effectivelyVisible());
  CHECK(!secondSecond.bodyVisible());
  CHECK(!secondSecond.effectivelyVisible());

  // Exact source sharing: N occurrences by M Bodies creates M source records,
  // while every instance carries the exact corresponding source identity.
  CHECK(&firstFirst.sourceKey() != &secondFirst.sourceKey());
  CHECK(firstFirst.sourceKey().shapeIdentity ==
        secondFirst.sourceKey().shapeIdentity);
}

void verifyDefinitionScopedIdsAndOrphanOmission() {
  auto first = std::make_unique<Document>();
  auto second = std::make_unique<Document>();
  const auto firstShape = box(1.0, 2.0, 3.0);
  const auto secondShape = box(4.0, 5.0, 6.0);
  addImportedBody(*first, 42, 77, firstShape);
  addImportedBody(*second, 42, 77, secondShape);
  CHECK(first->recompute());
  CHECK(second->recompute());

  auto orphan = std::make_unique<Document>();
  auto& orphanBody = orphan->addBody(500, "invalid orphan");
  auto orphanFeature = std::make_unique<ControlledShapeFeature>(
      501, box(2.0, 2.0, 2.0), "revision-zero orphan");
  orphanFeature->seedRetainedWithoutRevision();
  orphanBody.addFeature(std::move(orphanFeature));

  auto product = assemblyProduct(
      partInputs(PartInput{kOrphanDefinition, std::move(orphan)},
                 PartInput{kSecondPartDefinition, std::move(second)},
                 PartInput{kPartDefinition, std::move(first)}),
      {{kSecondOccurrence, kSecondPartDefinition},
       {kFirstOccurrence, kPartDefinition}});
  const auto built =
      AssemblyPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(built);
  CHECK(built.snapshot->sources().size() == 2);
  CHECK(built.snapshot->instances().size() == 2);
  CHECK(built.snapshot->sources()[0].key().definitionId == kPartDefinition);
  CHECK(built.snapshot->sources()[1].key().definitionId ==
        kSecondPartDefinition);
  CHECK(built.snapshot->sources()[0].key().bodyId == 42);
  CHECK(built.snapshot->sources()[1].key().bodyId == 42);
  CHECK(built.snapshot->sources()[0].key().featureId == 77);
  CHECK(built.snapshot->sources()[1].key().featureId == 77);
  CHECK(built.snapshot->sources()[0].key() !=
        built.snapshot->sources()[1].key());
  for (const auto& source : built.snapshot->sources())
    CHECK(source.key().definitionId != kOrphanDefinition);
}

void verifyCanonicalOrderingAndSemanticRepeat() {
  Document seed;
  addImportedBody(seed, 20, 201, box(2.0, 3.0, 4.0));
  addImportedBody(seed, 10, 101, box(5.0, 6.0, 7.0));
  CHECK(seed.recompute());
  const Body body20(seed.bodies()[0]);
  const Body body10(seed.bodies()[1]);

  auto forward = std::make_unique<Document>();
  CHECK(forward->applyBodySlice(
      0, std::optional<Body>(std::in_place, body10)));
  CHECK(forward->applyBodySlice(
      1, std::optional<Body>(std::in_place, body20)));
  auto reverse = std::make_unique<Document>();
  CHECK(reverse->applyBodySlice(
      0, std::optional<Body>(std::in_place, body20)));
  CHECK(reverse->applyBodySlice(
      1, std::optional<Body>(std::in_place, body10)));

  auto firstProduct = assemblyProduct(
      partInputs(PartInput{kPartDefinition, std::move(forward)}),
      {{kSecondOccurrence, kPartDefinition},
       {kFirstOccurrence, kPartDefinition}});
  auto secondProduct = assemblyProduct(
      partInputs(PartInput{kPartDefinition, std::move(reverse)}),
      {{kFirstOccurrence, kPartDefinition},
       {kSecondOccurrence, kPartDefinition}});
  const auto first =
      AssemblyPresentationBuilder::build(*firstProduct, kEpoch, kNormal);
  const auto repeated =
      AssemblyPresentationBuilder::build(*firstProduct, kEpoch, kNormal);
  const auto reversed =
      AssemblyPresentationBuilder::build(*secondProduct, kEpoch, kNormal);
  CHECK(first && repeated && reversed);
  CHECK(first.snapshot->semanticallyEquivalent(*repeated.snapshot));
  CHECK(first.snapshot->semanticallyEquivalent(*reversed.snapshot));
  CHECK(first.snapshot->sources()[0].key().bodyId == 10);
  CHECK(first.snapshot->sources()[1].key().bodyId == 20);
  CHECK(first.snapshot->instances()[0].occurrencePath() ==
        path(kFirstOccurrence));
  CHECK(first.snapshot->instances()[1].occurrencePath() ==
        path(kFirstOccurrence));
  CHECK(first.snapshot->instances()[2].occurrencePath() ==
        path(kSecondOccurrence));
  CHECK(first.snapshot->instances()[3].occurrencePath() ==
        path(kSecondOccurrence));
}

void verifyUpdatesAndIdentityDomains() {
  auto document = std::make_unique<Document>();
  addImportedBody(*document, 10, 101, box(3.0, 4.0, 5.0));
  CHECK(document->recompute());
  auto product = assemblyProduct(
      partInputs(PartInput{kPartDefinition, std::move(document)}),
      {{kFirstOccurrence, kPartDefinition}});

  const auto before =
      AssemblyPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(before);
  const auto moved = z90WithTranslation();
  CHECK(product->setRootOccurrencePlacement(path(kFirstOccurrence), moved) ==
        ProductOccurrenceMutationStatus::Committed);
  CHECK(product->setRootOccurrenceFlags(
            path(kFirstOccurrence), OccurrenceFlags{false, true, true}) ==
        ProductOccurrenceMutationStatus::Committed);
  const auto after =
      AssemblyPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(after);
  CHECK(before.snapshot->sources()[0].key() ==
        after.snapshot->sources()[0].key());
  CHECK(before.snapshot->sources()[0].shape().get() ==
        after.snapshot->sources()[0].shape().get());
  CHECK(before.snapshot->instances()[0].occurrenceIdentity() ==
        after.snapshot->instances()[0].occurrenceIdentity());
  CHECK(before.snapshot->instances()[0].sourceKey() ==
        after.snapshot->instances()[0].sourceKey());
  CHECK(after.snapshot->instances()[0].worldTransform() == moved);
  CHECK(!after.snapshot->instances()[0].occurrenceVisible());
  CHECK(after.snapshot->instances()[0].suppressed());
  CHECK(after.snapshot->instances()[0].grounded());

  const ProductPresentationEpoch nextEpoch{7002};
  const auto next =
      AssemblyPresentationBuilder::build(*product, nextEpoch, kNormal);
  const auto high =
      AssemblyPresentationBuilder::build(*product, kEpoch, kHigh);
  CHECK(next && high);
  CHECK(next.snapshot->sources()[0].key() !=
        after.snapshot->sources()[0].key());
  CHECK(next.snapshot->sources()[0].key().shapeIdentity ==
        after.snapshot->sources()[0].key().shapeIdentity);
  CHECK(next.snapshot->instances()[0].occurrenceIdentity() !=
        after.snapshot->instances()[0].occurrenceIdentity());
  CHECK(high.snapshot->sources()[0].key() !=
        after.snapshot->sources()[0].key());
  CHECK(high.snapshot->instances()[0].occurrenceIdentity() ==
        after.snapshot->instances()[0].occurrenceIdentity());

  TopoDS_Shape otherAddressToken;
  AssemblyMeshSourceKey addressVariant = after.snapshot->sources()[0].key();
  addressVariant.shapeIdentity = &otherAddressToken;
  CHECK(addressVariant != after.snapshot->sources()[0].key());
  CHECK(addressVariant.semanticKey() ==
        after.snapshot->sources()[0].key().semanticKey());
}

void verifyDefensiveOccurrenceValidation() {
  auto document = std::make_unique<Document>();
  addImportedBody(*document, 10, 101, box(3.0, 4.0, 5.0));
  CHECK(document->recompute());
  auto product = assemblyProduct(
      partInputs(PartInput{kPartDefinition, std::move(document)}),
      {{kFirstOccurrence, kPartDefinition}});

  auto* stored = const_cast<ComponentOccurrence*>(
      product->rootAssemblyDefinition()->find(kFirstOccurrence));
  CHECK(stored != nullptr);
  *stored = ComponentOccurrence(kFirstOccurrence, DefinitionId{99999},
                                RigidTransform3d::identity());
  const auto dangling =
      AssemblyPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(!dangling.snapshot);
  CHECK(dangling.status ==
        AssemblyPresentationBuildStatus::DanglingTargetDefinition);

  *stored = ComponentOccurrence(kFirstOccurrence, kAssemblyDefinition,
                                RigidTransform3d::identity());
  const auto wrongType =
      AssemblyPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(!wrongType.snapshot);
  CHECK(wrongType.status ==
        AssemblyPresentationBuildStatus::InvalidAssemblyDefinition);

  *stored = ComponentOccurrence(kSecondOccurrence, kPartDefinition,
                                RigidTransform3d::identity());
  const auto inconsistentId =
      AssemblyPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(!inconsistentId.snapshot);
  CHECK(inconsistentId.status ==
        AssemblyPresentationBuildStatus::InvalidOccurrenceId);
}

void verifyShapeLifetimeAndReadOnlyBehavior() {
  auto document = std::make_unique<Document>();
  const auto sourceShape = box(7.0, 8.0, 9.0);
  Body& body = addImportedBody(*document, 10, 101, sourceShape);
  CHECK(document->recompute());
  auto* feature = body.features().front().get();
  CHECK(feature != nullptr);
  CHECK(!hasTriangulation(*sourceShape));
  const std::uint64_t topologyBuildsBefore =
      TopologyIndex::buildAttemptCount();

  const Document* documentAddress = document.get();
  const Body* bodyAddress = &body;
  const ShapeFeature* featureAddress = feature;
  const BodyId bodyId = body.id();
  const FeatureId featureId = feature->id();
  const ShapeRevision revision = feature->shapeRevision();
  const FeatureState state = feature->state();

  auto product = assemblyProduct(
      partInputs(PartInput{kPartDefinition, std::move(document)}),
      {{kFirstOccurrence, kPartDefinition}});
  const auto built =
      AssemblyPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(built);
  CHECK(&product->definitions().findPart(kPartDefinition)->document() ==
        documentAddress);
  CHECK(&documentAddress->bodies().front() == bodyAddress);
  CHECK(documentAddress->bodies().front().features().front().get() ==
        featureAddress);
  CHECK(bodyAddress->id() == bodyId);
  CHECK(featureAddress->id() == featureId);
  CHECK(featureAddress->shapeRevision() == revision);
  CHECK(featureAddress->state() == state);
  CHECK(featureAddress->lastValidShape().get() == sourceShape.get());
  CHECK(built.snapshot->sources()[0].topologyLineage() ==
        featureAddress->lastValidTopologyLineage());
  CHECK(TopologyIndex::buildAttemptCount() == topologyBuildsBefore);
  CHECK(!hasTriangulation(*sourceShape));

  const TopoDS_Shape* ownedAddress = built.snapshot->sources()[0].shape().get();
  std::unique_ptr<ProductDocument> movedProduct = std::move(product);
  CHECK(movedProduct != nullptr);
  movedProduct.reset();
  CHECK(built.snapshot->sources()[0].shape());
  CHECK(built.snapshot->sources()[0].shape().get() == ownedAddress);
  CHECK(!built.snapshot->sources()[0].shape()->IsNull());
}

void verifyLastValidFallback() {
  auto document = std::make_unique<Document>();
  auto& body = document->addBody(10, "controlled");
  auto controlled = std::make_unique<ControlledShapeFeature>(
      101, box(4.0, 4.0, 4.0), "controlled");
  ControlledShapeFeature* feature = controlled.get();
  body.addFeature(std::move(controlled));
  CHECK(document->recompute());
  auto product = assemblyProduct(
      partInputs(PartInput{kPartDefinition, std::move(document)}),
      {{kFirstOccurrence, kPartDefinition}});
  const auto before =
      AssemblyPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(before);
  const auto keyBefore = before.snapshot->sources()[0].key();
  const auto* shapeBefore = before.snapshot->sources()[0].shape().get();
  const int rebuildCallsBefore = feature->rebuildCalls();

  feature->setFailure(true);
  feature->setDirty();
  auto& liveDocument = const_cast<Document&>(
      product->definitions().findPart(kPartDefinition)->document());
  CHECK(!liveDocument.recompute());
  CHECK(feature->isFailed());
  CHECK(feature->rebuildCalls() == rebuildCallsBefore + 1);
  CHECK(!feature->shape());
  CHECK(feature->lastValidShape().get() == shapeBefore);
  const auto after =
      AssemblyPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(after);
  CHECK(after.snapshot->sources()[0].key() == keyBefore);
  CHECK(after.snapshot->sources()[0].shape().get() == shapeBefore);
  CHECK(feature->rebuildCalls() == rebuildCallsBefore + 1);
}

void verifyFeaturelessAndTypedFailures() {
  auto emptyAssembly = assemblyProduct({}, {});
  const auto emptyAssemblyBuilt =
      AssemblyPresentationBuilder::build(*emptyAssembly, kEpoch, kNormal);
  CHECK(emptyAssemblyBuilt);
  CHECK(emptyAssemblyBuilt.snapshot->sources().empty());
  CHECK(emptyAssemblyBuilt.snapshot->instances().empty());

  auto empty = std::make_unique<Document>();
  empty->addBody(10, "featureless");
  auto emptyProduct = assemblyProduct(
      partInputs(PartInput{kPartDefinition, std::move(empty)}),
      {{kFirstOccurrence, kPartDefinition}});
  const auto emptyBuilt =
      AssemblyPresentationBuilder::build(*emptyProduct, kEpoch, kNormal);
  CHECK(emptyBuilt);
  CHECK(emptyBuilt.snapshot->sources().empty());
  CHECK(emptyBuilt.snapshot->instances().empty());

  auto entirelyEmpty = std::make_unique<Document>();
  auto entirelyEmptyProduct = assemblyProduct(
      partInputs(PartInput{kPartDefinition, std::move(entirelyEmpty)}),
      {{kFirstOccurrence, kPartDefinition}});
  const auto entirelyEmptyBuilt = AssemblyPresentationBuilder::build(
      *entirelyEmptyProduct, kEpoch, kNormal);
  CHECK(entirelyEmptyBuilt);
  CHECK(entirelyEmptyBuilt.snapshot->sources().empty());

  CHECK(AssemblyPresentationBuilder::build(
            *emptyProduct, kInvalidProductPresentationEpoch, kNormal)
            .status == AssemblyPresentationBuildStatus::InvalidEpoch);
  CHECK(AssemblyPresentationBuilder::build(
            *emptyProduct, kEpoch, AssemblyMeshProfile{})
            .status == AssemblyPresentationBuildStatus::InvalidMeshProfile);

  auto rootPart = std::make_unique<Document>();
  auto partProduct = partRootProduct(std::move(rootPart));
  const auto wrongRoot =
      AssemblyPresentationBuilder::build(*partProduct, kEpoch, kNormal);
  CHECK(!wrongRoot);
  CHECK(!wrongRoot.snapshot);
  CHECK(wrongRoot.status == AssemblyPresentationBuildStatus::RootIsNotAssembly);

  // A valid first Body must never escape as a partial snapshot when a later
  // featureful Body lacks a retained result.
  auto lateInvalid = std::make_unique<Document>();
  addImportedBody(*lateInvalid, 10, 101, box(2.0, 3.0, 4.0));
  auto& missingBody = lateInvalid->addBody(20, "late invalid");
  missingBody.addFeature(std::make_unique<ControlledShapeFeature>(
      201, box(1.0, 1.0, 1.0), "never rebuilt"));
  auto lateInvalidProduct = assemblyProduct(
      partInputs(PartInput{kPartDefinition, std::move(lateInvalid)}),
      {{kFirstOccurrence, kPartDefinition}});
  const auto lateFailure = AssemblyPresentationBuilder::build(
      *lateInvalidProduct, kEpoch, kNormal);
  CHECK(!lateFailure);
  CHECK(!lateFailure.snapshot);
  CHECK(lateFailure.status ==
        AssemblyPresentationBuildStatus::MissingRetainedShape);

  auto revisionZero = std::make_unique<Document>();
  auto& revisionZeroBody = revisionZero->addBody(10, "revision zero");
  auto revisionZeroFeature = std::make_unique<ControlledShapeFeature>(
      101, box(2.0, 2.0, 2.0), "revision zero");
  revisionZeroFeature->seedRetainedWithoutRevision();
  revisionZeroBody.addFeature(std::move(revisionZeroFeature));
  auto revisionZeroProduct = assemblyProduct(
      partInputs(PartInput{kPartDefinition, std::move(revisionZero)}),
      {{kFirstOccurrence, kPartDefinition}});
  const auto revisionFailure = AssemblyPresentationBuilder::build(
      *revisionZeroProduct, kEpoch, kNormal);
  CHECK(!revisionFailure.snapshot);
  CHECK(revisionFailure.status ==
        AssemblyPresentationBuildStatus::InvalidShapeRevision);

  auto nullShape = std::make_unique<Document>();
  auto& nullBody = nullShape->addBody(10, "null shape");
  auto nullFeature = std::make_unique<ControlledShapeFeature>(
      101, box(2.0, 2.0, 2.0), "null shape");
  ControlledShapeFeature* nullFeatureAddress = nullFeature.get();
  nullBody.addFeature(std::move(nullFeature));
  CHECK(nullShape->recompute());
  nullFeatureAddress->corruptToNullShape();
  auto nullProduct = assemblyProduct(
      partInputs(PartInput{kPartDefinition, std::move(nullShape)}),
      {{kFirstOccurrence, kPartDefinition}});
  const auto nullFailure =
      AssemblyPresentationBuilder::build(*nullProduct, kEpoch, kNormal);
  CHECK(!nullFailure.snapshot);
  CHECK(nullFailure.status ==
        AssemblyPresentationBuildStatus::NullRetainedShape);

  auto missingShape = std::make_unique<Document>();
  auto& missingOnlyBody = missingShape->addBody(10, "missing shape");
  auto missingOnlyFeature = std::make_unique<ControlledShapeFeature>(
      101, box(2.0, 2.0, 2.0), "missing shape");
  ControlledShapeFeature* missingOnlyAddress = missingOnlyFeature.get();
  missingOnlyBody.addFeature(std::move(missingOnlyFeature));
  CHECK(missingShape->recompute());
  missingOnlyAddress->corruptToMissingShape();
  auto missingProduct = assemblyProduct(
      partInputs(PartInput{kPartDefinition, std::move(missingShape)}),
      {{kFirstOccurrence, kPartDefinition}});
  const auto missingFailure = AssemblyPresentationBuilder::build(
      *missingProduct, kEpoch, kNormal);
  CHECK(!missingFailure.snapshot);
  CHECK(missingFailure.status ==
        AssemblyPresentationBuildStatus::MissingRetainedShape);
}

void verifyDuplicateStableIdentityFailure() {
  Document seed;
  addImportedBody(seed, 10, 101, box(2.0, 3.0, 4.0));
  CHECK(seed.recompute());
  const Body duplicate(seed.bodies().front());

  auto document = std::make_unique<Document>();
  CHECK(document->applyBodySlice(
      0, std::optional<Body>(std::in_place, duplicate)));
  CHECK(document->applyBodySlice(
      1, std::optional<Body>(std::in_place, duplicate)));
  auto product = assemblyProduct(
      partInputs(PartInput{kPartDefinition, std::move(document)}),
      {{kFirstOccurrence, kPartDefinition}});
  const auto failed =
      AssemblyPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(!failed);
  CHECK(!failed.snapshot);
  CHECK(failed.status ==
        AssemblyPresentationBuildStatus::DuplicateStableIdentity);
}

void verifyOccurrenceResourceBoundary() {
  auto document = std::make_unique<Document>();
  addImportedBody(*document, 10, 101, box(1.0, 1.0, 1.0));
  CHECK(document->recompute());

  std::vector<OccurrenceInput> occurrences;
  occurrences.reserve(AssemblyPresentationLimits::kMaximumOccurrences);
  for (std::size_t index = 0;
       index < AssemblyPresentationLimits::kMaximumOccurrences; ++index) {
    occurrences.push_back(
        {OccurrenceId{static_cast<std::uint64_t>(index + 1)},
         kPartDefinition});
  }
  auto product = assemblyProduct(
      partInputs(PartInput{kPartDefinition, std::move(document)}),
      std::move(occurrences));
  const auto boundary =
      AssemblyPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(boundary);
  CHECK(boundary.snapshot->sources().size() == 1);
  CHECK(boundary.snapshot->instances().size() ==
        AssemblyPresentationLimits::kMaximumOccurrences);

  CHECK(product->insertRootOccurrence(ComponentOccurrence(
            OccurrenceId{static_cast<std::uint64_t>(
                AssemblyPresentationLimits::kMaximumOccurrences + 1)},
            kPartDefinition, RigidTransform3d::identity())) ==
        ProductOccurrenceMutationStatus::Committed);
  const auto over =
      AssemblyPresentationBuilder::build(*product, kEpoch, kNormal);
  CHECK(!over);
  CHECK(!over.snapshot);
  CHECK(over.status ==
        AssemblyPresentationBuildStatus::ResourceLimitExceeded);
}

}  // namespace

int main() {
  verifyRecursiveSharedPresentation();
  verifySharedTwoBodyPresentation();
  verifyDefinitionScopedIdsAndOrphanOmission();
  verifyCanonicalOrderingAndSemanticRepeat();
  verifyUpdatesAndIdentityDomains();
  verifyDefensiveOccurrenceValidation();
  verifyShapeLifetimeAndReadOnlyBehavior();
  verifyLastValidFallback();
  verifyFeaturelessAndTypedFailures();
  verifyDuplicateStableIdentityFailure();
  verifyOccurrenceResourceBoundary();
  return EXIT_SUCCESS;
}
