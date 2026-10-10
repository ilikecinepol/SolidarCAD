#ifdef NDEBUG
#undef NDEBUG
#endif
#include "TestAssertions.h"
#include <cmath>

#include "model/macro/MacroExecutor.h"

namespace {
using namespace solidar;
using namespace solidar::macro;

bool near(double actual, double expected) {
  return std::abs(actual - expected) < 1.0e-9;
}

SketchPointReference addAnchor(DocumentSketch& profile, sketch::Point point) {
  profile.geometry.addLine(point, {point.xMm + 1.0, point.yMm});
  return {profile.id,
          profile.geometry.lineId(profile.geometry.lines().size() - 1),
          SketchPointKind::LineStart};
}

MacroDefinition circleAndLineMacro() {
  MacroDefinition definition;
  definition.id = 101;
  definition.name = "Relative marker";
  definition.description = "Creates geometry in Sketch-local coordinates";
  definition.inputs.push_back({1, "AnchorPoint", MacroInputType::SketchPoint});
  definition.parameters.push_back({1, "Radius", 3.0});
  definition.actions.push_back(MacroCreateCircleAction{
      1, 1, {1, 2.0, -4.0}, MacroParameterId{1}});
  definition.actions.push_back(MacroCreateLineAction{
      2, 2, {1, -2.0, 0.0}, {1, 2.0, 0.0}});
  return definition;
}
}  // namespace

int main() {
  const MacroExecutor executor;
  const MacroDefinition definition = circleAndLineMacro();

  Document document;
  auto& profile = document.addSketch("Two anchors");
  const SketchId profileId = profile.id;
  const auto anchorA = addAnchor(profile, {10.0, 10.0});
  const auto anchorB = addAnchor(profile, {50.0, 30.0});
  auto first = executor.execute(definition, document, {{1, anchorA}});
  CHECK(first.success);
  CHECK(first.instance.macroId == definition.id);
  CHECK(first.instance.createdObjects.size() == 2);
  auto second = executor.execute(definition, document, {{1, anchorB}});
  CHECK(second.success);
  const auto* applied = document.findSketch(profileId);
  CHECK(applied);
  CHECK(applied->geometry.circles().size() == 2);
  const auto& circleA = applied->geometry.circles()[0];
  const auto& circleB = applied->geometry.circles()[1];
  CHECK(near(circleA.center.xMm, 12.0));
  CHECK(near(circleA.center.yMm, 6.0));
  CHECK(near(circleB.center.xMm, 52.0));
  CHECK(near(circleB.center.yMm, 26.0));
  CHECK(near(circleA.radiusMm, circleB.radiusMm));

  // Identical local expressions work for every SketchPlacement. Macro core
  // never converts them to world XYZ.
  for (const auto& placement : {SketchPlacement::xy(), SketchPlacement::xz(),
                                SketchPlacement::yz(),
                                SketchPlacement{{7.0, 8.0, 9.0},
                                                {0.0, 1.0, 0.0},
                                                {0.0, 0.0, 1.0}}}) {
    auto& placed = document.addSketch("Placed");
    placed.placement = placement;
    const auto reference = addAnchor(placed, {4.0, 5.0});
    const SketchId id = placed.id;
    const auto result = executor.execute(definition, document, {{1, reference}});
    CHECK(result.success);
    const auto* resolved = document.findSketch(id);
    CHECK(resolved && resolved->geometry.circles().size() == 1);
    CHECK(near(resolved->geometry.circles()[0].center.xMm, 6.0));
    CHECK(near(resolved->geometry.circles()[0].center.yMm, 1.0));
    const auto world = resolved->placement.toWorld(6.0, 1.0);
    const auto local = resolved->placement.toLocal(world);
    CHECK(near(local.x, 6.0));
    CHECK(near(local.y, 1.0));
  }

  // Failure after a successful action restores the complete Document.
  MacroDefinition failing = definition;
  failing.actions[1] = MacroCreateCircleAction{2, 2, {1, 0.0, 0.0}, -1.0};
  auto* rollbackProfile = document.findSketch(profileId);
  CHECK(rollbackProfile);
  const auto lineCount = rollbackProfile->geometry.lines().size();
  const auto circleCount = rollbackProfile->geometry.circles().size();
  const auto failed = executor.execute(failing, document, {{1, anchorA}});
  CHECK(!failed.success);
  rollbackProfile = document.findSketch(profileId);
  CHECK(rollbackProfile);
  CHECK(rollbackProfile->geometry.lines().size() == lineCount);
  CHECK(rollbackProfile->geometry.circles().size() == circleCount);

  // Missing and stale semantic point references fail without mutation.
  const auto beforeInvalid = rollbackProfile->geometry.circles().size();
  const auto missing = executor.execute(definition, document, {});
  CHECK(!missing.success);
  const SketchPointReference stale{profileId, 999999,
                                   SketchPointKind::LineEnd};
  const auto invalid = executor.execute(definition, document, {{1, stale}});
  CHECK(!invalid.success);
  CHECK(document.findSketch(profileId)->geometry.circles().size() ==
         beforeInvalid);

  // Parameter binding is typed and overrides the serializable default.
  auto parameterized = executor.execute(
      definition, document, {{1, anchorA}}, {{1, MacroValue{5.5}}});
  CHECK(parameterized.success);
  CHECK(near(document.findSketch(profileId)->geometry.circles().back().radiusMm,
              5.5));
}
