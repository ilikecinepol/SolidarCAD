#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "model/ProductDocument.h"

namespace solidar {

inline constexpr std::size_t kAssemblyGraphMaximumDefinitions = 256;
inline constexpr std::size_t kAssemblyGraphMaximumAuthoredOccurrences = 4096;
inline constexpr std::size_t kAssemblyGraphMaximumPathDepth = 32;
inline constexpr std::size_t kAssemblyGraphMaximumExpandedVisits = 65536;
inline constexpr std::size_t kAssemblyGraphMaximumLeafInstances = 32768;
inline constexpr std::size_t kAssemblyGraphMaximumUniqueSources = 128;

[[nodiscard]] constexpr bool fitsAssemblyGraphBudget(
    std::size_t current, std::size_t delta, std::size_t limit) noexcept {
  return current <= limit && delta <= limit - current;
}

struct AssemblyGraphOccurrence final {
  OccurrenceId occurrenceId;
  DefinitionId targetDefinitionId;
  RigidTransform3d placement{RigidTransform3d::identity()};
  OccurrenceFlags flags;
  std::string displayNameUtf8;
};

struct AssemblyGraphDefinition final {
  DefinitionKind kind{DefinitionKind::Part};
  std::vector<AssemblyGraphOccurrence> occurrences;
};

struct AssemblyGraphView final {
  DefinitionId rootDefinitionId;
  std::map<DefinitionId, AssemblyGraphDefinition> definitions;
};

enum class AssemblyGraphStatus {
  Valid,
  InvalidRoot,
  RootIsNotAssembly,
  UnsupportedDefinitionKind,
  InvalidDefinitionId,
  InvalidOccurrenceId,
  DuplicateOccurrenceId,
  DanglingTarget,
  Cycle,
  DefinitionBudgetExceeded,
  OccurrenceBudgetExceeded,
  UnsupportedDepth,
  ExpandedWorkBudgetExceeded,
  LeafBudgetExceeded,
  UniqueSourceBudgetExceeded,
  InvalidTransform,
  ResourceFailure,
};

struct AssemblyGraphLeaf final {
  OccurrencePath path;
  DefinitionId terminalPartDefinitionId;
  RigidTransform3d worldTransform{RigidTransform3d::identity()};
  OccurrenceFlags authoredLeafFlags;
  bool effectiveVisible{true};
  bool effectiveSuppressed{false};

  friend bool operator==(const AssemblyGraphLeaf&,
                         const AssemblyGraphLeaf&) = default;
};

// One deterministic expanded occurrence visit.  Unlike a leaf, a visit also
// represents an intermediate subassembly occurrence.  It is the shared
// identity/transform source for presentation, viewport-session and tree
// projections; those layers must not reimplement graph traversal.
struct AssemblyGraphVisit final {
  OccurrencePath path;
  DefinitionId owningAssemblyDefinitionId;
  DefinitionId targetDefinitionId;
  DefinitionKind targetKind{DefinitionKind::Part};
  RigidTransform3d localTransform{RigidTransform3d::identity()};
  RigidTransform3d worldTransform{RigidTransform3d::identity()};
  OccurrenceFlags authoredFlags;
  std::string displayNameUtf8;
  bool effectiveVisible{true};
  bool effectiveSuppressed{false};

  [[nodiscard]] bool terminalPart() const noexcept {
    return targetKind == DefinitionKind::Part;
  }
  friend bool operator==(const AssemblyGraphVisit&,
                         const AssemblyGraphVisit&) = default;
};

struct AssemblyGraphAnalysis final {
  AssemblyGraphStatus status{AssemblyGraphStatus::ResourceFailure};
  std::vector<DefinitionId> childFirstAssemblyOrder;
  std::vector<AssemblyGraphVisit> visits;
  std::vector<AssemblyGraphLeaf> leaves;
  std::size_t authoredOccurrences{};
  std::size_t expandedVisits{};
  std::size_t uniqueSources{};
  std::size_t reachableSources{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == AssemblyGraphStatus::Valid;
  }
  friend bool operator==(const AssemblyGraphAnalysis&,
                         const AssemblyGraphAnalysis&) = default;
};

enum class AssemblyGraphFailurePoint {
  None,
  BeforeLeafPathAllocation,
};

// Single Qt-free validation and expansion boundary shared by the model and
// persistence scanners. All traversal is iterative and deterministic.
[[nodiscard]] AssemblyGraphAnalysis analyzeAssemblyGraph(
    const AssemblyGraphView& graph,
    AssemblyGraphFailurePoint failurePoint =
        AssemblyGraphFailurePoint::None) noexcept;

// Registry overload performs checked raw counts before cloning occurrence
// descriptors into the detached analysis view.
[[nodiscard]] AssemblyGraphAnalysis analyzeAssemblyGraph(
    const DefinitionRegistry& definitions,
    DefinitionId rootDefinitionId) noexcept;

[[nodiscard]] AssemblyGraphView makeAssemblyGraphView(
    const DefinitionRegistry& definitions,
    DefinitionId rootDefinitionId);

}  // namespace solidar
