#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS_Shape.hxx>
#include <TopAbs_ShapeEnum.hxx>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>

#include "model/Document.h"
#include "model/ExtrudeFeature.h"

namespace {

struct Extents {
  double x{};
  double y{};
  double z{};
};

Extents extentsOf(const TopoDS_Shape& shape) {
  Bnd_Box bounds;
  BRepBndLib::Add(shape, bounds);
  double xMin = 0.0;
  double yMin = 0.0;
  double zMin = 0.0;
  double xMax = 0.0;
  double yMax = 0.0;
  double zMax = 0.0;
  bounds.Get(xMin, yMin, zMin, xMax, yMax, zMax);
  return {xMax - xMin, yMax - yMin, zMax - zMin};
}

bool near(double actual, double expected) {
  return std::abs(actual - expected) <= 1e-5;
}

double volumeOf(const TopoDS_Shape& shape) {
  GProp_GProps properties;
  BRepGProp::VolumeProperties(shape, properties);
  return properties.Mass();
}

std::size_t topFaceOf(const TopoDS_Shape& shape, double z) {
  for (std::size_t index = 0; index < 64; ++index) {
    const auto resolved = solidar::resolveFacePlacement(shape, index);
    if (resolved.planar && resolved.placement.normal().z > 0.9 &&
        near(resolved.placement.origin.z, z))
      return index;
  }
  assert(false && "Top face was not found");
  return 0;
}

}  // namespace

int main() {
  const auto xy = solidar::SketchPlacement::xy();
  const auto xyPoint = xy.toWorld(10.0, 20.0);
  assert(near(xyPoint.x, 10.0));
  assert(near(xyPoint.y, 20.0));
  assert(near(xyPoint.z, 0.0));

  solidar::Document document;
  auto& sketch = document.addSketch("Rectangle");
  sketch.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
  const auto sketchId = sketch.id;

  auto& body = document.addBody();
  auto extrude = std::make_unique<solidar::ExtrudeFeature>(
      sketchId, 50.0, "Extrude 1");
  auto* extrudePtr = extrude.get();
  body.addFeature(std::move(extrude));

  assert(document.rebuild());
  assert(extrudePtr->isValid());
  assert(extrudePtr->hasShape());
  assert(body.resultShape());
  assert(!body.resultShape()->IsNull());
  assert(body.resultShape()->ShapeType() == TopAbs_SOLID);
  auto bounds = extentsOf(*body.resultShape());
  assert(near(bounds.x, 80.0));
  assert(near(bounds.y, 35.0));
  assert(near(bounds.z, 50.0));

  // BoxParameters is compatibility/UI data, never the source of Body geometry.
  document.setBox({999.0, 777.0, 333.0});
  assert(document.rebuild());
  bounds = extentsOf(*body.resultShape());
  assert(near(bounds.x, 80.0));
  assert(near(bounds.y, 35.0));
  assert(near(bounds.z, 50.0));

  const solidar::Document snapshot = document;
  extrudePtr->setLengthMm(80.0);
  assert(extrudePtr->isDirty());
  assert(document.rebuild());
  bounds = extentsOf(*body.resultShape());
  assert(near(bounds.z, 80.0));
  const auto* snapshotExtrude = dynamic_cast<const solidar::ExtrudeFeature*>(
      snapshot.activeBody()->activeFeature());
  assert(snapshotExtrude);
  assert(snapshotExtrude->id() == extrudePtr->id());
  assert(near(snapshotExtrude->lengthMm(), 50.0));
  assert(near(extentsOf(*snapshot.activeBody()->resultShape()).z, 50.0));

  // A failed rebuild must clear the old successful result, not leave stale 3D.
  extrudePtr->setLengthMm(0.0);
  assert(!document.rebuild());
  assert(!extrudePtr->hasShape());
  assert(!body.resultShape());

  solidar::Document xzDocument;
  auto& xzSketch = xzDocument.addSketch("XZ Rectangle");
  xzSketch.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
  xzSketch.placement = solidar::SketchPlacement::xz();
  auto& xzBody = xzDocument.addBody();
  xzBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      xzSketch.id, 50.0, "XZ Extrude"));
  assert(xzDocument.rebuild());
  const auto xzBounds = extentsOf(*xzBody.resultShape());
  assert(near(xzBounds.x, 80.0));
  assert(near(xzBounds.y, 50.0));
  assert(near(xzBounds.z, 35.0));

  solidar::Document attachedDocument;
  auto& baseSketch = attachedDocument.addSketch("Base");
  baseSketch.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
  auto& attachedBody = attachedDocument.addBody();
  auto baseExtrude = std::make_unique<solidar::ExtrudeFeature>(
      baseSketch.id, 50.0, "Base Extrude");
  auto* baseExtrudePtr = baseExtrude.get();
  attachedBody.addFeature(std::move(baseExtrude));
  assert(attachedDocument.rebuild());

  std::optional<std::size_t> topFaceIndex;
  for (std::size_t index = 0; index < 32; ++index) {
    const auto resolved =
        solidar::resolveFacePlacement(*attachedBody.resultShape(), index);
    if (!resolved.planar) continue;
    const auto normal = resolved.placement.normal();
    if (normal.z > 0.9 && near(resolved.placement.origin.z, 50.0)) {
      topFaceIndex = index;
      break;
    }
  }
  assert(topFaceIndex);
  auto& faceSketch = attachedDocument.addSketch("Sketch on top face");
  faceSketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
  assert(attachedDocument.attachSketchToFace(
      faceSketch.id,
      {attachedBody.id(), baseExtrudePtr->id(), *topFaceIndex}));
  assert(faceSketch.supportResolved);
  assert(near(faceSketch.placement.origin.z, 50.0));
  const auto faceOrigin = faceSketch.placement.toWorld(0.0, 0.0);
  const auto faceX = faceSketch.placement.toWorld(10.0, 0.0);
  const auto faceY = faceSketch.placement.toWorld(0.0, 10.0);
  const auto faceNormal = faceSketch.placement.normal();
  assert(near(faceOrigin.z, 50.0));
  assert(near(faceX.z, 50.0));
  assert(near(faceY.z, 50.0));
  assert(near(faceNormal.x, 0.0));
  assert(near(faceNormal.y, 0.0));
  assert(near(faceNormal.z, 1.0));
  assert(near(std::hypot(faceX.x - faceOrigin.x,
                         faceX.y - faceOrigin.y),
              10.0));
  assert(near(std::hypot(faceY.x - faceOrigin.x,
                         faceY.y - faceOrigin.y),
              10.0));

  const solidar::Document attachedSnapshot = attachedDocument;
  baseExtrudePtr->setLengthMm(80.0);
  assert(attachedDocument.rebuild());
  assert(faceSketch.supportResolved);
  assert(near(faceSketch.placement.origin.z, 80.0));
  const auto& snapshotFaceSketch = attachedSnapshot.sketches().back();
  assert(snapshotFaceSketch.support.type == solidar::SketchSupportType::Face);
  assert(near(snapshotFaceSketch.placement.origin.z, 50.0));

  faceSketch.support.face.faceIndex = 9999;
  attachedDocument.updateSketchPlacements();
  assert(!faceSketch.supportResolved);
  assert(!faceSketch.geometry.lines().empty());

  solidar::Document openProfile;
  auto& openSketch = openProfile.addSketch();
  openSketch.geometry.addLine({0.0, 0.0}, {10.0, 0.0});
  openSketch.geometry.addLine({10.0, 0.0}, {10.0, 10.0});
  auto& openBody = openProfile.addBody();
  auto openExtrude = std::make_unique<solidar::ExtrudeFeature>(
      openSketch.id, 10.0);
  auto* openPtr = openExtrude.get();
  openBody.addFeature(std::move(openExtrude));
  assert(!openProfile.rebuild());
  assert(openPtr->state() == solidar::FeatureState::Error);
  assert(!openPtr->hasShape());

  solidar::Document invalidLength;
  auto& validSketch = invalidLength.addSketch();
  validSketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
  auto& invalidBody = invalidLength.addBody();
  auto zero = std::make_unique<solidar::ExtrudeFeature>(validSketch.id, 0.0);
  auto* zeroPtr = zero.get();
  invalidBody.addFeature(std::move(zero));
  assert(!invalidLength.rebuild());
  assert(zeroPtr->state() == solidar::FeatureState::Error);
  assert(!zeroPtr->hasShape());

  zeroPtr->setLengthMm(std::numeric_limits<double>::infinity());
  assert(!invalidLength.rebuild());
  zeroPtr->setLengthMm(std::numeric_limits<double>::quiet_NaN());
  assert(!invalidLength.rebuild());

  solidar::Document missingProfile;
  auto& missingBody = missingProfile.addBody();
  auto missing = std::make_unique<solidar::ExtrudeFeature>(999999, 10.0);
  auto* missingPtr = missing.get();
  missingBody.addFeature(std::move(missing));
  assert(!missingProfile.rebuild());
  assert(missingPtr->state() == solidar::FeatureState::Error);
  assert(!missingPtr->hasShape());

  // A single Circle is a first-class profile on any Sketch placement.
  solidar::Document circleDocument;
  auto& circleSketch = circleDocument.addSketch("Circle");
  circleSketch.geometry.addCircle({0.0, 0.0}, 10.0);
  auto& circleBody = circleDocument.addBody();
  circleBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      circleSketch.id, 50.0, "Cylinder"));
  assert(circleDocument.rebuild());
  assert(std::abs(volumeOf(*circleBody.resultShape()) -
                  std::numbers::pi * 100.0 * 50.0) < 1e-3);

  solidar::Document xzCircleDocument;
  auto& xzCircle = xzCircleDocument.addSketch("XZ Circle");
  xzCircle.geometry.addCircle({0.0, 0.0}, 10.0);
  xzCircle.placement = solidar::SketchPlacement::xz();
  auto& xzCircleBody = xzCircleDocument.addBody();
  xzCircleBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      xzCircle.id, 50.0, "XZ Cylinder"));
  assert(xzCircleDocument.rebuild());
  const auto xzCylinderBounds = extentsOf(*xzCircleBody.resultShape());
  assert(near(xzCylinderBounds.x, 20.0));
  assert(near(xzCylinderBounds.y, 50.0));
  assert(near(xzCylinderBounds.z, 20.0));

  // Circle Join and Cut share the same prism builder and preserve one Body.
  solidar::Document booleanDocument;
  auto& booleanBase = booleanDocument.addSketch("Boolean base");
  booleanBase.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
  auto& booleanBody = booleanDocument.addBody();
  auto booleanBaseFeature = std::make_unique<solidar::ExtrudeFeature>(
      booleanBase.id, 50.0, "Base");
  const auto booleanBaseFeatureId = booleanBaseFeature->id();
  booleanBody.addFeature(std::move(booleanBaseFeature));
  assert(booleanDocument.rebuild());
  auto& joinCircle = booleanDocument.addSketch("Join circle");
  const auto joinCircleId = joinCircle.id;
  joinCircle.geometry.addCircle({10.0, 10.0}, 5.0);
  assert(booleanDocument.attachSketchToFace(
      joinCircle.id, {booleanBody.id(), booleanBaseFeatureId,
                      topFaceOf(*booleanBody.resultShape(), 50.0)}));
  booleanBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      joinCircle.id, 20.0, "Circle Join", solidar::ExtrudeOperation::Join));
  assert(booleanDocument.rebuild());
  assert(booleanDocument.bodies().size() == 1);
  assert(std::abs(volumeOf(*booleanBody.resultShape()) -
                  (140000.0 + std::numbers::pi * 25.0 * 20.0)) < 1e-2);

  auto& cutCircle = booleanDocument.addSketch("Cut circle");
  cutCircle.geometry.addCircle({25.0, 10.0}, 5.0);
  const auto* joinFeature = booleanBody.activeFeature();
  assert(booleanDocument.attachSketchToFace(
      cutCircle.id, {booleanBody.id(), joinFeature->id(),
                     topFaceOf(*booleanBody.resultShape(), 50.0)}));
  booleanBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      cutCircle.id, 20.0, "Circle Cut", solidar::ExtrudeOperation::Cut, true));
  assert(booleanDocument.rebuild());
  assert(booleanDocument.bodies().size() == 1);
  assert(std::abs(volumeOf(*booleanBody.resultShape()) - 140000.0) < 1e-2);

  auto* mutableBase = dynamic_cast<solidar::ExtrudeFeature*>(
      booleanBody.features().front().get());
  assert(mutableBase);
  mutableBase->setLengthMm(80.0);
  assert(booleanDocument.rebuild());
  assert(near(booleanDocument.findSketch(joinCircleId)->placement.origin.z, 80.0));
  assert(std::abs(volumeOf(*booleanBody.resultShape()) - 224000.0) < 1e-2);

  // New Body is independent from the active Body and becomes the new active Body.
  auto& secondProfile = booleanDocument.addSketch("Second body circle");
  secondProfile.geometry.addCircle({120.0, 0.0}, 7.0);
  auto& secondBody = booleanDocument.addBody();
  secondBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      secondProfile.id, 15.0, "Second Body"));
  assert(booleanDocument.rebuild());
  assert(booleanDocument.bodies().size() == 2);
  assert(booleanDocument.activeBody() == &booleanDocument.bodies().back());
  assert(booleanDocument.bodies()[0].resultShape());
  assert(booleanDocument.bodies()[1].resultShape());

  solidar::Document noIntersection;
  auto& noIntersectionBase = noIntersection.addSketch("Base");
  noIntersectionBase.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
  auto& noIntersectionBody = noIntersection.addBody();
  noIntersectionBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      noIntersectionBase.id, 10.0));
  auto& remote = noIntersection.addSketch("Remote");
  remote.geometry.addCircle({100.0, 100.0}, 5.0);
  noIntersectionBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      remote.id, 10.0, "Remote Join", solidar::ExtrudeOperation::Join));
  assert(!noIntersection.rebuild());
  assert(noIntersectionBody.activeFeature()->error().find("does not intersect") !=
         std::string::npos);

  solidar::Document mixedDocument;
  auto& mixed = mixedDocument.addSketch("Mixed");
  mixed.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
  mixed.geometry.addCircle({10.0, 10.0}, 3.0);
  auto& mixedBody = mixedDocument.addBody();
  mixedBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(mixed.id, 10.0));
  assert(!mixedDocument.rebuild());
  assert(mixedBody.activeFeature()->error().find("one profile") != std::string::npos);

  solidar::Document invalidCircleDocument;
  auto& invalidCircle = invalidCircleDocument.addSketch("Invalid circle");
  invalidCircle.geometry.addCircle({0.0, 0.0},
                                   std::numeric_limits<double>::quiet_NaN());
  auto& invalidCircleBody = invalidCircleDocument.addBody();
  invalidCircleBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      invalidCircle.id, 10.0));
  assert(!invalidCircleDocument.rebuild());
}
