#include "model/ComponentGraphAnalysis.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "model/AssemblyGraphAnalysis.h"

namespace solidar {

ComponentGraphStatus analyzeComponentGraph(
    const std::vector<ComponentDefinition>& components,
    DefinitionId rootDefinitionId,
    ComponentGraphFailurePoint failurePoint) noexcept {
  try {
    if (components.size() > kAssemblyGraphMaximumDefinitions)
      return ComponentGraphStatus::DefinitionBudgetExceeded;
    if (failurePoint ==
        ComponentGraphFailurePoint::BeforeValidationAllocation)
      return ComponentGraphStatus::ResourceFailure;

    ComponentGraphView graph;
    graph.rootDefinitionId = rootDefinitionId;
    for (const auto& component : components) {
      if (!component.id().isValid() ||
          !graph.definitions.emplace(component.id(),
                                     ComponentGraphDefinition{}).second)
        return ComponentGraphStatus::DuplicateDefinitionId;
      auto& definition = graph.definitions.at(component.id());
      definition.bodyIds.reserve(component.document().bodies().size());
      for (const auto& body : component.document().bodies())
        definition.bodyIds.push_back(body.id());
      definition.drawings.reserve(component.drawings().size());
      for (const auto& drawing : component.drawings())
        definition.drawings.push_back(
            {drawing.id(), drawing.sourceBodyIds()});
      auto& occurrences = definition.occurrences;
      occurrences.reserve(component.size());
      for (const auto& [occurrenceId, occurrence] :
           component.occurrences()) {
        if (occurrenceId != occurrence.id())
          return ComponentGraphStatus::UnsupportedComponentDefinition;
        occurrences.push_back(
            {occurrenceId, occurrence.targetDefinitionId()});
      }
    }
    return analyzeComponentGraph(graph);
  } catch (...) {
    return ComponentGraphStatus::ResourceFailure;
  }
}

ComponentGraphStatus analyzeComponentGraph(
    const DefinitionRegistry& definitions,
    DefinitionId rootDefinitionId) noexcept {
  try {
    if (definitions.size() > kAssemblyGraphMaximumDefinitions)
      return ComponentGraphStatus::DefinitionBudgetExceeded;
    ComponentGraphView graph;
    graph.rootDefinitionId = rootDefinitionId;
    for (const DefinitionId id : definitions.definitionIds()) {
      const auto* component = definitions.findComponent(id);
      if (!component)
        return ComponentGraphStatus::UnsupportedComponentDefinition;
      auto& node = graph.definitions[id];
      node.bodyIds.reserve(component->document().bodies().size());
      for (const auto& body : component->document().bodies())
        node.bodyIds.push_back(body.id());
      node.drawings.reserve(component->drawings().size());
      for (const auto& drawing : component->drawings())
        node.drawings.push_back({drawing.id(), drawing.sourceBodyIds()});
      node.occurrences.reserve(component->size());
      for (const auto& [occurrenceId, occurrence] :
           component->occurrences()) {
        if (occurrenceId != occurrence.id())
          return ComponentGraphStatus::UnsupportedComponentDefinition;
        node.occurrences.push_back(
            {occurrenceId, occurrence.targetDefinitionId()});
      }
    }
    return analyzeComponentGraph(graph);
  } catch (...) {
    return ComponentGraphStatus::ResourceFailure;
  }
}

ComponentGraphStatus analyzeComponentGraph(
    const ComponentGraphView& graph,
    ComponentGraphFailurePoint failurePoint) noexcept {
  try {
    if (graph.definitions.size() > kAssemblyGraphMaximumDefinitions)
      return ComponentGraphStatus::DefinitionBudgetExceeded;
    if (failurePoint ==
        ComponentGraphFailurePoint::BeforeValidationAllocation)
      return ComponentGraphStatus::ResourceFailure;
    if (!graph.definitions.contains(graph.rootDefinitionId))
      return ComponentGraphStatus::MissingRootDefinition;

    std::size_t authoredOccurrences = 0;
    std::size_t authoredDrawings = 0;
    std::size_t authoredDrawingBodyReferences = 0;
    std::map<DefinitionId, std::vector<DefinitionId>> edges;
    for (const auto& [id, definition] : graph.definitions) {
      if (!id.isValid())
        return ComponentGraphStatus::DuplicateDefinitionId;
      std::set<BodyId> bodyIds;
      for (const BodyId bodyId : definition.bodyIds)
        bodyIds.insert(bodyId);
      if (definition.drawings.size() >
          kComponentGraphMaximumDrawings -
              std::min(authoredDrawings,
                       kComponentGraphMaximumDrawings))
        return ComponentGraphStatus::DrawingBudgetExceeded;
      authoredDrawings += definition.drawings.size();
      std::set<DrawingId> drawingIds;
      for (const auto& drawing : definition.drawings) {
        if (!drawing.drawingId.isValid())
          return ComponentGraphStatus::InvalidDrawing;
        if (!drawingIds.insert(drawing.drawingId).second)
          return ComponentGraphStatus::DuplicateDrawingId;
        if (!std::is_sorted(drawing.sourceBodyIds.begin(),
                            drawing.sourceBodyIds.end()) ||
            std::adjacent_find(drawing.sourceBodyIds.begin(),
                               drawing.sourceBodyIds.end()) !=
                drawing.sourceBodyIds.end())
          return ComponentGraphStatus::InvalidDrawing;
        if (drawing.sourceBodyIds.size() >
            kComponentGraphMaximumDrawingBodyReferences -
                std::min(authoredDrawingBodyReferences,
                         kComponentGraphMaximumDrawingBodyReferences))
          return ComponentGraphStatus::DrawingBudgetExceeded;
        authoredDrawingBodyReferences += drawing.sourceBodyIds.size();
        for (const BodyId bodyId : drawing.sourceBodyIds)
          if (!bodyIds.contains(bodyId))
            return ComponentGraphStatus::DanglingDrawingBody;
      }
      if (!fitsAssemblyGraphBudget(
              authoredOccurrences, definition.occurrences.size(),
              kAssemblyGraphMaximumAuthoredOccurrences))
        return ComponentGraphStatus::OccurrenceBudgetExceeded;
      authoredOccurrences += definition.occurrences.size();
      auto& targets = edges[id];
      targets.reserve(definition.occurrences.size());
      for (const auto& occurrence : definition.occurrences) {
        if (!occurrence.occurrenceId.isValid())
          return ComponentGraphStatus::UnsupportedComponentDefinition;
        if (!graph.definitions.contains(occurrence.targetDefinitionId))
          return ComponentGraphStatus::DanglingTarget;
        targets.push_back(occurrence.targetDefinitionId);
      }
    }

    enum class Color : std::uint8_t { White, Gray, Black };
    std::map<DefinitionId, Color> colors;
    for (const auto& [id, definition] : graph.definitions) {
      static_cast<void>(definition);
      colors.emplace(id, Color::White);
    }
    struct DfsFrame final {
      DefinitionId id;
      std::size_t next{};
    };
    std::vector<DfsFrame> dfs;
    std::vector<DefinitionId> childFirstOrder;
    childFirstOrder.reserve(graph.definitions.size());
    for (const auto& [startId, definition] : graph.definitions) {
      static_cast<void>(definition);
      if (colors.at(startId) != Color::White) continue;
      colors.at(startId) = Color::Gray;
      dfs.push_back({startId, 0});
      while (!dfs.empty()) {
        auto& frame = dfs.back();
        const auto& targets = edges.at(frame.id);
        if (frame.next < targets.size()) {
          const DefinitionId target = targets[frame.next++];
          if (colors.at(target) == Color::Gray)
            return ComponentGraphStatus::Cycle;
          if (colors.at(target) == Color::White) {
            colors.at(target) = Color::Gray;
            dfs.push_back({target, 0});
          }
          continue;
        }
        colors.at(frame.id) = Color::Black;
        childFirstOrder.push_back(frame.id);
        dfs.pop_back();
      }
    }

    std::map<DefinitionId, std::size_t> depths;
    for (const DefinitionId id : childFirstOrder) {
      std::size_t maximumDepth = 0;
      for (const DefinitionId target : edges.at(id)) {
        const auto found = depths.find(target);
        if (found == depths.end() ||
            found->second >= kAssemblyGraphMaximumPathDepth)
          return ComponentGraphStatus::UnsupportedDepth;
        maximumDepth = std::max(maximumDepth, found->second + 1);
      }
      if (maximumDepth > kAssemblyGraphMaximumPathDepth)
        return ComponentGraphStatus::UnsupportedDepth;
      depths.emplace(id, maximumDepth);
    }

    std::size_t expandedVisits = 0;
    std::vector<DefinitionId> stack{graph.rootDefinitionId};
    while (!stack.empty()) {
      const DefinitionId current = stack.back();
      stack.pop_back();
      for (const DefinitionId target : edges.at(current)) {
        if (!fitsAssemblyGraphBudget(
                expandedVisits, 1,
                kAssemblyGraphMaximumExpandedVisits))
          return ComponentGraphStatus::ExpandedWorkBudgetExceeded;
        ++expandedVisits;
        stack.push_back(target);
      }
    }
    return ComponentGraphStatus::Valid;
  } catch (...) {
    return ComponentGraphStatus::ResourceFailure;
  }
}

ComponentGraphTraversal expandComponentGraph(
    const DefinitionRegistry& definitions,
    DefinitionId rootDefinitionId) noexcept {
  try {
    ComponentGraphTraversal result;
    result.graphStatus =
        analyzeComponentGraph(definitions, rootDefinitionId);
    if (result.graphStatus != ComponentGraphStatus::Valid) {
      result.status = ComponentGraphTraversalStatus::GraphRejected;
      return result;
    }

    struct Frame final {
      DefinitionId definitionId;
      std::vector<OccurrenceId> path;
      RigidTransform3d worldTransform{RigidTransform3d::identity()};
      bool effectiveVisible{true};
      bool effectiveSuppressed{false};
    };
    std::vector<Frame> stack;
    stack.push_back({rootDefinitionId, {}, RigidTransform3d::identity(),
                     true, false});
    result.visits.push_back(
        {rootDefinitionId, std::nullopt, RigidTransform3d::identity(),
         OccurrenceFlags{true, false, false}, true, false});
    while (!stack.empty()) {
      Frame current = std::move(stack.back());
      stack.pop_back();
      const auto* component =
          definitions.findComponent(current.definitionId);
      if (!component) {
        result.status = ComponentGraphTraversalStatus::GraphRejected;
        result.graphStatus =
            ComponentGraphStatus::UnsupportedComponentDefinition;
        result.visits.clear();
        return result;
      }
      for (auto it = component->occurrences().rbegin();
           it != component->occurrences().rend(); ++it) {
        const auto& occurrence = it->second;
        const auto composed = RigidTransform3d::compose(
            current.worldTransform, occurrence.placement());
        if (!composed) {
          result.status = ComponentGraphTraversalStatus::InvalidTransform;
          result.visits.clear();
          return result;
        }
        std::vector<OccurrenceId> path = current.path;
        path.push_back(it->first);
        auto occurrencePath = OccurrencePath::create(path);
        if (!occurrencePath) {
          result.status =
              ComponentGraphTraversalStatus::InvalidOccurrencePath;
          result.visits.clear();
          return result;
        }
        const auto flags = occurrence.flags();
        const bool visible = current.effectiveVisible && flags.visible;
        const bool suppressed =
            current.effectiveSuppressed || flags.suppressed;
        result.visits.push_back(
            {occurrence.targetDefinitionId(),
             std::move(occurrencePath.path), *composed.transform, flags,
             visible, suppressed});
        stack.push_back({occurrence.targetDefinitionId(), std::move(path),
                         *composed.transform, visible, suppressed});
      }
    }
    std::sort(result.visits.begin() + 1, result.visits.end(),
              [](const ComponentGraphVisit& left,
                 const ComponentGraphVisit& right) {
                return left.path->entries() < right.path->entries();
              });
    result.status = ComponentGraphTraversalStatus::Valid;
    return result;
  } catch (...) {
    return {};
  }
}

}  // namespace solidar
