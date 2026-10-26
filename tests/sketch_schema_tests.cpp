#include "TestAssertions.h"

#include "sketch/Sketch.h"
#include "sketch/SketchSchema.h"

#include <type_traits>
#include <algorithm>
#include <variant>
#include <vector>

using namespace solidar::sketch;

static_assert(!std::is_aggregate_v<PointReference>);
static_assert(std::is_constructible_v<PointReference,
                                      LineEndpointReference>);
static_assert(std::is_constructible_v<PointReference,
                                      CircleCenterReference>);
static_assert(!std::is_constructible_v<PointReference, GeometryId, bool,
                                       GeometryId>);
static_assert(std::variant_size_v<PointReference::Payload> == 6);
static_assert(std::variant_size_v<ConstraintPayload> ==
              static_cast<std::size_t>(ConstraintType::Count) + 1);

namespace {

void taggedPointReferenceHasExactlyOneSource() {
  PointReference point{LineEndpointReference{17, Endpoint::End}};
  CHECK(std::holds_alternative<LineEndpointReference>(point.payload()));
  point.circleId = 29;
  CHECK(std::holds_alternative<CircleCenterReference>(point.payload()));
  CHECK(point.lineId == kInvalidGeometryId);
  CHECK(point.circleId == 29);
}

void completeConstraintSchemaMatrix() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {10.0, 0.0});
  sketch.addLine({0.0, 5.0}, {10.0, 5.0});
  sketch.addLine({0.0, 0.0}, {0.0, 10.0});
  sketch.addCircle({20.0, 0.0}, 4.0);
  sketch.addArc({30.0, 0.0}, 5.0, 0.0, 1.5);
  const auto l0 = sketch.lineId(0);
  const auto l1 = sketch.lineId(1);
  const auto l2 = sketch.lineId(2);
  const auto circle = sketch.circleId(0);
  const auto arc = sketch.arcId(0);
  const PointReference p0{LineEndpointReference{l0, Endpoint::Start}};
  const PointReference p1{LineEndpointReference{l1, Endpoint::Start}};

  std::vector<Constraint> valid{
      Constraint{HorizontalConstraint{l0}},
      Constraint{VerticalConstraint{l2}},
      Constraint{CoincidentConstraint{p0, p1}},
      Constraint{PointOnLineConstraint{l0, p1}},
      Constraint{DistanceConstraint{p0, p1, 5.0}},
      Constraint{LengthConstraint{l0, 10.0}},
      Constraint{RadiusConstraint{circle, 4.0}},
      Constraint{DiameterConstraint{circle, 8.0}},
      Constraint{ParallelConstraint{l0, l1}},
      Constraint{PerpendicularConstraint{l0, l2}},
      Constraint{EqualConstraint{l0, l1}},
      Constraint{AngleConstraint{l0, l2, 90.0}},
      Constraint{DistanceXConstraint{p0, p1, 1.0}},
      Constraint{DistanceYConstraint{p0, p1, 5.0}},
      Constraint{PointOnCircleConstraint{circle, p0}},
      Constraint{TangentConstraint{l0, circle}},
      Constraint{LineDistanceConstraint{l0, l1, 5.0}},
      Constraint{LockConstraint{l0}},
      Constraint{PointOnArcConstraint{arc, p0}},
      Constraint{MidpointConstraint{l0, p1}},
      Constraint{PointOnXAxisConstraint{p0}},
      Constraint{PointOnYAxisConstraint{p1}},
  };
  CHECK(valid.size() == static_cast<std::size_t>(ConstraintType::Count));
  for (std::size_t index = 0; index < valid.size(); ++index) {
    valid[index].id = static_cast<ConstraintId>(index + 1);
    CHECK(validateConstraintSchema(sketch, valid[index]).valid());
  }

  Constraint wrongKind{LengthConstraint{circle, 10.0}};
  wrongKind.id = 50;
  CHECK(!validateConstraintSchema(sketch, wrongKind).valid());
  Constraint self{ParallelConstraint{l0, l0}};
  self.id = 51;
  const auto selfResult = validateConstraintSchema(sketch, self);
  CHECK(!selfResult.valid());
  CHECK(selfResult.error->code == SketchSchemaErrorCode::SelfReference);
  Constraint missingValue{LengthConstraint{l0, std::nullopt}};
  missingValue.id = 52;
  const auto valueResult = validateConstraintSchema(sketch, missingValue);
  CHECK(!valueResult.valid());
  CHECK(valueResult.error->code ==
        SketchSchemaErrorCode::InvalidParameterRange);
  Constraint reference{LengthConstraint{l0, std::nullopt},
                       ConstraintMode::Reference, 53};
  CHECK(validateConstraintSchema(sketch, reference).valid());
  Constraint illegalReference{HorizontalConstraint{l0},
                              ConstraintMode::Reference, 54};
  CHECK(!validateConstraintSchema(sketch, illegalReference).valid());
  Constraint invalidMode{HorizontalConstraint{l0},
                         static_cast<ConstraintMode>(99), 55};
  CHECK(!validateConstraintSchema(sketch, invalidMode).valid());
  Constraint unknown{HorizontalConstraint{l0}};
  unknown.type = static_cast<ConstraintType>(999);
  const auto unknownResult = validateConstraintSchema(sketch, unknown);
  CHECK(!unknownResult.valid());
  CHECK(unknownResult.error->code ==
        SketchSchemaErrorCode::UnsupportedConstraintType);
  Constraint lineLineTangent{TangentConstraint{l0, l1},
                             ConstraintMode::Driving, 56};
  CHECK(!validateConstraintSchema(sketch, lineLineTangent).valid());
}

void publicMutationBoundariesPreserveAnnotationLinks() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {10.0, 0.0});
  sketch.addLine({0.0, 5.0}, {10.0, 5.0});
  const auto line = sketch.lineId(0);
  const auto secondLine = sketch.lineId(1);

  const auto beforeRejectedConstraints = sketch.semanticFingerprint();
  Constraint explicitId{HorizontalConstraint{line},
                        ConstraintMode::Driving, 99};
  CHECK(sketch.addConstraint(explicitId) == kInvalidConstraintId);
  Constraint stale{HorizontalConstraint{999999}};
  CHECK(sketch.addConstraint(stale) == kInvalidConstraintId);
  Constraint invalidMode{HorizontalConstraint{line},
                         static_cast<ConstraintMode>(99)};
  CHECK(sketch.addConstraint(invalidMode) == kInvalidConstraintId);
  Constraint lineLineTangent{TangentConstraint{line, secondLine}};
  CHECK(sketch.addConstraint(lineLineTangent) == kInvalidConstraintId);
  CHECK(sketch.semanticFingerprint() == beforeRejectedConstraints);

  CHECK(sketch.restoreConstraints({
      Constraint{LengthConstraint{line, std::nullopt},
                 ConstraintMode::Reference, 100},
      Constraint{HorizontalConstraint{line}, ConstraintMode::Driving, 101},
      Constraint{LengthConstraint{line, std::nullopt},
                 ConstraintMode::Reference, 102}}));

  Dimension first;
  first.id = 700;
  first.constraintId = 100;
  CHECK(sketch.storeDimension(first));
  CHECK(sketch.dimensions().size() == 1);

  Dimension unlinked;
  unlinked.id = 699;
  unlinked.valueMm = 10.0;
  CHECK(!sketch.storeDimension(unlinked));
  CHECK(sketch.dimensions().size() == 1);

  Dimension duplicateId = first;
  duplicateId.constraintId = 102;
  CHECK(!sketch.storeDimension(duplicateId));
  CHECK(sketch.dimensions().size() == 1);
  Dimension duplicateLink;
  duplicateLink.id = 701;
  duplicateLink.constraintId = 100;
  CHECK(!sketch.storeDimension(duplicateLink));
  CHECK(sketch.dimensions().size() == 1);
  Dimension nonDimensional;
  nonDimensional.id = 702;
  nonDimensional.constraintId = 101;
  CHECK(!sketch.storeDimension(nonDimensional));
  CHECK(sketch.dimensions().size() == 1);
  CHECK(!sketch.setDimensionConstraint(0, 101));

  const auto beforeRejectedRestore = sketch.semanticFingerprint();
  CHECK(!sketch.restoreConstraints({
      Constraint{HorizontalConstraint{line}, ConstraintMode::Driving, 101}}));
  CHECK(sketch.semanticFingerprint() == beforeRejectedRestore);

  CHECK(sketch.removeConstraint(100));
  CHECK(sketch.dimensions().empty());
  CHECK(!sketch.constraintIndex(100));
  CHECK(validateSketchSchema(sketch).valid());
}

void undoRedoPreservesExactIdentityAndMode() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {10.0, 0.0});
  const auto line = sketch.lineId(0);
  constexpr ConstraintId constraintId = 701;
  CHECK(sketch.restoreConstraints({Constraint{
      LengthConstraint{line, std::nullopt}, ConstraintMode::Reference,
      constraintId}}));
  Dimension annotation;
  annotation.constraintId = constraintId;
  sketch.storeDimension(annotation);

  sketch.beginDeltaJournal();
  const auto changed = sketch.setConstraintMode(
      constraintId, ConstraintMode::Driving, 10.0);
  CHECK(changed.committed());
  const auto delta = sketch.finishDeltaJournal();
  CHECK(!delta.empty());
  CHECK(sketch.constraints()[0].id == constraintId);
  CHECK(sketch.constraints()[0].mode == ConstraintMode::Driving);
  CHECK(sketch.dimensions()[0].constraintId == constraintId);

  CHECK(sketch.applyDelta(delta, false));
  CHECK(sketch.constraints()[0].id == constraintId);
  CHECK(sketch.constraints()[0].mode == ConstraintMode::Reference);
  CHECK(sketch.dimensions()[0].constraintId == constraintId);
  CHECK(sketch.applyDelta(delta, true));
  CHECK(sketch.constraints()[0].id == constraintId);
  CHECK(sketch.constraints()[0].mode == ConstraintMode::Driving);
  CHECK(validateSketchSchema(sketch).valid());
}

void deterministicInsertDeleteReorderProperty() {
  Sketch sketch;
  std::uint32_t state = 0x5eed1234u;
  ConstraintId nextConstraintId = 1000;

  for (int step = 0; step < 96; ++step) {
    state = state * 1664525u + 1013904223u;
    const int operation = static_cast<int>((state >> 16) % 3u);
    if (operation == 0 || sketch.lines().empty()) {
      const double x = static_cast<double>(step * 3);
      sketch.addLine({x, 0.0}, {x + 2.0, 0.0});
      const auto lineId = sketch.lineId(sketch.lines().size() - 1);
      auto constraints = sketch.constraints();
      const auto id = nextConstraintId++;
      constraints.emplace_back(LengthConstraint{lineId, std::nullopt},
                               ConstraintMode::Reference, id);
      CHECK(sketch.restoreConstraints(std::move(constraints)));
      Dimension annotation;
      annotation.constraintId = id;
      annotation.offsetMm = 2.0 + step;
      sketch.storeDimension(annotation);
    } else if (operation == 1) {
      const auto index = static_cast<std::size_t>(
          state % static_cast<std::uint32_t>(sketch.lines().size()));
      sketch.removeLine(index);
    } else {
      auto constraints = sketch.constraints();
      if (constraints.size() > 1) {
        const auto shift = static_cast<std::size_t>(
            state % static_cast<std::uint32_t>(constraints.size()));
        std::rotate(constraints.begin(), constraints.begin() + shift,
                    constraints.end());
      }
      CHECK(sketch.restoreConstraints(std::move(constraints)));
    }
    CHECK(validateSketchSchema(sketch).valid());
    for (const auto& annotation : sketch.dimensions())
      CHECK(sketch.constraintIndex(annotation.constraintId).has_value());
  }
}

void annotationIdentityIsExactAndUnique() {
  Sketch sketch;
  sketch.addLine({0.0, 0.0}, {10.0, 0.0});
  const auto line = sketch.lineId(0);
  Constraint first{LengthConstraint{line, std::nullopt},
                   ConstraintMode::Reference, 100};
  Constraint second{LengthConstraint{line, std::nullopt},
                    ConstraintMode::Reference, 101};
  CHECK(sketch.restoreConstraints({first, second}));

  Dimension a;
  a.constraintId = 100;
  a.offsetMm = 2.0;
  sketch.storeDimension(a);
  Dimension b;
  b.constraintId = 101;
  b.offsetMm = 4.0;
  sketch.storeDimension(b);
  CHECK(validateSketchSchema(sketch).valid());
  CHECK(sketch.dimensions().size() == 2);
  CHECK(sketch.removeDimension(0));
  CHECK(sketch.constraints().size() == 2);
  CHECK(sketch.dimensions().size() == 1);
  CHECK(sketch.dimensions()[0].constraintId == 101);
  CHECK(validateSketchSchema(sketch).valid());
}

void radiusConstraintCanOwnAnAnnotation() {
  Sketch sketch;
  sketch.addCircle({0.0, 0.0}, 12.0);
  const auto circle = sketch.circleId(0);
  constexpr ConstraintId constraintId = 810;
  CHECK(sketch.restoreConstraints({Constraint{
      RadiusConstraint{circle, 12.0}, ConstraintMode::Driving,
      constraintId}}));

  Dimension radius;
  radius.kind = DimensionKind::CircleRadius;
  radius.geometryId = circle;
  radius.constraintId = constraintId;
  radius.valueMm = 12.0;
  CHECK(sketch.storeDimension(radius));
  CHECK(validateSketchSchema(sketch).valid());
}

}  // namespace

int main() {
  taggedPointReferenceHasExactlyOneSource();
  completeConstraintSchemaMatrix();
  publicMutationBoundariesPreserveAnnotationLinks();
  annotationIdentityIsExactAndUnique();
  radiusConstraintCanOwnAnAnnotation();
  undoRedoPreservesExactIdentityAndMode();
  deterministicInsertDeleteReorderProperty();
  return 0;
}
