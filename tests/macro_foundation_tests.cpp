#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
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
  assert(first.success);
  assert(first.instance.macroId == definition.id);
  assert(first.instance.createdObjects.size() == 2);
  auto second = executor.execute(definition, document, {{1, anchorB}});
  assert(second.success);
  const auto* applied = document.findSketch(profileId);
  assert(applied);
  assert(applied->geometry.circles().size() == 2);
  const auto& circleA = applied->geometry.circles()[0];
  const auto& circleB = applied->geometry.circles()[1];
  assert(near(circleA.center.xMm, 12.0));
  assert(near(circleA.center.yMm, 6.0));
  assert(near(circleB.center.xMm, 52.0));
  assert(near(circleB.center.yMm, 26.0));
  assert(near(circleA.radiusMm, circleB.radiusMm));

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
    assert(result.success);
    const auto* resolved = document.findSketch(id);
    assert(resolved && resolved->geometry.circles().size() == 1);
    assert(near(resolved->geometry.circles()[0].center.xMm, 6.0));
    assert(near(resolved->geometry.circles()[0].center.yMm, 1.0));
    const auto world = resolved->placement.toWorld(6.0, 1.0);
    const auto local = resolved->placement.toLocal(world);
    assert(near(local.x, 6.0));
    assert(near(local.y, 1.0));
  }

  // Failure after a successful action restores the complete Document.
  MacroDefinition failing = definition;
  failing.actions[1] = MacroCreateCircleAction{2, 2, {1, 0.0, 0.0}, -1.0};
  auto* rollbackProfile = document.findSketch(profileId);
  assert(rollbackProfile);
  const auto lineCount = rollbackProfile->geometry.lines().size();
  const auto circleCount = rollbackProfile->geometry.circles().size();
  const auto failed = executor.execute(failing, document, {{1, anchorA}});
  assert(!failed.success);
  rollbackProfile = document.findSketch(profileId);
  assert(rollbackProfile);
  assert(rollbackProfile->geometry.lines().size() == lineCount);
  assert(rollbackProfile->geometry.circles().size() == circleCount);

  // Missing and stale semantic point references fail without mutation.
  const auto beforeInvalid = rollbackProfile->geometry.circles().size();
  const auto missing = executor.execute(definition, document, {});
  assert(!missing.success);
  const SketchPointReference stale{profileId, 999999,
                                   SketchPointKind::LineEnd};
  const auto invalid = executor.execute(definition, document, {{1, stale}});
  assert(!invalid.success);
  assert(document.findSketch(profileId)->geometry.circles().size() ==
         beforeInvalid);

  // Parameter binding is typed and overrides the serializable default.
  auto parameterized = executor.execute(
      definition, document, {{1, anchorA}}, {{1, MacroValue{5.5}}});
  assert(parameterized.success);
  assert(near(document.findSketch(profileId)->geometry.circles().back().radiusMm,
              5.5));
}
