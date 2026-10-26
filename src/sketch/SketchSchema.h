#pragma once

#include "sketch/Sketch.h"

#include <optional>
#include <vector>

namespace solidar::sketch {

enum class SketchSchemaErrorCode {
  InvalidGeometryId,
  DuplicateGeometryId,
  InvalidConstraintId,
  DuplicateConstraintId,
  InvalidDimensionId,
  DuplicateDimensionId,
  StaleGeometryReference,
  WrongGeometryKind,
  InvalidPointReference,
  InvalidParameterRange,
  SelfReference,
  UnsupportedConstraintType,
  UnsupportedConstraintMode,
  StaleConstraintReference,
};

struct SketchSchemaError {
  SketchSchemaErrorCode code{SketchSchemaErrorCode::InvalidGeometryId};
  std::vector<GeometryId> geometryIds;
  std::vector<ConstraintId> constraintIds;
  std::vector<DimensionId> dimensionIds;
};

struct SketchSchemaValidation {
  std::optional<SketchSchemaError> error;
  [[nodiscard]] bool valid() const noexcept { return !error.has_value(); }
  explicit operator bool() const noexcept { return valid(); }
};

// This is the sole structural validation boundary for materialized sketch
// model data.  Parsing a historical JSON shape is a codec concern; once IDs
// have been resolved, every loader, mutator and solver uses these functions.
[[nodiscard]] SketchSchemaValidation validatePointReferenceSchema(
    const Sketch& sketch, const PointReference& reference);
[[nodiscard]] SketchSchemaValidation validateConstraintSchema(
    const Sketch& sketch, const Constraint& constraint);
[[nodiscard]] SketchSchemaValidation validateSketchSchema(
    const Sketch& sketch);

}  // namespace solidar::sketch
