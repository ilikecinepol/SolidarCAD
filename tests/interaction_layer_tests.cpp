#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <cstdlib>
#include <iostream>
#include <memory>

#include "model/Document.h"
#include "ui/interaction/ContextActionResolver.h"
#include "ui/interaction/InteractionInputMapper.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

int main() {
  using namespace solidar;

  // --- Input mapper: mouse device produces expected intents ---
  {
    InteractionInputMapper mapper;
    CHECK(mapper.map(NormalizedPointerInput{InteractionDeviceKind::Mouse,
                                            PointerPhase::Move,
                                            PointerButton::None,
                                            false, false, true, false,
                                            false}) == InteractionIntent::Orbit);
    CHECK(mapper.map(NormalizedPointerInput{InteractionDeviceKind::Mouse,
                                            PointerPhase::Move,
                                            PointerButton::None,
                                            false, true, false, false,
                                            false}) == InteractionIntent::Pan);
    CHECK(mapper.map(NormalizedPointerInput{InteractionDeviceKind::Mouse,
                                            PointerPhase::Move,
                                            PointerButton::None,
                                            false, false, false, false,
                                            false}) == InteractionIntent::Hover);
    CHECK(mapper.map(NormalizedPointerInput{InteractionDeviceKind::Mouse,
                                            PointerPhase::Press,
                                            PointerButton::Primary,
                                            false, false, false, true,
                                            false}) == InteractionIntent::DirectDrag);
    CHECK(mapper.map(NormalizedPointerInput{InteractionDeviceKind::Mouse,
                                            PointerPhase::Press,
                                            PointerButton::Primary,
                                            false, false, false, false,
                                            true}) == InteractionIntent::Select);
    CHECK(mapper.map(NormalizedPointerInput{InteractionDeviceKind::Mouse,
                                            PointerPhase::Press,
                                            PointerButton::Primary,
                                            false, false, false, false,
                                            false}) == InteractionIntent::BoxSelect);
    CHECK(mapper.map(NormalizedKeyInput{
              NormalizedKeyInput::Key::Enter}) == InteractionIntent::Confirm);
    CHECK(mapper.map(NormalizedKeyInput{
              NormalizedKeyInput::Key::Escape}) == InteractionIntent::Cancel);
    CHECK(mapper.map(NormalizedWheelInput{
              InteractionDeviceKind::Mouse, 1.0}) == InteractionIntent::Zoom);
    // Non-mouse devices are extension points: no mapping yet.
    CHECK(mapper.map(NormalizedPointerInput{InteractionDeviceKind::Pen,
                                            PointerPhase::Press,
                                            PointerButton::Primary,
                                            false, false, false, false,
                                            true}) == InteractionIntent::None);
    CHECK(mapper.map(NormalizedPointerInput{InteractionDeviceKind::Touch,
                                            PointerPhase::Press,
                                            PointerButton::Primary,
                                            false, false, false, false,
                                            true}) == InteractionIntent::None);
  }

  // --- Context resolver: single closed profile -> Extrude capability ---
  {
    auto profile = [&](solidar::SketchId id) {
      DocumentSketch p;
      p.id = id;
      p.geometry.addRectangle({-20.0, -20.0}, {20.0, 20.0});
      return p;
    };

    // No body: standalone base-plane profile -> NewBody.
    {
      SketchProfileSelectionContext ctx;
      ctx.profile = profile(1);
      ctx.activeBodyId = kInvalidBodyId;
      const auto cap = resolveSketchProfileExtrude(ctx);
      CHECK(cap.has_value());
      CHECK(cap->operation == ExtrudeOperation::NewBody);
      CHECK(!cap->operationFollowsDirection);
    }

    // Face-supported profile on the active body -> Join + follows direction.
    {
      DocumentSketch p = profile(2);
      p.support.type = SketchSupportType::Face;
      p.support.face.bodyId = 7;
      p.support.face.featureId = 11;
      p.supportResolved = true;
      SketchProfileSelectionContext ctx;
      ctx.profile = p;
      ctx.activeBodyId = 7;
      ctx.activeFeatureId = 11;
      ctx.activeBodyShape = std::make_shared<TopoDS_Shape>(
          BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape());
      const auto cap = resolveSketchProfileExtrude(ctx);
      CHECK(cap.has_value());
      CHECK(cap->operation == ExtrudeOperation::Join);
      CHECK(cap->operationFollowsDirection);
    }

    // Disjoint closed regions are one valid multi-region NewBody capability.
    {
      DocumentSketch p = profile(3);
      p.geometry.addRectangle({40.0, -20.0}, {80.0, 20.0});
      SketchProfileSelectionContext ctx;
      ctx.profile = p;
      ctx.activeBodyId = kInvalidBodyId;
      const auto cap = resolveSketchProfileExtrude(ctx);
      CHECK(cap.has_value());
      CHECK(cap->kind == ContextActionKind::Extrude);
      CHECK(cap->operation == ExtrudeOperation::NewBody);
      CHECK(!cap->operationFollowsDirection);
      CHECK(cap->profile.id == p.id);
      CHECK(cap->profile.geometry.lines().size() == 8);
    }

    // Open and nested contours stay unavailable to the context action.
    {
      DocumentSketch p;
      p.id = 30;
      p.geometry.addLine({0.0, 0.0}, {10.0, 0.0});
      SketchProfileSelectionContext ctx;
      ctx.profile = p;
      ctx.activeBodyId = kInvalidBodyId;
      CHECK(!resolveSketchProfileExtrude(ctx).has_value());
    }
    {
      DocumentSketch p = profile(31);
      p.geometry.addCircle({0.0, 0.0}, 5.0);
      SketchProfileSelectionContext ctx;
      ctx.profile = p;
      ctx.activeBodyId = kInvalidBodyId;
      CHECK(!resolveSketchProfileExtrude(ctx).has_value());
    }

    // Face-supported profile with NO body -> rejected.
    {
      DocumentSketch p = profile(4);
      p.support.type = SketchSupportType::Face;
      p.support.face.bodyId = 7;
      SketchProfileSelectionContext ctx;
      ctx.profile = p;
      ctx.activeBodyId = kInvalidBodyId;
      CHECK(!resolveSketchProfileExtrude(ctx).has_value());
    }

    // Datum-plane profile with a body present -> rejected (no NewBody guess).
    {
      SketchProfileSelectionContext ctx;
      ctx.profile = profile(5);
      ctx.activeBodyId = 7;
      ctx.activeFeatureId = 11;
      CHECK(!resolveSketchProfileExtrude(ctx).has_value());
    }
  }

  // --- Registry: first matching provider wins; empty registry -> nullopt ---
  {
    ContextActionRegistry registry;
    CHECK(!registry.resolve(SketchProfileSelectionContext{}).has_value());
    registry.addProvider([](const SketchProfileSelectionContext&) {
      return std::optional<ContextActionCapability>{};
    });
    registry.addProvider([](const SketchProfileSelectionContext& c) {
      ContextActionCapability cap;
      cap.kind = ContextActionKind::Extrude;
      cap.profile = c.profile;
      return std::optional<ContextActionCapability>{cap};
    });
    DocumentSketch p;
    p.id = 6;
    p.geometry.addRectangle({-5.0, -5.0}, {5.0, 5.0});
    SketchProfileSelectionContext ctx;
    ctx.profile = p;
    const auto cap = registry.resolve(ctx);
    CHECK(cap.has_value());
    CHECK(cap->kind == ContextActionKind::Extrude);
  }

  // --- Direct interaction state machine ---
  {
    DirectInteractionController controller;
    CHECK(controller.state() == DirectInteractionState::Idle);
    controller.hover(true);
    CHECK(controller.state() == DirectInteractionState::Hovering);
    controller.hover(false);
    CHECK(controller.state() == DirectInteractionState::Idle);
    controller.selected();
    CHECK(controller.state() == DirectInteractionState::Selected);
    controller.ready(true);
    CHECK(controller.state() == DirectInteractionState::Ready);
    controller.ready(false);
    CHECK(controller.state() == DirectInteractionState::PreviewInvalid);
    controller.dragging();
    CHECK(controller.state() == DirectInteractionState::Dragging);
    controller.reset();
    CHECK(controller.state() == DirectInteractionState::Idle);
  }

  return EXIT_SUCCESS;
}
