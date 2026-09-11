#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

#include "model/ChamferToolSession.h"
#include "model/FilletToolSession.h"
#include "model/TopologyReferenceResolver.h"
#include "ui/tools/PartDesignToolController.h"
#include "ui/PartDesignToolHelp.h"

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
  CHECK(definitions.size() == 10);
  for (const auto& definition : definitions) {
    CHECK(definition.kind != solidar::PartDesignToolKind::None);
    CHECK(!definition.selections.empty());
    const auto* help = solidar::partDesignToolHelp(definition.kind);
    CHECK(help != nullptr);
    CHECK(!help->title.isEmpty());
    CHECK(!help->detailedDescription.isEmpty());
  }
  const auto& mirror = definitions[5];
  CHECK(solidar::acceptsSelection(mirror.selections[1],
                                  solidar::SelectionType::Plane));
  CHECK(!solidar::acceptsSelection(mirror.selections[1],
                                   solidar::SelectionType::Body));
  const auto& circular = definitions[7];
  CHECK(solidar::acceptsSelection(circular.selections[0],
                                  solidar::SelectionType::Feature));
  CHECK(solidar::acceptsSelection(circular.selections[1],
                                  solidar::SelectionType::Axis));
  CHECK(solidar::partDesignToolStepHint(
            solidar::PartDesignToolKind::Revolve,
            solidar::ToolSelectionStage::SelectingInput) ==
        solidar::partDesignToolHelp(
            solidar::PartDesignToolKind::Revolve)->selectionHint);
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

  revolve.cancelled = false;
  controller.activate(solidar::PartDesignToolKind::Revolve);
  CHECK(revolve.cancelled);
  CHECK(presentationClears == 1);

  controller.activate(solidar::PartDesignToolKind::Fillet);
  CHECK(revolve.cancelled);
  CHECK(presentationClears == 2);
  CHECK(controller.activeTool() == solidar::PartDesignToolKind::Fillet);
  CHECK(!controller.handleEscape());
  CHECK(fillet.cancelled);
  CHECK(controller.activeTool() == solidar::PartDesignToolKind::None);

  // Fillet/Chamfer sessions expose a finite unit-direction manipulator whose
  // value tracks radius/distance, and whose direction is translation-invariant
  // (derived purely from the selected edge and its adjacent faces).
  {
    const solidar::BodyId bodyId = 7;
    const solidar::FeatureId featureId = 9;
    const auto box = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(30.0, 20.0, 10.0).Shape());
    const auto edge = solidar::makeEdgeReference(*box, bodyId, featureId, 0);
    CHECK(edge.signature);

    const auto unitLength = [](solidar::Vector3d v) {
      return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    };
    const auto finiteUnit = [&](solidar::Vector3d v) {
      return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
             std::abs(unitLength(v) - 1.0) < 1e-6;
    };

    solidar::FilletToolSession fillet;
    fillet.begin(bodyId, featureId, box, {edge}, 3.0);
    const auto filletManip = fillet.manipulator();
    CHECK(filletManip.has_value());
    CHECK(finiteUnit(filletManip->direction));
    CHECK(std::isfinite(filletManip->origin.x) &&
          std::isfinite(filletManip->origin.y) &&
          std::isfinite(filletManip->origin.z));
    CHECK(std::abs(filletManip->valueMm - fillet.radiusMm()) < 1e-9);

    solidar::ChamferToolSession chamfer;
    chamfer.begin(bodyId, featureId, box, {edge}, 2.5);
    const auto chamferManip = chamfer.manipulator();
    CHECK(chamferManip.has_value());
    CHECK(finiteUnit(chamferManip->direction));
    CHECK(std::abs(chamferManip->valueMm - chamfer.distanceMm()) < 1e-9);

    // Translation invariance through the session seam: translating the base
    // shape shifts the midpoint but leaves the unit direction unchanged.
    gp_Trsf translation;
    translation.SetTranslation(gp_Vec(1000.0, -700.0, 350.0));
    const auto movedBox = std::make_shared<TopoDS_Shape>(
        BRepBuilderAPI_Transform(*box, translation).Shape());
    const auto movedEdge =
        solidar::makeEdgeReference(*movedBox, bodyId, featureId, 0);
    CHECK(movedEdge.signature);
    solidar::FilletToolSession movedFillet;
    movedFillet.begin(bodyId, featureId, movedBox, {movedEdge}, 3.0);
    const auto movedManip = movedFillet.manipulator();
    CHECK(movedManip.has_value());
    CHECK(finiteUnit(movedManip->direction));
    CHECK(std::abs(movedManip->direction.x - filletManip->direction.x) < 1e-6);
    CHECK(std::abs(movedManip->direction.y - filletManip->direction.y) < 1e-6);
    CHECK(std::abs(movedManip->direction.z - filletManip->direction.z) < 1e-6);
    CHECK(std::abs(movedManip->origin.x - (filletManip->origin.x + 1000.0)) <
          1e-6);
    CHECK(std::abs(movedManip->origin.y - (filletManip->origin.y - 700.0)) <
          1e-6);
    CHECK(std::abs(movedManip->origin.z - (filletManip->origin.z + 350.0)) <
          1e-6);
  }

  return EXIT_SUCCESS;
}
