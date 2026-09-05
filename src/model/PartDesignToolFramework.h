#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "model/Feature.h"

namespace solidar {

enum class PartDesignToolKind {
  None,
  Extrude,
  Pocket,
  Revolve,
  Fillet,
  Chamfer,
  Mirror,
  LinearPattern,
  CircularPattern
};

enum class SelectionType {
  Sketch,
  SketchLine,
  Edge,
  Face,
  Axis,
  Plane,
  Body,
  Feature
};

struct SelectionRequirement {
  SelectionType type{SelectionType::Feature};
  std::string prompt;
  std::size_t minimumCount{1};
  std::size_t maximumCount{1};
  bool multiSelect{false};
};

enum class ToolSelectionStage {
  None,
  SelectingInput,
  SelectingReference,
  EditingParameters
};

enum class ToolParameterType { Distance, Angle, Integer, Boolean, Enum };
enum class ToolManipulatorType { None, Linear, Angular };
using ToolParameterValue = std::variant<double, int, bool, std::string>;

struct ToolParameterDescriptor {
  std::string id;
  std::string label;
  ToolParameterType type{ToolParameterType::Distance};
  ToolParameterValue value{0.0};
  double minimum{};
  double maximum{};
  double step{1.0};
  std::string unit;
  bool editableInHud{false};
  ToolManipulatorType manipulatorType{ToolManipulatorType::None};
};

struct PartDesignToolDefinition {
  PartDesignToolKind kind{PartDesignToolKind::None};
  std::vector<SelectionRequirement> selections;
  std::vector<ToolParameterDescriptor> parameters;
};

// Canonical contracts for every current Part Design tool. UI/controller code
// consumes these definitions instead of inventing per-tool selection stages.
[[nodiscard]] const std::vector<PartDesignToolDefinition>&
standardPartDesignToolDefinitions();

}  // namespace solidar
