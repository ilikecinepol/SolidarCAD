#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Shape.hxx>
#include <TopAbs_ShapeEnum.hxx>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <limits>
#include <memory>
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
}
