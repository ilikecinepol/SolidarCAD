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
