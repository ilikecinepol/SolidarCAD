#pragma once

#include <functional>
#include <map>
#include <optional>

#include "model/PartDesignToolFramework.h"
#include "model/ToolSession.h"

namespace solidar {

// Owns the application-wide active-tool invariant and nested re-selection
// routing. CAD state remains in ToolSession; panels are only views/bindings.
class PartDesignToolController final {
 public:
  struct Registration {
    ToolSession* session{};
    std::function<void()> cancel;
    std::function<void()> clearPresentation;
  };

  void registerTool(PartDesignToolKind kind, Registration registration);
  void activate(PartDesignToolKind kind);
  void deactivate(PartDesignToolKind kind) noexcept;
  void cancelActive();

  void beginReselection(ToolSelectionStage stage);
  void finishReselection() noexcept;
  // Returns true when only nested re-selection was cancelled; false means the
  // complete active tool was cancelled (or no tool was active).
  bool handleEscape();

  [[nodiscard]] PartDesignToolKind activeTool() const noexcept;
  [[nodiscard]] ToolSession* activeSession() const noexcept;
  [[nodiscard]] ToolSelectionStage selectionStage() const noexcept;
  [[nodiscard]] bool isReselecting() const noexcept;

 private:
  std::map<PartDesignToolKind, Registration> registrations_;
  PartDesignToolKind active_{PartDesignToolKind::None};
  std::optional<ToolSelectionStage> temporaryStage_;
  ToolSelectionStage returnStage_{ToolSelectionStage::None};
};

}  // namespace solidar
