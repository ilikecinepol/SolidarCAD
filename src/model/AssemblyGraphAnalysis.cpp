#include "model/AssemblyGraphAnalysis.h"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace solidar {
namespace {

bool checkedAdd(std::size_t current, std::size_t delta,
                std::size_t limit, std::size_t* result) noexcept {
  if (!result || !fitsAssemblyGraphBudget(current, delta, limit)) return false;
  *result = current + delta;
  return true;
}

AssemblyGraphAnalysis failed(AssemblyGraphStatus status) noexcept {
  AssemblyGraphAnalysis result;
  result.status = status;
  return result;
}

}  // namespace

AssemblyGraphView makeAssemblyGraphView(const DefinitionRegistry& registry,
                                        DefinitionId rootDefinitionId) {
  AssemblyGraphView result;
  result.rootDefinitionId = rootDefinitionId;
  for (const DefinitionId id : registry.definitionIds()) {
    AssemblyGraphDefinition node;
    if (registry.findPart(id)) {
      node.kind = DefinitionKind::Part;
    } else if (const auto* assembly = registry.findAssembly(id)) {
      node.kind = DefinitionKind::Assembly;
      node.occurrences.reserve(assembly->size());
      for (const auto& [occurrenceId, occurrence] : assembly->occurrences())
        node.occurrences.push_back(
            {occurrenceId, occurrence.targetDefinitionId(),
             occurrence.placement(), occurrence.flags(),
             occurrence.displayNameUtf8()});
    } else {
      node.kind = DefinitionKind::Component;
    }
    result.definitions.emplace(id, std::move(node));
  }
  return result;
}

AssemblyGraphAnalysis analyzeAssemblyGraph(
    const DefinitionRegistry& registry,
    DefinitionId rootDefinitionId) noexcept {
  try {
    if (registry.size() > kAssemblyGraphMaximumDefinitions)
      return failed(AssemblyGraphStatus::DefinitionBudgetExceeded);
    const auto ids = registry.definitionIds();
    std::size_t occurrences = 0;
    std::size_t sources = 0;
    for (const DefinitionId id : ids) {
      if (registry.findPart(id)) {
        if (++sources > kAssemblyGraphMaximumUniqueSources)
          return failed(AssemblyGraphStatus::UniqueSourceBudgetExceeded);
        continue;
      }
      const auto* assembly = registry.findAssembly(id);
      if (!assembly)
        return failed(AssemblyGraphStatus::UnsupportedDefinitionKind);
      for (const auto& [occurrenceId, occurrence] : assembly->occurrences()) {
        if (occurrenceId != occurrence.id())
          return failed(AssemblyGraphStatus::InvalidOccurrenceId);
      }
      if (!checkedAdd(occurrences, assembly->size(),
                      kAssemblyGraphMaximumAuthoredOccurrences,
                      &occurrences))
        return failed(AssemblyGraphStatus::OccurrenceBudgetExceeded);
    }
    return analyzeAssemblyGraph(makeAssemblyGraphView(registry,
                                                       rootDefinitionId));
  } catch (...) {
    return failed(AssemblyGraphStatus::ResourceFailure);
  }
}

AssemblyGraphAnalysis analyzeAssemblyGraph(
    const AssemblyGraphView& graph,
    AssemblyGraphFailurePoint failurePoint) noexcept {
  try {
    if (!graph.rootDefinitionId.isValid())
      return failed(AssemblyGraphStatus::InvalidRoot);
    if (graph.definitions.size() > kAssemblyGraphMaximumDefinitions)
      return failed(AssemblyGraphStatus::DefinitionBudgetExceeded);
    const auto root = graph.definitions.find(graph.rootDefinitionId);
    if (root == graph.definitions.end())
      return failed(AssemblyGraphStatus::InvalidRoot);
    if (root->second.kind != DefinitionKind::Assembly)
      return failed(AssemblyGraphStatus::RootIsNotAssembly);

    AssemblyGraphAnalysis result;
    std::size_t occurrenceTotal = 0;
    std::size_t partCount = 0;
    for (const auto& [definitionId, definition] : graph.definitions) {
      if (!definitionId.isValid())
        return failed(AssemblyGraphStatus::InvalidDefinitionId);
      if (definition.kind == DefinitionKind::Part) {
        ++partCount;
        if (!definition.occurrences.empty())
          return failed(AssemblyGraphStatus::DanglingTarget);
        continue;
      }
      if (definition.kind != DefinitionKind::Assembly)
        return failed(AssemblyGraphStatus::UnsupportedDefinitionKind);
      if (!checkedAdd(occurrenceTotal, definition.occurrences.size(),
                      kAssemblyGraphMaximumAuthoredOccurrences,
                      &occurrenceTotal))
        return failed(AssemblyGraphStatus::OccurrenceBudgetExceeded);
      OccurrenceId previous;
      for (const auto& occurrence : definition.occurrences) {
        if (!occurrence.occurrenceId.isValid())
          return failed(AssemblyGraphStatus::InvalidOccurrenceId);
        if (previous.isValid() &&
            occurrence.occurrenceId.value() <= previous.value())
          return failed(AssemblyGraphStatus::DuplicateOccurrenceId);
        previous = occurrence.occurrenceId;
        if (!graph.definitions.contains(occurrence.targetDefinitionId))
          return failed(AssemblyGraphStatus::DanglingTarget);
      }
    }
    if (partCount > kAssemblyGraphMaximumUniqueSources)
      return failed(AssemblyGraphStatus::UniqueSourceBudgetExceeded);
    result.authoredOccurrences = occurrenceTotal;
    result.uniqueSources = partCount;

    // Validate every component, including orphans. A gray edge is a true
    // active-stack cycle; black nodes may be shared by an arbitrary DAG.
    enum class Color : std::uint8_t { White, Gray, Black };
    std::map<DefinitionId, Color> colors;
    for (const auto& [id, definition] : graph.definitions) {
      static_cast<void>(definition);
      colors.emplace(id, Color::White);
    }
    struct DfsFrame { DefinitionId id; std::size_t next{}; };
    std::vector<DfsFrame> dfs;
    for (const auto& [startId, startDefinition] : graph.definitions) {
      if (startDefinition.kind != DefinitionKind::Assembly ||
          colors.at(startId) != Color::White)
        continue;
      colors[startId] = Color::Gray;
      dfs.push_back({startId, 0});
      while (!dfs.empty()) {
        auto& frame = dfs.back();
        const auto& definition = graph.definitions.at(frame.id);
        bool descended = false;
        while (frame.next < definition.occurrences.size()) {
          const DefinitionId target =
              definition.occurrences[frame.next++].targetDefinitionId;
          const auto& targetDefinition = graph.definitions.at(target);
          if (targetDefinition.kind == DefinitionKind::Component)
            return failed(AssemblyGraphStatus::UnsupportedDefinitionKind);
          if (targetDefinition.kind != DefinitionKind::Assembly) continue;
          if (colors.at(target) == Color::Gray)
            return failed(AssemblyGraphStatus::Cycle);
          if (colors.at(target) == Color::White) {
            colors[target] = Color::Gray;
            dfs.push_back({target, 0});
            descended = true;
            break;
          }
        }
        if (descended) continue;
        colors[frame.id] = Color::Black;
        result.childFirstAssemblyOrder.push_back(frame.id);
        dfs.pop_back();
      }
    }

    // Depth is a graph-wide persistence invariant, including disconnected
    // orphan subassemblies. Child-first order makes this a bounded iterative
    // dynamic program rather than recursive descent.
    std::map<DefinitionId, std::size_t> assemblyDepths;
    for (const DefinitionId id : result.childFirstAssemblyOrder) {
      std::size_t maximumDepth = 0;
      for (const auto& occurrence : graph.definitions.at(id).occurrences) {
        std::size_t targetDepth = 1;
        if (graph.definitions.at(occurrence.targetDefinitionId).kind ==
            DefinitionKind::Assembly) {
          const auto found = assemblyDepths.find(
              occurrence.targetDefinitionId);
          if (found == assemblyDepths.end() ||
              found->second >= kAssemblyGraphMaximumPathDepth)
            return failed(AssemblyGraphStatus::UnsupportedDepth);
          targetDepth += found->second;
        }
        maximumDepth = std::max(maximumDepth, targetDepth);
      }
      if (maximumDepth > kAssemblyGraphMaximumPathDepth)
        return failed(AssemblyGraphStatus::UnsupportedDepth);
      assemblyDepths.emplace(id, maximumDepth);
    }

    struct ExpansionFrame {
      DefinitionId definitionId;
      std::vector<OccurrenceId> path;
      RigidTransform3d transform{RigidTransform3d::identity()};
      bool visible{true};
      bool suppressed{false};
    };
    std::vector<ExpansionFrame> stack;
    stack.push_back({graph.rootDefinitionId, {},
                     RigidTransform3d::identity(), true, false});
    std::set<DefinitionId> visitedParts;
    while (!stack.empty()) {
      ExpansionFrame current = std::move(stack.back());
      stack.pop_back();
      const auto& assembly = graph.definitions.at(current.definitionId);
      for (auto iterator = assembly.occurrences.rbegin();
           iterator != assembly.occurrences.rend(); ++iterator) {
        std::size_t work = 0;
        if (!checkedAdd(result.expandedVisits, 1,
                        kAssemblyGraphMaximumExpandedVisits, &work))
          return failed(AssemblyGraphStatus::ExpandedWorkBudgetExceeded);
        result.expandedVisits = work;
        if (current.path.size() >= kAssemblyGraphMaximumPathDepth)
          return failed(AssemblyGraphStatus::UnsupportedDepth);
        const auto composed = RigidTransform3d::compose(current.transform,
                                                        iterator->placement);
        if (!composed)
          return failed(AssemblyGraphStatus::InvalidTransform);
        const bool visible = current.visible && iterator->flags.visible;
        const bool suppressed = current.suppressed || iterator->flags.suppressed;
        const auto& target = graph.definitions.at(iterator->targetDefinitionId);
        if (target.kind == DefinitionKind::Part &&
            result.leaves.size() >= kAssemblyGraphMaximumLeafInstances)
          return failed(AssemblyGraphStatus::LeafBudgetExceeded);
        if (target.kind == DefinitionKind::Part &&
            failurePoint ==
                AssemblyGraphFailurePoint::BeforeLeafPathAllocation)
          return failed(AssemblyGraphStatus::ResourceFailure);
        std::vector<OccurrenceId> path = current.path;
        path.push_back(iterator->occurrenceId);
        auto occurrencePath = OccurrencePath::create(path);
        if (!occurrencePath)
          return failed(occurrencePath.status ==
                                OccurrencePathStatus::UnsupportedDepth
                            ? AssemblyGraphStatus::UnsupportedDepth
                            : AssemblyGraphStatus::InvalidOccurrenceId);
        result.visits.push_back(
            {*occurrencePath.path, current.definitionId,
             iterator->targetDefinitionId, target.kind, iterator->placement,
             *composed.transform, iterator->flags, iterator->displayNameUtf8,
             visible, suppressed});
        if (target.kind == DefinitionKind::Assembly) {
          stack.push_back({iterator->targetDefinitionId, std::move(path),
                           *composed.transform, visible, suppressed});
          continue;
        }
        visitedParts.insert(iterator->targetDefinitionId);
        result.leaves.push_back(
            {std::move(*occurrencePath.path), iterator->targetDefinitionId,
             *composed.transform, iterator->flags, visible, suppressed});
      }
    }
    if (visitedParts.size() > kAssemblyGraphMaximumUniqueSources)
      return failed(AssemblyGraphStatus::UniqueSourceBudgetExceeded);
    result.reachableSources = visitedParts.size();
    // Reverse-stack expansion visits roots in lexicographic occurrence order;
    // enforce this invariant independently of implementation details.
    std::sort(result.leaves.begin(), result.leaves.end(),
              [](const AssemblyGraphLeaf& left,
                 const AssemblyGraphLeaf& right) {
                return left.path.entries() < right.path.entries();
              });
    std::sort(result.visits.begin(), result.visits.end(),
              [](const AssemblyGraphVisit& left,
                 const AssemblyGraphVisit& right) {
                return left.path.entries() < right.path.entries();
              });
    result.status = AssemblyGraphStatus::Valid;
    return result;
  } catch (...) {
    return failed(AssemblyGraphStatus::ResourceFailure);
  }
}

}  // namespace solidar
