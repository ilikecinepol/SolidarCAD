#include "TestAssertions.h"

#include <cstdlib>
#include <algorithm>
#include <memory>
#include <optional>

#include "model/ExtrudeFeature.h"
#include "model/OccurrenceDelta.h"
#include "sketch/Sketch.h"
#include "ui/AssemblyViewportSession.h"
#include "ui/application/AssemblyMateToolSession.h"

using namespace solidar;

namespace {

constexpr DefinitionId kRoot{1};
constexpr DefinitionId kPart{2};
constexpr OccurrenceId kFixed{3};
constexpr OccurrenceId kMoving{4};

OccurrencePath path(OccurrenceId id) {
  auto made = OccurrencePath::create({id});
  CHECK(made);
  return std::move(*made.path);
}

OccurrencePath path(std::initializer_list<OccurrenceId> ids) {
  auto made = OccurrencePath::create(std::vector<OccurrenceId>(ids));
  CHECK(made);
  return std::move(*made.path);
}

struct Fixture final {
  std::unique_ptr<ProductDocument> product;
  FaceReference face;
};

Fixture fixture(bool nested = false) {
  constexpr SketchId sketchId{11};
  constexpr BodyId bodyId{12};
  constexpr FeatureId featureId{13};
  auto document = std::make_unique<Document>();
  sketch::Sketch profile;
  profile.addRectangle({0.0, 0.0}, {10.0, 8.0});
  document->addSketch(sketchId, "mate profile", std::move(profile));
  auto& body = document->addBody(bodyId, "mate body");
  body.addFeature(std::make_unique<ExtrudeFeature>(
      featureId, sketchId, 6.0, "mate extrude"));
  CHECK(document->recompute());
  std::optional<FaceReference> face;
  for (std::size_t index = 0; index < 32 && !face; ++index) {
    auto made = document->createFaceReferenceV2(bodyId, featureId, index);
    if (made) face = std::move(made.reference);
  }
  CHECK(face && face->isV2());

  constexpr DefinitionId nestedId{5};
  constexpr OccurrenceId leafId{7};
  AssemblyDefinition root(kRoot);
  AssemblyDefinition nestedAssembly(nestedId);
  if (nested)
    CHECK(nestedAssembly.insert(ComponentOccurrence(
              leafId, kPart, RigidTransform3d::identity())) ==
          AssemblyOccurrenceInsertStatus::Inserted);
  const DefinitionId occurrenceTarget = nested ? nestedId : kPart;
  CHECK(root.insert(ComponentOccurrence(
            kFixed, occurrenceTarget, RigidTransform3d::identity(),
            {.visible = true, .suppressed = false, .grounded = true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  const auto moved = RigidTransform3d::create(
      {17.0, 19.0, 23.0}, {0.9, 0.1, 0.2, 0.3});
  CHECK(moved);
  CHECK(root.insert(ComponentOccurrence(kMoving, occurrenceTarget,
                                        *moved.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  ProductDocumentCreateResult made;
  if (nested) {
    std::vector<PartDefinition> parts;
    parts.emplace_back(kPart, std::move(document));
    std::vector<AssemblyDefinition> assemblies;
    assemblies.push_back(std::move(nestedAssembly));
    assemblies.push_back(std::move(root));
    made = ProductDocument::createNested(std::move(parts),
                                         std::move(assemblies), kRoot);
  } else {
    DefinitionRegistry definitions;
    CHECK(definitions.insert(PartDefinition(kPart, std::move(document))) ==
          DefinitionInsertStatus::Inserted);
    CHECK(definitions.insert(std::move(root)) ==
          DefinitionInsertStatus::Inserted);
    made = ProductDocument::create(std::move(definitions), kRoot);
  }
  CHECK(made);
  return {std::move(made.product), std::move(*face)};
}

AssemblyFaceRayHit hit(const AssemblyViewportSession& viewport,
                       ProductPresentationEpoch epoch, OccurrenceId id,
                       const FaceReference& face) {
  return {{{epoch, kRoot, path(id)}, kPart, face},
          {}, 0.0, 0.0, viewport.publicationHandle()};
}

AssemblyFaceRayHit nestedHit(const AssemblyViewportSession& viewport,
                             ProductPresentationEpoch epoch,
                             OccurrenceId prefix,
                             const FaceReference& face) {
  return {{{epoch, kRoot, path({prefix, OccurrenceId{7}})}, kPart, face},
          {}, 0.0, 0.0, viewport.publicationHandle()};
}

}  // namespace

int main() {
  auto value = fixture();
  constexpr ProductPresentationEpoch epoch{77};
  auto viewport = AssemblyViewportSession::prepare(
      *value.product, epoch,
      AssemblyMeshProfile{AssemblyMeshQuality::Normal});
  CHECK(viewport);
  CHECK(viewport.session->scene());

  // Shared terminal definitions remain occurrence-scoped through full nested
  // paths. Strict filter failures preserve the exact session state/picks.
  {
    auto nested = fixture(true);
    constexpr ProductPresentationEpoch nestedEpoch{79};
    auto nestedViewport = AssemblyViewportSession::prepare(
        *nested.product, nestedEpoch,
        AssemblyMeshProfile{AssemblyMeshQuality::Normal});
    CHECK(nestedViewport);
    AssemblyMateToolSession nestedSession;
    CHECK(nestedSession.begin(*nested.product,
                              nestedViewport.session->publicationHandle()));
    auto first = nestedHit(*nestedViewport.session, nestedEpoch, kFixed,
                           nested.face);
    auto wrongTerminal = first;
    wrongTerminal.address.definitionId = kRoot;
    CHECK(nestedSession
              .acceptPick(*nested.product, wrongTerminal,
                          nestedViewport.session->publicationHandle())
              .status == AssemblyMateToolPickStatus::InvalidAddress);
    auto v1 = first;
    v1.address.face = FaceReference{};
    CHECK(nestedSession
              .acceptPick(*nested.product, v1,
                          nestedViewport.session->publicationHandle())
              .status == AssemblyMateToolPickStatus::UnsupportedFace);
    CHECK(nestedSession
              .acceptPick(*nested.product, first,
                          nestedViewport.session->publicationHandle())
              .status == AssemblyMateToolPickStatus::Accepted);
    CHECK(nestedSession.firstPick()->occurrence.path.depth() == 2);
    const auto suppressDelta = OccurrenceDelta::suppressed(
        {nested.product->incarnation(), kRoot, path(kMoving),
         DefinitionId{5}},
        false, true);
    CHECK(nested.product->applyOccurrenceDelta(suppressDelta) ==
          OccurrenceDeltaStatus::Committed);
    auto second = nestedHit(*nestedViewport.session, nestedEpoch, kMoving,
                            nested.face);
    CHECK(nestedSession
              .acceptPick(*nested.product, second,
                          nestedViewport.session->publicationHandle())
              .status == AssemblyMateToolPickStatus::UnsupportedFace);
    CHECK(nestedSession.state() == AssemblyMateToolState::SelectingSecond);
    CHECK(!nestedSession.secondPick());
    CHECK(nested.product->revertOccurrenceDelta(suppressDelta) ==
          OccurrenceDeltaStatus::Committed);
    CHECK(nestedSession
              .acceptPick(*nested.product, second,
                          nestedViewport.session->publicationHandle())
              .status == AssemblyMateToolPickStatus::Accepted);
    CHECK(nestedSession.secondPick()->occurrence.path.depth() == 2);
    CHECK(nestedSession.currentPreview());
  }

  // Every allocating endpoint/relation construction sits behind the noexcept
  // session boundary. Deterministic failures retain the accepted picks and the
  // last installed visual without enabling Apply.
  {
    AssemblyMateToolSession allocationSession;
    CHECK(allocationSession.begin(*value.product,
                                  viewport.session->publicationHandle()));
    const auto first = hit(*viewport.session, epoch, kFixed, value.face);
    const auto second = hit(*viewport.session, epoch, kMoving, value.face);
    detail::AssemblyMateToolSessionTestAccess::injectAllocationFailure(
        detail::AssemblyMateToolAllocationFailurePoint::FirstPick);
    CHECK(allocationSession
              .acceptPick(*value.product, first,
                          viewport.session->publicationHandle())
              .status == AssemblyMateToolPickStatus::ResourceFailure);
    CHECK(allocationSession.state() ==
          AssemblyMateToolState::SelectingFirst);
    CHECK(!allocationSession.firstPick());
    CHECK(allocationSession
              .acceptPick(*value.product, first,
                          viewport.session->publicationHandle())
              .status == AssemblyMateToolPickStatus::Accepted);
    detail::AssemblyMateToolSessionTestAccess::injectAllocationFailure(
        detail::AssemblyMateToolAllocationFailurePoint::SecondRefresh);
    CHECK(allocationSession
              .acceptPick(*value.product, second,
                          viewport.session->publicationHandle())
              .status == AssemblyMateToolPickStatus::ResourceFailure);
    CHECK(allocationSession.firstPick());
    CHECK(!allocationSession.secondPick());
    CHECK(allocationSession.state() ==
          AssemblyMateToolState::PreviewFailed);
    CHECK(allocationSession
              .acceptPick(*value.product, second,
                          viewport.session->publicationHandle())
              .status == AssemblyMateToolPickStatus::Accepted);
    CHECK(allocationSession.currentPreview());
    CHECK(allocationSession.presentationSucceeded(
        allocationSession.callbackRevision()));
    auto displayed = allocationSession.displayedPreview();
    detail::AssemblyMateToolSessionTestAccess::injectAllocationFailure(
        detail::AssemblyMateToolAllocationFailurePoint::SecondRefresh);
    CHECK(allocationSession
              .acceptPick(*value.product, second,
                          viewport.session->publicationHandle())
              .status == AssemblyMateToolPickStatus::ResourceFailure);
    CHECK(allocationSession.displayedPreview() == displayed);
    CHECK(!allocationSession.canApply());
    CHECK(allocationSession.setOrientation(
              *value.product, PlanarMateOrientation::Aligned) !=
          MateSolveStatus::ResourceFailure);
    CHECK(allocationSession.presentationSucceeded(
        allocationSession.callbackRevision()));
    displayed = allocationSession.displayedPreview();
    detail::AssemblyMateToolSessionTestAccess::injectAllocationFailure(
        detail::AssemblyMateToolAllocationFailurePoint::OrientationRefresh);
    CHECK(allocationSession.setOrientation(
              *value.product, PlanarMateOrientation::Opposed) ==
          MateSolveStatus::ResourceFailure);
    CHECK(!allocationSession.canApply());
    CHECK(allocationSession.displayedPreview() == displayed);
    CHECK(allocationSession.setOrientation(
              *value.product, PlanarMateOrientation::Aligned) !=
          MateSolveStatus::ResourceFailure);
    CHECK(allocationSession.presentationSucceeded(
        allocationSession.callbackRevision()));
    detail::AssemblyMateToolSessionTestAccess::injectAllocationFailure(
        detail::AssemblyMateToolAllocationFailurePoint::PrepareCommit);
    CHECK(!allocationSession.prepareCommit(*value.product));
    CHECK(allocationSession.state() ==
          AssemblyMateToolState::PreviewFailed);
    CHECK(allocationSession.displayedPreview());
    CHECK(!allocationSession.canApply());
  }

  // begin is non-reentrant, CommitPending is busy, and Cancel is idempotent
  // from every reachable state.
  {
    const auto firstPick = hit(*viewport.session, epoch, kFixed, value.face);
    const auto secondPick = hit(*viewport.session, epoch, kMoving, value.face);
    const auto cancelTwice = [](AssemblyMateToolSession& candidate) {
      candidate.cancel();
      CHECK(candidate.state() == AssemblyMateToolState::Inactive);
      const auto revision = candidate.callbackRevision();
      candidate.cancel();
      CHECK(candidate.state() == AssemblyMateToolState::Inactive);
      CHECK(candidate.callbackRevision() == revision);
    };
    AssemblyMateToolSession selectingFirst;
    CHECK(selectingFirst.begin(*value.product,
                               viewport.session->publicationHandle()));
    CHECK(!selectingFirst.begin(*value.product,
                                viewport.session->publicationHandle()));
    cancelTwice(selectingFirst);

    AssemblyMateToolSession selectingSecond;
    CHECK(selectingSecond.begin(*value.product,
                                viewport.session->publicationHandle()));
    CHECK(selectingSecond
              .acceptPick(*value.product, firstPick,
                          viewport.session->publicationHandle())
              .status == AssemblyMateToolPickStatus::Accepted);
    cancelTwice(selectingSecond);

    AssemblyMateToolSession previewFailed;
    CHECK(previewFailed.begin(*value.product,
                              viewport.session->publicationHandle()));
    CHECK(previewFailed.acceptPick(*value.product, firstPick,
                                   viewport.session->publicationHandle()).status ==
          AssemblyMateToolPickStatus::Accepted);
    CHECK(previewFailed.acceptPick(*value.product, secondPick,
                                   viewport.session->publicationHandle()).status ==
          AssemblyMateToolPickStatus::Accepted);
    CHECK(previewFailed.state() == AssemblyMateToolState::PreviewFailed);
    cancelTwice(previewFailed);

    AssemblyMateToolSession previewValid;
    CHECK(previewValid.begin(*value.product,
                             viewport.session->publicationHandle()));
    CHECK(previewValid.acceptPick(*value.product, firstPick,
                                  viewport.session->publicationHandle()).status ==
          AssemblyMateToolPickStatus::Accepted);
    CHECK(previewValid.acceptPick(*value.product, secondPick,
                                  viewport.session->publicationHandle()).status ==
          AssemblyMateToolPickStatus::Accepted);
    CHECK(previewValid.presentationSucceeded(
        previewValid.callbackRevision()));
    cancelTwice(previewValid);

    AssemblyMateToolSession commitPending;
    CHECK(commitPending.begin(*value.product,
                              viewport.session->publicationHandle()));
    CHECK(commitPending.acceptPick(*value.product, firstPick,
                                   viewport.session->publicationHandle()).status ==
          AssemblyMateToolPickStatus::Accepted);
    CHECK(commitPending.acceptPick(*value.product, secondPick,
                                   viewport.session->publicationHandle()).status ==
          AssemblyMateToolPickStatus::Accepted);
    CHECK(commitPending.presentationSucceeded(
        commitPending.callbackRevision()));
    auto transaction = commitPending.prepareCommit(*value.product);
    CHECK(transaction);
    CHECK(commitPending.state() == AssemblyMateToolState::CommitPending);
    CHECK(commitPending
              .acceptPick(*value.product, firstPick,
                          viewport.session->publicationHandle())
              .status == AssemblyMateToolPickStatus::Busy);
    cancelTwice(commitPending);
  }

  AssemblyMateToolController controller;
  auto& session = controller.session();
  CHECK(session.begin(*value.product,
                      viewport.session->publicationHandle()));
  CHECK(session.state() == AssemblyMateToolState::SelectingFirst);
  CHECK(session.orientation() == PlanarMateOrientation::Aligned);
  const auto* root = value.product->rootAssemblyDefinition();
  CHECK(root);
  const MateRegistry registryBefore = root->mateRegistry();
  const auto fixedBefore = root->find(kFixed)->placement();
  const auto movingBefore = root->find(kMoving)->placement();

  const auto first = hit(*viewport.session, epoch, kFixed, value.face);
  CHECK(session.acceptPick(*value.product, first,
                           viewport.session->publicationHandle())
            .status == AssemblyMateToolPickStatus::Accepted);
  CHECK(session.state() == AssemblyMateToolState::SelectingSecond);
  const auto firstRevision = session.callbackRevision();
  const auto firstAddress = *session.firstPick();
  CHECK(session.acceptPick(*value.product, first,
                           viewport.session->publicationHandle())
            .status == AssemblyMateToolPickStatus::SameOccurrencePath);
  CHECK(session.callbackRevision() == firstRevision);
  CHECK(*session.firstPick() == firstAddress);

  auto wrongOwner = hit(*viewport.session, epoch, kMoving, value.face);
  wrongOwner.address.occurrence.rootAssemblyDefinitionId = kPart;
  CHECK(session.acceptPick(*value.product, wrongOwner,
                           viewport.session->publicationHandle())
            .status == AssemblyMateToolPickStatus::ForeignProduct);
  auto wrongPath = hit(*viewport.session, epoch, kMoving, value.face);
  wrongPath.address.occurrence.path = path(OccurrenceId{99});
  CHECK(session.acceptPick(*value.product, wrongPath,
                           viewport.session->publicationHandle())
            .status == AssemblyMateToolPickStatus::InvalidAddress);
  auto v1Second = hit(*viewport.session, epoch, kMoving, value.face);
  v1Second.address.face = FaceReference{};
  CHECK(session.acceptPick(*value.product, v1Second,
                           viewport.session->publicationHandle())
            .status == AssemblyMateToolPickStatus::UnsupportedFace);
  CHECK(session.state() == AssemblyMateToolState::SelectingSecond);
  CHECK(session.callbackRevision() == firstRevision);
  CHECK(*session.firstPick() == firstAddress);
  CHECK(!session.secondPick());

  auto foreignViewport = AssemblyViewportSession::prepare(
      *value.product, ProductPresentationEpoch{78},
      AssemblyMeshProfile{AssemblyMeshQuality::Normal});
  CHECK(foreignViewport);
  const auto foreign = hit(*foreignViewport.session,
                           ProductPresentationEpoch{78}, kMoving, value.face);
  CHECK(session.acceptPick(*value.product, foreign,
                           viewport.session->publicationHandle())
            .status == AssemblyMateToolPickStatus::StalePublication);

  const auto second = hit(*viewport.session, epoch, kMoving, value.face);
  CHECK(session.acceptPick(*value.product, second,
                           viewport.session->publicationHandle())
            .status == AssemblyMateToolPickStatus::Accepted);
  CHECK(session.state() == AssemblyMateToolState::PreviewFailed);
  CHECK(!session.canApply());
  CHECK(session.currentPreview());
  const auto alignedExpected = RigidTransform3d::create(
      {17.0, 0.0, 23.0},
      {0.97618706018395296, 0.0, 0.2169304578186555, 0.0});
  CHECK(alignedExpected);
  const auto alignedChange = std::find_if(
      session.currentPreview()->candidate().changes().begin(),
      session.currentPreview()->candidate().changes().end(),
      [](const MatePlacementChange& change) {
        return change.before.occurrenceId == kMoving;
      });
  CHECK(alignedChange !=
        session.currentPreview()->candidate().changes().end());
  CHECK(alignedChange->afterPlacement == *alignedExpected.transform);
  CHECK(root->mateRegistry() == registryBefore);
  CHECK(root->find(kFixed)->placement() == fixedBefore);
  CHECK(root->find(kMoving)->placement() == movingBefore);

  const auto committedScene = viewport.session->scene();
  const auto previewScene = viewport.session->prepareMatePreview(
      session.currentPreview()->candidate());
  CHECK(previewScene);
  CHECK(previewScene != committedScene);
  CHECK(previewScene->publicationHandle().isSameCurrentPublication(
      committedScene->publicationHandle()));
  CHECK(previewScene->sources().size() == committedScene->sources().size());
  CHECK(previewScene->sources().front().mesh ==
        committedScene->sources().front().mesh);
  CHECK(viewport.session->scene() == committedScene);
  CHECK(session.presentationSucceeded(session.callbackRevision()));
  CHECK(session.state() == AssemblyMateToolState::PreviewValid);
  CHECK(session.canApply());
  CHECK(session.displayedPreview());

  CHECK(session.setOrientation(*value.product,
                               PlanarMateOrientation::Opposed) !=
        MateSolveStatus::InvalidInput);
  CHECK(session.currentPreview());
  const auto opposedExpected = RigidTransform3d::create(
      {17.0, 0.0, 23.0},
      {0.0, 0.31622776601684066, 0.0, 0.94868329805051288});
  CHECK(opposedExpected);
  const auto opposedChange = std::find_if(
      session.currentPreview()->candidate().changes().begin(),
      session.currentPreview()->candidate().changes().end(),
      [](const MatePlacementChange& change) {
        return change.before.occurrenceId == kMoving;
      });
  CHECK(opposedChange !=
        session.currentPreview()->candidate().changes().end());
  CHECK(opposedChange->afterPlacement == *opposedExpected.transform);
  const auto alignedDisplayed = session.displayedPreview();
  session.presentationFailed(session.callbackRevision());
  CHECK(session.displayedPreview() == alignedDisplayed);
  CHECK(!session.canApply());
  CHECK(session.setOrientation(*value.product,
                               PlanarMateOrientation::Opposed) !=
        MateSolveStatus::InvalidInput);
  CHECK(session.presentationSucceeded(session.callbackRevision()));
  CHECK(session.canApply());
  CHECK(session.displayedPreview()->relation().orientation() ==
        PlanarMateOrientation::Opposed);
  auto* mutableRoot = const_cast<AssemblyDefinition*>(root);
  CHECK(mutableRoot->mateRegistry().insert(
            session.displayedPreview()->relation()) ==
        MateRegistryMutationStatus::Inserted);
  CHECK(!session.prepareCommit(*value.product));
  CHECK(session.state() == AssemblyMateToolState::PreviewFailed);
  CHECK(!session.canApply());
  CHECK(mutableRoot->mateRegistry().erase(MateId{1}) ==
        MateRegistryMutationStatus::Erased);
  CHECK(session.setOrientation(*value.product,
                               PlanarMateOrientation::Opposed) !=
        MateSolveStatus::InvalidInput);
  CHECK(session.presentationSucceeded(session.callbackRevision()));
  const auto lastValidRelation = session.displayedPreview()->relation();
  const auto lastValidChanges =
      session.displayedPreview()->candidate().changes();

  MateSolverOptions lowWork = root->mateRegistry().options();
  lowWork.maximumWorkUnits = 1;
  CHECK(mutableRoot->mateRegistry().setOptions(lowWork) ==
        MateRegistryMutationStatus::Inserted);
  CHECK(session.setOrientation(*value.product,
                               PlanarMateOrientation::Aligned) ==
        MateSolveStatus::ResourceFailure);
  CHECK(session.state() == AssemblyMateToolState::PreviewFailed);
  CHECK(!session.canApply());
  CHECK(session.displayedPreview());
  CHECK(session.displayedPreview()->relation() == lastValidRelation);
  CHECK(session.displayedPreview()->candidate().changes() == lastValidChanges);

  session.escape();
  CHECK(session.state() == AssemblyMateToolState::SelectingSecond);
  CHECK(session.firstPick());
  CHECK(!session.secondPick());
  CHECK(!session.displayedPreview());
  session.escape();
  CHECK(session.state() == AssemblyMateToolState::SelectingFirst);
  CHECK(!session.firstPick());
  session.escape();
  CHECK(session.state() == AssemblyMateToolState::Inactive);
  controller.cancel();
  CHECK(session.state() == AssemblyMateToolState::Inactive);
  return EXIT_SUCCESS;
}
