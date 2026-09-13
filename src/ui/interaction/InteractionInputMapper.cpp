#include "ui/interaction/InteractionInputMapper.h"

namespace solidar {

InteractionIntent InteractionInputMapper::map(
    const NormalizedPointerInput& input) const {
  if (input.device != InteractionDeviceKind::Mouse) return InteractionIntent::None;
  if (input.phase == PointerPhase::Move && input.secondaryDown)
    return InteractionIntent::Orbit;
  if (input.middleDown || input.button == PointerButton::Middle)
    return InteractionIntent::Pan;
  if (input.phase == PointerPhase::Move && !input.primaryDown)
    return InteractionIntent::Hover;
  if (input.button == PointerButton::Primary &&
      input.phase == PointerPhase::Press)
    return input.overDirectTarget ? InteractionIntent::DirectDrag
                                  : input.overSelectable
                                        ? InteractionIntent::Select
                                        : InteractionIntent::BoxSelect;
  return InteractionIntent::None;
}

InteractionIntent InteractionInputMapper::map(
    const NormalizedKeyInput& input) const {
  if (input.key == NormalizedKeyInput::Key::Enter) return InteractionIntent::Confirm;
  if (input.key == NormalizedKeyInput::Key::Escape) return InteractionIntent::Cancel;
  if (input.key == NormalizedKeyInput::Key::Menu)
    return InteractionIntent::ContextAction;
  return InteractionIntent::None;
}

InteractionIntent InteractionInputMapper::map(
    const NormalizedWheelInput& input) const {
  return input.device == InteractionDeviceKind::Mouse && input.delta != 0.0
             ? InteractionIntent::Zoom : InteractionIntent::None;
}

}  // namespace solidar
