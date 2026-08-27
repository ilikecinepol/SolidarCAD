#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <memory>
#include <numbers>

#include "TestGeometryUtils.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"

namespace {

struct BaseFixture {
  solidar::Document document;
  solidar::Body* body{};
  solidar::FeatureId baseFeatureId{};

  BaseFixture() {
    auto& sketch = document.addSketch("Base");
    sketch.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
    body = &document.addBody();
    auto feature =
        std::make_unique<solidar::ExtrudeFeature>(sketch.id, 50.0, "Base");
    baseFeatureId = feature->id();
    body->addFeature(std::move(feature));
    assert(document.rebuild());
  }

  solidar::DocumentSketch& rectangleOnTop(double x = -25.0,
                                           double y = 5.0) {
    auto& sketch = document.addSketch("Rectangle on top");
    sketch.geometry.addRectangle({x, y}, {x + 10.0, y + 10.0});
    const auto face = solidar::test::topPlanarFace(*body->resultShape(), 50.0);
    assert(face);
    assert(document.attachSketchToFace(
        sketch.id, {body->id(), body->activeFeature()->id(), *face}));
    return sketch;
  }

  solidar::DocumentSketch& circleOnTop(double x = 10.0, double y = 10.0) {
    auto& sketch = document.addSketch("Circle on top");
    sketch.geometry.addCircle({x, y}, 5.0);
    const auto face = solidar::test::topPlanarFace(*body->resultShape(), 50.0);
    assert(face);
    assert(document.attachSketchToFace(
        sketch.id, {body->id(), body->activeFeature()->id(), *face}));
    return sketch;
  }
};

void assertResult(const BaseFixture& fixture, double expectedVolume) {
  assert(fixture.body->activeFeature()->isValid());
  assert(fixture.document.bodies().size() == 1);
  assert(solidar::test::solidCount(*fixture.body->resultShape()) == 1);
  const double actualVolume =
      solidar::test::volumeOf(*fixture.body->resultShape());
  if (!solidar::test::near(actualVolume, expectedVolume, 1e-2))
    std::cerr << "Expected volume " << expectedVolume << ", actual volume "
              << actualVolume << ", feature error: "
              << fixture.body->activeFeature()->error() << '\n';
  assert(solidar::test::near(actualVolume, expectedVolume, 1e-2));
}

}  // namespace

int main() {
  {
    BaseFixture fixture;
    auto& sketch = fixture.rectangleOnTop();
    fixture.body->addFeature(std::make_unique<solidar::ExtrudeFeature>(
        sketch.id, 20.0, "Rectangle Join", solidar::ExtrudeOperation::Join));
    assert(fixture.document.rebuild());
    assertResult(fixture, 142000.0);
  }
  {
    BaseFixture fixture;
    auto& sketch = fixture.circleOnTop();
    fixture.body->addFeature(std::make_unique<solidar::ExtrudeFeature>(
        sketch.id, 20.0, "Circle Join", solidar::ExtrudeOperation::Join));
    assert(fixture.document.rebuild());
    assertResult(fixture, 140000.0 + std::numbers::pi * 25.0 * 20.0);
  }
  {
    BaseFixture fixture;
    auto& sketch = fixture.rectangleOnTop();
    fixture.body->addFeature(std::make_unique<solidar::ExtrudeFeature>(
        sketch.id, 20.0, "Rectangle Cut", solidar::ExtrudeOperation::Cut,
        true));
    assert(fixture.document.rebuild());
    assertResult(fixture, 138000.0);
  }
  {
    BaseFixture fixture;
    auto& sketch = fixture.circleOnTop();
    fixture.body->addFeature(std::make_unique<solidar::ExtrudeFeature>(
        sketch.id, 20.0, "Circle Cut", solidar::ExtrudeOperation::Cut, true));
    assert(fixture.document.rebuild());
    assertResult(fixture, 140000.0 - std::numbers::pi * 25.0 * 20.0);
  }
  {
    BaseFixture fixture;
    auto& sketch = fixture.rectangleOnTop();
    fixture.body->addFeature(std::make_unique<solidar::ExtrudeFeature>(
        sketch.id, 100.0, "Through Cut", solidar::ExtrudeOperation::Cut,
        true));
    assert(fixture.document.rebuild());
    assertResult(fixture, 135000.0);
  }
  {
    BaseFixture fixture;
    auto& sketch = fixture.document.addSketch("Remote Join");
    sketch.geometry.addRectangle({200.0, 200.0}, {210.0, 210.0});
    auto feature = std::make_unique<solidar::ExtrudeFeature>(
        sketch.id, 20.0, "Remote Join", solidar::ExtrudeOperation::Join);
    auto* extrude = feature.get();
    fixture.body->addFeature(std::move(feature));
    assert(!fixture.document.rebuild());
    assert(extrude->state() == solidar::FeatureState::Error);
    assert(extrude->error().find("does not intersect") != std::string::npos);
    assert(!fixture.body->resultShape());
  }
  {
    BaseFixture fixture;
    auto& sketch = fixture.document.addSketch("Remote Cut");
    sketch.geometry.addCircle({200.0, 200.0}, 5.0);
    auto feature = std::make_unique<solidar::ExtrudeFeature>(
        sketch.id, 20.0, "Remote Cut", solidar::ExtrudeOperation::Cut);
    auto* extrude = feature.get();
    fixture.body->addFeature(std::move(feature));
    assert(!fixture.document.rebuild());
    assert(extrude->state() == solidar::FeatureState::Error);
    assert(extrude->error().find("does not intersect") != std::string::npos);
    assert(!fixture.body->resultShape());
  }
  {
    BaseFixture fixture;
    auto& sketch = fixture.rectangleOnTop();
    auto feature = std::make_unique<solidar::ExtrudeFeature>(
        sketch.id, 20.0, "Editable", solidar::ExtrudeOperation::Join);
    auto* extrude = feature.get();
    fixture.body->addFeature(std::move(feature));
    assert(fixture.document.rebuild());
    assertResult(fixture, 142000.0);
    extrude->setOperation(solidar::ExtrudeOperation::Cut);
    extrude->setReversed(true);
    assert(extrude->isDirty() && fixture.document.rebuild());
    assertResult(fixture, 138000.0);
  }
  {
    BaseFixture fixture;
    auto& sketch = fixture.rectangleOnTop();
    auto feature = std::make_unique<solidar::ExtrudeFeature>(
        sketch.id, 20.0, "Reverse Join", solidar::ExtrudeOperation::Join,
        true);
    auto* extrude = feature.get();
    fixture.body->addFeature(std::move(feature));
    assert(!fixture.document.rebuild());
    assert(extrude->state() == solidar::FeatureState::Error);
  }
}
