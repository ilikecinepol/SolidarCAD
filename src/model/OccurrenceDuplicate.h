#pragma once

#include <cstddef>
#include <string>

#include "model/OccurrenceDelta.h"

namespace solidar {

inline constexpr std::size_t kMaximumRootOccurrences = 4096;

// Cold structural payload for one flat root occurrence duplication. It owns
// only stable authored values; no live map node, Document, B-Rep, scene or UI
// object crosses this boundary.
struct DuplicateOccurrenceColdPayload final {
  RootOccurrenceAddress source;
  OccurrenceId duplicateId;
  DefinitionId targetDefinitionId;
  RigidTransform3d placement{RigidTransform3d::identity()};
  RigidTransform3d homePlacement{RigidTransform3d::identity()};
  OccurrenceFlags flags{};
  std::string displayNameUtf8;

  friend bool operator==(const DuplicateOccurrenceColdPayload&,
                         const DuplicateOccurrenceColdPayload&) = default;
};

enum class DuplicateOccurrenceStatus {
  Committed,
  Extracted,
  Busy,
  InvalidPayload,
  ProductIncarnationMismatch,
  RootDefinitionMismatch,
  RootIsNotAssembly,
  UnsupportedPathDepth,
  SourceMissing,
  TargetDefinitionMismatch,
  SourceSnapshotMismatch,
  TargetPartMissing,
  DuplicateIdCollision,
  DuplicateMissing,
  DuplicateGuardMismatch,
  OccurrenceLimit,
  AllocationFailure,
  NodeFailure,
  SuccessorRejected,
  SuccessorResourceFailure,
};

// Deterministic proof seams. Production callers always pass None.
enum class DuplicateOccurrenceFailurePoint {
  None,
  OccurrenceConstruction,
  NodeAllocation,
  NodeCommit,
  NodeExtract,
};

}  // namespace solidar
