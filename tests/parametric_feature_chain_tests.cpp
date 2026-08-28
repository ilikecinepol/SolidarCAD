#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <memory>
#include <optional>
#include <string>

#include "TestGeometryUtils.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletBuilder.h"
#include "model/FilletFeature.h"
#include "model/PocketFeature.h"

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
}
