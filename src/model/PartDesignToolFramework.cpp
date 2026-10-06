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
       {{SelectionType::Sketch, "Select profiles", 1,
         std::numeric_limits<std::size_t>::max(), true},
        one(SelectionType::Axis, "Select revolution axis")}, {angle()}},
      {PartDesignToolKind::Fillet,
       {{SelectionType::Edge, "Select edges", 1,
         std::numeric_limits<std::size_t>::max(), true}},
       {distance("radius", "Radius")}},
      {PartDesignToolKind::Chamfer,
       {{SelectionType::Edge, "Select edges", 1,
         std::numeric_limits<std::size_t>::max(), true}},
       {distance("distance", "Distance")}},
      {PartDesignToolKind::JoinBodies,
       {{SelectionType::Body, "Select two bodies", 2, 2, true}}, {}},
      {PartDesignToolKind::Move,
       {one(SelectionType::Body, "Select body")},
       {{"offset_x", "X", ToolParameterType::Distance, 0.0, -100000.0,
         100000.0, 0.1, "mm", true, ToolManipulatorType::Linear},
        {"offset_y", "Y", ToolParameterType::Distance, 0.0, -100000.0,
         100000.0, 0.1, "mm", true, ToolManipulatorType::Linear},
        {"offset_z", "Z", ToolParameterType::Distance, 0.0, -100000.0,
         100000.0, 0.1, "mm", true, ToolManipulatorType::Linear}}},
      {PartDesignToolKind::Mirror,
       {one(SelectionType::Body, "Select body"),
        one(SelectionType::Plane, "Select mirror plane")}, {}},
      {PartDesignToolKind::LinearPattern,
       {one(SelectionType::Body, "Select body"),
        one(SelectionType::Axis, "Select direction")},
       {distance("spacing", "Spacing"), count()}},
      {PartDesignToolKind::CircularPattern,
       {one(SelectionType::Body, "Select body"),
        one(SelectionType::Axis, "Select axis")}, {angle(), count()}},
      {PartDesignToolKind::Shell,
         {{SelectionType::Face, "Select faces to remove", 1,
           std::numeric_limits<std::size_t>::max(), true}},
         {distance("thickness", "Thickness"),
          {"outside", "Direction", ToolParameterType::Boolean, false, 0.0,
           1.0, 1.0, {}, false, ToolManipulatorType::None}}},
      {PartDesignToolKind::Draft,
         {one(SelectionType::Face, "Select surface"),
          one(SelectionType::Axis, "Select X/Y/Z or adjacent edge")},
         {{"angle", "Angle", ToolParameterType::Angle, 5.0, -89.99, 89.99,
          0.5, "deg", true, ToolManipulatorType::Angular}}}};
  return definitions;
}

bool acceptsSelection(const SelectionRequirement& requirement,
                      SelectionType candidate) noexcept {
  return requirement.type == candidate;
}

}  // namespace solidar
