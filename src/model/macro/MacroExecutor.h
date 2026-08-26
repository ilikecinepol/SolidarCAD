#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "model/macro/MacroDefinition.h"
#include "model/macro/MacroReferences.h"

namespace solidar::macro {

using MacroInputBindings =
    std::unordered_map<MacroInputId, MacroInputBinding>;
using MacroParameterValues =
    std::unordered_map<MacroParameterId, MacroValue>;

struct MacroInstance {
  MacroInstanceId id{};
  MacroId macroId{kInvalidMacroId};
  MacroInputBindings inputBindings;
  std::vector<CreatedObjectReference> createdObjects;
};

struct MacroExecutionResult {
  bool success{false};
  std::string error;
  MacroInstance instance;
};

struct MacroExecutionContext {
  Document& document;
  MacroInputBindings inputs;
  MacroParameterValues parameters;
  std::unordered_map<MacroObjectId, CreatedObjectReference> objects;
};

class MacroExecutor final {
 public:
  [[nodiscard]] MacroExecutionResult execute(
      const MacroDefinition& definition, Document& document,
      const MacroInputBindings& inputs,
      const MacroParameterValues& parameters = {}) const;
};

}  // namespace solidar::macro
