#include "ui/tools/PartDesignToolController.h"

#include <utility>

namespace solidar {

void PartDesignToolController::registerTool(
    PartDesignToolKind kind, Registration registration) {
  registrations_[kind] = std::move(registration);
}

void PartDesignToolController::activate(PartDesignToolKind kind) {
  if (kind == active_) return;
  cancelActive();
  active_ = kind;
  temporaryStage_.reset();
}

void PartDesignToolController::deactivate(PartDesignToolKind kind) noexcept {
  if (active_ != kind) return;
  active_ = PartDesignToolKind::None;
  temporaryStage_.reset();
  returnStage_ = ToolSelectionStage::None;
}

void PartDesignToolController::cancelActive() {
  if (active_ == PartDesignToolKind::None) return;
  const auto found = registrations_.find(active_);
  const PartDesignToolKind previous = active_;
  active_ = PartDesignToolKind::None;
  temporaryStage_.reset();
  returnStage_ = ToolSelectionStage::None;
  if (found != registrations_.end()) {
    if (found->second.cancel) found->second.cancel();
    if (found->second.clearPresentation) found->second.clearPresentation();
  }
  (void)previous;
}

void PartDesignToolController::beginReselection(ToolSelectionStage stage) {
  if (active_ == PartDesignToolKind::None || temporaryStage_) return;
  returnStage_ = selectionStage();
  temporaryStage_ = stage;
}

void PartDesignToolController::finishReselection() noexcept {
  temporaryStage_.reset();
  returnStage_ = ToolSelectionStage::None;
}

bool PartDesignToolController::handleEscape() {
  if (temporaryStage_) {
    temporaryStage_.reset();
    return true;
  }
  cancelActive();
  return false;
}

PartDesignToolKind PartDesignToolController::activeTool() const noexcept {
  return active_;
}

ToolSession* PartDesignToolController::activeSession() const noexcept {
  const auto found = registrations_.find(active_);
  return found == registrations_.end() ? nullptr : found->second.session;
}

ToolSelectionStage PartDesignToolController::selectionStage() const noexcept {
  if (temporaryStage_) return *temporaryStage_;
  if (auto* session = activeSession()) return session->selectionStage();
  return returnStage_;
}

bool PartDesignToolController::isReselecting() const noexcept {
  return temporaryStage_.has_value();
}

}  // namespace solidar
