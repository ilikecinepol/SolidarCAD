#include "TestAssertions.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include "ui/application/AssemblyInsertToolSession.h"
#include "model/ImportedShapeFeature.h"
#include "ui/AssemblyPlacementPreview.h"

using namespace solidar;

int main() {
  AssemblyInsertToolSession session;
  AssemblyInsertController controller;
  controller.activate(session, DefinitionId{20}, OccurrenceId{30});
  CHECK(controller.active() == &session);
  CHECK(session.state() == AssemblyInsertToolState::AwaitingSource);

  StagedEmbeddedPart source{std::make_unique<Document>(), QByteArray("cold")};
  CHECK(session.acceptSource(std::move(source)));
  CHECK(session.state() == AssemblyInsertToolState::Placing);
  CHECK(session.source());
  CHECK(session.source()->document);

  CHECK(session.updateTranslation({10.0, -20.0, 30.0}));
  CHECK(session.updateRotationDegrees({90.0, 0.0, 45.0}));
  const RigidTransform3d valid = session.placement();
  const std::array<double, 3> expectedTranslation{10.0, -20.0, 30.0};
  CHECK(valid.translationMm() == expectedTranslation);
  CHECK(!session.updateTranslation(
      {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}));
  CHECK(!session.updateRotationDegrees(
      {0.0, std::numeric_limits<double>::infinity(), 0.0}));
  CHECK(session.placement() == valid);

  CHECK(session.beginNestedTransform());
  CHECK(session.updateTranslation({40.0, 50.0, 60.0}));
  CHECK(controller.handleEscape());
  CHECK(session.state() == AssemblyInsertToolState::Placing);
  CHECK(session.placement() == valid);
  CHECK(!controller.handleEscape());
  CHECK(!controller.active());
  CHECK(session.state() == AssemblyInsertToolState::Inactive);

  controller.activate(session, DefinitionId{21}, OccurrenceId{31});
  CHECK(session.beginNestedSelection());
  CHECK(controller.handleEscape());
  CHECK(session.state() == AssemblyInsertToolState::AwaitingSource);
  CHECK(!controller.handleEscape());

  controller.activate(session, DefinitionId{22}, OccurrenceId{32});
  CHECK(session.acceptSource(StagedEmbeddedPart{
      std::make_unique<Document>(), QByteArray("authored")}));
  CHECK(session.updateTranslation({1.0, 2.0, 3.0}));
  CHECK(session.beginCommit());
  CHECK(!session.beginCommit());
  auto payload = session.takeRuntimePayload();
  CHECK(payload.definitionId == DefinitionId{22});
  CHECK(payload.occurrenceId == OccurrenceId{32});
  CHECK(payload.document);
  CHECK(payload.flags == OccurrenceFlags{});
  session.restoreRuntimePayload(std::move(payload));
  CHECK(session.source()->document);
  session.restoreAfterFailedCommit();
  CHECK(session.state() == AssemblyInsertToolState::Placing);
  CHECK(session.beginCommit());
  auto applied = session.takeRuntimePayload();
  CHECK(applied.document);
  session.finishCommit();
  CHECK(session.state() == AssemblyInsertToolState::Inactive);
  CHECK(!session.source());

  controller.cancelActive();
  controller.cancelActive();
  session.cancel();
  session.cancel();

  controller.activate(session, DefinitionId{23}, OccurrenceId{33});
  CHECK(session.acceptSource(StagedEmbeddedPart{
      std::make_unique<Document>(), QByteArray("first")}));
  CHECK(session.updateTranslation({9.0, 8.0, 7.0}));
  CHECK(session.acceptSource(StagedEmbeddedPart{
      std::make_unique<Document>(), QByteArray("replacement")}));
  CHECK(session.placement() == RigidTransform3d::identity());
  bool cancelObserved = false;
  controller.cancelActive([&cancelObserved](const StagedEmbeddedPart* staged) {
    cancelObserved = staged != nullptr && staged->document != nullptr;
  });
  CHECK(cancelObserved);
  CHECK(session.state() == AssemblyInsertToolState::Inactive);

  Document previewDocument;
  auto& body = previewDocument.addBody(100, "preview");
  body.addFeature(std::make_unique<ImportedShapeFeature>(
      101, std::make_shared<const TopoDS_Shape>(
               BRepPrimAPI_MakeBox(4.0, 5.0, 6.0).Shape()),
      "box"));
  CHECK(previewDocument.recompute());
  AssemblyPlacementPreview preview;
  CHECK(preview.prepare(previewDocument, ViewportMeshQuality::Normal));
  const auto token = preview.token();
  CHECK(token != 0);
  const auto builds = preview.meshBuildCount();
  CHECK(preview.setPlacement(valid));
  CHECK(preview.setPlacement(RigidTransform3d::identity()));
  CHECK(preview.token() == token);
  CHECK(preview.meshBuildCount() == builds);
  CHECK(preview.placementUpdateCount() == 2);
  preview.clear();
  CHECK(!preview.active());
  CHECK(preview.token() == 0);
  return EXIT_SUCCESS;
}
