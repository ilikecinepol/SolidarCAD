#include "model/PartDesignToolFramework.h"

#include <limits>

namespace solidar {
namespace {
SelectionRequirement one(SelectionType type, const char* prompt) {
  return {type, prompt, 1, 1, false};
}
ToolParameterDescriptor distance(const char* id, const char* label) {
  return {id, label, ToolParameterType::Distance, 10.0, 0.01, 100000.0,
          0.1, "mm", true, ToolManipulatorType::Linear};
}
ToolParameterDescriptor angle() {
  return {"angle", "Angle", ToolParameterType::Angle, 360.0, 0.01, 360.0,
          1.0, "deg", true, ToolManipulatorType::Angular};
}
ToolParameterDescriptor count() {
  return {"count", "Count", ToolParameterType::Integer, 2, 2.0, 100.0,
          1.0, {}, true, ToolManipulatorType::None};
}
}

const std::vector<PartDesignToolDefinition>&
standardPartDesignToolDefinitions() {
  static const std::vector<PartDesignToolDefinition> definitions{
      {PartDesignToolKind::Extrude, {one(SelectionType::Sketch, "Select profile")},
       {distance("distance", "Distance")}},
      {PartDesignToolKind::Pocket, {one(SelectionType::Sketch, "Select profile")},
       {distance("depth", "Depth")}},
      {PartDesignToolKind::Revolve,
       {one(SelectionType::Sketch, "Select profile"),
        one(SelectionType::Axis, "Select revolution axis")}, {angle()}},
      {PartDesignToolKind::Fillet,
       {{SelectionType::Edge, "Select edges", 1,
         std::numeric_limits<std::size_t>::max(), true}},
       {distance("radius", "Radius")}},
      {PartDesignToolKind::Chamfer,
       {{SelectionType::Edge, "Select edges", 1,
         std::numeric_limits<std::size_t>::max(), true}},
       {distance("distance", "Distance")}},
      {PartDesignToolKind::Mirror,
       {one(SelectionType::Feature, "Select source"),
        one(SelectionType::Plane, "Select mirror plane")}, {}},
      {PartDesignToolKind::LinearPattern,
       {one(SelectionType::Feature, "Select source"),
        one(SelectionType::Axis, "Select direction")},
       {distance("spacing", "Spacing"), count()}},
      {PartDesignToolKind::CircularPattern,
       {one(SelectionType::Feature, "Select source"),
        one(SelectionType::Axis, "Select axis")}, {angle(), count()}}};
  return definitions;
}

}  // namespace solidar
