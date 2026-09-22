#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "TestGeometryUtils.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletBuilder.h"
#include "model/FilletFeature.h"
#include "model/PocketFeature.h"
#include "model/TopologyReferenceResolver.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

namespace {

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

std::optional<std::size_t> filletableEdge(const TopoDS_Shape& shape) {
  for (std::size_t index = 0; index < 64; ++index) {
    std::string error;
    if (solidar::buildFilletShape(shape, {index}, 2.0, &error)) return index;
    if (error == "Fillet edge could not be resolved") break;
  }
  return std::nullopt;
}

}  // namespace

int main() {
  solidar::Document document;
  auto& baseSketch = document.addSketch("Base profile");
  const auto baseSketchId = baseSketch.id;
  baseSketch.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});

  auto& body = document.addBody("Parametric body");
  const auto bodyId = body.id();
  auto extrude = std::make_unique<solidar::ExtrudeFeature>(
      baseSketchId, 50.0, "Extrude");
  auto* extrudePtr = extrude.get();
  body.addFeature(std::move(extrude));
  assert(document.recompute());

  const auto topFace = solidar::test::topPlanarFace(*body.resultShape(), 50.0);
  assert(topFace);
  auto& pocketSketch = document.addSketch("Sketch on face");
  const auto pocketSketchId = pocketSketch.id;
  assert(document.attachSketchToFace(
      pocketSketchId, {bodyId, extrudePtr->id(), *topFace}));
  const auto localA = toLocal(pocketSketch.placement, {20.0, 10.0, 50.0});
  const auto localB = toLocal(pocketSketch.placement, {40.0, 20.0, 50.0});
  pocketSketch.geometry.addRectangle(localA, localB);

  auto pocket = std::make_unique<solidar::PocketFeature>(
      pocketSketchId, 20.0, "Pocket");
  auto* pocketPtr = pocket.get();
  body.addFeature(std::move(pocket));
  assert(document.recompute());
  const auto edgeIndex = filletableEdge(*body.resultShape());
  assert(edgeIndex);

  auto fillet = std::make_unique<solidar::FilletFeature>(
      solidar::EdgeReference{bodyId, pocketPtr->id(), *edgeIndex}, 2.0,
      "Fillet");
  auto* filletPtr = fillet.get();
  body.addFeature(std::move(fillet));
  assert(document.recompute());
  assert(body.features().size() == 3);
  assert(extrudePtr->isValid() && pocketPtr->isValid() && filletPtr->isValid());
  const double initialVolume = solidar::test::volumeOf(*body.resultShape());

  // Editing the root sketch dirties and rebuilds every downstream feature.
  solidar::sketch::Sketch widerProfile;
  widerProfile.addRectangle({0.0, 0.0}, {100.0, 35.0});
  assert(document.replaceSketchGeometry(baseSketchId, widerProfile));
  assert(extrudePtr->isDirty());
  assert(pocketPtr->isDirty());
  assert(filletPtr->isDirty());
  const bool rebuilt = document.recompute();
  assert(extrudePtr->isValid());
  assert(pocketPtr->isValid());
  assert(pocketSketch.supportResolved);
  if (rebuilt) {
    assert(filletPtr->isValid());
    assert(solidar::test::volumeOf(*body.resultShape()) > initialVolume);
  } else {
    // A legacy subshape index is allowed to become invalid, but it must fail
    // explicitly and leave the rebuilt upstream history usable.
    assert(filletPtr->isFailed());
    assert(!filletPtr->error().empty());
    assert(pocketPtr->shape());
  }

  // A curved root profile must preserve the complete attached Pocket chain
  // when only its Arc changes and the Document propagates dirtiness.
  {
    constexpr double kPi = 3.14159265358979323846;
    solidar::Document curvedDocument;

    auto& curvedBase = curvedDocument.addSketch("Line and Arc base");
    const auto curvedBaseId = curvedBase.id;
    curvedBase.geometry.addLine({-10.0, 0.0}, {10.0, 0.0});
    curvedBase.geometry.addArc({0.0, 0.0}, 10.0, 0.0, kPi);
    CHECK(curvedBase.geometry.arcs().size() == 1);
    CHECK(curvedBase.geometry.isClosed());

    auto& curvedBody = curvedDocument.addBody("Curved downstream body");
    const auto curvedBodyId = curvedBody.id();
    auto curvedExtrude = std::make_unique<solidar::ExtrudeFeature>(
        curvedBaseId, 25.0, "Curved Extrude");
    auto* curvedExtrudePtr = curvedExtrude.get();
    const auto curvedExtrudeId = curvedExtrudePtr->id();
    curvedBody.addFeature(std::move(curvedExtrude));
    CHECK(curvedDocument.recompute());

    const auto curvedTopFace =
        solidar::test::topPlanarFace(*curvedBody.resultShape(), 25.0);
    CHECK(curvedTopFace);
    const auto curvedTopReference = solidar::makeFaceReference(
        *curvedBody.resultShape(), curvedBodyId, curvedExtrudeId,
        *curvedTopFace);
    CHECK(curvedTopReference.signature);
    CHECK(!curvedTopReference.persistentTag.empty());

    auto& curvedPocketSketch =
        curvedDocument.addSketch("Pocket on curved Extrude");
    const auto curvedPocketSketchId = curvedPocketSketch.id;
    CHECK(curvedDocument.attachSketchToFace(curvedPocketSketchId,
                                            curvedTopReference));
    const auto pocketCenter =
        toLocal(curvedPocketSketch.placement, {0.0, 2.0, 25.0});
    curvedPocketSketch.geometry.addCircle(pocketCenter, 1.0);

    auto curvedPocket = std::make_unique<solidar::PocketFeature>(
        curvedPocketSketchId, 5.0, "Pocket after curved Extrude");
    auto* curvedPocketPtr = curvedPocket.get();
    const auto curvedPocketId = curvedPocketPtr->id();
    curvedBody.addFeature(std::move(curvedPocket));

    CHECK(curvedDocument.recompute());
    CHECK(curvedDocument.findSketch(curvedBaseId));
    CHECK(curvedExtrudePtr->isValid());
    CHECK(curvedPocketPtr->isValid());
    CHECK(curvedBody.resultShape());
    CHECK(!curvedBody.resultShape()->IsNull());
    CHECK(solidar::test::solidCount(*curvedBody.resultShape()) == 1);
    const auto finalShapeBefore = curvedBody.resultShape();
    const auto extrudeShapeBefore = curvedExtrudePtr->shape();
    const double volumeBefore =
        solidar::test::volumeOf(*curvedBody.resultShape());
    CHECK(volumeBefore > 0.0);

    auto* editedBase = curvedDocument.findSketch(curvedBaseId);
    CHECK(editedBase);
    editedBase->geometry.removeArc(0);
    const double centerOffset = std::sqrt(12.0 * 12.0 - 10.0 * 10.0);
    const double startAngle = std::atan2(centerOffset, 10.0);
    editedBase->geometry.addArc({0.0, -centerOffset}, 12.0, startAngle,
                                kPi - 2.0 * startAngle);
    CHECK(editedBase->geometry.arcs().size() == 1);
    CHECK(editedBase->geometry.isClosed());
    CHECK(curvedDocument.markSketchDirty(curvedBaseId));
    CHECK(curvedExtrudePtr->isDirty());
    CHECK(curvedPocketPtr->isDirty());

    CHECK(curvedDocument.recompute());
    CHECK(curvedExtrudePtr->id() == curvedExtrudeId);
    CHECK(curvedPocketPtr->id() == curvedPocketId);
    CHECK(curvedExtrudePtr->isValid());
    CHECK(curvedPocketPtr->isValid());
    CHECK(curvedExtrudePtr->shape());
    CHECK(curvedExtrudePtr->shape().get() != extrudeShapeBefore.get());
    CHECK(curvedBody.resultShape());
    CHECK(!curvedBody.resultShape()->IsNull());
    CHECK(curvedBody.resultShape().get() != finalShapeBefore.get());
    CHECK(solidar::test::solidCount(*curvedBody.resultShape()) == 1);
    const auto* attachedSketch =
        curvedDocument.findSketch(curvedPocketSketchId);
    CHECK(attachedSketch);
    CHECK(attachedSketch->support.type == solidar::SketchSupportType::Face);
    CHECK(attachedSketch->support.face.bodyId == curvedBodyId);
    CHECK(attachedSketch->support.face.featureId == curvedExtrudeId);
    CHECK(attachedSketch->support.face.persistentTag ==
          curvedTopReference.persistentTag);
    CHECK(attachedSketch->support.face.signature);
    CHECK(attachedSketch->supportResolved);
    CHECK(std::abs(attachedSketch->placement.origin.z - 25.0) < 1e-6);
    const double volumeAfter =
        solidar::test::volumeOf(*curvedBody.resultShape());
    CHECK(std::abs(volumeAfter - volumeBefore) > 1e-4);
  }

  return EXIT_SUCCESS;
}
