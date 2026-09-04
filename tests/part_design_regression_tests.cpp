#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "TestGeometryUtils.h"
#include "model/ChamferBuilder.h"
#include "model/ChamferFeature.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletBuilder.h"
#include "model/FilletFeature.h"
#include "model/PocketFeature.h"
#include "model/RevolveFeature.h"
#include "model/TopologyReferenceResolver.h"
#include "project/ProjectFile.h"

namespace {

class TestFailure final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition))                                                        \
      throw TestFailure(std::string(__FILE__) + ":" +                       \
                        std::to_string(__LINE__) + ": " #condition);         \
  } while (false)

solidar::sketch::Point toLocal(const solidar::SketchPlacement& placement,
                               solidar::Point3d world) {
  const solidar::Vector3d delta{world.x - placement.origin.x,
                                world.y - placement.origin.y,
                                world.z - placement.origin.z};
  return {delta.x * placement.xDirection.x +
              delta.y * placement.xDirection.y +
              delta.z * placement.xDirection.z,
          delta.x * placement.yDirection.x +
              delta.y * placement.yDirection.y +
              delta.z * placement.yDirection.z};
}

std::optional<std::size_t> filletableEdge(const TopoDS_Shape& shape,
                                          double radius) {
  for (std::size_t index = 0; index < 64; ++index) {
    std::string error;
    if (solidar::buildFilletShape(shape, {index}, radius, &error)) return index;
    if (error == "Fillet edge could not be resolved") break;
  }
  return std::nullopt;
}

std::optional<std::size_t> chamferableEdge(const TopoDS_Shape& shape,
                                           double distance) {
  for (std::size_t index = 0; index < 64; ++index) {
    std::string error;
    if (solidar::buildChamferShape(shape, {index}, distance, &error))
      return index;
    if (error == "Chamfer edge could not be resolved") break;
  }
  return std::nullopt;
}

class SilentFailureFeature final : public solidar::ShapeFeature {
 public:
  SilentFailureFeature() : ShapeFeature("Silent failure") {}

  bool rebuild(const solidar::RebuildContext&) override {
    clearShape();
    markError({});
    return false;
  }
  [[nodiscard]] std::string typeName() const override { return "SilentFailure"; }
  [[nodiscard]] std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<SilentFailureFeature>(*this);
  }
};

void checkAllValid(const solidar::Document& document) {
  for (const auto& body : document.bodies()) {
    CHECK(body.resultShape());
    CHECK(!body.resultShape()->IsNull());
    for (const auto& feature : body.features()) {
      CHECK(feature->isValid());
      CHECK(feature->hasShape());
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    QCoreApplication application(argc, argv);
    solidar::Document document;

    auto& baseSketch = document.addSketch("Base profile");
    const auto baseSketchId = baseSketch.id;
    baseSketch.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});

    auto& partBody = document.addBody("Part Design body");
    const auto partBodyId = partBody.id();
    auto extrude = std::make_unique<solidar::ExtrudeFeature>(
        baseSketchId, 50.0, "Extrude");
    auto* extrudePtr = extrude.get();
    const auto extrudeId = extrudePtr->id();
    partBody.addFeature(std::move(extrude));
    CHECK(document.recompute());

    const auto topFace =
        solidar::test::topPlanarFace(*partBody.resultShape(), 50.0);
    CHECK(topFace);
    auto& pocketSketch = document.addSketch("Sketch on face");
    const auto pocketSketchId = pocketSketch.id;
    const auto topFaceReference = solidar::makeFaceReference(
        *partBody.resultShape(), partBodyId, extrudeId, *topFace);
    CHECK(topFaceReference.signature);
    CHECK(!topFaceReference.persistentTag.empty());
    CHECK(document.attachSketchToFace(pocketSketchId, topFaceReference));
    const auto cornerA =
        toLocal(pocketSketch.placement, {20.0, 10.0, 50.0});
    const auto cornerB =
        toLocal(pocketSketch.placement, {40.0, 25.0, 50.0});
    pocketSketch.geometry.addRectangle(cornerA, cornerB);

    auto pocket = std::make_unique<solidar::PocketFeature>(
        pocketSketchId, 15.0, "Pocket");
    auto* pocketPtr = pocket.get();
    const auto pocketId = pocketPtr->id();
    partBody.addFeature(std::move(pocket));
    CHECK(document.recompute());

    const auto edgeIndex = filletableEdge(*partBody.resultShape(), 2.0);
    CHECK(edgeIndex);
    const auto filletEdge = solidar::makeEdgeReference(
        *partBody.resultShape(), partBodyId, pocketId, *edgeIndex);
    CHECK(filletEdge.signature);
    auto fillet = std::make_unique<solidar::FilletFeature>(filletEdge, 2.0,
                                                          "Fillet");
    auto* filletPtr = fillet.get();
    const auto filletId = filletPtr->id();
    partBody.addFeature(std::move(fillet));

    auto& revolveSketch = document.addSketch("Revolve profile");
    const auto revolveSketchId = revolveSketch.id;
    revolveSketch.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
    auto& revolveBody = document.addBody("Revolve body");
    const auto revolveBodyId = revolveBody.id();
    auto revolve = std::make_unique<solidar::RevolveFeature>(
        revolveSketchId,
        solidar::AxisReference{
            solidar::AxisReferenceType::SketchHorizontalAxis,
            revolveSketchId, solidar::sketch::kInvalidGeometryId},
        270.0, "Revolve", solidar::ExtrudeOperation::NewBody);
    auto* revolvePtr = revolve.get();
    const auto revolveId = revolvePtr->id();
    revolveBody.addFeature(std::move(revolve));

    CHECK(document.recompute());
    auto* chamferBody = document.findBody(partBodyId);
    CHECK(chamferBody && chamferBody->resultShape());
    const auto chamferEdgeIndex =
        chamferableEdge(*chamferBody->resultShape(), 1.0);
    CHECK(chamferEdgeIndex);
    const auto chamferEdge = solidar::makeEdgeReference(
        *chamferBody->resultShape(), partBodyId, filletId, *chamferEdgeIndex);
    CHECK(chamferEdge.signature);
    auto chamfer = std::make_unique<solidar::ChamferFeature>(
        chamferEdge, 1.0, "Chamfer");
    auto* chamferPtr = chamfer.get();
    const auto chamferId = chamferPtr->id();
    chamferBody->addFeature(std::move(chamfer));
    CHECK(document.recompute());
    auto* partBodyPtr = document.findBody(partBodyId);
    auto* revolveBodyPtr = document.findBody(revolveBodyId);
    CHECK(partBodyPtr && revolveBodyPtr);
    checkAllValid(document);
    CHECK(document.bodies().size() == 2);
    CHECK(document.sketches().size() == 3);
    CHECK(partBodyPtr->features().size() == 4);
    CHECK(revolveBodyPtr->features().size() == 1);
    CHECK(document.rebuildError().empty());

    double partVolume = solidar::test::volumeOf(*partBodyPtr->resultShape());
    double revolveVolume =
        solidar::test::volumeOf(*revolveBodyPtr->resultShape());

    // Root sketch edits must dirty and rebuild the complete dependent chain.
    solidar::sketch::Sketch widerProfile;
    widerProfile.addRectangle({0.0, 0.0}, {80.0, 40.0});
    CHECK(document.replaceSketchGeometry(baseSketchId, widerProfile));
    CHECK(extrudePtr->isDirty());
    CHECK(pocketPtr->isDirty());
    CHECK(filletPtr->isDirty());
    CHECK(chamferPtr->isDirty());
    if (!document.recompute())
      throw TestFailure("dependent Chamfer recompute failed: " +
                        document.rebuildError());
    checkAllValid(document);
    CHECK(std::abs(solidar::test::volumeOf(*partBodyPtr->resultShape()) -
                   partVolume) > 1e-4);
    CHECK(partBodyPtr->id() == partBodyId);
    CHECK(extrudePtr->id() == extrudeId);
    CHECK(pocketPtr->id() == pocketId);
    CHECK(filletPtr->id() == filletId);
    CHECK(chamferPtr->id() == chamferId);

    // Editing every supported 3D parameter must change real B-Rep geometry.
    partVolume = solidar::test::volumeOf(*partBodyPtr->resultShape());
    extrudePtr->setLengthMm(60.0);
    CHECK(extrudePtr->isDirty());
    CHECK(document.recompute());
    checkAllValid(document);
    CHECK(std::abs(solidar::test::volumeOf(*partBodyPtr->resultShape()) -
                   partVolume) > 1e-4);

    partVolume = solidar::test::volumeOf(*partBodyPtr->resultShape());
    pocketPtr->setDepthMm(20.0);
    CHECK(pocketPtr->isDirty());
    CHECK(document.recompute());
    checkAllValid(document);
    CHECK(std::abs(solidar::test::volumeOf(*partBodyPtr->resultShape()) -
                   partVolume) > 1e-4);

    partVolume = solidar::test::volumeOf(*partBodyPtr->resultShape());
    filletPtr->setRadiusMm(1.5);
    CHECK(filletPtr->isDirty());
    CHECK(document.recompute());
    CHECK(filletPtr->isValid());
    CHECK(std::abs(solidar::test::volumeOf(*partBodyPtr->resultShape()) -
                   partVolume) > 1e-4);

    partVolume = solidar::test::volumeOf(*partBodyPtr->resultShape());
    chamferPtr->setDistanceMm(0.75);
    CHECK(chamferPtr->isDirty());
    CHECK(document.recompute());
    CHECK(chamferPtr->isValid());
    CHECK(std::abs(solidar::test::volumeOf(*partBodyPtr->resultShape()) -
                   partVolume) > 1e-4);

    revolvePtr->setAngleDeg(180.0);
    CHECK(revolvePtr->isDirty());
    CHECK(document.recompute());
    CHECK(revolvePtr->isValid());
    CHECK(std::abs(solidar::test::volumeOf(*revolveBodyPtr->resultShape()) -
                   revolveVolume) > 1e-4);

    // OCCT failure must be diagnostic and recoverable without stale geometry.
    filletPtr->setRadiusMm(1000.0);
    CHECK(!document.recompute());
    CHECK(filletPtr->isFailed());
    CHECK(!filletPtr->hasShape());
    CHECK(!filletPtr->error().empty());
    CHECK(document.rebuildError() == filletPtr->error());
    filletPtr->setRadiusMm(2.0);
    CHECK(document.recompute());
    checkAllValid(document);
    CHECK(document.rebuildError().empty());

    // A failed feature without text must still produce a useful fallback.
    solidar::Document silentFailure;
    silentFailure.addBody("Fallback body")
        .addFeature(std::make_unique<SilentFailureFeature>());
    CHECK(!silentFailure.recompute());
    CHECK(!silentFailure.rebuildError().empty());
    CHECK(silentFailure.rebuildError().find("without a diagnostic") !=
          std::string::npos);

    // Persist the complete two-body history and rebuild it in a new Document.
    QTemporaryDir directory(
        QDir::current().filePath("part-design-regression-XXXXXX"));
    CHECK(directory.isValid());
    const QString path = directory.filePath("round-trip.solidar");
    QString error;
    CHECK(solidar::project::ProjectFile::saveDocument(path, document, &error));
    solidar::Document restored;
    CHECK(solidar::project::ProjectFile::loadDocument(path, &restored, &error));
    CHECK(restored.recompute());
    CHECK(restored.bodies().size() == document.bodies().size());
    CHECK(restored.sketches().size() == document.sketches().size());

    const auto* restoredPart = restored.findBody(partBodyId);
    const auto* restoredRevolveBody = restored.findBody(revolveBodyId);
    CHECK(restoredPart && restoredRevolveBody);
    CHECK(restoredPart->features().size() == 4);
    CHECK(restoredRevolveBody->features().size() == 1);
    CHECK(restored.findSketch(baseSketchId));
    const auto* restoredPocketSketch = restored.findSketch(pocketSketchId);
    CHECK(restoredPocketSketch);
    CHECK(restored.findSketch(revolveSketchId));
    CHECK(restoredPocketSketch->support.type ==
          solidar::SketchSupportType::Face);
    CHECK(restoredPocketSketch->support.face.bodyId == partBodyId);
    CHECK(restoredPocketSketch->support.face.featureId == extrudeId);
    CHECK(restoredPocketSketch->support.face.persistentTag ==
          topFaceReference.persistentTag);
    CHECK(restoredPocketSketch->support.face.signature);

    const auto* restoredExtrude = dynamic_cast<const solidar::ExtrudeFeature*>(
        restoredPart->features()[0].get());
    const auto* restoredPocket = dynamic_cast<const solidar::PocketFeature*>(
        restoredPart->features()[1].get());
    const auto* restoredFillet = dynamic_cast<const solidar::FilletFeature*>(
        restoredPart->features()[2].get());
    const auto* restoredChamfer = dynamic_cast<const solidar::ChamferFeature*>(
        restoredPart->features()[3].get());
    const auto* restoredRevolve = dynamic_cast<const solidar::RevolveFeature*>(
        restoredRevolveBody->features()[0].get());
    CHECK(restoredExtrude && restoredPocket && restoredFillet &&
          restoredChamfer &&
          restoredRevolve);
    CHECK(restoredFillet->edge().signature);
    CHECK(restoredChamfer->edge().signature);
    CHECK(restoredExtrude->id() == extrudeId);
    CHECK(restoredPocket->id() == pocketId);
    CHECK(restoredFillet->id() == filletId);
    CHECK(restoredChamfer->id() == chamferId);
    CHECK(restoredRevolve->id() == revolveId);
    CHECK(solidar::test::near(restoredExtrude->lengthMm(), 60.0));
    CHECK(solidar::test::near(restoredPocket->depthMm(), 20.0));
    CHECK(solidar::test::near(restoredFillet->radiusMm(), 2.0));
    CHECK(solidar::test::near(restoredChamfer->distanceMm(), 0.75));
    CHECK(solidar::test::near(restoredRevolve->angleDeg(), 180.0));
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*restoredPart->resultShape()),
        solidar::test::volumeOf(*partBodyPtr->resultShape()), 1e-4));
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*restoredRevolveBody->resultShape()),
        solidar::test::volumeOf(*revolveBodyPtr->resultShape()), 1e-4));
    checkAllValid(restored);
    CHECK(restored.rebuildError().empty());
  } catch (const std::exception& error) {
    std::cerr << "part design regression failure: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
