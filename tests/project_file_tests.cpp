#include "project/ProjectFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>

#include "TestGeometryUtils.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletBuilder.h"
#include "model/FilletFeature.h"
#include "model/PocketFeature.h"

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, \
                   #condition);                                              \
      return 1;                                                              \
    }                                                                        \
  } while (false)

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  // Keep test artifacts under CTest's writable build directory. This also
  // avoids platform policies that deny atomic QSaveFile replacement in the
  // user's global temporary directory.
  QTemporaryDir directory(
      QDir::current().filePath(QStringLiteral("project-file-tests-XXXXXX")));
  if (!directory.isValid()) {
    std::fprintf(stderr, "QTemporaryDir failed: %s\n",
                 directory.errorString().toUtf8().constData());
    return 1;
  }
  const QString path = directory.filePath("sample.solidar");

  QString error;
  if (!solidar::project::ProjectFile::create(path, &error)) {
    std::fprintf(stderr, "ProjectFile::create failed for %s: %s\n",
                 path.toUtf8().constData(), error.toUtf8().constData());
    return 1;
  }
  assert(QFile::exists(path));
  assert(solidar::project::ProjectFile::validate(path, &error));

  // A freshly created project is already a canonical v2 document.  This is
  // important because Home -> New immediately feeds the file into the editor's
  // document loader; it must not take a legacy-only initialization path.
  solidar::Document createdDocument;
  assert(solidar::project::ProjectFile::loadDocument(
      path, &createdDocument, &error));
  assert(createdDocument.sketches().empty());
  assert(createdDocument.bodies().empty());

  // Project v2 round-trip preserves IDs, parameters, face support and the
  // complete editable feature chain, then rebuilds B-Rep from history.
  solidar::Document source;
  auto& arcSketch = source.addSketch("Arc constraints");
  const auto arcSketchId = arcSketch.id;
  arcSketch.geometry.addLine({-50.0, 0.0}, {50.0, 0.0});
  arcSketch.geometry.addArc({10.0, 20.0}, 8.0,
                            3.14159265358979323846,
                            3.14159265358979323846);
  solidar::sketch::Constraint arcTangent;
  arcTangent.type = solidar::sketch::ConstraintType::Tangent;
  arcTangent.firstGeometry = arcSketch.geometry.lineId(0);
  arcTangent.secondGeometry = arcSketch.geometry.arcId(0);
  assert(arcSketch.geometry.addConstraint(arcTangent) !=
         solidar::sketch::kInvalidConstraintId);
  auto& baseSketch = source.addSketch("Base");
  const auto baseSketchId = baseSketch.id;
  baseSketch.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
  auto& body = source.addBody("Body");
  const auto bodyId = body.id();
  auto extrude = std::make_unique<solidar::ExtrudeFeature>(
      baseSketchId, 50.0, "Extrude");
  auto* extrudePtr = extrude.get();
  body.addFeature(std::move(extrude));
  assert(source.recompute());
  const auto topFace =
      solidar::test::topPlanarFace(*body.resultShape(), 50.0);
  assert(topFace);
  auto& pocketSketch = source.addSketch("Pocket sketch");
  const auto pocketSketchId = pocketSketch.id;
  assert(source.attachSketchToFace(
      pocketSketchId, {bodyId, extrudePtr->id(), *topFace}));
  pocketSketch.geometry.addRectangle({-10.0, -5.0}, {10.0, 5.0});
  auto pocket = std::make_unique<solidar::PocketFeature>(
      pocketSketchId, 10.0, "Pocket");
  auto* pocketPtr = pocket.get();
  body.addFeature(std::move(pocket));
  assert(source.recompute());
  std::size_t edgeIndex = 0;
  for (;; ++edgeIndex) {
    std::string buildError;
    if (solidar::buildFilletShape(*body.resultShape(), {edgeIndex}, 2.0,
                                  &buildError))
      break;
    assert(edgeIndex < 64);
  }
  auto fillet = std::make_unique<solidar::FilletFeature>(
      solidar::EdgeReference{bodyId, pocketPtr->id(), edgeIndex}, 2.0,
      "Fillet");
  const auto filletId = fillet->id();
  body.addFeature(std::move(fillet));
  assert(source.recompute());
  const double sourceVolume =
      solidar::test::volumeOf(*body.resultShape());

  const QString modelPath = directory.filePath("parametric-v2.solidar");
  assert(solidar::project::ProjectFile::saveDocument(modelPath, source,
                                                      &error));
  solidar::Document restored;
  assert(solidar::project::ProjectFile::loadDocument(modelPath, &restored,
                                                      &error));
  const auto* restoredBody = restored.findBody(bodyId);
  assert(restoredBody && restoredBody->features().size() == 3);
  assert(restored.findSketch(baseSketchId));
  const auto* restoredArcSketch = restored.findSketch(arcSketchId);
  assert(restoredArcSketch);
  assert(restoredArcSketch->geometry.arcs().size() == 1);
  assert(restoredArcSketch->geometry.constraints().size() == 1);
  assert(restoredArcSketch->geometry.constraints().front().type ==
         solidar::sketch::ConstraintType::Tangent);
  assert(restoredArcSketch->geometry.constraints().front().secondGeometry ==
         restoredArcSketch->geometry.arcId(0));
  const auto* restoredPocketSketch = restored.findSketch(pocketSketchId);
  assert(restoredPocketSketch);
  assert(restoredPocketSketch->support.type ==
         solidar::SketchSupportType::Face);
  assert(restoredPocketSketch->support.face.featureId == extrudePtr->id());
  assert(restoredBody->features().back()->id() == filletId);
  assert(restoredBody->features().back()->typeName() == "Fillet");
  assert(solidar::test::near(
      solidar::test::volumeOf(*restoredBody->resultShape()), sourceVolume,
      1e-4));

  // A native Line + Arc profile remains editable after a complete project
  // round-trip. IDs and feature parameters must survive serialization, while
  // the B-Rep is rebuilt from the restored parametric history.
  {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kExtrudeLength = 25.0;
    solidar::Document lineArcDocument;
    auto& lineArcSketch = lineArcDocument.addSketch("Line and Arc profile");
    const auto lineArcSketchId = lineArcSketch.id;
    lineArcSketch.geometry.addLine({-10.0, 0.0}, {10.0, 0.0});
    lineArcSketch.geometry.addArc({0.0, 0.0}, 10.0, 0.0, kPi);
    CHECK(lineArcSketch.geometry.lines().size() == 1);
    CHECK(lineArcSketch.geometry.arcs().size() == 1);
    CHECK(lineArcSketch.geometry.circles().empty());
    CHECK(lineArcSketch.geometry.isClosed());
    const auto sourceArc = lineArcSketch.geometry.arcs().front();

    auto& lineArcBody = lineArcDocument.addBody("Line and Arc body");
    const auto lineArcBodyId = lineArcBody.id();
    auto lineArcExtrude = std::make_unique<solidar::ExtrudeFeature>(
        lineArcSketchId, kExtrudeLength, "Line and Arc Extrude",
        solidar::ExtrudeOperation::NewBody, true);
    auto* lineArcExtrudePtr = lineArcExtrude.get();
    const auto lineArcExtrudeId = lineArcExtrudePtr->id();
    const auto sourceOperation = lineArcExtrudePtr->operation();
    const bool sourceReversed = lineArcExtrudePtr->reversed();
    lineArcBody.addFeature(std::move(lineArcExtrude));

    CHECK(lineArcDocument.recompute());
    CHECK(lineArcExtrudePtr->state() == solidar::FeatureState::Valid);
    CHECK(lineArcExtrudePtr->shape());
    CHECK(!lineArcExtrudePtr->shape()->IsNull());
    CHECK(solidar::test::solidCount(*lineArcExtrudePtr->shape()) == 1);
    const double lineArcVolume =
        solidar::test::volumeOf(*lineArcExtrudePtr->shape());
    CHECK(lineArcVolume > 0.0);

    const QString lineArcPath = directory.filePath("line-arc.solidar");
    CHECK(solidar::project::ProjectFile::saveDocument(
        lineArcPath, lineArcDocument, &error));

    solidar::Document loadedLineArcDocument;
    CHECK(solidar::project::ProjectFile::loadDocument(
        lineArcPath, &loadedLineArcDocument, &error));
    auto* loadedLineArcSketch =
        loadedLineArcDocument.findSketch(lineArcSketchId);
    CHECK(loadedLineArcSketch);
    CHECK(loadedLineArcSketch->id == lineArcSketchId);
    CHECK(loadedLineArcSketch->geometry.lines().size() == 1);
    CHECK(loadedLineArcSketch->geometry.arcs().size() == 1);
    CHECK(loadedLineArcSketch->geometry.circles().empty());
    CHECK(loadedLineArcSketch->geometry.isClosed());
    const auto& loadedArc = loadedLineArcSketch->geometry.arcs().front();
    CHECK(solidar::test::near(loadedArc.center.xMm, sourceArc.center.xMm));
    CHECK(solidar::test::near(loadedArc.center.yMm, sourceArc.center.yMm));
    CHECK(solidar::test::near(loadedArc.radiusMm, sourceArc.radiusMm));
    CHECK(solidar::test::near(loadedArc.startAngleRad,
                              sourceArc.startAngleRad));
    CHECK(solidar::test::near(loadedArc.sweepAngleRad,
                              sourceArc.sweepAngleRad));

    auto* loadedLineArcBody =
        loadedLineArcDocument.findBody(lineArcBodyId);
    CHECK(loadedLineArcBody);
    CHECK(loadedLineArcBody->features().size() == 1);
    auto* loadedLineArcExtrude = dynamic_cast<solidar::ExtrudeFeature*>(
        loadedLineArcBody->features().front().get());
    CHECK(loadedLineArcExtrude);
    CHECK(loadedLineArcExtrude->id() == lineArcExtrudeId);
    CHECK(loadedLineArcExtrude->profileSketchId() == lineArcSketchId);
    CHECK(solidar::test::near(loadedLineArcExtrude->lengthMm(),
                              kExtrudeLength));
    CHECK(loadedLineArcExtrude->operation() == sourceOperation);
    CHECK(loadedLineArcExtrude->reversed() == sourceReversed);

    CHECK(loadedLineArcDocument.recompute());
    CHECK(loadedLineArcExtrude->state() == solidar::FeatureState::Valid);
    CHECK(loadedLineArcExtrude->shape());
    CHECK(!loadedLineArcExtrude->shape()->IsNull());
    CHECK(solidar::test::solidCount(*loadedLineArcExtrude->shape()) == 1);
    const double loadedLineArcVolume =
        solidar::test::volumeOf(*loadedLineArcExtrude->shape());
    CHECK(solidar::test::near(loadedLineArcVolume, lineArcVolume, 1e-4));

    loadedLineArcSketch->geometry.removeArc(0);
    const double editedRadius = 12.0;
    const double centerOffset =
        std::sqrt(editedRadius * editedRadius - 10.0 * 10.0);
    const double startAngle = std::atan2(centerOffset, 10.0);
    loadedLineArcSketch->geometry.addArc(
        {0.0, -centerOffset}, editedRadius, startAngle,
        kPi - 2.0 * startAngle);
    CHECK(loadedLineArcSketch->geometry.lines().size() == 1);
    CHECK(loadedLineArcSketch->geometry.arcs().size() == 1);
    CHECK(loadedLineArcSketch->geometry.isClosed());
    CHECK(loadedLineArcDocument.markSketchDirty(lineArcSketchId));
    CHECK(loadedLineArcExtrude->isDirty());
    CHECK(loadedLineArcDocument.recompute());
    CHECK(loadedLineArcExtrude->id() == lineArcExtrudeId);
    CHECK(loadedLineArcExtrude->state() == solidar::FeatureState::Valid);
    CHECK(loadedLineArcExtrude->shape());
    CHECK(!loadedLineArcExtrude->shape()->IsNull());
    CHECK(solidar::test::solidCount(*loadedLineArcExtrude->shape()) == 1);
    CHECK(std::abs(solidar::test::volumeOf(*loadedLineArcExtrude->shape()) -
                   loadedLineArcVolume) > 1e-4);
  }

  // Extrude from one selected region of a multi-profile sketch.  The source
  // sketch remains intact, while the feature stores only the picked region.
  {
    solidar::Document selectedProfileDocument;
    auto& sourceSketch = selectedProfileDocument.addSketch("Multi profile");
    const auto sourceSketchId = sourceSketch.id;
    sourceSketch.geometry.addRectangle({0.0, 0.0}, {40.0, 25.0});
    sourceSketch.geometry.addCircle({70.0, 12.5}, 10.0);

    solidar::sketch::Sketch selectedRegion;
    selectedRegion.addRectangle({0.0, 0.0}, {40.0, 25.0});

    auto& selectedBody = selectedProfileDocument.addBody("Selected body");
    auto selectedExtrude = std::make_unique<solidar::ExtrudeFeature>(
        sourceSketchId, 12.0, "Selected region");
    selectedExtrude->setProfileOverride(selectedRegion);
    selectedBody.addFeature(std::move(selectedExtrude));

    assert(selectedProfileDocument.recompute());
    assert(solidar::test::near(
        solidar::test::volumeOf(*selectedBody.resultShape()),
        40.0 * 25.0 * 12.0, 1e-4));

    const QString selectedPath =
        directory.filePath("selected-profile.solidar");
    assert(solidar::project::ProjectFile::saveDocument(
        selectedPath, selectedProfileDocument, &error));

    solidar::Document selectedRestored;
    assert(solidar::project::ProjectFile::loadDocument(
        selectedPath, &selectedRestored, &error));
    const auto* restoredSelectedBody = selectedRestored.activeBody();
    assert(restoredSelectedBody);
    assert(restoredSelectedBody->features().size() == 1);
    const auto* restoredExtrude =
        dynamic_cast<const solidar::ExtrudeFeature*>(
            restoredSelectedBody->features().front().get());
    assert(restoredExtrude);
    assert(restoredExtrude->profileOverride());
    assert(solidar::test::near(
        solidar::test::volumeOf(*restoredSelectedBody->resultShape()),
        40.0 * 25.0 * 12.0, 1e-4));
  }
  QFile broken(directory.filePath("broken.solidar"));
  assert(broken.open(QIODevice::WriteOnly));
  broken.write("not json");
  broken.close();
  assert(!solidar::project::ProjectFile::validate(broken.fileName(), &error));
  return 0;
}
