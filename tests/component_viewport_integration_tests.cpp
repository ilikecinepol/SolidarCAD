#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QThread>
#include <QWheelEvent>

#include <cstdlib>
#include <cmath>
#include <memory>
#include <vector>

#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "ui/Viewport.h"

namespace {
using namespace solidar;
constexpr AssemblyMeshProfile kNormal{AssemblyMeshQuality::Normal};

std::unique_ptr<Document> document(double size) {
  auto result = std::make_unique<Document>();
  auto& body = result->addBody(BodyId{7}, "body");
  body.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{70}, std::make_shared<const TopoDS_Shape>(
                         BRepPrimAPI_MakeBox(size, size, size).Shape()),
      "box"));
  CHECK(result->recompute());
  return result;
}

RigidTransform3d translate(double x) {
  const auto made = RigidTransform3d::create({x, 0, 0}, {1, 0, 0, 0});
  CHECK(made);
  return *made.transform;
}

std::unique_ptr<ProductDocument> componentProduct() {
  ComponentDefinition root(DefinitionId{300}, document(10));
  ComponentDefinition child(DefinitionId{200}, document(8));
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{10}, DefinitionId{200}, translate(30))) ==
        ComponentOccurrenceInsertStatus::Inserted);
  std::vector<ComponentDefinition> definitions;
  definitions.push_back(std::move(child));
  definitions.push_back(std::move(root));
  auto made = ProductDocument::createComponentGraph(
      std::move(definitions), DefinitionId{300});
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> emptyRootComponentProduct() {
  ComponentDefinition root(DefinitionId{300}, std::make_unique<Document>());
  ComponentDefinition child(DefinitionId{200}, document(8));
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{10}, DefinitionId{200}, translate(30))) ==
        ComponentOccurrenceInsertStatus::Inserted);
  std::vector<ComponentDefinition> definitions;
  definitions.push_back(std::move(child));
  definitions.push_back(std::move(root));
  auto made = ProductDocument::createComponentGraph(
      std::move(definitions), DefinitionId{300});
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> assemblyProduct() {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(
            PartDefinition(DefinitionId{20}, document(6))) ==
        DefinitionInsertStatus::Inserted);
  AssemblyDefinition root(DefinitionId{100});
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{1}, DefinitionId{20}, RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{100});
  CHECK(made);
  return std::move(made.product);
}

QPointF rootFacePixel(const Viewport& viewport,
                      const ComponentViewportSession& session) {
  const auto navigation = viewport.navigationState();
  const auto camera = session.camera(
      navigation.yawDeg, navigation.pitchDeg, navigation.zoom,
      navigation.pan, viewport.size());
  return camera.worldToScreen({5, 5, 10});
}

QPointF childFacePixel(const Viewport& viewport,
                       const ComponentViewportSession& session) {
  const auto child = std::find_if(
      session.scene()->instances().begin(), session.scene()->instances().end(),
      [](const ComponentViewportSceneInstance& instance) {
        return !instance.bodyIdentity.component.rootInstance();
      });
  CHECK(child != session.scene()->instances().end());
  const auto center = child->worldBounds.center();
  const auto maximum = child->worldBounds.maximum();
  const auto navigation = viewport.navigationState();
  const auto camera = session.camera(
      navigation.yawDeg, navigation.pitchDeg, navigation.zoom,
      navigation.pan, viewport.size());
  return camera.worldToScreen({center.x, center.y, maximum.z});
}

void sendMouse(Viewport& viewport, QEvent::Type type, QPointF position,
               Qt::MouseButton button, Qt::MouseButtons buttons) {
  QMouseEvent event(type, position, position, button, buttons,
                    Qt::NoModifier);
  QApplication::sendEvent(&viewport, &event);
}
}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);
  auto components = componentProduct();
  auto prepared = ComponentViewportSession::prepare(
      *components, ProductPresentationEpoch{801}, kNormal);
  CHECK(prepared);

  Viewport viewport;
  viewport.resize(800, 600);
  int selectedSignals = 0;
  int hoveredSignals = 0;
  int faceSignals = 0;
  int placementSignals = 0;
  std::optional<ComponentPlacementDragRequest> lastPlacementRequest;
  QObject::connect(&viewport, &Viewport::componentSelected, &viewport,
                   [&](const ComponentInstanceSelection&) {
                     ++selectedSignals;
                   });
  QObject::connect(&viewport, &Viewport::componentHovered, &viewport,
                   [&](const ComponentInstanceSelection&) {
                     ++hoveredSignals;
                   });
  QObject::connect(&viewport, &Viewport::componentFacePicked, &viewport,
                   [&](const ComponentFaceRayHit&) { ++faceSignals; });
  QObject::connect(
      &viewport, &Viewport::componentPlacementDragRequested, &viewport,
      [&](ComponentPlacementDragRequest request) {
        ++placementSignals;
        lastPlacementRequest = std::move(request);
      });

  viewport.installComponentSession(std::move(prepared.session));
  CHECK(viewport.componentSession());
  CHECK(!viewport.assemblySession());
  CHECK(viewport.componentSession()->scene()->instances().size() == 2);
  CHECK(viewport.componentSession()->scene()->worldBounds());
  viewport.fitAll();
  const QPointF rootPixel =
      rootFacePixel(viewport, *viewport.componentSession());
  const auto face = viewport.componentFaceAt(rootPixel);
  CHECK(face);
  CHECK(face.hit->address.body.component.rootInstance());
  const auto edge = viewport.componentEdgeAt(
      viewport.componentSession()
          ->camera(viewport.navigationState().yawDeg,
                   viewport.navigationState().pitchDeg,
                   viewport.navigationState().zoom,
                   viewport.navigationState().pan, viewport.size())
          .worldToScreen({0, 0, 10}),
      12.0);
  CHECK(edge);

  sendMouse(viewport, QEvent::MouseMove, rootPixel, Qt::NoButton,
            Qt::NoButton);
  QThread::msleep(25);
  QApplication::processEvents();
  CHECK(hoveredSignals >= 1);
  CHECK(viewport.componentSession()->hoveredComponent());
  sendMouse(viewport, QEvent::MouseButtonPress, rootPixel,
            Qt::LeftButton, Qt::LeftButton);
  CHECK(selectedSignals == 1);
  CHECK(faceSignals == 1);
  CHECK(viewport.selectedComponentView());
  CHECK(viewport.componentSession()->selectedHighlight());
  CHECK(viewport.componentSession()->selectedHighlight()->visibleBodies(
            *viewport.componentSession()->scene()).size() == 1);
  CHECK(!viewport.componentPlacementDragArmed());

  // Root is identity-only and never draggable. A descendant arms on press,
  // remains a normal click below the shared 3 px UX threshold, then publishes
  // one owning request only after an active preview is released.
  const QPointF childPixel =
      childFacePixel(viewport, *viewport.componentSession());
  const auto childHit = viewport.componentFaceAt(childPixel);
  CHECK(childHit && childHit.hit);
  CHECK(!childHit.hit->address.body.component.rootInstance());
  sendMouse(viewport, QEvent::MouseButtonPress, childPixel,
            Qt::LeftButton, Qt::LeftButton);
  CHECK(viewport.componentPlacementDragArmed());
  sendMouse(viewport, QEvent::MouseMove, childPixel + QPointF(2.0, 0.0),
            Qt::NoButton, Qt::LeftButton);
  CHECK(viewport.componentPlacementDragArmed());
  CHECK(!viewport.componentPlacementPreview());
  sendMouse(viewport, QEvent::MouseButtonRelease,
            childPixel + QPointF(2.0, 0.0), Qt::LeftButton, Qt::NoButton);
  CHECK(!viewport.componentPlacementDragArmed());
  CHECK(!viewport.componentPlacementPreview());
  CHECK(placementSignals == 0);

  sendMouse(viewport, QEvent::MouseButtonPress, childPixel,
            Qt::LeftButton, Qt::LeftButton);
  sendMouse(viewport, QEvent::MouseMove, childPixel + QPointF(20.0, 12.0),
            Qt::NoButton, Qt::LeftButton);
  CHECK(viewport.componentPlacementDragArmed());
  CHECK(viewport.componentPlacementPreview());
  const auto expectedDraggedAddress =
      viewport.componentPlacementPreview()->component();
  sendMouse(viewport, QEvent::MouseButtonRelease,
            childPixel + QPointF(20.0, 12.0), Qt::LeftButton,
            Qt::NoButton);
  CHECK(!viewport.componentPlacementDragArmed());
  CHECK(!viewport.componentPlacementPreview());
  CHECK(placementSignals == 1);
  CHECK(lastPlacementRequest);
  CHECK(lastPlacementRequest->component == expectedDraggedAddress);
  CHECK(!lastPlacementRequest->component.rootInstance());
  CHECK(lastPlacementRequest->publication.isSameCurrentPublication(
      viewport.componentSession()->publicationHandle()));

  // Escape, wheel and explicit ownership transfer cancel the transient state
  // without a request and leave the immutable session scene untouched.
  sendMouse(viewport, QEvent::MouseButtonPress, childPixel,
            Qt::LeftButton, Qt::LeftButton);
  sendMouse(viewport, QEvent::MouseMove, childPixel + QPointF(12.0, 0.0),
            Qt::NoButton, Qt::LeftButton);
  CHECK(viewport.componentPlacementPreview());
  const auto committedTransformBeforeUngrab =
      viewport.componentSession()->scene()->instances().back().localToWorld;
  QEvent ungrabMouse(QEvent::UngrabMouse);
  QApplication::sendEvent(&viewport, &ungrabMouse);
  CHECK(!viewport.componentPlacementDragArmed());
  CHECK(!viewport.componentPlacementPreview());
  CHECK(viewport.cursor().shape() == Qt::ArrowCursor);
  CHECK(viewport.componentSession()->scene()->instances().back().localToWorld ==
        committedTransformBeforeUngrab);
  sendMouse(viewport, QEvent::MouseButtonRelease,
            childPixel + QPointF(12.0, 0.0), Qt::LeftButton,
            Qt::NoButton);
  CHECK(placementSignals == 1);

  sendMouse(viewport, QEvent::MouseButtonPress, childPixel,
            Qt::LeftButton, Qt::LeftButton);
  sendMouse(viewport, QEvent::MouseMove, childPixel + QPointF(12.0, 0.0),
            Qt::NoButton, Qt::LeftButton);
  CHECK(viewport.componentPlacementPreview());
  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(&viewport, &escape);
  CHECK(!viewport.componentPlacementDragArmed());
  CHECK(placementSignals == 1);

  sendMouse(viewport, QEvent::MouseButtonPress, childPixel,
            Qt::LeftButton, Qt::LeftButton);
  sendMouse(viewport, QEvent::MouseMove, childPixel + QPointF(12.0, 0.0),
            Qt::NoButton, Qt::LeftButton);
  CHECK(viewport.componentPlacementPreview());
  QWheelEvent wheel(childPixel, childPixel, {}, {0, 120}, Qt::NoButton,
                    Qt::NoModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(&viewport, &wheel);
  CHECK(!viewport.componentPlacementDragArmed());
  CHECK(placementSignals == 1);

  // Queued hover is generation-bound. Clearing before delivery prevents a
  // callback into the retired component publication.
  sendMouse(viewport, QEvent::MouseMove, rootPixel, Qt::NoButton,
            Qt::NoButton);
  const int hoverBeforeClear = hoveredSignals;
  auto taken = viewport.takeComponentSession();
  CHECK(taken);
  CHECK(!viewport.componentSession());
  QThread::msleep(25);
  QApplication::processEvents();
  CHECK(hoveredSignals == hoverBeforeClear);
  viewport.restoreComponentSession(std::move(taken));
  CHECK(viewport.componentSession());

  const QPointF restoredChildPixel =
      childFacePixel(viewport, *viewport.componentSession());
  sendMouse(viewport, QEvent::MouseButtonPress, restoredChildPixel,
            Qt::LeftButton, Qt::LeftButton);
  sendMouse(viewport, QEvent::MouseMove,
            restoredChildPixel + QPointF(10.0, 0.0), Qt::NoButton,
            Qt::LeftButton);
  CHECK(viewport.componentPlacementPreview());
  auto dragTaken = viewport.takeComponentSession();
  CHECK(dragTaken);
  CHECK(!viewport.componentPlacementDragArmed());
  CHECK(!viewport.componentPlacementPreview());
  CHECK(placementSignals == 1);
  viewport.restoreComponentSession(std::move(dragTaken));
  CHECK(viewport.componentSession());

  sendMouse(viewport, QEvent::MouseMove,
            rootFacePixel(viewport, *viewport.componentSession()),
            Qt::NoButton, Qt::NoButton);
  const int hoverBeforeExplicitClear = hoveredSignals;
  viewport.clearComponentSession();
  QThread::msleep(25);
  QApplication::processEvents();
  CHECK(hoveredSignals == hoverBeforeExplicitClear);

  // Ordinary Part publication explicitly replaces component ownership while
  // preserving the pre-existing Part behavior when no component is installed.
  viewport.setBodyShape(std::make_shared<const TopoDS_Shape>(
                            BRepPrimAPI_MakeBox(4, 5, 6).Shape()),
                        BodyId{90}, FeatureId{91});
  CHECK(!viewport.componentSession());

  auto assembly = assemblyProduct();
  auto assemblySession = AssemblyViewportSession::prepare(
      *assembly, ProductPresentationEpoch{802}, kNormal);
  CHECK(assemblySession);
  viewport.installAssemblySession(std::move(assemblySession.session));
  CHECK(viewport.assemblySession());
  CHECK(!viewport.componentSession());

  auto nextComponent = ComponentViewportSession::prepare(
      *components, ProductPresentationEpoch{803}, kNormal);
  CHECK(nextComponent);
  viewport.installComponentSession(std::move(nextComponent.session));
  CHECK(viewport.componentSession());
  CHECK(!viewport.assemblySession());
  viewport.resetScene();
  CHECK(!viewport.componentSession());
  CHECK(!viewport.assemblySession());
  CHECK(!viewport.selectedComponentView());

  // Null is the nearest public invalid-session boundary. A live stale session
  // cannot be constructed: its coordinator is first-owned and destroyed last.
  viewport.installComponentSession({});
  CHECK(!viewport.componentSession());

  // Signal payloads are owning local snapshots. The first direct receiver may
  // retire the publication and install another session while a later receiver
  // still observes the complete original selection. The old click must not
  // continue into a face notification for the replacement publication.
  {
    auto sourceProduct = componentProduct();
    auto replacementProduct = componentProduct();
    auto source = ComponentViewportSession::prepare(
        *sourceProduct, ProductPresentationEpoch{805}, kNormal);
    auto replacement = ComponentViewportSession::prepare(
        *replacementProduct, ProductPresentationEpoch{806}, kNormal);
    CHECK(source);
    CHECK(replacement);
    Viewport reentrantViewport;
    reentrantViewport.resize(800, 600);
    reentrantViewport.installComponentSession(std::move(source.session));
    CHECK(reentrantViewport.componentSession());
    const auto sourceHandle =
        reentrantViewport.componentSession()->publicationHandle();
    const auto expectedAddress =
        reentrantViewport.componentSession()->components().front().address;
    const QPointF pixel = rootFacePixel(
        reentrantViewport, *reentrantViewport.componentSession());
    bool firstReceiverTookSession = false;
    int secondReceiverCalls = 0;
    int faceCallsAfterReplacement = 0;
    std::optional<ComponentInstanceAddress> copiedAddress;
    std::uint64_t copiedGeneration = 0;
    auto replacementSession = std::move(replacement.session);
    QObject::connect(
        &reentrantViewport, &Viewport::componentSelected,
        &reentrantViewport,
        [&](const ComponentInstanceSelection&) {
          auto retired = reentrantViewport.takeComponentSession();
          firstReceiverTookSession = retired != nullptr;
          reentrantViewport.installComponentSession(
              std::move(replacementSession));
        },
        Qt::DirectConnection);
    QObject::connect(
        &reentrantViewport, &Viewport::componentSelected, &application,
        [&](const ComponentInstanceSelection& delivered) {
          ++secondReceiverCalls;
          copiedAddress = delivered.component();
          copiedGeneration = delivered.publication().generation().value();
        },
        Qt::DirectConnection);
    QObject::connect(
        &reentrantViewport, &Viewport::componentFacePicked, &application,
        [&](const ComponentFaceRayHit&) { ++faceCallsAfterReplacement; },
        Qt::DirectConnection);
    sendMouse(reentrantViewport, QEvent::MouseButtonPress, pixel,
              Qt::LeftButton, Qt::LeftButton);
    CHECK(firstReceiverTookSession);
    CHECK(secondReceiverCalls == 1);
    CHECK(copiedAddress && *copiedAddress == expectedAddress);
    CHECK(copiedGeneration == sourceHandle.generation().value());
    CHECK(faceCallsAfterReplacement == 0);
    CHECK(reentrantViewport.componentSession());
    CHECK(reentrantViewport.componentSession()->epoch().value() == 806);
    CHECK(!reentrantViewport.componentSession()->selectedComponent());
  }

  // Hover has the same exact-publication guard. Replacing the session from the
  // first receiver must not let the retired hit set a pointing cursor or
  // mutate interaction state in the replacement session.
  {
    auto sourceProduct = componentProduct();
    auto replacementProduct = componentProduct();
    auto source = ComponentViewportSession::prepare(
        *sourceProduct, ProductPresentationEpoch{807}, kNormal);
    auto replacement = ComponentViewportSession::prepare(
        *replacementProduct, ProductPresentationEpoch{808}, kNormal);
    CHECK(source);
    CHECK(replacement);
    Viewport hoverViewport;
    hoverViewport.resize(800, 600);
    hoverViewport.installComponentSession(std::move(source.session));
    const QPointF pixel =
        rootFacePixel(hoverViewport, *hoverViewport.componentSession());
    const auto sourceHandle = hoverViewport.componentSession()->publicationHandle();
    const auto expectedAddress =
        hoverViewport.componentSession()->components().front().address;
    auto replacementSession = std::move(replacement.session);
    int secondReceiverCalls = 0;
    std::optional<ComponentInstanceAddress> copiedAddress;
    std::uint64_t copiedGeneration = 0;
    QObject::connect(
        &hoverViewport, &Viewport::componentHovered, &hoverViewport,
        [&](const ComponentInstanceSelection&) {
          auto retired = hoverViewport.takeComponentSession();
          hoverViewport.installComponentSession(std::move(replacementSession));
        },
        Qt::DirectConnection);
    QObject::connect(
        &hoverViewport, &Viewport::componentHovered, &application,
        [&](const ComponentInstanceSelection& delivered) {
          ++secondReceiverCalls;
          copiedAddress = delivered.component();
          copiedGeneration = delivered.publication().generation().value();
        },
        Qt::DirectConnection);
    sendMouse(hoverViewport, QEvent::MouseMove, pixel, Qt::NoButton,
              Qt::NoButton);
    QThread::msleep(25);
    QApplication::processEvents();
    CHECK(secondReceiverCalls == 1);
    CHECK(copiedAddress && *copiedAddress == expectedAddress);
    CHECK(copiedGeneration == sourceHandle.generation().value());
    CHECK(hoverViewport.componentSession());
    CHECK(hoverViewport.componentSession()->epoch().value() == 808);
    CHECK(!hoverViewport.componentSession()->hoveredComponent());
    CHECK(hoverViewport.cursor().shape() == Qt::ArrowCursor);
  }

  // The drag signal owns its publication/address payload. A first direct
  // receiver may replace the session; later receivers still see the complete
  // source request and the old event performs no work in the successor.
  {
    auto sourceProduct = componentProduct();
    auto replacementProduct = componentProduct();
    auto source = ComponentViewportSession::prepare(
        *sourceProduct, ProductPresentationEpoch{810}, kNormal);
    auto replacement = ComponentViewportSession::prepare(
        *replacementProduct, ProductPresentationEpoch{811}, kNormal);
    CHECK(source && replacement);
    Viewport dragViewport;
    dragViewport.resize(800, 600);
    dragViewport.installComponentSession(std::move(source.session));
    const auto sourceHandle =
        dragViewport.componentSession()->publicationHandle();
    const QPointF pixel =
        childFacePixel(dragViewport, *dragViewport.componentSession());
    const auto sourceHit = dragViewport.componentFaceAt(pixel);
    CHECK(sourceHit && sourceHit.hit);
    const auto expectedAddress =
        sourceHit.hit->address.body.component;
    auto replacementSession = std::move(replacement.session);
    bool firstReplaced = false;
    int laterCalls = 0;
    std::optional<ComponentPlacementDragRequest> laterRequest;
    QObject::connect(
        &dragViewport, &Viewport::componentPlacementDragRequested,
        &dragViewport,
        [&](ComponentPlacementDragRequest) {
          auto retired = dragViewport.takeComponentSession();
          firstReplaced = retired != nullptr;
          dragViewport.installComponentSession(
              std::move(replacementSession));
        },
        Qt::DirectConnection);
    QObject::connect(
        &dragViewport, &Viewport::componentPlacementDragRequested,
        &application,
        [&](ComponentPlacementDragRequest request) {
          ++laterCalls;
          laterRequest = std::move(request);
        },
        Qt::DirectConnection);
    sendMouse(dragViewport, QEvent::MouseButtonPress, pixel,
              Qt::LeftButton, Qt::LeftButton);
    sendMouse(dragViewport, QEvent::MouseMove,
              pixel + QPointF(18.0, -7.0), Qt::NoButton,
              Qt::LeftButton);
    CHECK(dragViewport.componentPlacementPreview());
    sendMouse(dragViewport, QEvent::MouseButtonRelease,
              pixel + QPointF(18.0, -7.0), Qt::LeftButton,
              Qt::NoButton);
    CHECK(firstReplaced);
    CHECK(laterCalls == 1);
    CHECK(laterRequest);
    CHECK(laterRequest->component == expectedAddress);
    CHECK(laterRequest->publication.generation() == sourceHandle.generation());
    CHECK(dragViewport.componentSession());
    CHECK(dragViewport.componentSession()->epoch().value() == 811);
    CHECK(!dragViewport.componentPlacementDragArmed());
  }

  // Root-component authoring is a hybrid workspace: installing the ordinary
  // Part body must retain the component session, clicks on the root fall
  // through to Part selection, and a real mouse drag on a child still emits a
  // placement request.
  {
    auto hybridProduct = componentProduct();
    auto hybridPrepared = ComponentViewportSession::prepare(
        *hybridProduct, ProductPresentationEpoch{809}, kNormal);
    CHECK(hybridPrepared);
    Viewport hybridViewport;
    hybridViewport.resize(800, 600);
    hybridViewport.installComponentSession(std::move(hybridPrepared.session));
    CHECK(hybridViewport.componentSession());
    const auto rootState = std::find_if(
        hybridViewport.componentSession()->components().begin(),
        hybridViewport.componentSession()->components().end(),
        [](const ComponentInstanceSessionState& state) {
          return state.address.rootInstance();
        });
    CHECK(rootState != hybridViewport.componentSession()->components().end());
    hybridViewport.setComponentEditContext(
        hybridViewport.componentSession()->scene(), rootState->address);
    hybridViewport.setBodyShape(
        std::make_shared<const TopoDS_Shape>(
            BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape()),
        BodyId{7}, FeatureId{70});
    CHECK(hybridViewport.componentSession());

    const auto navigation = hybridViewport.navigationState();
    const ViewportCameraState hybridCamera{
        navigation.yawDeg, navigation.pitchDeg, navigation.zoom,
        navigation.pan, hybridViewport.size(), 1.0F, {5.0, 5.0, 5.0},
        std::sqrt(300.0) * 3.0};
    const QPointF rootPixel = hybridCamera.worldToScreen({5.0, 5.0, 10.0});
    sendMouse(hybridViewport, QEvent::MouseButtonPress, rootPixel,
              Qt::LeftButton, Qt::LeftButton);
    CHECK(!hybridViewport.selectedComponentView());
    sendMouse(hybridViewport, QEvent::MouseButtonRelease, rootPixel,
              Qt::LeftButton, Qt::NoButton);

    const QPointF childPixel =
        hybridCamera.worldToScreen({34.0, 4.0, 8.0});
    const auto childHit = hybridViewport.componentFaceAt(childPixel);
    CHECK(childHit && childHit.hit);
    CHECK(!childHit.hit->address.body.component.rootInstance());
    std::optional<ComponentPlacementDragRequest> hybridRequest;
    QObject::connect(
        &hybridViewport, &Viewport::componentPlacementDragRequested,
        &application,
        [&](ComponentPlacementDragRequest request) {
          hybridRequest = std::move(request);
        },
        Qt::DirectConnection);
    sendMouse(hybridViewport, QEvent::MouseButtonPress, childPixel,
              Qt::LeftButton, Qt::LeftButton);
    sendMouse(hybridViewport, QEvent::MouseMove,
              childPixel + QPointF(20.0, -8.0), Qt::NoButton,
              Qt::LeftButton);
    CHECK(hybridViewport.componentPlacementPreview());
    sendMouse(hybridViewport, QEvent::MouseButtonRelease,
              childPixel + QPointF(20.0, -8.0), Qt::LeftButton,
              Qt::NoButton);
    CHECK(hybridRequest);
    CHECK(hybridRequest->component ==
          childHit.hit->address.body.component);
  }

  // With an empty active root, picking uses the same full-scene camera as the
  // renderer. Visible descendant bodies must remain selectable.
  {
    auto emptyRootProduct = emptyRootComponentProduct();
    auto emptyRootPrepared = ComponentViewportSession::prepare(
        *emptyRootProduct, ProductPresentationEpoch{813}, kNormal);
    CHECK(emptyRootPrepared);
    Viewport emptyRootViewport;
    emptyRootViewport.resize(800, 600);
    emptyRootViewport.installComponentSession(
        std::move(emptyRootPrepared.session));
    CHECK(emptyRootViewport.componentSession());
    const auto rootState = std::find_if(
        emptyRootViewport.componentSession()->components().begin(),
        emptyRootViewport.componentSession()->components().end(),
        [](const ComponentInstanceSessionState& state) {
          return state.address.rootInstance();
        });
    CHECK(rootState !=
          emptyRootViewport.componentSession()->components().end());
    emptyRootViewport.setComponentEditContext(
        emptyRootViewport.componentSession()->scene(), rootState->address);
    emptyRootViewport.fitAll();
    const QPointF childPixel = childFacePixel(
        emptyRootViewport, *emptyRootViewport.componentSession());
    const auto childHit = emptyRootViewport.componentFaceAt(childPixel);
    CHECK(childHit && childHit.hit);
    CHECK(!childHit.hit->address.body.component.rootInstance());
  }

  // Deleting the sender from a direct receiver is equally safe: the guarded
  // emitter performs no post-signal access through the destroyed Viewport.
  {
    auto deletingProduct = componentProduct();
    auto preparedDeleting = ComponentViewportSession::prepare(
        *deletingProduct, ProductPresentationEpoch{812}, kNormal);
    CHECK(preparedDeleting);
    auto deletingViewport = std::make_unique<Viewport>();
    deletingViewport->resize(800, 600);
    deletingViewport->installComponentSession(
        std::move(preparedDeleting.session));
    const QPointF pixel = childFacePixel(
        *deletingViewport, *deletingViewport->componentSession());
    CHECK(deletingViewport->componentFaceAt(pixel));
    QPointer<Viewport> deletionGuard(deletingViewport.get());
    Viewport* deletingRaw = deletingViewport.get();
    QObject::connect(
        deletingRaw, &Viewport::componentPlacementDragRequested,
        &application,
        [&](ComponentPlacementDragRequest) { deletingViewport.reset(); },
        Qt::DirectConnection);
    sendMouse(*deletingRaw, QEvent::MouseButtonPress, pixel,
              Qt::LeftButton, Qt::LeftButton);
    sendMouse(*deletingRaw, QEvent::MouseMove,
              pixel + QPointF(15.0, 8.0), Qt::NoButton,
              Qt::LeftButton);
    CHECK(deletingRaw->componentPlacementPreview());
    sendMouse(*deletingRaw, QEvent::MouseButtonRelease,
              pixel + QPointF(15.0, 8.0), Qt::LeftButton,
              Qt::NoButton);
    CHECK(!deletionGuard);
  }

  // A direct receiver may destroy the sender itself. The notification boundary
  // must observe QPointer invalidation and return without touching Viewport.
  {
    auto deletingProduct = componentProduct();
    auto preparedDeleting = ComponentViewportSession::prepare(
        *deletingProduct, ProductPresentationEpoch{809}, kNormal);
    CHECK(preparedDeleting);
    auto deletingViewport = std::make_unique<Viewport>();
    deletingViewport->resize(800, 600);
    deletingViewport->installComponentSession(
        std::move(preparedDeleting.session));
    auto selection = ComponentInstanceSelection::create(
        deletingViewport->componentSession()->publicationHandle(),
        deletingViewport->componentSession()->components().front().address);
    CHECK(selection);
    QPointer<Viewport> deletionGuard(deletingViewport.get());
    Viewport* deletingRaw = deletingViewport.get();
    QObject::connect(
        deletingRaw, &Viewport::componentSelected, &application,
        [&](const ComponentInstanceSelection&) { deletingViewport.reset(); },
        Qt::DirectConnection);
    CHECK(!deletingRaw->setSelectedComponent(selection, true));
    CHECK(!deletionGuard);
  }

  int callbacksAfterDestruction = 0;
  {
    auto doomedProduct = componentProduct();
    auto doomedPrepared = ComponentViewportSession::prepare(
        *doomedProduct, ProductPresentationEpoch{804}, kNormal);
    CHECK(doomedPrepared);
    auto doomedViewport = std::make_unique<Viewport>();
    doomedViewport->resize(800, 600);
    QObject::connect(doomedViewport.get(), &Viewport::componentHovered,
                     &application,
                     [&](const ComponentInstanceSelection&) {
                       ++callbacksAfterDestruction;
                     });
    doomedViewport->installComponentSession(std::move(doomedPrepared.session));
    CHECK(doomedViewport->componentSession());
    sendMouse(*doomedViewport, QEvent::MouseMove,
              rootFacePixel(*doomedViewport,
                            *doomedViewport->componentSession()),
              Qt::NoButton, Qt::NoButton);
    doomedViewport.reset();
    QThread::msleep(25);
    QApplication::processEvents();
  }
  CHECK(callbacksAfterDestruction == 0);
  return EXIT_SUCCESS;
}
