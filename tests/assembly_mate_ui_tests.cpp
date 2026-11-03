#include "TestAssertions.h"

#include <QApplication>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QMouseEvent>
#include <QPushButton>
#include <QCheckBox>
#include <QStackedWidget>

#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <iostream>

#include "app/AppSettings.h"
#include "model/ExtrudeFeature.h"
#include "model/IdGeneration.h"
#include "project/ProductEnvelopeV7Encoder.h"
#include "project/ProductEnvelopeV8Encoder.h"
#include "project/internal/ProjectFormatV4AtomicWriting.h"
#include "sketch/Sketch.h"
#include "ui/MainWindow.h"
#include "ui/RigidTransform3dOps.h"
#include "ui/Viewport.h"

namespace solidar {

class MainWindowUndoTestAccess final {
 public:
  static void beginMate(MainWindow& window) { window.beginAssemblyMate(); }
  static void applyMate(MainWindow& window) { window.applyAssemblyMate(); }
  static void cancelMate(MainWindow& window) { window.cancelAssemblyMate(); }
  static void escapeMate(MainWindow& window) {
    window.assemblyMateController_->session().escape();
    if (window.viewport_) window.viewport_->clearAssemblyMatePreview();
    window.updateAssemblyMateUi();
  }
  static bool mateActive(const MainWindow& window) {
    return window.assemblyMateActive();
  }
  static bool canApply(const MainWindow& window) {
    return window.assemblyMateController_->session().canApply();
  }
  static AssemblyMateToolState mateState(const MainWindow& window) {
    return window.assemblyMateController_->session().state();
  }
  static std::size_t mateCount(const MainWindow& window) {
    const auto* root = window.productDocument_.rootAssemblyDefinition();
    return root ? root->mateRegistry().size() : 0;
  }
  static std::size_t historySize(const MainWindow& window) {
    return window.assemblyCommandHistory_.size();
  }
  static project::ProjectPersistenceFormat format(const MainWindow& window) {
    return window.projectPersistenceState_.format;
  }
  static bool previewActive(const MainWindow& window) {
    return window.viewport_->assemblyMatePreviewScene() != nullptr;
  }
  static const AssemblyViewportScene* previewScene(
      const MainWindow& window) {
    return window.viewport_->assemblyMatePreviewScene().get();
  }
  static bool applyEnabled(const MainWindow& window) {
    return window.assemblyApplyButton_->isEnabled();
  }
  static void failNextPreviewPrepare(MainWindow& window) {
    window.assemblyMatePreviewPrepareFailureForTesting_ = true;
  }
  static void failNextPreviewInstall(MainWindow& window) {
    window.assemblyMatePreviewInstallFailureForTesting_ = true;
  }
  static void setOpposed(MainWindow& window, bool opposed) {
    window.assemblyMateOpposedCheck_->setChecked(opposed);
  }
  static void setReentryHook(MainWindow& window,
                             std::function<void()> hook) {
    window.assemblyMateReentryHookForTesting_ = std::move(hook);
  }
  static void undo(MainWindow& window) { window.undoLastAction(); }
  static void redo(MainWindow& window) { window.redoLastAction(); }
  static bool partProject(const MainWindow& window) {
    return window.partProjectActive();
  }
  static bool visibleRibbonOwnsStackHeight(const MainWindow& window) {
    if (!window.ribbonStack_ || !window.ribbonStack_->currentWidget())
      return false;
    const int currentHeight =
        window.ribbonStack_->currentWidget()->sizeHint().height();
    return currentHeight > 0 &&
           window.ribbonStack_->minimumHeight() == currentHeight &&
           window.ribbonStack_->maximumHeight() == currentHeight &&
           window.ribbonStack_->height() == currentHeight;
  }

  struct ScreenFacePick final {
    AssemblyFaceRayHit hit;
    QPointF logicalPosition;
  };

  static std::optional<ScreenFacePick> faceHit(MainWindow& window,
                                                std::size_t leafIndex) {
    auto* session = window.viewport_->assemblySession();
    if (!session) return std::nullopt;
    std::vector<AssemblyOccurrenceSessionState> leaves;
    for (const auto& occurrence : session->occurrences()) {
      const auto resolved =
          window.productDocument_.resolveOccurrence(occurrence.identity.path);
      if (resolved && resolved.terminalPart()) leaves.push_back(occurrence);
    }
    if (leafIndex >= leaves.size()) return std::nullopt;
    const auto& leaf = leaves[leafIndex];
    const auto resolved =
        window.productDocument_.resolveOccurrence(leaf.identity.path);
    if (!resolved || !resolved.targetPart) return std::nullopt;
    const auto& document = resolved.targetPart->document();
    for (const auto& body : document.bodies()) {
      for (const auto& feature : body.features()) {
        for (std::size_t index = 0; index < 64; ++index) {
          auto face = document.createFaceReferenceV2(body.id(), feature->id(),
                                                     index);
          if (!face) continue;
          MateEndpoint endpoint{leaf.identity.path, face.reference};
          if (!classifyPlanarMateEndpoint(
                  window.productDocument_,
                  window.productDocument_.rootDefinitionId(), endpoint))
            continue;
          const auto camera = window.viewport_->assemblyCamera();
          for (int y = 2; y < window.viewport_->height(); y += 3) {
            for (int x = 2; x < window.viewport_->width(); x += 3) {
              const auto picked = session->faceAt(
                  camera, QPointF{static_cast<qreal>(x),
                                  static_cast<qreal>(y)});
              if (!picked || !picked.hit ||
                  picked.hit->address.occurrence.path != leaf.identity.path)
                continue;
              MateEndpoint pickedEndpoint{
                  picked.hit->address.occurrence.path,
                  picked.hit->address.face};
              if (!classifyPlanarMateEndpoint(
                      window.productDocument_,
                      window.productDocument_.rootDefinitionId(),
                      pickedEndpoint))
                continue;
              const QPointF projected =
                  camera.worldToScreen(picked.hit->worldPoint);
              const auto authoritative = session->faceAt(camera, projected);
              if (authoritative && authoritative.hit &&
                  authoritative.hit->address.occurrence.path ==
                      leaf.identity.path)
                return ScreenFacePick{*authoritative.hit, projected};
            }
          }
        }
      }
    }
    return std::nullopt;
  }

  static void mousePick(MainWindow& window, QPointF logicalPosition) {
    QMouseEvent event(QEvent::MouseButtonPress, logicalPosition,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(window.viewport_, &event);
  }
  static void deliverHit(MainWindow& window, const AssemblyFaceRayHit& hit) {
    window.acceptAssemblyMatePick(hit);
  }
  static std::optional<AssemblyOccurrenceIdentity> selection(
      const MainWindow& window) {
    const auto* session = window.viewport_->assemblySession();
    return session && session->selectedOccurrence()
               ? std::optional<AssemblyOccurrenceIdentity>{
                     session->selectedOccurrence()->occurrence}
               : std::nullopt;
  }
  static bool hoverActive(const MainWindow& window) {
    const auto* session = window.viewport_->assemblySession();
    return session && session->hoveredOccurrence().has_value();
  }
  struct PreviewPixelOracle final {
    QPointF committedOnly;
    QPointF previewOnly;
    Point3d previewWorldPoint;
    RigidTransform3d committedPlacement{RigidTransform3d::identity()};
    RigidTransform3d previewPlacement{RigidTransform3d::identity()};
  };
  static std::optional<PreviewPixelOracle> previewPixelOracle(
      MainWindow& window, const ScreenFacePick& committedPick) {
    const auto* session = window.viewport_->assemblySession();
    const auto preview = window.viewport_->assemblyMatePreviewScene();
    if (!session || !preview || !session->scene()) return std::nullopt;
    const auto& path = committedPick.hit.address.occurrence.path;
    const auto findInstance = [&path](const AssemblyViewportScene& scene) {
      return std::find_if(
          scene.instances().begin(), scene.instances().end(),
          [&path](const AssemblyViewportSceneInstance& instance) {
            return instance.occurrenceIdentity.path == path;
          });
    };
    const auto committed = findInstance(*session->scene());
    const auto moved = findInstance(*preview);
    if (committed == session->scene()->instances().end() ||
        moved == preview->instances().end() || !moved->mesh ||
        committed->localToWorld == moved->localToWorld)
      return std::nullopt;
    const auto camera = window.viewport_->assemblyCamera();
    const QRectF viewportRect(QPointF{}, QSizeF(window.viewport_->size()));
    for (const auto triangle : moved->mesh->triangles()) {
      const Point3d local{(triangle.a.x + triangle.b.x + triangle.c.x) / 3.0,
                          (triangle.a.y + triangle.b.y + triangle.c.y) / 3.0,
                          (triangle.a.z + triangle.b.z + triangle.c.z) / 3.0};
      const auto world = transformPoint(moved->localToWorld, local);
      if (!world) continue;
      const auto minimum = moved->worldBounds.minimum();
      const auto maximum = moved->worldBounds.maximum();
      if (world->x < minimum.x || world->x > maximum.x ||
          world->y < minimum.y || world->y > maximum.y ||
          world->z < minimum.z || world->z > maximum.z)
        continue;
      const QPointF pixel = camera.worldToScreen(*world);
      if (!viewportRect.contains(pixel) ||
          QLineF(pixel, committedPick.logicalPosition).length() < 8.0)
        continue;
      const auto baseQuery = session->faceAt(camera, pixel);
      if (baseQuery.status != AssemblyViewportPickingQueryStatus::Miss)
        continue;
      const auto committedQuery =
          session->faceAt(camera, committedPick.logicalPosition);
      if (!committedQuery ||
          committedQuery.hit->address.occurrence.path != path)
        return std::nullopt;
      return PreviewPixelOracle{committedPick.logicalPosition, pixel, *world,
                                committed->localToWorld,
                                moved->localToWorld};
    }
    return std::nullopt;
  }
  static Viewport* viewport(MainWindow& window) { return window.viewport_; }
};

}  // namespace solidar

namespace {

using namespace solidar;

constexpr ProjectId kProject{
    {0x81, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
     0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x10}};

std::unique_ptr<ProductDocument> mateProduct() {
  ::solidar::detail::ScopedExplicitIdReservationPause pause;
  auto document = std::make_unique<Document>();
  auto& sketch = document->addSketch("mate-ui-profile");
  sketch.geometry.addRectangle({0.0, 0.0}, {12.0, 8.0});
  auto& body = document->addBody("mate-ui-body");
  body.addFeature(std::make_unique<ExtrudeFeature>(
      sketch.id, 6.0, "mate-ui-extrude"));
  CHECK(document->recompute());
  const DefinitionId part{10};
  const DefinitionId rootId{100};
  AssemblyDefinition root(rootId);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{1}, part, RigidTransform3d::identity(),
            {.visible = true, .suppressed = false, .grounded = true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  const auto moved = RigidTransform3d::create(
      {31.0, 80.0, 23.0}, {0.9, 0.1, 0.2, 0.3});
  CHECK(moved);
  CHECK(root.insert(ComponentOccurrence(OccurrenceId{2}, part,
                                        *moved.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(part, std::move(document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), rootId);
  CHECK(made);
  return std::move(made.product);
}

bool writeFixture(const QString& path,
                  project::ProjectPersistenceFormat format) {
  auto product = mateProduct();
  const project::ProductEnvelopeV5Metadata metadata{
      QStringLiteral("A8a mate UI"), QStringLiteral("2026-10-30T00:00:00Z")};
  QByteArray bytes;
  if (format == project::ProjectPersistenceFormat::V7) {
    const auto encoded =
        project::encodeProductEnvelopeV7Root(*product, kProject, metadata);
    if (!encoded.succeeded()) {
      std::cerr << encoded.error.toStdString() << '\n';
      return false;
    }
    bytes = encoded.payload->bytes;
  } else {
    const auto encoded =
        project::encodeProductEnvelopeV8Root(*product, kProject, metadata);
    if (!encoded.succeeded()) {
      std::cerr << encoded.error.toStdString() << '\n';
      return false;
    }
    bytes = encoded.payload->bytes;
  }
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    std::cerr << file.errorString().toStdString() << '\n';
    return false;
  }
  const auto written = file.write(bytes);
  const bool flushed = file.flush();
  if (written != bytes.size() || !flushed)
    std::cerr << "write=" << written << " bytes=" << bytes.size()
              << " error=" << file.errorString().toStdString() << '\n';
  return written == bytes.size() && flushed;
}

QByteArray bytesOf(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

}  // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  QTemporaryDir directory(
      QDir::current().filePath(QStringLiteral("a8a-mate-ui-XXXXXX")));
  CHECK(directory.isValid());
  AppSettings settings;
  using Access = MainWindowUndoTestAccess;
  Document standalonePart;
  standalonePart.setBox({9.0, 7.0, 5.0});
  const QString standalonePartPath =
      directory.filePath(QStringLiteral("standalone-part.solidar"));
  CHECK(project::detail::writeProjectFormatV4DocumentAtomically(
            standalonePartPath, standalonePart)
            .succeeded());

  for (const auto format : {project::ProjectPersistenceFormat::V7,
                            project::ProjectPersistenceFormat::V8}) {
    const QString path = directory.filePath(
        format == project::ProjectPersistenceFormat::V7 ? QStringLiteral("mate-v7.solidar")
                                               : QStringLiteral("mate-v8.solidar"));
    CHECK(writeFixture(path, format));
    const QByteArray original = bytesOf(path);
    MainWindow window(settings);
    window.resize(1100, 760);
    window.show();
    QApplication::processEvents();
    CHECK(Access::visibleRibbonOwnsStackHeight(window));
    QString error;
    CHECK(window.loadProject(path, &error));
    QApplication::processEvents();
    CHECK(Access::visibleRibbonOwnsStackHeight(window));
    CHECK(Access::format(window) == format);
    CHECK(Access::mateCount(window) == 0);

    Access::beginMate(window);
    CHECK(Access::mateActive(window));
    const auto first = Access::faceHit(window, 0);
    const auto second = Access::faceHit(window, 1);
    CHECK(first && second);
    Access::mousePick(window, first->logicalPosition);
    CHECK(Access::mateState(window) ==
          AssemblyMateToolState::SelectingSecond);
    Access::mousePick(window, second->logicalPosition);
    CHECK(Access::canApply(window));
    CHECK(Access::previewActive(window));
    const auto* installedAligned = Access::previewScene(window);
    CHECK(installedAligned);
    const auto pixelOracle = Access::previewPixelOracle(window, *second);
    CHECK(pixelOracle);
    CHECK(pixelOracle->committedPlacement != pixelOracle->previewPlacement);
    Access::failNextPreviewPrepare(window);
    Access::setOpposed(window, true);
    CHECK(Access::mateState(window) == AssemblyMateToolState::PreviewFailed);
    CHECK(!Access::canApply(window));
    CHECK(!Access::applyEnabled(window));
    CHECK(Access::previewScene(window) == installedAligned);
    const auto frozenState = Access::mateState(window);
    const auto frozenSelection = Access::selection(window);
    // The committed-only pixel is an authoritative base-scene hit. The
    // preview-only pixel is a transformed preview triangle centroid for which
    // the committed picking index reports Miss. Both are frozen while the
    // retained candidate visual is installed.
    Access::mousePick(window, pixelOracle->committedOnly);
    Access::mousePick(window, pixelOracle->previewOnly);
    CHECK(Access::mateState(window) == frozenState);
    CHECK(Access::selection(window) == frozenSelection);
    Access::escapeMate(window);
    CHECK(Access::mateState(window) ==
          AssemblyMateToolState::SelectingSecond);
    CHECK(!Access::previewActive(window));
    Access::mousePick(window, pixelOracle->committedOnly);
    CHECK(Access::canApply(window));
    CHECK(Access::applyEnabled(window));
    const auto* recoveredOpposed = Access::previewScene(window);
    CHECK(recoveredOpposed);
    Access::failNextPreviewInstall(window);
    Access::setOpposed(window, false);
    CHECK(Access::mateState(window) == AssemblyMateToolState::PreviewFailed);
    CHECK(!Access::canApply(window));
    CHECK(!Access::applyEnabled(window));
    CHECK(Access::previewScene(window) == recoveredOpposed);
    Access::setOpposed(window, true);
    CHECK(Access::canApply(window));
    CHECK(Access::applyEnabled(window));
    const auto selectionBeforeFrozenClick = Access::selection(window);
    const auto revisionBeforeFrozenClick =
        Access::mateState(window);
    Access::mousePick(window, first->logicalPosition);
    CHECK(Access::mateState(window) == revisionBeforeFrozenClick);
    CHECK(Access::selection(window) == selectionBeforeFrozenClick);
    CHECK(!Access::hoverActive(window));
    CHECK(Access::mateCount(window) == 0);
    CHECK(Access::format(window) == format);
    CHECK(bytesOf(path) == original);
    Access::cancelMate(window);
    CHECK(!Access::mateActive(window));
    CHECK(!Access::previewActive(window));
    CHECK(Access::mateCount(window) == 0);
    CHECK(Access::format(window) == format);
    CHECK(bytesOf(path) == original);

    Access::beginMate(window);
    Access::mousePick(window, Access::faceHit(window, 0)->logicalPosition);
    Access::mousePick(window, Access::faceHit(window, 1)->logicalPosition);
    CHECK(Access::canApply(window));
    CHECK(!window.loadProject(
        directory.filePath(QStringLiteral("missing-a8a.solidar")), &error));
    CHECK(Access::mateActive(window));
    CHECK(Access::canApply(window));
    Access::applyMate(window);
    CHECK(!Access::mateActive(window));
    CHECK(Access::mateCount(window) == 1);
    CHECK(Access::historySize(window) == 1);
    CHECK(Access::format(window) == project::ProjectPersistenceFormat::V9);
    Access::undo(window);
    CHECK(Access::mateCount(window) == 0);
    Access::redo(window);
    CHECK(Access::mateCount(window) == 1);

    CHECK(window.loadProject(path, &error));
    Access::beginMate(window);
    const auto reentrantHit = Access::faceHit(window, 0);
    CHECK(reentrantHit);
    const auto reentrant = QObject::connect(
        Access::viewport(window), &Viewport::assemblyMateFacePicked,
        &window, [&window](const AssemblyFaceRayHit&) {
          Access::cancelMate(window);
          Access::beginMate(window);
        }, Qt::DirectConnection);
    Access::mousePick(window, reentrantHit->logicalPosition);
    QObject::disconnect(reentrant);
    CHECK(Access::mateActive(window));
    CHECK(Access::mateState(window) ==
          AssemblyMateToolState::SelectingFirst);

    // Re-entry happens inside the production accept handler, before its outer
    // publication/revision guard resumes. The stale outer callback cannot
    // publish over the newly begun session.
    const auto nestedReentrantHit = Access::faceHit(window, 0);
    CHECK(nestedReentrantHit);
    Access::setReentryHook(window, [&window] {
      Access::cancelMate(window);
      Access::beginMate(window);
    });
    Access::mousePick(window, nestedReentrantHit->logicalPosition);
    CHECK(Access::mateActive(window));
    CHECK(Access::mateState(window) ==
          AssemblyMateToolState::SelectingFirst);
    CHECK(!Access::previewActive(window));

    Access::beginMate(window);
    CHECK(Access::mateActive(window));
    const auto oldAssemblyHit = Access::faceHit(window, 0);
    CHECK(oldAssemblyHit);
    CHECK(window.loadProject(path, &error));
    CHECK(!Access::mateActive(window));
    CHECK(Access::mateCount(window) == 0);
    CHECK(Access::format(window) == format);
    const auto historyAfterAssemblySwitch = Access::historySize(window);
    Access::deliverHit(window, oldAssemblyHit->hit);
    Access::setOpposed(window, true);
    CHECK(!Access::mateActive(window));
    CHECK(Access::historySize(window) == historyAfterAssemblySwitch);
    CHECK(Access::mateCount(window) == 0);

    Access::beginMate(window);
    CHECK(Access::mateActive(window));
    const auto partSwitchFirst = Access::faceHit(window, 0);
    CHECK(partSwitchFirst);
    Access::mousePick(window, partSwitchFirst->logicalPosition);
    CHECK(window.loadProject(standalonePartPath, &error));
    CHECK(!Access::mateActive(window));
    CHECK(Access::partProject(window));
    Access::deliverHit(window, partSwitchFirst->hit);
    Access::setOpposed(window, true);
    CHECK(!Access::mateActive(window));
    CHECK(Access::partProject(window));
  }
  return EXIT_SUCCESS;
}
