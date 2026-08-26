#include "model/macro/MacroExecutor.h"

#include <atomic>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace solidar::macro {
namespace {
std::atomic<MacroInstanceId> g_nextMacroInstanceId{1};

MacroInputType bindingType(const MacroInputBinding& binding) {
  return std::visit(
      [](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, SketchPointReference>)
          return MacroInputType::SketchPoint;
        else if constexpr (std::is_same_v<T, SketchEntityReference>)
          return MacroInputType::SketchEntity;
        else if constexpr (std::is_same_v<T, SketchReference>)
          return MacroInputType::Sketch;
        else if constexpr (std::is_same_v<T, FaceReference>)
          return MacroInputType::Face;
        else if constexpr (std::is_same_v<T, EdgeReference>)
          return MacroInputType::Edge;
        else
          return MacroInputType::Body;
      },
      binding);
}

std::optional<double> scalarValue(const MacroScalarExpression& expression,
                                  const MacroExecutionContext& context) {
  if (const auto* literal = std::get_if<double>(&expression)) return *literal;
  const auto parameterId = std::get<MacroParameterId>(expression);
  const auto found = context.parameters.find(parameterId);
  if (found == context.parameters.end()) return std::nullopt;
  const auto* value = std::get_if<double>(&found->second);
  return value ? std::optional<double>(*value) : std::nullopt;
}

struct ResolvedPoint {
  SketchId sketchId{kInvalidSketchId};
  sketch::Point point;
};

std::optional<ResolvedPoint> pointValue(const MacroPointExpression& expression,
                                        const MacroExecutionContext& context) {
  const auto binding = context.inputs.find(expression.anchor);
  if (binding == context.inputs.end()) return std::nullopt;
  const auto* reference = std::get_if<SketchPointReference>(&binding->second);
  if (!reference) return std::nullopt;
  const auto anchor = resolveSketchPoint(context.document, *reference);
  const auto dx = scalarValue(expression.offsetX, context);
  const auto dy = scalarValue(expression.offsetY, context);
  if (!anchor || !dx || !dy || !std::isfinite(*dx) || !std::isfinite(*dy))
    return std::nullopt;
  return ResolvedPoint{reference->sketchId,
                       {anchor->xMm + *dx, anchor->yMm + *dy}};
}

bool executeAction(const MacroCreateLineAction& action,
                   MacroExecutionContext& context, std::string* error) {
  const auto start = pointValue(action.start, context);
  const auto end = pointValue(action.end, context);
  if (!start || !end || start->sketchId != end->sketchId) {
    *error = "CreateLine requires two valid points in the same Sketch";
    return false;
  }
  if (start->point.xMm == end->point.xMm &&
      start->point.yMm == end->point.yMm) {
    *error = "CreateLine endpoints must be different";
    return false;
  }
  auto* profile = context.document.findSketch(start->sketchId);
  if (!profile) {
    *error = "CreateLine target Sketch does not exist";
    return false;
  }
  profile->geometry.addLine(start->point, end->point);
  const auto geometryId =
      profile->geometry.lineId(profile->geometry.lines().size() - 1);
  context.objects[action.output] = {profile->id, geometryId};
  return true;
}

bool executeAction(const MacroCreateCircleAction& action,
                   MacroExecutionContext& context, std::string* error) {
  const auto center = pointValue(action.center, context);
  const auto radius = scalarValue(action.radiusMm, context);
  if (!center || !radius || !std::isfinite(*radius) || *radius <= 0.0) {
    *error = "CreateCircle requires a valid center and positive radius";
    return false;
  }
  auto* profile = context.document.findSketch(center->sketchId);
  if (!profile) {
    *error = "CreateCircle target Sketch does not exist";
    return false;
  }
  profile->geometry.addCircle(center->point, *radius);
  const auto geometryId =
      profile->geometry.circleId(profile->geometry.circles().size() - 1);
  context.objects[action.output] = {profile->id, geometryId};
  return true;
}
}  // namespace

MacroExecutionResult MacroExecutor::execute(
    const MacroDefinition& definition, Document& document,
    const MacroInputBindings& inputs,
    const MacroParameterValues& parameters) const {
  MacroExecutionResult result;
  result.instance.id =
      g_nextMacroInstanceId.fetch_add(1, std::memory_order_relaxed);
  result.instance.macroId = definition.id;
  result.instance.inputBindings = inputs;
  if (definition.formatVersion != 1) {
    result.error = "Unsupported macro format version";
    return result;
  }

  for (const auto& input : definition.inputs) {
    const auto binding = inputs.find(input.id);
    if (binding == inputs.end() || bindingType(binding->second) != input.type) {
      result.error = "Missing or incompatible macro input: " + input.name;
      return result;
    }
    if (const auto* point =
            std::get_if<SketchPointReference>(&binding->second);
        point && !resolveSketchPoint(document, *point)) {
      result.error = "Macro SketchPoint input cannot be resolved: " + input.name;
      return result;
    }
  }

  MacroParameterValues resolvedParameters;
  for (const auto& parameter : definition.parameters)
    resolvedParameters.emplace(parameter.id, parameter.defaultValue);
  for (const auto& [id, value] : parameters) resolvedParameters[id] = value;

  Document snapshot = document;
  MacroExecutionContext context{document, inputs, std::move(resolvedParameters), {}};
  try {
    for (const auto& action : definition.actions) {
      std::string error;
      const bool succeeded = std::visit(
          [&](const auto& value) { return executeAction(value, context, &error); },
          action);
      if (!succeeded) {
        document = std::move(snapshot);
        result.error = std::move(error);
        return result;
      }
    }
  } catch (const std::exception& exception) {
    document = std::move(snapshot);
    result.error = exception.what();
    return result;
  } catch (...) {
    document = std::move(snapshot);
    result.error = "Unexpected macro execution failure";
    return result;
  }

  result.success = true;
  result.instance.createdObjects.reserve(context.objects.size());
  for (const auto& [localId, object] : context.objects) {
    (void)localId;
    result.instance.createdObjects.push_back(object);
  }
  return result;
}

}  // namespace solidar::macro
