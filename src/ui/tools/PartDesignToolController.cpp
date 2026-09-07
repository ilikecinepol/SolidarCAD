#include "ui/tools/PartDesignToolController.h"

#include <utility>
#include <iostream>

#ifndef NDEBUG
#define SOLIDAR_TOOL_LOG(message) (std::clog << "[PartDesign] " << message << '\n')
#else
#define SOLIDAR_TOOL_LOG(message) ((void)0)
#endif

namespace solidar {

void PartDesignToolController::registerTool(
    PartDesignToolKind kind, Registration registration) {
  SOLIDAR_TOOL_LOG("Tool registered kind=" << static_cast<int>(kind));
  registrations_[kind] = std::move(registration);
}

void PartDesignToolController::activate(PartDesignToolKind kind) {
  // Reopening the current tool is a fresh lifecycle, not a no-op. This also
  // recovers a controller whose UI session was completed by an Apply handler.
  cancelActive();
  active_ = kind;
  temporaryStage_.reset();
  SOLIDAR_TOOL_LOG("Tool opened kind=" << static_cast<int>(kind));
}

void PartDesignToolController::deactivate(PartDesignToolKind kind) noexcept {
  if (active_ != kind) return;
  const auto found = registrations_.find(active_);
  if (found != registrations_.end() && found->second.clearPresentation)
    found->second.clearPresentation();
  SOLIDAR_TOOL_LOG("Tool applied/closed kind=" << static_cast<int>(kind));
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
  SOLIDAR_TOOL_LOG("Tool cancelled kind=" << static_cast<int>(previous));
  (void)previous;
}

void PartDesignToolController::beginReselection(ToolSelectionStage stage) {
  if (active_ == PartDesignToolKind::None || temporaryStage_) return;
  returnStage_ = selectionStage();
  temporaryStage_ = stage;
  SOLIDAR_TOOL_LOG("Selection context stage=" << static_cast<int>(stage));
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

std::size_t PartDesignToolController::registrationCount() const noexcept {
  return registrations_.size();
}

}  // namespace solidar
