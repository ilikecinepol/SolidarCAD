#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace solidar::macro {

using MacroId = std::uint64_t;
using MacroInputId = std::uint64_t;
using MacroParameterId = std::uint64_t;
using MacroActionId = std::uint64_t;
using MacroObjectId = std::uint64_t;
using MacroInstanceId = std::uint64_t;

inline constexpr MacroId kInvalidMacroId = 0;
inline constexpr MacroInputId kInvalidMacroInputId = 0;
inline constexpr MacroParameterId kInvalidMacroParameterId = 0;
inline constexpr MacroObjectId kInvalidMacroObjectId = 0;

enum class MacroInputType { SketchPoint, SketchEntity, Sketch, Face, Edge, Body };

struct MacroInputDefinition {
  MacroInputId id{kInvalidMacroInputId};
  std::string name;
  MacroInputType type{MacroInputType::SketchPoint};
};

using MacroValue = std::variant<double, bool, std::string>;

struct MacroParameterDefinition {
  MacroParameterId id{kInvalidMacroParameterId};
  std::string name;
  MacroValue defaultValue{0.0};
};

using MacroScalarExpression = std::variant<double, MacroParameterId>;

struct MacroPointExpression {
  MacroInputId anchor{kInvalidMacroInputId};
  MacroScalarExpression offsetX{0.0};
  MacroScalarExpression offsetY{0.0};
};

struct MacroCreateLineAction {
  MacroActionId id{};
  MacroObjectId output{kInvalidMacroObjectId};
  MacroPointExpression start;
  MacroPointExpression end;
};

struct MacroCreateCircleAction {
  MacroActionId id{};
  MacroObjectId output{kInvalidMacroObjectId};
  MacroPointExpression center;
  MacroScalarExpression radiusMm{1.0};
};

using MacroAction =
    std::variant<MacroCreateLineAction, MacroCreateCircleAction>;

struct MacroDefinition {
  std::uint32_t formatVersion{1};
  MacroId id{kInvalidMacroId};
  std::string name;
  std::string description;
  std::vector<MacroInputDefinition> inputs;
  std::vector<MacroParameterDefinition> parameters;
  std::vector<MacroAction> actions;
};

}  // namespace solidar::macro
