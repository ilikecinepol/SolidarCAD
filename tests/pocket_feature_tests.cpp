#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>

#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/PocketFeature.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

namespace {
bool near(double actual, double expected, double tolerance = 1e-4) {
  return std::abs(actual - expected) <= tolerance;
}

double volumeOf(const TopoDS_Shape& shape) {
  GProp_GProps properties;
  BRepGProp::VolumeProperties(shape, properties);
  return properties.Mass();
}

std::size_t solidCount(const TopoDS_Shape& shape) {
  std::size_t count = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_SOLID); explorer.More();
       explorer.Next())
    ++count;
  return count;
}

std::optional<std::size_t> topFaceIndex(const TopoDS_Shape& shape,
                                        double expectedZ) {
  for (std::size_t index = 0; index < 32; ++index) {
    const auto resolved = solidar::resolveFacePlacement(shape, index);
    if (!resolved.planar) continue;
    const auto normal = resolved.placement.normal();
    if (normal.z > 0.9 && near(resolved.placement.origin.z, expectedZ))
      return index;
  }
  return std::nullopt;
}

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
}  // namespace

int main() {
  solidar::Document document;
  auto& baseSketch = document.addSketch("Base");
  baseSketch.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
  auto& body = document.addBody("Body");
  auto extrude = std::make_unique<solidar::ExtrudeFeature>(
      baseSketch.id, 50.0, "Extrude 1");
  auto* extrudePtr = extrude.get();
  body.addFeature(std::move(extrude));
  assert(document.rebuild());
  assert(near(volumeOf(*body.resultShape()), 140000.0));

  const auto topIndex = topFaceIndex(*body.resultShape(), 50.0);
  assert(topIndex);
  auto& pocketSketch = document.addSketch("Pocket profile");
  assert(document.attachSketchToFace(
      pocketSketch.id, {body.id(), extrudePtr->id(), *topIndex}));
  const auto localA =
      toLocal(pocketSketch.placement, {20.0, 10.0, 50.0});
  const auto localB =
      toLocal(pocketSketch.placement, {40.0, 20.0, 50.0});
  pocketSketch.geometry.addRectangle(localA, localB);

  auto pocket = std::make_unique<solidar::PocketFeature>(
      pocketSketch.id, 20.0, "Pocket 1");
  auto* pocketPtr = pocket.get();
  body.addFeature(std::move(pocket));
  assert(document.rebuild());
  assert(pocketPtr->isValid());
  assert(pocketPtr->hasShape());
  assert(body.resultShape()->ShapeType() == TopAbs_SOLID);
  assert(near(volumeOf(*body.resultShape()), 136000.0));

  Bnd_Box bounds;
  BRepBndLib::Add(*body.resultShape(), bounds);
  double xMin, yMin, zMin, xMax, yMax, zMax;
  bounds.Get(xMin, yMin, zMin, xMax, yMax, zMax);
  assert(near(xMax - xMin, 80.0));
  assert(near(yMax - yMin, 35.0));
  assert(near(zMax - zMin, 50.0));

  const solidar::Document snapshot = document;
  pocketPtr->setDepthMm(30.0);
  assert(pocketPtr->isDirty());
  assert(document.rebuild());
  assert(near(volumeOf(*body.resultShape()), 134000.0));
  const auto* snapshotPocket = dynamic_cast<const solidar::PocketFeature*>(
      snapshot.activeBody()->activeFeature());
  assert(snapshotPocket);
  assert(snapshotPocket->id() == pocketPtr->id());
  assert(near(snapshotPocket->depthMm(), 20.0));
  assert(near(volumeOf(*snapshot.activeBody()->resultShape()), 136000.0));

  pocketPtr->setDepthMm(20.0);
  extrudePtr->setLengthMm(80.0);
  assert(document.rebuild());
  assert(near(pocketSketch.placement.origin.z, 80.0));
  assert(near(pocketPtr->depthMm(), 20.0));
  assert(near(volumeOf(*body.resultShape()), 220000.0));

  pocketPtr->setDepthMm(0.0);
  assert(!document.rebuild());
  assert(!pocketPtr->hasShape());
  assert(!body.resultShape());
  pocketPtr->setDepthMm(std::numeric_limits<double>::quiet_NaN());
  assert(!document.rebuild());
  pocketPtr->setDepthMm(std::numeric_limits<double>::infinity());
  assert(!document.rebuild());

  solidar::Document missingBase;
  auto& orphanSketch = missingBase.addSketch();
  orphanSketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
  auto& orphanBody = missingBase.addBody();
  auto orphanPocket = std::make_unique<solidar::PocketFeature>(
      orphanSketch.id, 10.0);
  auto* orphanPtr = orphanPocket.get();
  orphanBody.addFeature(std::move(orphanPocket));
  assert(!missingBase.rebuild());
  assert(orphanPtr->error() == "Pocket base shape is missing");

  solidar::Document missingSketch;
  auto& missingBaseSketch = missingSketch.addSketch();
  missingBaseSketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
  auto& missingBody = missingSketch.addBody();
  missingBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      missingBaseSketch.id, 10.0));
  auto missingPocket = std::make_unique<solidar::PocketFeature>(999999, 2.0);
  auto* missingPtr = missingPocket.get();
  missingBody.addFeature(std::move(missingPocket));
  assert(!missingSketch.rebuild());
  assert(!missingPtr->hasShape());

  solidar::Document unresolved = snapshot;
  auto& unresolvedProfile = unresolved.sketches().back();
  unresolvedProfile.support.face.faceIndex = 9999;
  unresolved.updateSketchPlacements();
  assert(unresolvedProfile.supportResolved);
  unresolvedProfile.support.face.persistentTag.clear();
  unresolvedProfile.support.face.signature.reset();
  unresolved.updateSketchPlacements();
  assert(!unresolvedProfile.supportResolved);
  unresolved.activeBody()->activeFeature()->setDirty();
  assert(!unresolved.rebuild());
  assert(!unresolved.activeBody()->resultShape());

  // Pocket preserves a multi-solid Extrude container, rejects a cut that
  // splits one of its solids, and recovers in place without changing IDs.
  solidar::Document multiSolidDocument;
  auto& multiBaseSketch = multiSolidDocument.addSketch("Multi-solid base");
  multiBaseSketch.geometry.addRectangle({0.0, 0.0}, {20.0, 10.0});
  multiBaseSketch.geometry.addRectangle({40.0, 0.0}, {50.0, 10.0});
  auto& multiBody = multiSolidDocument.addBody("Multi-solid body");
  multiBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      multiBaseSketch.id, 10.0, "Multi-region Extrude"));
  auto& multiPocketSketch = multiSolidDocument.addSketch("Multi-solid pocket");
  multiPocketSketch.placement.origin.z = 10.0;
  multiPocketSketch.geometry.addRectangle({2.0, 2.0}, {6.0, 6.0});
  const auto multiPocketSketchId = multiPocketSketch.id;
  auto multiPocket = std::make_unique<solidar::PocketFeature>(
      multiPocketSketchId, 5.0, "Multi-solid Pocket");
  auto* multiPocketPtr = multiPocket.get();
  const auto multiPocketId = multiPocketPtr->id();
  multiBody.addFeature(std::move(multiPocket));

  CHECK(multiSolidDocument.rebuild());
  CHECK(multiPocketPtr->state() == solidar::FeatureState::Valid);
  CHECK(multiPocketPtr->id() == multiPocketId);
  CHECK(multiPocketPtr->hasShape());
  CHECK(solidCount(*multiPocketPtr->shape()) == 2);
  CHECK(near(volumeOf(*multiPocketPtr->shape()), 2920.0));

  multiPocketSketch.geometry.clear();
  multiPocketSketch.geometry.addRectangle({9.0, -1.0}, {11.0, 11.0});
  multiPocketPtr->setDepthMm(10.0);
  CHECK(multiSolidDocument.markSketchDirty(multiPocketSketchId));
  CHECK(!multiSolidDocument.rebuild());
  CHECK(multiPocketPtr->id() == multiPocketId);
  CHECK(multiPocketPtr->state() == solidar::FeatureState::Error);
  CHECK(multiPocketPtr->error().find("split") != std::string::npos);
  CHECK(!multiPocketPtr->hasShape());
  CHECK(!multiBody.resultShape());

  multiPocketSketch.geometry.clear();
  multiPocketSketch.geometry.addRectangle({2.0, 2.0}, {6.0, 6.0});
  multiPocketPtr->setDepthMm(5.0);
  CHECK(multiSolidDocument.markSketchDirty(multiPocketSketchId));
  CHECK(multiSolidDocument.rebuild());
  CHECK(multiPocketPtr->id() == multiPocketId);
  CHECK(multiPocketPtr->state() == solidar::FeatureState::Valid);
  CHECK(multiPocketPtr->hasShape());
  CHECK(solidCount(*multiPocketPtr->shape()) == 2);
  CHECK(near(volumeOf(*multiPocketPtr->shape()), 2920.0));
}
