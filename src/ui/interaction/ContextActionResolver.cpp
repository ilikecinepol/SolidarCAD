#include "ui/interaction/ContextActionResolver.h"

#include <utility>

#include "model/SketchExtrudeBuilder.h"

namespace solidar {

void ContextActionRegistry::addProvider(Provider provider) {
  providers_.push_back(std::move(provider));
}

std::optional<ContextActionCapability> ContextActionRegistry::resolve(
    const SketchProfileSelectionContext& context) const {
  for (const auto& provider : providers_)
    if (auto capability = provider(context)) return capability;
  return std::nullopt;
}

std::optional<ContextActionCapability> resolveSketchProfileExtrude(
    const SketchProfileSelectionContext& context) {
  if (!isSupportedSketchProfile(context.profile)) return std::nullopt;
  if (context.activeBodyId == kInvalidBodyId) {
    if (context.profile.support.type == SketchSupportType::Face)
      return std::nullopt;
    return ContextActionCapability{ContextActionKind::Extrude, context.profile,
                                   kInvalidBodyId, kInvalidFeatureId, {},
                                   ExtrudeOperation::NewBody, false};
  }
  // With a body present, V1 is intentionally limited to a resolved sketch on
  // the current active feature's face. Datum-plane guesses are not NewBody.
  if (!context.activeBodyShape || context.activeBodyShape->IsNull() ||
      context.profile.support.type != SketchSupportType::Face ||
      !context.profile.supportResolved ||
      context.profile.support.face.bodyId != context.activeBodyId ||
      context.profile.support.face.featureId != context.activeFeatureId)
    return std::nullopt;
  return ContextActionCapability{ContextActionKind::Extrude, context.profile,
                                 context.activeBodyId, context.activeFeatureId,
                                 context.activeBodyShape,
                                 ExtrudeOperation::Join, true};
}

void DirectInteractionController::hover(bool available) noexcept {
  if (state_ == DirectInteractionState::Idle ||
      state_ == DirectInteractionState::Hovering)
    state_ = available ? DirectInteractionState::Hovering
                       : DirectInteractionState::Idle;
}
void DirectInteractionController::selected() noexcept {
  state_ = DirectInteractionState::Selected;
}
void DirectInteractionController::ready(bool valid) noexcept {
  state_ = valid ? DirectInteractionState::Ready
                 : DirectInteractionState::PreviewInvalid;
}
void DirectInteractionController::dragging() noexcept {
  state_ = DirectInteractionState::Dragging;
}
void DirectInteractionController::numericEditing() noexcept {
  state_ = DirectInteractionState::NumericEditing;
}
void DirectInteractionController::previewValidity(bool valid) noexcept {
  state_ = valid ? DirectInteractionState::Ready
                 : DirectInteractionState::PreviewInvalid;
}
void DirectInteractionController::committing() noexcept {
  state_ = DirectInteractionState::Committing;
}
void DirectInteractionController::cancelling() noexcept {
  state_ = DirectInteractionState::Cancelling;
}
void DirectInteractionController::reset() noexcept {
  state_ = DirectInteractionState::Idle;
}

}  // namespace solidar
