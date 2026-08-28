#include <TopoDS_Shape.hxx>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <limits>
#include <memory>

#include "model/Document.h"
#include "model/RevolveFeature.h"

namespace {
std::unique_ptr<solidar::RevolveFeature> feature(
    solidar::SketchId id, solidar::AxisReferenceType type,
    double angle = 360.0, bool reversed = false,
    solidar::sketch::GeometryId line = solidar::sketch::kInvalidGeometryId) {
  return std::make_unique<solidar::RevolveFeature>(
      id, solidar::AxisReference{type, id, line}, angle, "Revolve 1",
      solidar::ExtrudeOperation::NewBody, reversed);
}
}

int main() {
  for (const double angle : {360.0, 180.0}) {
    solidar::Document document;
    auto& sketch = document.addSketch();
    sketch.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
    auto& body = document.addBody();
    body.addFeature(feature(sketch.id,
                            solidar::AxisReferenceType::SketchHorizontalAxis,
                            angle));
    assert(document.recompute());
    assert(body.activeFeature()->isValid());
    assert(body.resultShape() && !body.resultShape()->IsNull());
  }

  solidar::Document editable;
  auto& profile = editable.addSketch();
  profile.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
  auto& body = editable.addBody();
  auto revolve = feature(profile.id,
      solidar::AxisReferenceType::SketchHorizontalAxis, 270.0, true);
  auto* ptr = revolve.get();
  body.addFeature(std::move(revolve));
  assert(editable.recompute());
  ptr->setAngleDeg(180.0);
  assert(ptr->isDirty() && editable.recompute() && ptr->isValid());
  auto changed = profile.geometry;
  changed.clear();
  changed.addRectangle({12.0, 5.0}, {35.0, 18.0});
  assert(editable.replaceSketchGeometry(profile.id, std::move(changed)));
  assert(ptr->isDirty() && editable.recompute());

  for (const double bad : {0.0, -1.0, 361.0,
                           std::numeric_limits<double>::infinity()}) {
    ptr->setAngleDeg(bad);
    assert(!editable.recompute());
    assert(ptr->isFailed() && !ptr->hasShape());
  }
  ptr->setAngleDeg(360.0);
  assert(editable.recompute());

  solidar::Document vertical;
  auto& verticalProfile = vertical.addSketch();
  verticalProfile.geometry.addRectangle({5.0, 10.0}, {15.0, 30.0});
  auto& verticalBody = vertical.addBody();
  verticalBody.addFeature(feature(verticalProfile.id,
      solidar::AxisReferenceType::SketchVerticalAxis));
  assert(vertical.recompute());

  solidar::Document lineAxis;
  auto& lineProfile = lineAxis.addSketch();
  lineProfile.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
  lineProfile.geometry.addLine({0.0, 0.0}, {40.0, 0.0});
  const auto axisLineId = lineProfile.geometry.lineId(4);
  lineProfile.geometry.setElementDashed(
      lineProfile.geometry.lines()[4].elementId, true);
  auto& lineBody = lineAxis.addBody();
  lineBody.addFeature(feature(lineProfile.id,
      solidar::AxisReferenceType::SketchLine, 360.0, false, axisLineId));
  assert(lineAxis.recompute());

  solidar::Document invalid;
  auto& invalidBody = invalid.addBody();
  invalidBody.addFeature(feature(9999,
      solidar::AxisReferenceType::SketchHorizontalAxis));
  assert(!invalid.recompute());

  solidar::Document open;
  auto& openSketch = open.addSketch();
  openSketch.geometry.addLine({1.0, 1.0}, {2.0, 1.0});
  openSketch.geometry.addLine({2.0, 1.0}, {2.0, 2.0});
  auto& openBody = open.addBody();
  openBody.addFeature(feature(openSketch.id,
      solidar::AxisReferenceType::SketchHorizontalAxis));
  assert(!open.recompute());
}
