#pragma once

#include <map>
#include <optional>
#include <vector>

#include "model/ProductDocument.h"

namespace solidar {

enum class ComponentGraphStatus {
  Valid,
  DuplicateDefinitionId,
  MissingRootDefinition,
  UnsupportedComponentDefinition,
  DanglingTarget,
  Cycle,
  DefinitionBudgetExceeded,
  OccurrenceBudgetExceeded,
  InvalidDrawing,
  DuplicateDrawingId,
  DanglingDrawingBody,
  DrawingBudgetExceeded,
  UnsupportedDepth,
  ExpandedWorkBudgetExceeded,
  ResourceFailure,
};

enum class ComponentGraphFailurePoint {
  None,
  BeforeValidationAllocation,
};

struct ComponentGraphOccurrence final {
  OccurrenceId occurrenceId;
  DefinitionId targetDefinitionId;

  friend bool operator==(const ComponentGraphOccurrence&,
                         const ComponentGraphOccurrence&) = default;
};

struct ComponentGraphDrawing final {
  DrawingId drawingId;
  std::vector<BodyId> sourceBodyIds;

  friend bool operator==(const ComponentGraphDrawing&,
                         const ComponentGraphDrawing&) = default;
};

struct ComponentGraphDefinition final {
  std::vector<BodyId> bodyIds;
  std::vector<ComponentGraphDrawing> drawings;
  std::vector<ComponentGraphOccurrence> occurrences;

  friend bool operator==(const ComponentGraphDefinition&,
                         const ComponentGraphDefinition&) = default;
};

struct ComponentGraphView final {
  DefinitionId rootDefinitionId;
  std::map<DefinitionId, ComponentGraphDefinition> definitions;

  friend bool operator==(const ComponentGraphView&,
                         const ComponentGraphView&) = default;
};

[[nodiscard]] ComponentGraphStatus analyzeComponentGraph(
    const ComponentGraphView& graph,
    ComponentGraphFailurePoint failurePoint =
        ComponentGraphFailurePoint::None) noexcept;

// Qt-free, allocation-contained validation of a complete component graph.
// Every authored definition is validated, including definitions unreachable
// from the selected root. Expanded-work accounting starts at the root only.
[[nodiscard]] ComponentGraphStatus analyzeComponentGraph(
    const std::vector<ComponentDefinition>& definitions,
    DefinitionId rootDefinitionId,
    ComponentGraphFailurePoint failurePoint =
        ComponentGraphFailurePoint::None) noexcept;

[[nodiscard]] ComponentGraphStatus analyzeComponentGraph(
    const DefinitionRegistry& definitions,
    DefinitionId rootDefinitionId) noexcept;

struct ComponentGraphVisit final {
  DefinitionId definitionId;
  std::optional<OccurrencePath> path;
  RigidTransform3d worldTransform{RigidTransform3d::identity()};
  OccurrenceFlags authoredFlags;
  bool effectiveVisible{true};
  bool effectiveSuppressed{false};

  [[nodiscard]] bool rootInstance() const noexcept { return !path; }
  friend bool operator==(const ComponentGraphVisit&,
                         const ComponentGraphVisit&) = default;
};

enum class ComponentGraphTraversalStatus {
  Valid,
  GraphRejected,
  InvalidTransform,
  InvalidOccurrencePath,
  ResourceFailure,
};

struct ComponentGraphTraversal final {
  ComponentGraphTraversalStatus status{
      ComponentGraphTraversalStatus::ResourceFailure};
  ComponentGraphStatus graphStatus{ComponentGraphStatus::ResourceFailure};
  std::vector<ComponentGraphVisit> visits;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentGraphTraversalStatus::Valid;
  }
};

// Deterministic expanded component-instance traversal shared by presentation
// and future tree projections. The root is visit zero after sorting and has no
// occurrence path; every descendant carries its full stable path.
[[nodiscard]] ComponentGraphTraversal expandComponentGraph(
    const DefinitionRegistry& definitions,
    DefinitionId rootDefinitionId) noexcept;

}  // namespace solidar
