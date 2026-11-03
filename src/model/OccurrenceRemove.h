#pragma once

#include <string>

#include "model/OccurrenceDuplicate.h"

namespace solidar {

// Cold structural payload for removing/restoring exactly one flat root
// occurrence. Definitions and their Documents remain owned by ProductDocument.
struct RemoveOccurrenceColdPayload final {
  RootOccurrenceAddress occurrence;
  RigidTransform3d placement{RigidTransform3d::identity()};
  OccurrenceFlags flags{};
  std::string displayNameUtf8;
  RigidTransform3d homePlacement{RigidTransform3d::identity()};

  friend bool operator==(const RemoveOccurrenceColdPayload&,
                         const RemoveOccurrenceColdPayload&) = default;
};

enum class RemoveOccurrenceStatus {
  Removed,
  Restored,
  Busy,
  InvalidPayload,
  ProductIncarnationMismatch,
  RootDefinitionMismatch,
  RootIsNotAssembly,
  UnsupportedPathDepth,
  OccurrenceMissing,
  TargetDefinitionMismatch,
  SnapshotMismatch,
  TargetPartMissing,
  OccurrenceIdCollision,
  OccurrenceLimit,
  AllocationFailure,
  NodeFailure,
  SuccessorRejected,
  SuccessorResourceFailure,
};

// Deterministic proof seams. Production callers always pass None.
enum class RemoveOccurrenceFailurePoint {
  None,
  NodeExtract,
  OccurrenceConstruction,
  NodeAllocation,
  NodeCommit,
};

}  // namespace solidar
