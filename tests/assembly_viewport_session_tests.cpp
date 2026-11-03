#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "model/ImportedShapeFeature.h"
#include "model/OccurrenceDelta.h"
#include "model/OccurrenceDuplicate.h"
#include "model/OccurrenceRemove.h"
#include "model/ProductDocument.h"
#include "ui/AssemblyViewportSession.h"
#include "ui/AssemblyViewportFit.h"
#include "ui/AssemblyViewportRuler.h"
#include "ui/Viewport.h"

namespace {

using namespace solidar;

using AssemblySelectionViewGetter =
    const std::optional<AssemblyOccurrenceSelection>& (Viewport::*)()
        const noexcept;
static_assert(std::is_same_v<
              decltype(&Viewport::selectedAssemblyOccurrenceView),
              AssemblySelectionViewGetter>);
static_assert(noexcept(std::declval<const Viewport&>()
                           .selectedAssemblyOccurrenceView()));

template <typename T>
concept HasOwningAssemblySelectionGetter = requires(const T& viewport) {
  viewport.selectedAssemblyOccurrence();
};
static_assert(!HasOwningAssemblySelectionGetter<Viewport>);

std::unique_ptr<Document> shapedPart(bool bodyVisible = true) {
  auto document = std::make_unique<Document>();
  Body& body = document->addBody(11, "body");
  body.setVisible(bodyVisible);
  body.addFeature(std::make_unique<ImportedShapeFeature>(
      12, std::make_shared<const TopoDS_Shape>(
              BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape()),
      "shape"));
  CHECK(document->recompute());
  return document;
}

std::unique_ptr<ProductDocument> product() {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(
            PartDefinition(DefinitionId{10}, shapedPart())) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(
            PartDefinition(DefinitionId{20}, shapedPart(false))) ==
        DefinitionInsertStatus::Inserted);
  AssemblyDefinition root(DefinitionId{100});
  const auto at = [](double x) {
    const auto made = RigidTransform3d::create({x, 0.0, 0.0},
                                                {1.0, 0.0, 0.0, 0.0});
    CHECK(made);
    return *made.transform;
  };
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{1}, DefinitionId{10}, at(0.0))) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{2}, DefinitionId{10}, at(30.0),
            {.visible = false, .suppressed = false, .grounded = true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{3}, DefinitionId{20}, at(60.0))) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{4}, DefinitionId{10}, at(90.0),
            {.visible = true, .suppressed = true, .grounded = false})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{5}, DefinitionId{10}, at(120.0))) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{100});
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> emptyProduct() {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(AssemblyDefinition(DefinitionId{200})) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{200});
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> nestedProduct() {
  std::vector<PartDefinition> parts;
  parts.emplace_back(DefinitionId{10}, shapedPart());
  std::vector<AssemblyDefinition> assemblies;
  AssemblyDefinition child(DefinitionId{200});
  CHECK(child.insert(ComponentOccurrence(
            OccurrenceId{7}, DefinitionId{10}, RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  AssemblyDefinition root(DefinitionId{100});
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{1}, DefinitionId{200},
            RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  const auto shifted = RigidTransform3d::create(
      {30.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(shifted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{2}, DefinitionId{200}, *shifted.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  assemblies.push_back(std::move(child));
  assemblies.push_back(std::move(root));
  auto made = ProductDocument::createNested(
      std::move(parts), std::move(assemblies), DefinitionId{100});
  CHECK(made);
  return std::move(made.product);
}

AssemblyOccurrenceIdentity identity(ProductPresentationEpoch epoch,
                                    std::initializer_list<std::uint64_t> ids) {
  std::vector<OccurrenceId> entries;
  entries.reserve(ids.size());
  for (const auto id : ids) entries.emplace_back(id);
  auto path = OccurrencePath::create(std::move(entries));
  CHECK(path);
  return {epoch, DefinitionId{100}, std::move(*path.path)};
}

AssemblyOccurrenceIdentity identity(ProductPresentationEpoch epoch,
                                    OccurrenceId id) {
  auto path = OccurrencePath::create({id});
  CHECK(path);
  return {epoch, DefinitionId{100}, std::move(*path.path)};
}

bool samePoint(const Point3d& left, const Point3d& right) {
  return left.x == right.x && left.y == right.y && left.z == right.z;
}

class AcceptSuccessor final : public ProductDocumentSuccessor {
 public:
  ProductDocumentSuccessorStatus prepare(
      const ProductDocument&,
      ProductDocumentSuccessorImpact impact) noexcept override {
    lastImpact = impact;
    return ProductDocumentSuccessorStatus::Prepared;
  }

  ProductDocumentSuccessorImpact lastImpact{
      ProductDocumentSuccessorImpact::FullProjection};
};

DuplicateOccurrenceColdPayload duplicatePayload(
    const ProductDocument& product, const AssemblyOccurrenceIdentity& source,
    OccurrenceId duplicateId) {
  const auto resolved = product.resolveRootOccurrence(source.path);
  CHECK(resolved);
  return {{product.incarnation(), product.rootDefinitionId(), source.path,
           resolved.occurrence->targetDefinitionId()},
          duplicateId,
          resolved.occurrence->targetDefinitionId(),
          resolved.occurrence->placement(),
          resolved.occurrence->homePlacement(),
          resolved.occurrence->flags(),
          resolved.occurrence->displayNameUtf8()};
}

RemoveOccurrenceColdPayload removePayload(
    const ProductDocument& product,
    const AssemblyOccurrenceIdentity& occurrenceIdentity) {
  const auto resolved =
      product.resolveRootOccurrence(occurrenceIdentity.path);
  CHECK(resolved);
  return {{product.incarnation(), product.rootDefinitionId(),
           occurrenceIdentity.path,
           resolved.occurrence->targetDefinitionId()},
          resolved.occurrence->placement(), resolved.occurrence->flags(),
          resolved.occurrence->displayNameUtf8(),
          resolved.occurrence->homePlacement()};
}

}  // namespace

int main() {
  auto empty = emptyProduct();
  auto emptyPrepared = AssemblyViewportSession::prepare(
      *empty, ProductPresentationEpoch{1},
      AssemblyMeshProfile{AssemblyMeshQuality::Normal});
  CHECK(emptyPrepared);
  CHECK(emptyPrepared.session->scene());

  // Recursive graph identities are retained for assembly occurrences and
  // terminal leaves. Intermediate assembly rows are selectable, but cannot
  // acquire a viewport highlight or be isolated as a truncated leaf.
  auto nested = nestedProduct();
  constexpr ProductPresentationEpoch nestedEpoch{2};
  auto nestedPrepared = AssemblyViewportSession::prepare(
      *nested, nestedEpoch, AssemblyMeshProfile{AssemblyMeshQuality::Normal});
  CHECK(nestedPrepared);
  CHECK(nestedPrepared.session->occurrences().size() == 4);
  const auto leftAssembly = identity(nestedEpoch, {1});
  const auto leftLeaf = identity(nestedEpoch, {1, 7});
  const auto rightLeaf = identity(nestedEpoch, {2, 7});
  AssemblyOccurrenceSelection leftAssemblySelection{
      nestedPrepared.session->publicationHandle(), leftAssembly};
  AssemblyOccurrenceSelection leftLeafSelection{
      nestedPrepared.session->publicationHandle(), leftLeaf};
  CHECK(nestedPrepared.session->setSelectedOccurrence(
      leftAssemblySelection));
  CHECK(nestedPrepared.session->selectedOccurrence());
  CHECK(!nestedPrepared.session->selectedHighlight());
  CHECK(nestedPrepared.session->setHoveredOccurrence(leftLeafSelection));
  CHECK(nestedPrepared.session->hoveredOccurrence());
  CHECK(nestedPrepared.session->hoveredHighlight());
  CHECK(!nestedPrepared.session->canIsolate(leftAssembly));
  CHECK(nestedPrepared.session->prepareIsolation(leftAssembly).status ==
        PrepareAssemblyIsolationStatus::NotMeaningful);
  CHECK(nestedPrepared.session->canIsolate(leftLeaf));
  CHECK(nestedPrepared.session->canIsolate(rightLeaf));
  const auto nestedIsolation =
      nestedPrepared.session->prepareIsolation(leftLeaf);
  CHECK(nestedIsolation);
  CHECK(nestedIsolation.state->displayedScene->instances().size() == 1);
  CHECK(nestedIsolation.state->displayedScene->instances().front()
            .occurrenceIdentity == leftLeaf);
  CHECK(emptyPrepared.session->scene()->isCurrent());
  CHECK(emptyPrepared.session->scene()->instances().empty());
  CHECK(emptyPrepared.session->occurrences().empty());

  auto value = product();
  constexpr ProductPresentationEpoch epoch{17};
  constexpr AssemblyMeshProfile profile{AssemblyMeshQuality::Normal};

  for (const auto point : {
           AssemblyViewportSessionFailurePoint::Presentation,
           AssemblyViewportSessionFailurePoint::Scene,
           AssemblyViewportSessionFailurePoint::Picking,
           AssemblyViewportSessionFailurePoint::Spatial,
           AssemblyViewportSessionFailurePoint::Selection,
           AssemblyViewportSessionFailurePoint::Resource,
           AssemblyViewportSessionFailurePoint::FinalPreparation}) {
    const auto failed =
        AssemblyViewportSession::prepare(*value, epoch, profile, point);
    CHECK(!failed);
    CHECK(!failed.session);
  }

  // Failure does not consume or mutate the caller-owned epoch: the exact same
  // candidate epoch can immediately produce the complete session.
  auto prepared = AssemblyViewportSession::prepare(*value, epoch, profile);
  CHECK(prepared);
  CHECK(prepared.session->epoch() == epoch);
  CHECK(prepared.session->scene());
  CHECK(prepared.session->scene()->isCurrent());
  CHECK(prepared.session->occurrences().size() == 5);
  CHECK(prepared.session->scene()->instances().size() == 2);

  const auto visible = identity(epoch, OccurrenceId{1});
  CHECK(prepared.session->setSelectedOccurrence(AssemblyOccurrenceSelection{
      prepared.session->publicationHandle(), visible}));
  CHECK(prepared.session->selectedOccurrence());
  CHECK(prepared.session->selectedOccurrence()->occurrence == visible);
  CHECK(prepared.session->selectedHighlight());

  const auto hidden = identity(epoch, OccurrenceId{2});
  CHECK(prepared.session->setSelectedOccurrence(AssemblyOccurrenceSelection{
      prepared.session->publicationHandle(), hidden}));
  CHECK(prepared.session->selectedOccurrence());
  CHECK(prepared.session->selectedOccurrence()->occurrence == hidden);
  CHECK(!prepared.session->selectedHighlight());

  const auto bodyHidden = identity(epoch, OccurrenceId{3});
  CHECK(prepared.session->setSelectedOccurrence(AssemblyOccurrenceSelection{
      prepared.session->publicationHandle(), bodyHidden}));
  CHECK(!prepared.session->selectedHighlight());
  const auto suppressed = identity(epoch, OccurrenceId{4});
  CHECK(prepared.session->setSelectedOccurrence(AssemblyOccurrenceSelection{
      prepared.session->publicationHandle(), suppressed}));
  CHECK(!prepared.session->selectedHighlight());
  const auto visiblePeer = identity(epoch, OccurrenceId{5});

  auto foreignSession = AssemblyViewportSession::prepare(*value, epoch, profile);
  CHECK(foreignSession);
  CHECK(foreignSession.session->publicationHandle().generation() ==
        prepared.session->publicationHandle().generation());
  CHECK(!prepared.session->setSelectedOccurrence(AssemblyOccurrenceSelection{
      foreignSession.session->publicationHandle(), visible}));
  CHECK(prepared.session->selectedOccurrence());
  CHECK(prepared.session->selectedOccurrence()->occurrence == suppressed);

  const auto foreignEpoch = identity(ProductPresentationEpoch{18},
                                     OccurrenceId{1});
  CHECK(!prepared.session->setSelectedOccurrence(AssemblyOccurrenceSelection{
      prepared.session->publicationHandle(), foreignEpoch}));
  CHECK(prepared.session->selectedOccurrence());
  CHECK(prepared.session->selectedOccurrence()->occurrence == suppressed);

  prepared.session->clearInteraction();
  CHECK(!prepared.session->selectedOccurrence());
  CHECK(!prepared.session->selectedHighlight());

  // A5d has a dedicated same-epoch definition projection. One edited source
  // is rebuilt exactly once and shared by every occurrence of that
  // definition; an unrelated source is reused byte-for-byte/pointer-for-
  // pointer, and the publication identity is always fresh.
  {
    DefinitionRegistry editDefinitions;
    CHECK(editDefinitions.insert(
              PartDefinition(DefinitionId{10}, shapedPart())) ==
          DefinitionInsertStatus::Inserted);
    CHECK(editDefinitions.insert(
              PartDefinition(DefinitionId{20}, shapedPart())) ==
          DefinitionInsertStatus::Inserted);
    AssemblyDefinition editRoot(DefinitionId{100});
    const auto at = [](double x) {
      const auto made = RigidTransform3d::create(
          {x, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
      CHECK(made);
      return *made.transform;
    };
    CHECK(editRoot.insert(ComponentOccurrence(
              OccurrenceId{1}, DefinitionId{10}, at(0.0),
              {.visible = true, .suppressed = false, .grounded = false},
              "edited first")) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    CHECK(editRoot.insert(ComponentOccurrence(
              OccurrenceId{2}, DefinitionId{10}, at(30.0),
              {.visible = true, .suppressed = false, .grounded = true},
              "edited second")) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    CHECK(editRoot.insert(ComponentOccurrence(
              OccurrenceId{3}, DefinitionId{20}, at(60.0),
              {.visible = true, .suppressed = false, .grounded = false},
              "unaffected")) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    CHECK(editDefinitions.insert(std::move(editRoot)) ==
          DefinitionInsertStatus::Inserted);
    auto editMade = ProductDocument::create(std::move(editDefinitions),
                                            DefinitionId{100});
    CHECK(editMade);
    auto incumbent =
        AssemblyViewportSession::prepare(*editMade.product, epoch, profile);
    CHECK(incumbent);
    CHECK(incumbent.session->scene()->sources().size() == 2);
    CHECK(incumbent.session->scene()->instances().size() == 3);
    const auto oldPublication = incumbent.session->publicationHandle();
    const auto sourceFor = [](const AssemblyViewportSession& session,
                              DefinitionId definition) {
      const auto found = std::find_if(
          session.scene()->sources().begin(), session.scene()->sources().end(),
          [definition](const auto& source) {
            return source.key.definitionId == definition.value();
          });
      CHECK(found != session.scene()->sources().end());
      return &*found;
    };
    const auto oldEditedSource = sourceFor(*incumbent.session, DefinitionId{10});
    const auto oldOtherSource = sourceFor(*incumbent.session, DefinitionId{20});
    const auto oldEditedMesh = oldEditedSource->mesh;
    const auto oldOtherMesh = oldOtherSource->mesh;
    const auto oldOtherTopology = oldOtherSource->topologyLineage;
    const auto presentationFor = [](const AssemblyViewportSession& session,
                                    OccurrenceId id) {
      const auto found = std::find_if(
          session.scene()->snapshot()->instances().begin(),
          session.scene()->snapshot()->instances().end(),
          [id](const auto& instance) {
            return instance.occurrencePath().entries() ==
                   std::vector<OccurrenceId>{id};
          });
      CHECK(found != session.scene()->snapshot()->instances().end());
      return &*found;
    };
    const auto* oldFirst = presentationFor(*incumbent.session, OccurrenceId{1});
    const auto* oldSecond = presentationFor(*incumbent.session, OccurrenceId{2});
    const auto* oldOther = presentationFor(*incumbent.session, OccurrenceId{3});
    const auto firstIdentity = oldFirst->occurrenceIdentity();
    const auto secondIdentity = oldSecond->occurrenceIdentity();
    const auto otherIdentity = oldOther->occurrenceIdentity();
    const auto firstTransform = oldFirst->worldTransform();
    const auto secondTransform = oldSecond->worldTransform();
    const auto otherTransform = oldOther->worldTransform();

    const Document* current = editMade.product->partDocument(DefinitionId{10});
    CHECK(current);
    Document candidate = *current;
    Body* editedBody = candidate.findBody(BodyId{11});
    CHECK(editedBody);
    editedBody->eraseFeaturesFrom(0);
    editedBody->addFeature(std::make_unique<ImportedShapeFeature>(
        12, std::make_shared<const TopoDS_Shape>(
                BRepPrimAPI_MakeBox(16.0, 8.0, 6.0).Shape()),
        "edited shape"));
    AcceptSuccessor editSuccessor;
    const DefinitionEditAddress editAddress{
        editMade.product->incarnation(), editMade.product->rootDefinitionId(),
        DefinitionId{10},
        editMade.product->definitionStateId(DefinitionId{10})};
    CHECK(editMade.product
              ->applyDefinitionEdit(editAddress, candidate, editSuccessor)
              .status == DefinitionEditStatus::Committed);
    CHECK(editSuccessor.lastImpact ==
          ProductDocumentSuccessorImpact::DefinitionProjection);

    auto projected = AssemblyViewportSession::prepareDefinitionProjection(
        *editMade.product, *incumbent.session, profile);
    CHECK(projected);
    CHECK(projected.session->epoch() == epoch);
    CHECK(!projected.session->publicationHandle().isSameCurrentPublication(
        oldPublication));
    CHECK(projected.session->scene()->sources().size() == 2);
    CHECK(projected.session->scene()->instances().size() == 3);
    CHECK(projected.session->sceneDiagnostics().meshBuildAttempts == 1);
    CHECK(projected.session->picking()
              .diagnostics()
              .topologySourceBuildAttempts == 1);
    CHECK(projected.session->picking()
              .diagnostics()
              .residentTopologySources == 2);
    const auto newEditedSource = sourceFor(*projected.session, DefinitionId{10});
    const auto newOtherSource = sourceFor(*projected.session, DefinitionId{20});
    const auto newEditedMesh = newEditedSource->mesh;
    CHECK(newEditedMesh != oldEditedMesh);
    CHECK(newEditedSource->key != oldEditedSource->key);
    CHECK(newEditedSource->shape.get() != oldEditedSource->shape.get());
    CHECK(newOtherSource->mesh == oldOtherMesh);
    CHECK(newOtherSource->topologyLineage == oldOtherTopology);
    const auto editedInstances = std::count_if(
        projected.session->scene()->instances().begin(),
        projected.session->scene()->instances().end(),
        [](const auto& instance) {
          return instance.sourceKey.definitionId == DefinitionId{10}.value();
        });
    CHECK(editedInstances == 2);
    for (const auto& instance : projected.session->scene()->instances()) {
      if (instance.sourceKey.definitionId == DefinitionId{10}.value())
        CHECK(instance.mesh == newEditedMesh);
    }
    const auto* newFirst = presentationFor(*projected.session, OccurrenceId{1});
    const auto* newSecond = presentationFor(*projected.session, OccurrenceId{2});
    const auto* newOther = presentationFor(*projected.session, OccurrenceId{3});
    CHECK(newFirst->occurrenceIdentity() == firstIdentity);
    CHECK(newSecond->occurrenceIdentity() == secondIdentity);
    CHECK(newOther->occurrenceIdentity() == otherIdentity);
    CHECK(newFirst->worldTransform() == firstTransform);
    CHECK(newSecond->worldTransform() == secondTransform);
    CHECK(newOther->worldTransform() == otherTransform);
    CHECK(newFirst->occurrenceVisible() == oldFirst->occurrenceVisible());
    CHECK(newFirst->suppressed() == oldFirst->suppressed());
    CHECK(newFirst->grounded() == oldFirst->grounded());
    CHECK(newSecond->occurrenceVisible() == oldSecond->occurrenceVisible());
    CHECK(newSecond->suppressed() == oldSecond->suppressed());
    CHECK(newSecond->grounded() == oldSecond->grounded());
    CHECK(newOther->occurrenceVisible() == oldOther->occurrenceVisible());
    CHECK(newOther->suppressed() == oldOther->suppressed());
    CHECK(newOther->grounded() == oldOther->grounded());
    const auto* projectedRoot = editMade.product->rootAssemblyDefinition();
    CHECK(projectedRoot);
    CHECK(projectedRoot->find(OccurrenceId{1})->displayNameUtf8() ==
          "edited first");
    CHECK(projectedRoot->find(OccurrenceId{2})->displayNameUtf8() ==
          "edited second");
    CHECK(projectedRoot->find(OccurrenceId{3})->displayNameUtf8() ==
          "unaffected");
    CHECK(newFirst->sourceKey() == newSecond->sourceKey());
    CHECK(newFirst->sourceKey().definitionId == DefinitionId{10});
    CHECK(newOther->sourceKey().definitionId == DefinitionId{20});

    for (const auto point : {
             AssemblyViewportSessionFailurePoint::Presentation,
             AssemblyViewportSessionFailurePoint::Scene,
             AssemblyViewportSessionFailurePoint::Picking,
             AssemblyViewportSessionFailurePoint::Spatial,
             AssemblyViewportSessionFailurePoint::Selection,
             AssemblyViewportSessionFailurePoint::Resource,
             AssemblyViewportSessionFailurePoint::FinalPreparation}) {
      const auto failed = AssemblyViewportSession::prepareDefinitionProjection(
          *editMade.product, *incumbent.session, profile, point);
      CHECK(!failed && !failed.session);
      CHECK(incumbent.session->publicationHandle()
                .isSameCurrentPublication(oldPublication));
      CHECK(sourceFor(*incumbent.session, DefinitionId{10})->mesh ==
            oldEditedMesh);
      CHECK(sourceFor(*incumbent.session, DefinitionId{20})->mesh ==
            oldOtherMesh);
    }
  }

  // Visibility replacement keeps the product epoch/source keys, publishes a
  // fresh exact handle and reuses resident source mesh/topology caches.
  auto authored = AssemblyViewportSession::prepare(*value, epoch, profile);
  CHECK(authored);
  CHECK(authored.session->scene()->instances().size() == 2);
  CHECK(authored.session->setSelectedOccurrence(AssemblyOccurrenceSelection{
      authored.session->publicationHandle(), visible}));
  const auto authoredPublication = authored.session->publicationHandle();
  const auto authoredSource = authored.session->scene()->sources().front().mesh;

  CHECK(value->setRootOccurrenceFlags(
            identity(epoch, OccurrenceId{1}).path,
            {.visible = false, .suppressed = false, .grounded = false}) ==
        ProductOccurrenceMutationStatus::Committed);
  auto visibility = AssemblyViewportSession::prepareVisibilityProjection(
      *value, *authored.session, profile);
  CHECK(visibility);
  CHECK(visibility.session->epoch() == epoch);
  CHECK(!visibility.session->publicationHandle().isSameCurrentPublication(
      authoredPublication));
  CHECK(visibility.session->scene()->instances().size() == 1);
  CHECK(visibility.session->scene()->sources().size() == 1);
  CHECK(visibility.session->scene()->sources().front().mesh == authoredSource);
  CHECK(visibility.session->scene()->instances().front().occurrenceIdentity ==
        visiblePeer);
  CHECK(visibility.session->scene()->worldBounds());
  CHECK(visibility.session->scene()->worldBounds()->minimum().x ==
        visibility.session->scene()->instances().front().worldBounds.minimum().x);
  CHECK(visibility.session->scene()->worldBounds()->maximum().z ==
        visibility.session->scene()->instances().front().worldBounds.maximum().z);

  // Restore authored visibility and verify isolate/show-all remains inside
  // the same session/publication and retains all base sources.
  CHECK(value->setRootOccurrenceFlags(
            identity(epoch, OccurrenceId{1}).path,
            {.visible = true, .suppressed = false, .grounded = false}) ==
        ProductOccurrenceMutationStatus::Committed);

  // Grounding republishes occurrence metadata only.  The epoch, source
  // meshes, instance transforms, bounds and picking membership are reused
  // exactly while the publication generation changes.
  auto beforeGrounding = AssemblyViewportSession::prepare(
      *value, epoch, profile);
  CHECK(beforeGrounding);
  CHECK(beforeGrounding.session->setSelectedOccurrence(
      AssemblyOccurrenceSelection{beforeGrounding.session->publicationHandle(),
                                  visible}));
  CHECK(beforeGrounding.session->setHoveredOccurrence(
      AssemblyOccurrenceSelection{beforeGrounding.session->publicationHandle(),
                                  visiblePeer}));
  const auto groundingPublication =
      beforeGrounding.session->publicationHandle();
  const auto groundingSource =
      beforeGrounding.session->scene()->sources().front().mesh;
  const auto groundingInstances =
      beforeGrounding.session->scene()->instances();
  const auto groundingBounds = beforeGrounding.session->scene()->worldBounds();
  const auto groundingPickingInstances =
      beforeGrounding.session->picking().diagnostics().activeInstances;
  auto ground = OccurrenceDelta::grounded(
      {value->incarnation(), value->rootDefinitionId(),
       identity(epoch, OccurrenceId{1}).path, DefinitionId{10}},
      false, true);
  CHECK(value->applyOccurrenceDelta(ground) ==
        OccurrenceDeltaStatus::Committed);
  auto grounding = AssemblyViewportSession::prepareGroundingProjection(
      *value, *beforeGrounding.session, profile);
  CHECK(grounding);
  CHECK(grounding.session->epoch() == epoch);
  CHECK(!grounding.session->publicationHandle().isSameCurrentPublication(
      groundingPublication));
  CHECK(grounding.session->scene()->sources().front().mesh ==
        groundingSource);
  CHECK(grounding.session->scene()->instances().size() ==
        groundingInstances.size());
  for (std::size_t index = 0; index < groundingInstances.size(); ++index) {
    const auto& oldInstance = groundingInstances[index];
    const auto& newInstance =
        grounding.session->scene()->instances()[index];
    CHECK(newInstance.sourceKey == oldInstance.sourceKey);
    CHECK(newInstance.mesh == oldInstance.mesh);
    CHECK(newInstance.localToWorld == oldInstance.localToWorld);
    CHECK(newInstance.occurrenceIdentity == oldInstance.occurrenceIdentity);
    CHECK(newInstance.worldBounds.minimum().x ==
          oldInstance.worldBounds.minimum().x);
    CHECK(newInstance.worldBounds.minimum().y ==
          oldInstance.worldBounds.minimum().y);
    CHECK(newInstance.worldBounds.minimum().z ==
          oldInstance.worldBounds.minimum().z);
    CHECK(newInstance.worldBounds.maximum().x ==
          oldInstance.worldBounds.maximum().x);
    CHECK(newInstance.worldBounds.maximum().y ==
          oldInstance.worldBounds.maximum().y);
    CHECK(newInstance.worldBounds.maximum().z ==
          oldInstance.worldBounds.maximum().z);
    CHECK(newInstance.grounded ==
          (newInstance.occurrenceIdentity == visible ||
           oldInstance.grounded));
  }
  CHECK(grounding.session->scene()->worldBounds().has_value() ==
        groundingBounds.has_value());
  CHECK(groundingBounds);
  CHECK(grounding.session->scene()->worldBounds()->minimum().x ==
        groundingBounds->minimum().x);
  CHECK(grounding.session->scene()->worldBounds()->minimum().y ==
        groundingBounds->minimum().y);
  CHECK(grounding.session->scene()->worldBounds()->minimum().z ==
        groundingBounds->minimum().z);
  CHECK(grounding.session->scene()->worldBounds()->maximum().x ==
        groundingBounds->maximum().x);
  CHECK(grounding.session->scene()->worldBounds()->maximum().y ==
        groundingBounds->maximum().y);
  CHECK(grounding.session->scene()->worldBounds()->maximum().z ==
        groundingBounds->maximum().z);
  CHECK(grounding.session->picking().diagnostics().activeInstances ==
        groundingPickingInstances);
  // Selection rebinding is prepared by MainWindow against this new exact
  // publication; the session projection itself does not copy a stale handle.
  CHECK(!grounding.session->selectedOccurrence());
  CHECK(!grounding.session->selectedHighlight());
  CHECK(!grounding.session->hoveredHighlight());
  const auto groundedOccurrence = std::find_if(
      grounding.session->occurrences().begin(),
      grounding.session->occurrences().end(), [&](const auto& state) {
        return state.identity == visible;
      });
  const auto ungroundedPeer = std::find_if(
      grounding.session->occurrences().begin(),
      grounding.session->occurrences().end(), [&](const auto& state) {
        return state.identity == visiblePeer;
      });
  CHECK(groundedOccurrence != grounding.session->occurrences().end());
  CHECK(ungroundedPeer != grounding.session->occurrences().end());
  CHECK(groundedOccurrence->grounded);
  CHECK(!ungroundedPeer->grounded);
  for (const auto point : {
           AssemblyViewportSessionFailurePoint::Presentation,
           AssemblyViewportSessionFailurePoint::Scene,
           AssemblyViewportSessionFailurePoint::Picking,
           AssemblyViewportSessionFailurePoint::Spatial,
           AssemblyViewportSessionFailurePoint::Selection,
           AssemblyViewportSessionFailurePoint::Resource,
           AssemblyViewportSessionFailurePoint::FinalPreparation}) {
    const auto failed = AssemblyViewportSession::prepareGroundingProjection(
        *value, *beforeGrounding.session, profile, point);
    CHECK(!failed);
    CHECK(!failed.session);
  }
  CHECK(value->revertOccurrenceDelta(ground) ==
        OccurrenceDeltaStatus::Committed);

  // Suppression uses the same same-epoch publication machinery but removes
  // the occurrence from every effective scene consumer.  A selected
  // suppressed row remains a semantic selection without a viewport highlight.
  auto beforeSuppression = AssemblyViewportSession::prepare(
      *value, epoch, profile);
  CHECK(beforeSuppression);
  const auto suppressionPublication =
      beforeSuppression.session->publicationHandle();
  const auto suppressionSource =
      beforeSuppression.session->scene()->sources().front().mesh;
  const auto suppressionSourceKey =
      beforeSuppression.session->scene()->sources().front().key;
  const auto beforeSuppressionInstances =
      beforeSuppression.session->scene()->instances().size();
  auto suppress = OccurrenceDelta::suppressed(
      {value->incarnation(), value->rootDefinitionId(),
       identity(epoch, OccurrenceId{1}).path, DefinitionId{10}},
      false, true);
  CHECK(value->applyOccurrenceDelta(suppress) ==
        OccurrenceDeltaStatus::Committed);
  auto suppression = AssemblyViewportSession::prepareSuppressionProjection(
      *value, *beforeSuppression.session, profile);
  CHECK(suppression);
  CHECK(suppression.session->epoch() == epoch);
  CHECK(!suppression.session->publicationHandle().isSameCurrentPublication(
      suppressionPublication));
  CHECK(suppression.session->scene()->sources().size() == 1);
  CHECK(suppression.session->scene()->sources().front().key ==
        suppressionSourceKey);
  CHECK(suppression.session->scene()->sources().front().mesh ==
        suppressionSource);
  CHECK(suppression.session->scene()->instances().size() + 1 ==
        beforeSuppressionInstances);
  CHECK(std::none_of(suppression.session->scene()->instances().begin(),
                     suppression.session->scene()->instances().end(),
                     [&](const auto& instance) {
                       return instance.occurrenceIdentity == visible;
                     }));
  CHECK(std::any_of(suppression.session->scene()->instances().begin(),
                    suppression.session->scene()->instances().end(),
                    [&](const auto& instance) {
                      return instance.occurrenceIdentity == visiblePeer;
                    }));
  const auto selectedSuppressed =
      AssemblyOccurrenceSelection{suppression.session->publicationHandle(),
                                  visible};
  auto preparedSuppressed =
      suppression.session->prepareSelectedOccurrence(selectedSuppressed);
  CHECK(preparedSuppressed.status ==
        PrepareAssemblySelectionStatus::Prepared);
  suppression.session->installPreparedSelectedOccurrence(
      std::move(preparedSuppressed.state));
  CHECK(suppression.session->selectedOccurrence());
  CHECK(!suppression.session->selectedHighlight());
  CHECK(!suppression.session->hoveredHighlight());
  const auto suppressedOccurrence = std::find_if(
      suppression.session->occurrences().begin(),
      suppression.session->occurrences().end(), [&](const auto& state) {
        return state.identity == visible;
      });
  CHECK(suppressedOccurrence != suppression.session->occurrences().end());
  CHECK(suppressedOccurrence->suppressed);
  CHECK(!suppressedOccurrence->effectivelyVisible);
  for (const auto point : {
           AssemblyViewportSessionFailurePoint::Presentation,
           AssemblyViewportSessionFailurePoint::Scene,
           AssemblyViewportSessionFailurePoint::Picking,
           AssemblyViewportSessionFailurePoint::Spatial,
           AssemblyViewportSessionFailurePoint::Selection,
           AssemblyViewportSessionFailurePoint::Resource,
           AssemblyViewportSessionFailurePoint::FinalPreparation}) {
    const auto failed = AssemblyViewportSession::prepareSuppressionProjection(
        *value, *beforeSuppression.session, profile, point);
    CHECK(!failed);
    CHECK(!failed.session);
  }
  CHECK(value->revertOccurrenceDelta(suppress) ==
        OccurrenceDeltaStatus::Committed);

  // Unsuppressing does not bypass the independent authored-visible or
  // body-visible filters.  Both peers remain absent on either side of the
  // suppression bit transition.
  const auto checkUnsuppressDoesNotReveal =
      [&](DefinitionId target,
          const AssemblyOccurrenceIdentity& occurrence) {
        auto hiddenSuppression = OccurrenceDelta::suppressed(
            {value->incarnation(), value->rootDefinitionId(),
             occurrence.path, target},
            false, true);
        CHECK(value->applyOccurrenceDelta(hiddenSuppression) ==
              OccurrenceDeltaStatus::Committed);
        auto whileSuppressed =
            AssemblyViewportSession::prepareSuppressionProjection(
                *value, *beforeSuppression.session, profile);
        CHECK(whileSuppressed);
        CHECK(std::none_of(
            whileSuppressed.session->scene()->instances().begin(),
            whileSuppressed.session->scene()->instances().end(),
            [&](const auto& instance) {
              return instance.occurrenceIdentity == occurrence;
            }));
        CHECK(value->revertOccurrenceDelta(hiddenSuppression) ==
              OccurrenceDeltaStatus::Committed);
        auto afterUnsuppress =
            AssemblyViewportSession::prepareSuppressionProjection(
                *value, *beforeSuppression.session, profile);
        CHECK(afterUnsuppress);
        CHECK(std::none_of(
            afterUnsuppress.session->scene()->instances().begin(),
            afterUnsuppress.session->scene()->instances().end(),
            [&](const auto& instance) {
              return instance.occurrenceIdentity == occurrence;
            }));
        CHECK(afterUnsuppress.session->scene()->instances().size() == 2);
      };
  checkUnsuppressDoesNotReveal(DefinitionId{10}, hidden);
  checkUnsuppressDoesNotReveal(DefinitionId{20}, bodyHidden);

  // Duplicate publishes a fresh same-epoch occurrence projection while
  // sharing the exact source mesh/topology. Visible copies add one instance;
  // authored-hidden, suppressed and body-hidden copies remain semantic-only.
  auto beforeDuplicate = AssemblyViewportSession::prepare(
      *value, epoch, profile);
  CHECK(beforeDuplicate);
  const auto duplicatePublication =
      beforeDuplicate.session->publicationHandle();
  const auto duplicateMesh =
      beforeDuplicate.session->scene()->sources().front().mesh;
  const auto duplicateSourceKey =
      beforeDuplicate.session->scene()->sources().front().key;
  const auto duplicateInstances =
      beforeDuplicate.session->scene()->instances().size();
  const auto duplicatePickingInstances =
      beforeDuplicate.session->picking().diagnostics().activeInstances;
  const auto duplicateTopologySources =
      beforeDuplicate.session->picking().diagnostics()
          .residentTopologySources;
  auto visibleDuplicatePayload =
      duplicatePayload(*value, visible, OccurrenceId{6});
  AcceptSuccessor acceptDuplicate;
  CHECK(value->applyDuplicateOccurrence(
            visibleDuplicatePayload, acceptDuplicate,
            DuplicateOccurrenceFailurePoint::None) ==
        DuplicateOccurrenceStatus::Committed);
  CHECK(acceptDuplicate.lastImpact ==
        ProductDocumentSuccessorImpact::DuplicateProjection);
  auto duplicated = AssemblyViewportSession::prepareDuplicateProjection(
      *value, *beforeDuplicate.session, profile);
  CHECK(duplicated);
  CHECK(duplicated.session->epoch() == epoch);
  CHECK(!duplicated.session->publicationHandle().isSameCurrentPublication(
      duplicatePublication));
  CHECK(duplicated.session->scene()->sources().size() == 1);
  CHECK(duplicated.session->scene()->sources().front().key ==
        duplicateSourceKey);
  CHECK(duplicated.session->scene()->sources().front().mesh == duplicateMesh);
  CHECK(duplicated.session->sceneDiagnostics().meshBuildAttempts == 0);
  CHECK(duplicated.session->scene()->instances().size() ==
        duplicateInstances + 1);
  CHECK(duplicated.session->picking().diagnostics().activeInstances ==
        duplicatePickingInstances + 1);
  CHECK(duplicated.session->picking().diagnostics()
            .topologySourceBuildAttempts == 0);
  CHECK(duplicated.session->picking().diagnostics()
            .topologySourceBuildSuccesses == 0);
  CHECK(duplicated.session->picking().diagnostics().topologySourceReuses ==
        duplicated.session->scene()->sources().size());
  CHECK(duplicated.session->picking().diagnostics()
            .residentTopologySources == duplicateTopologySources);
  const auto duplicatedIdentity = identity(epoch, OccurrenceId{6});
  const auto duplicateInstance = std::find_if(
      duplicated.session->scene()->instances().begin(),
      duplicated.session->scene()->instances().end(),
      [&](const auto& instance) {
        return instance.occurrenceIdentity == duplicatedIdentity;
      });
  CHECK(duplicateInstance != duplicated.session->scene()->instances().end());
  const auto sourceInstance = std::find_if(
      duplicated.session->scene()->instances().begin(),
      duplicated.session->scene()->instances().end(),
      [&](const auto& instance) {
        return instance.occurrenceIdentity == visible;
      });
  CHECK(sourceInstance != duplicated.session->scene()->instances().end());
  CHECK(duplicateInstance->sourceKey == sourceInstance->sourceKey);
  CHECK(duplicateInstance->mesh == sourceInstance->mesh);
  CHECK(duplicateInstance->localToWorld == sourceInstance->localToWorld);
  CHECK(duplicateInstance->worldBounds.minimum().x ==
        sourceInstance->worldBounds.minimum().x);
  CHECK(duplicateInstance->worldBounds.maximum().z ==
        sourceInstance->worldBounds.maximum().z);

  // A rectangle query is deterministic for exact-overlap duplicates whereas
  // a single ray has an intentional tie. The topology-backed current
  // publication must expose both distinct occurrence identities, never two
  // aliases of the source and never a stale publication.
  constexpr QSize duplicateLogicalSize{800, 600};
  const auto duplicateFit = fitAssemblyViewport(
      duplicated.session->scene(), duplicateLogicalSize, 0.0F, 0.0F);
  CHECK(duplicateFit);
  const ViewportCameraState duplicateCamera{
      0.0F, 0.0F, duplicateFit.fit->zoom, duplicateFit.fit->pan,
      duplicateLogicalSize, 1.0F, duplicateFit.fit->center,
      duplicateFit.fit->depthExtent};
  const auto duplicateRectangle =
      duplicated.session->picking().facesInRectangle(
          duplicateCamera, QPointF{0.0, 0.0}, QPointF{799.0, 599.0});
  CHECK(duplicateRectangle);
  CHECK(std::all_of(duplicateRectangle.hits.begin(),
                    duplicateRectangle.hits.end(), [&](const auto& hit) {
                      return hit.publication.isSameCurrentPublication(
                          duplicated.session->publicationHandle());
                    }));
  CHECK(std::any_of(duplicateRectangle.hits.begin(),
                    duplicateRectangle.hits.end(), [&](const auto& hit) {
                      return hit.address.occurrence == visible;
                    }));
  CHECK(std::any_of(duplicateRectangle.hits.begin(),
                    duplicateRectangle.hits.end(), [&](const auto& hit) {
                      return hit.address.occurrence == duplicatedIdentity;
                    }));
  CHECK(duplicated.session->picking().diagnostics()
            .topologySourceBuildAttempts == 0);
  auto preparedDuplicateSelection =
      duplicated.session->prepareSelectedOccurrence(
          AssemblyOccurrenceSelection{
              duplicated.session->publicationHandle(), duplicatedIdentity});
  CHECK(preparedDuplicateSelection.status ==
        PrepareAssemblySelectionStatus::Prepared);
  duplicated.session->installPreparedSelectedOccurrence(
      std::move(preparedDuplicateSelection.state));
  CHECK(duplicated.session->selectedHighlight());

  for (const auto point : {
           AssemblyViewportSessionFailurePoint::Presentation,
           AssemblyViewportSessionFailurePoint::Scene,
           AssemblyViewportSessionFailurePoint::Picking,
           AssemblyViewportSessionFailurePoint::Spatial,
           AssemblyViewportSessionFailurePoint::Selection,
           AssemblyViewportSessionFailurePoint::Resource,
           AssemblyViewportSessionFailurePoint::FinalPreparation}) {
    const auto failed = AssemblyViewportSession::prepareDuplicateProjection(
        *value, *beforeDuplicate.session, profile, point);
    CHECK(!failed);
    CHECK(!failed.session);
  }
  CHECK(value->extractDuplicateOccurrence(
            visibleDuplicatePayload, acceptDuplicate,
            DuplicateOccurrenceFailurePoint::None) ==
        DuplicateOccurrenceStatus::Extracted);

  // Remove is a fresh same-epoch publication. Removing one of two effective
  // users retains the exact mesh/topology source; Undo restores the stable ID
  // and highlight. Hidden removal changes semantic rows only.
  auto beforeRemove = AssemblyViewportSession::prepare(*value, epoch, profile);
  CHECK(beforeRemove);
  const auto removePublication =
      beforeRemove.session->publicationHandle();
  const auto removeMesh = beforeRemove.session->scene()->sources().front().mesh;
  const auto removeSourceKey =
      beforeRemove.session->scene()->sources().front().key;
  const auto removeInstances = beforeRemove.session->scene()->instances().size();
  const auto removePickingInstances =
      beforeRemove.session->picking().diagnostics().activeInstances;
  const auto removeTopologySources =
      beforeRemove.session->picking().diagnostics().residentTopologySources;
  auto visibleRemovePayload = removePayload(*value, visible);
  CHECK(value->applyRemoveOccurrence(
            visibleRemovePayload, acceptDuplicate,
            RemoveOccurrenceFailurePoint::None) ==
        RemoveOccurrenceStatus::Removed);
  CHECK(acceptDuplicate.lastImpact ==
        ProductDocumentSuccessorImpact::RemoveProjection);
  auto removedProjection = AssemblyViewportSession::prepareRemoveProjection(
      *value, *beforeRemove.session, profile);
  CHECK(removedProjection);
  CHECK(removedProjection.session->epoch() == epoch);
  CHECK(!removedProjection.session->publicationHandle()
             .isSameCurrentPublication(removePublication));
  CHECK(removedProjection.session->scene()->sources().size() == 1);
  CHECK(removedProjection.session->scene()->sources().front().key ==
        removeSourceKey);
  CHECK(removedProjection.session->scene()->sources().front().mesh ==
        removeMesh);
  CHECK(removedProjection.session->sceneDiagnostics().meshBuildAttempts == 0);
  CHECK(removedProjection.session->scene()->instances().size() ==
        removeInstances - 1);
  CHECK(removedProjection.session->picking().diagnostics().activeInstances ==
        removePickingInstances - 1);
  CHECK(removedProjection.session->picking().diagnostics()
            .topologySourceBuildAttempts == 0);
  CHECK(removedProjection.session->picking().diagnostics()
            .residentTopologySources == removeTopologySources);
  CHECK(std::none_of(
      removedProjection.session->scene()->instances().begin(),
      removedProjection.session->scene()->instances().end(),
      [&](const auto& instance) {
        return instance.occurrenceIdentity.path == visible.path;
      }));
  for (const auto point : {
           AssemblyViewportSessionFailurePoint::Presentation,
           AssemblyViewportSessionFailurePoint::Scene,
           AssemblyViewportSessionFailurePoint::Picking,
           AssemblyViewportSessionFailurePoint::Spatial,
           AssemblyViewportSessionFailurePoint::Selection,
           AssemblyViewportSessionFailurePoint::Resource,
           AssemblyViewportSessionFailurePoint::FinalPreparation}) {
    const auto failed = AssemblyViewportSession::prepareRemoveProjection(
        *value, *beforeRemove.session, profile, point);
    CHECK(!failed);
    CHECK(!failed.session);
  }
  CHECK(value->restoreRemovedOccurrence(
            visibleRemovePayload, acceptDuplicate,
            RemoveOccurrenceFailurePoint::None) ==
        RemoveOccurrenceStatus::Restored);
  auto restoredProjection = AssemblyViewportSession::prepareRemoveProjection(
      *value, *removedProjection.session, profile);
  CHECK(restoredProjection);
  auto restoredSelection =
      restoredProjection.session->prepareSelectedOccurrence(
          AssemblyOccurrenceSelection{
              restoredProjection.session->publicationHandle(), visible});
  CHECK(restoredSelection.status == PrepareAssemblySelectionStatus::Prepared);
  restoredProjection.session->installPreparedSelectedOccurrence(
      std::move(restoredSelection.state));
  CHECK(restoredProjection.session->selectedHighlight());
  CHECK(restoredProjection.session->scene()->sources().front().mesh ==
        removeMesh);

  auto hiddenRemovePayload = removePayload(*value, hidden);
  const auto hiddenRemoveInstances =
      restoredProjection.session->scene()->instances().size();
  CHECK(value->applyRemoveOccurrence(
            hiddenRemovePayload, acceptDuplicate,
            RemoveOccurrenceFailurePoint::None) ==
        RemoveOccurrenceStatus::Removed);
  auto hiddenRemoved = AssemblyViewportSession::prepareRemoveProjection(
      *value, *restoredProjection.session, profile);
  CHECK(hiddenRemoved);
  CHECK(hiddenRemoved.session->scene()->instances().size() ==
        hiddenRemoveInstances);
  CHECK(hiddenRemoved.session->sceneDiagnostics().meshBuildAttempts == 0);
  CHECK(value->restoreRemovedOccurrence(
            hiddenRemovePayload, acceptDuplicate,
            RemoveOccurrenceFailurePoint::None) ==
        RemoveOccurrenceStatus::Restored);

  const auto checkInvisibleDuplicate =
      [&](const AssemblyOccurrenceIdentity& sourceIdentity,
          OccurrenceId duplicateId) {
        auto payload =
            duplicatePayload(*value, sourceIdentity, duplicateId);
        CHECK(value->applyDuplicateOccurrence(
                  payload, acceptDuplicate,
                  DuplicateOccurrenceFailurePoint::None) ==
              DuplicateOccurrenceStatus::Committed);
        auto projected = AssemblyViewportSession::prepareDuplicateProjection(
            *value, *beforeDuplicate.session, profile);
        CHECK(projected);
        const auto duplicateIdentity = identity(epoch, duplicateId);
        CHECK(projected.session->scene()->instances().size() ==
              duplicateInstances);
        CHECK(std::none_of(
            projected.session->scene()->instances().begin(),
            projected.session->scene()->instances().end(),
            [&](const auto& instance) {
              return instance.occurrenceIdentity == duplicateIdentity;
            }));
        auto selection = projected.session->prepareSelectedOccurrence(
            AssemblyOccurrenceSelection{
                projected.session->publicationHandle(), duplicateIdentity});
        CHECK(selection.status == PrepareAssemblySelectionStatus::Prepared);
        projected.session->installPreparedSelectedOccurrence(
            std::move(selection.state));
        CHECK(projected.session->selectedOccurrence());
        CHECK(!projected.session->selectedHighlight());
        CHECK(value->extractDuplicateOccurrence(
                  payload, acceptDuplicate,
                  DuplicateOccurrenceFailurePoint::None) ==
              DuplicateOccurrenceStatus::Extracted);
      };
  checkInvisibleDuplicate(hidden, OccurrenceId{6});
  checkInvisibleDuplicate(bodyHidden, OccurrenceId{6});
  checkInvisibleDuplicate(suppressed, OccurrenceId{6});

  auto isolateSession = AssemblyViewportSession::prepare(*value, epoch, profile);
  CHECK(isolateSession);
  CHECK(isolateSession.session->scene()->instances().size() == 2);
  const auto isolationPublication =
      isolateSession.session->publicationHandle();
  CHECK(isolateSession.session->setSelectedOccurrence(
      AssemblyOccurrenceSelection{isolationPublication, visible}));
  CHECK(isolateSession.session->setHoveredOccurrence(
      AssemblyOccurrenceSelection{isolationPublication, visiblePeer}));
  CHECK(isolateSession.session->hoveredHighlight());
  CHECK(isolateSession.session->canIsolate(visible));
  CHECK(!isolateSession.session->canIsolate(hidden));
  CHECK(!isolateSession.session->canIsolate(bodyHidden));
  CHECK(!isolateSession.session->canIsolate(suppressed));

  // Every spatial consumer observes the same authored base scene.  The
  // separated peers make stale caches visible: both effective occurrences
  // must be queryable before isolate, only the target during isolate, and both
  // again after Show All. Authored-hidden, suppressed and body-hidden peers
  // must never appear in any result.
  const QSize logicalSize{800, 600};
  const auto fittedBefore = fitAssemblyViewport(
      isolateSession.session->scene(), logicalSize, 0.0F, 0.0F);
  CHECK(fittedBefore);
  const ViewportCameraState queryCamera{
      0.0F, 0.0F, fittedBefore.fit->zoom, fittedBefore.fit->pan, logicalSize,
      1.0F, fittedBefore.fit->center, fittedBefore.fit->depthExtent};
  const auto pointFor = [](const AssemblyViewportSceneInstance& instance) {
    const auto center = instance.worldBounds.center();
    return Point3d{center.x, center.y, instance.worldBounds.maximum().z};
  };
  const auto* visibleInstance = &isolateSession.session->scene()->instances()[0];
  const auto* peerInstance = &isolateSession.session->scene()->instances()[1];
  CHECK(visibleInstance->occurrenceIdentity == visible);
  CHECK(peerInstance->occurrenceIdentity == visiblePeer);
  const QPointF visiblePixel = queryCamera.worldToScreen(pointFor(*visibleInstance));
  const QPointF peerPixel = queryCamera.worldToScreen(pointFor(*peerInstance));
  const auto visibleFace = isolateSession.session->faceAt(queryCamera, visiblePixel);
  const auto peerFace = isolateSession.session->faceAt(queryCamera, peerPixel);
  CHECK(visibleFace && visibleFace.hit->address.occurrence == visible);
  CHECK(peerFace && peerFace.hit->address.occurrence == visiblePeer);
  const auto beforeRectangle =
      isolateSession.session->picking().facesInRectangle(
          queryCamera, QPointF{0.0, 0.0}, QPointF{799.0, 599.0});
  CHECK(beforeRectangle);
  CHECK(std::all_of(beforeRectangle.hits.begin(), beforeRectangle.hits.end(),
                    [&](const auto& hit) {
                      return hit.publication.isSameCurrentPublication(
                                 isolationPublication) &&
                             (hit.address.occurrence == visible ||
                              hit.address.occurrence == visiblePeer);
                    }));
  CHECK(std::any_of(beforeRectangle.hits.begin(), beforeRectangle.hits.end(),
                    [&](const auto& hit) {
                      return hit.address.occurrence == visible;
                    }));
  CHECK(std::any_of(beforeRectangle.hits.begin(), beforeRectangle.hits.end(),
                    [&](const auto& hit) {
                      return hit.address.occurrence == visiblePeer;
                    }));
  const auto beforeVisibleSnap = isolateSession.session->picking().snapAt(
      queryCamera, visiblePixel, 12.0);
  const auto beforePeerSnap = isolateSession.session->picking().snapAt(
      queryCamera, peerPixel, 12.0);
  CHECK(beforeVisibleSnap && beforeVisibleSnap.hit->occurrence == visible);
  CHECK(beforePeerSnap && beforePeerSnap.hit->occurrence == visiblePeer);
  AssemblyViewportRuler ruler;
  CHECK(ruler.commit(beforeVisibleSnap) ==
        AssemblyViewportRulerCommitStatus::AcceptedFirstPoint);
  CHECK(ruler.commit(beforePeerSnap) ==
        AssemblyViewportRulerCommitStatus::AcceptedSecondPoint);
  CHECK(ruler.distance().has_value());

  const auto baseScene = isolateSession.session->scene();
  const auto basePickingDiagnostics =
      isolateSession.session->picking().diagnostics();
  for (const auto point : {AssemblyIsolationFailurePoint::Scene,
                           AssemblyIsolationFailurePoint::Picking,
                           AssemblyIsolationFailurePoint::Resource}) {
    const auto failed = isolateSession.session->prepareIsolation(visible, point);
    CHECK(!failed);
    CHECK(isolateSession.session->scene() == baseScene);
    CHECK(!isolateSession.session->isolationActive());
    CHECK(isolateSession.session->publicationHandle()
              .isSameCurrentPublication(isolationPublication));
    CHECK(isolateSession.session->selectedHighlight());
    CHECK(isolateSession.session->hoveredHighlight());
    CHECK(isolateSession.session->picking().diagnostics().activeGeneration ==
          basePickingDiagnostics.activeGeneration);
  }
  auto isolate = isolateSession.session->prepareIsolation(visible);
  CHECK(isolate);
  CHECK(isolate.state->displayedScene->sources().size() ==
        isolateSession.session->scene()->sources().size());
  CHECK(isolate.state->displayedScene->instances().size() == 1);
  CHECK(isolate.state->displayedScene->instances().front().occurrenceIdentity ==
        visible);
  CHECK(isolate.state->displayedScene->publicationHandle()
            .isSameCurrentPublication(isolationPublication));
  isolateSession.session->installPreparedIsolation(std::move(*isolate.state));
  CHECK(isolateSession.session->isolationActive());
  CHECK(isolateSession.session->scene()->instances().size() == 1);
  CHECK(isolateSession.session->selectedHighlight());
  CHECK(!isolateSession.session->hoveredHighlight());
  const auto fittedIsolated = fitAssemblyViewport(
      isolateSession.session->scene(), logicalSize, 0.0F, 0.0F);
  CHECK(fittedIsolated);
  CHECK(samePoint(fittedIsolated.fit->center,
                  visibleInstance->worldBounds.center()));
  const auto isolatedVisibleFace =
      isolateSession.session->faceAt(queryCamera, visiblePixel);
  const auto isolatedPeerFace =
      isolateSession.session->faceAt(queryCamera, peerPixel);
  CHECK(isolatedVisibleFace &&
        isolatedVisibleFace.hit->address.occurrence == visible);
  CHECK(isolatedPeerFace.status == AssemblyViewportPickingQueryStatus::Miss);
  const auto isolatedRectangle =
      isolateSession.session->picking().facesInRectangle(
          queryCamera, QPointF{0.0, 0.0}, QPointF{799.0, 599.0});
  CHECK(isolatedRectangle);
  CHECK(std::all_of(isolatedRectangle.hits.begin(),
                    isolatedRectangle.hits.end(), [&](const auto& hit) {
                      return hit.address.occurrence == visible &&
                             hit.publication.isSameCurrentPublication(
                                 isolationPublication);
                    }));
  const auto isolatedPeerSnap = isolateSession.session->picking().snapAt(
      queryCamera, peerPixel, 12.0);
  CHECK(isolatedPeerSnap.status == AssemblyViewportPickingQueryStatus::Miss);
  ruler.validateCurrent(isolateSession.session->publicationHandle());
  CHECK(ruler.firstPoint() && ruler.secondPoint());
  auto showAll = isolateSession.session->prepareIsolation(std::nullopt);
  CHECK(showAll);
  isolateSession.session->installPreparedIsolation(
      std::move(*showAll.state));
  CHECK(!isolateSession.session->isolationActive());
  CHECK(isolateSession.session->scene()->instances().size() == 2);
  CHECK(isolateSession.session->publicationHandle()
            .isSameCurrentPublication(isolationPublication));
  const auto fittedAfter = fitAssemblyViewport(
      isolateSession.session->scene(), logicalSize, 0.0F, 0.0F);
  CHECK(fittedAfter);
  CHECK(samePoint(fittedAfter.fit->center, fittedBefore.fit->center));
  CHECK(fittedAfter.fit->zoom == fittedBefore.fit->zoom);
  CHECK(fittedAfter.fit->pan == fittedBefore.fit->pan);
  CHECK(isolateSession.session->faceAt(queryCamera, peerPixel));
  CHECK(isolateSession.session->picking()
            .snapAt(queryCamera, peerPixel, 12.0));

  // Last-user Remove drops both mesh and topology residents. Its empty-scene
  // camera keeps the caller-owned yaw/pitch/zoom/pan and uses the documented
  // neutral derived center/depth. Undo reacquires topology and every hit is
  // rebound to the new current publication.
  {
    DefinitionRegistry lastDefinitions;
    CHECK(lastDefinitions.insert(
              PartDefinition(DefinitionId{10}, shapedPart())) ==
          DefinitionInsertStatus::Inserted);
    AssemblyDefinition lastRoot(DefinitionId{100});
    CHECK(lastRoot.insert(ComponentOccurrence(
              OccurrenceId{1}, DefinitionId{10},
              RigidTransform3d::identity())) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    CHECK(lastDefinitions.insert(std::move(lastRoot)) ==
          DefinitionInsertStatus::Inserted);
    auto lastMade =
        ProductDocument::create(std::move(lastDefinitions), DefinitionId{100});
    CHECK(lastMade);
    auto lastSession =
        AssemblyViewportSession::prepare(*lastMade.product, epoch, profile);
    CHECK(lastSession);
    const QSize lastSize{640, 480};
    const auto lastFit = fitAssemblyViewport(lastSession.session->scene(),
                                             lastSize, 0.0F, 0.0F);
    CHECK(lastFit);
    const ViewportCameraState lastCamera{
        19.0F, -11.0F, lastFit.fit->zoom, lastFit.fit->pan, lastSize, 1.0F,
        lastFit.fit->center, lastFit.fit->depthExtent};
    const auto initialHits = lastSession.session->picking().facesInRectangle(
        lastCamera, QPointF{0.0, 0.0},
        QPointF{static_cast<double>(lastSize.width() - 1),
                static_cast<double>(lastSize.height() - 1)});
    CHECK(initialHits && !initialHits.hits.empty());
    CHECK(lastSession.session->picking().diagnostics().residentTopologySources ==
          1);

    const auto lastIdentity = identity(epoch, OccurrenceId{1});
    const auto lastPayload = removePayload(*lastMade.product, lastIdentity);
    AcceptSuccessor lastSuccessor;
    CHECK(lastMade.product->applyRemoveOccurrence(
              lastPayload, lastSuccessor,
              RemoveOccurrenceFailurePoint::None) ==
          RemoveOccurrenceStatus::Removed);
    auto emptyProjection = AssemblyViewportSession::prepareRemoveProjection(
        *lastMade.product, *lastSession.session, profile);
    CHECK(emptyProjection);
    CHECK(emptyProjection.session->scene()->instances().empty());
    CHECK(emptyProjection.session->scene()->sources().empty());
    CHECK(emptyProjection.session->picking().diagnostics().activeInstances == 0);
    CHECK(emptyProjection.session->picking().diagnostics().activeSources == 0);
    CHECK(emptyProjection.session->picking()
              .diagnostics()
              .residentTopologySources == 0);
    const QPointF preservedPan{17.0, -9.0};
    const auto emptyCamera = emptyProjection.session->camera(
        37.0F, -21.0F, 2.75F, preservedPan, lastSize);
    CHECK(emptyCamera.yawDeg == 37.0F && emptyCamera.pitchDeg == -21.0F &&
          emptyCamera.zoom == 2.75F && emptyCamera.pan == preservedPan &&
          emptyCamera.logicalSize == lastSize &&
          samePoint(emptyCamera.center, Point3d{}) &&
          emptyCamera.depthExtent == 1.0);

    CHECK(lastMade.product->restoreRemovedOccurrence(
              lastPayload, lastSuccessor,
              RemoveOccurrenceFailurePoint::None) ==
          RemoveOccurrenceStatus::Restored);
    auto reacquired = AssemblyViewportSession::prepareRemoveProjection(
        *lastMade.product, *emptyProjection.session, profile);
    CHECK(reacquired);
    CHECK(reacquired.session->scene()->sources().size() == 1);
    CHECK(reacquired.session->sceneDiagnostics().meshBuildAttempts > 0);
    CHECK(reacquired.session->picking()
              .diagnostics()
              .topologySourceBuildAttempts > 0);
    CHECK(reacquired.session->picking().diagnostics().residentTopologySources ==
          1);
    const auto reacquiredFit = fitAssemblyViewport(reacquired.session->scene(),
                                                   lastSize, 0.0F, 0.0F);
    CHECK(reacquiredFit);
    const ViewportCameraState reacquiredCamera{
        0.0F, 0.0F, reacquiredFit.fit->zoom, reacquiredFit.fit->pan, lastSize,
        1.0F, reacquiredFit.fit->center, reacquiredFit.fit->depthExtent};
    const auto reacquiredHits =
        reacquired.session->picking().facesInRectangle(
            reacquiredCamera, QPointF{0.0, 0.0},
            QPointF{static_cast<double>(lastSize.width() - 1),
                    static_cast<double>(lastSize.height() - 1)});
    CHECK(reacquiredHits && !reacquiredHits.hits.empty());
    CHECK(std::all_of(
        reacquiredHits.hits.begin(), reacquiredHits.hits.end(),
        [&](const auto& hit) {
          return hit.publication.isSameCurrentPublication(
                     reacquired.session->publicationHandle()) &&
                 hit.address.occurrence == lastIdentity;
        }));
  }
  return EXIT_SUCCESS;
}
