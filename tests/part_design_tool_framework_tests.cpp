#include <TopoDS_Shape.hxx>

#include <cstdlib>
#include <iostream>

#include "ui/tools/PartDesignToolController.h"

namespace {
#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                          \
      std::cerr << "CHECK failed: " #condition << '\n';                        \
      return EXIT_FAILURE;                                                       \
    }                                                                            \
  } while (false)

class FakeSession final : public solidar::ToolSession {
 public:
  solidar::ToolLifecycle lifecycleValue{solidar::ToolLifecycle::SelectingInput};
  solidar::ToolSelectionStage stage{solidar::ToolSelectionStage::SelectingInput};
  bool cancelled{};
  solidar::ToolLifecycle lifecycle() const noexcept override { return lifecycleValue; }
  solidar::ToolSelectionStage selectionStage() const noexcept override { return stage; }
  std::shared_ptr<const TopoDS_Shape> previewShape() const override { return {}; }
  const std::string& error() const noexcept override { return errorValue; }
  bool updatePreview() override { return false; }
  void cancel() noexcept override {
    cancelled = true;
    lifecycleValue = solidar::ToolLifecycle::Inactive;
  }
 private:
  std::string errorValue;
};
}

int main() {
  const auto& definitions = solidar::standardPartDesignToolDefinitions();
  CHECK(definitions.size() == 8);
  for (const auto& definition : definitions) {
    CHECK(definition.kind != solidar::PartDesignToolKind::None);
    CHECK(!definition.selections.empty());
  }
  solidar::PartDesignToolController controller;
  FakeSession revolve;
  FakeSession fillet;
  int presentationClears = 0;
  controller.registerTool(solidar::PartDesignToolKind::Revolve,
                          {&revolve, [&] { revolve.cancel(); },
                           [&] { ++presentationClears; }});
  controller.registerTool(solidar::PartDesignToolKind::Fillet,
                          {&fillet, [&] { fillet.cancel(); }, {}});

  controller.activate(solidar::PartDesignToolKind::Revolve);
  CHECK(controller.activeSession() == &revolve);
  controller.beginReselection(solidar::ToolSelectionStage::SelectingReference);
  CHECK(controller.isReselecting());
  CHECK(controller.handleEscape());
  CHECK(!revolve.cancelled);
  CHECK(controller.activeTool() == solidar::PartDesignToolKind::Revolve);

  controller.activate(solidar::PartDesignToolKind::Fillet);
  CHECK(revolve.cancelled);
  CHECK(presentationClears == 1);
  CHECK(controller.activeTool() == solidar::PartDesignToolKind::Fillet);
  CHECK(!controller.handleEscape());
  CHECK(fillet.cancelled);
  CHECK(controller.activeTool() == solidar::PartDesignToolKind::None);
  return EXIT_SUCCESS;
}
