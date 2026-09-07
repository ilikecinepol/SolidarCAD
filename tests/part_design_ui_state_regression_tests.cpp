#include <TopoDS_Shape.hxx>

#include <cstdlib>
#include <iostream>

#include "ui/tools/PartDesignToolController.h"

#define CHECK(condition) do { if (!(condition)) { \
  std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
  return EXIT_FAILURE; } } while (false)

namespace {
class Session final : public solidar::ToolSession {
 public:
  solidar::ToolLifecycle lifecycle() const noexcept override { return state; }
  std::shared_ptr<const TopoDS_Shape> previewShape() const override { return {}; }
  const std::string& error() const noexcept override { return message; }
  bool updatePreview() override { state = solidar::ToolLifecycle::PreviewValid; return true; }
  void cancel() noexcept override { ++cancels; state = solidar::ToolLifecycle::Inactive; }
  solidar::ToolLifecycle state{solidar::ToolLifecycle::SelectingInput};
  int cancels{};
  std::string message;
};
}

int main() {
  solidar::PartDesignToolController controller;
  Session extrude, revolve, mirror, circular;
  int presentationClears = 0;
  const auto add = [&](solidar::PartDesignToolKind kind, Session& session) {
    controller.registerTool(kind, {&session, [&session] { session.cancel(); },
                                   [&] { ++presentationClears; }});
  };
  add(solidar::PartDesignToolKind::Extrude, extrude);
  add(solidar::PartDesignToolKind::Revolve, revolve);
  add(solidar::PartDesignToolKind::Mirror, mirror);
  add(solidar::PartDesignToolKind::CircularPattern, circular);
  add(solidar::PartDesignToolKind::Extrude, extrude);
  CHECK(controller.registrationCount() == 4);

  for (int pass = 0; pass < 20; ++pass) {
    controller.activate(solidar::PartDesignToolKind::Extrude);
    CHECK(controller.activeTool() == solidar::PartDesignToolKind::Extrude);
    controller.activate(solidar::PartDesignToolKind::Revolve);
    CHECK(controller.activeTool() == solidar::PartDesignToolKind::Revolve);
    controller.beginReselection(solidar::ToolSelectionStage::SelectingReference);
    CHECK(controller.handleEscape());
    controller.activate(solidar::PartDesignToolKind::Mirror);
    controller.activate(solidar::PartDesignToolKind::CircularPattern);
    controller.cancelActive();
    CHECK(controller.activeTool() == solidar::PartDesignToolKind::None);
    CHECK(!controller.isReselecting());
  }
  CHECK(extrude.cancels == 20);
  CHECK(revolve.cancels == 20);
  CHECK(mirror.cancels == 20);
  CHECK(circular.cancels == 20);
  CHECK(presentationClears == 80);
  return EXIT_SUCCESS;
}
