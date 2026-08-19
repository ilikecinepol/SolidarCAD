#include "sketch/SketchSolver.h"

#include "sketch/Sketch.h"

#include <algorithm>

namespace solidar::sketch {

SolveResult BasicSketchSolver::solve(Sketch& sketch) {
  SolveResult result;

  // Resolve point coincidence first. H/V constraints are applied afterwards,
  // so an orthogonal line can move the whole coincident endpoint cluster.
  for (const auto& constraint : sketch.constraints()) {
    if (constraint.type != ConstraintType::Coincident) continue;

    const auto validPointReference =
        [&sketch](PointReference reference) {
      if (reference.circleId != kInvalidGeometryId)
        return sketch.circleIndex(reference.circleId).has_value();

      return reference.lineId != kInvalidGeometryId &&
             sketch.lineIndex(reference.lineId).has_value();
    };

    if (!validPointReference(constraint.firstPoint) ||
        !validPointReference(constraint.secondPoint)) {
      ++result.invalidReferences;
      continue;
    }

    if (sketch.setPointsCoincident(constraint.firstPoint,
                                   constraint.secondPoint))
      ++result.applied;
    else
      ++result.invalidReferences;
  }

  for (const auto& constraint : sketch.constraints()) {
    switch (constraint.type) {
      case ConstraintType::Horizontal:
        if (constraint.firstGeometry == kInvalidGeometryId ||
            !sketch.lineIndex(constraint.firstGeometry)) {
          ++result.invalidReferences;
          break;
        }
        if (sketch.setLineHorizontalById(constraint.firstGeometry))
          ++result.applied;
        else
          ++result.invalidReferences;
        break;

      case ConstraintType::Vertical:
        if (constraint.firstGeometry == kInvalidGeometryId ||
            !sketch.lineIndex(constraint.firstGeometry)) {
          ++result.invalidReferences;
          break;
        }
        if (sketch.setLineVerticalById(constraint.firstGeometry))
          ++result.applied;
        else
          ++result.invalidReferences;
        break;

      case ConstraintType::Length:
        if (constraint.firstGeometry == kInvalidGeometryId ||
            !sketch.lineIndex(constraint.firstGeometry) ||
            constraint.value <= 0.0) {
          ++result.invalidReferences;
          break;
        }
        if (sketch.setLineLengthById(constraint.firstGeometry,
                                     constraint.value))
          ++result.applied;
        else
          ++result.invalidReferences;
        break;

      case ConstraintType::Distance:
      case ConstraintType::DistanceX:
      case ConstraintType::DistanceY:
        if (constraint.firstPoint.lineId == kInvalidGeometryId ||
            constraint.secondPoint.lineId == kInvalidGeometryId ||
            !sketch.lineIndex(constraint.firstPoint.lineId) ||
            !sketch.lineIndex(constraint.secondPoint.lineId) ||
            constraint.value <= 0.0) {
          ++result.invalidReferences;
          break;
        }

        if ((constraint.type == ConstraintType::Distance &&
             sketch.setPointDistance(constraint.firstPoint,
                                     constraint.secondPoint,
                                     constraint.value)) ||
            (constraint.type == ConstraintType::DistanceX &&
             sketch.setPointDistanceX(constraint.firstPoint,
                                      constraint.secondPoint,
                                      constraint.value)) ||
            (constraint.type == ConstraintType::DistanceY &&
             sketch.setPointDistanceY(constraint.firstPoint,
                                      constraint.secondPoint,
                                      constraint.value)))
          ++result.applied;
        else
          ++result.invalidReferences;
        break;

      case ConstraintType::Angle:
        // Applied in a final pass below. This simple solver is sequential,
        // so angular driving constraints must be last to keep orientation.
        break;
      case ConstraintType::Diameter:
        if (constraint.firstGeometry == kInvalidGeometryId ||
            !sketch.circleIndex(constraint.firstGeometry) ||
            constraint.value <= 0.0) {
          ++result.invalidReferences;
          break;
        }

        if (sketch.setCircleDiameterById(constraint.firstGeometry,
                                         constraint.value))
          ++result.applied;
        else
          ++result.invalidReferences;
        break;

      case ConstraintType::Coincident:
        // Already handled in the first pass.
        break;

      default:
        ++result.unsupported;
        break;
    }
  }

  // Final angular pass. Angle is orientation-sensitive and can otherwise be
  // overwritten by constraints that happen to appear later in the vector.
  for (const auto& constraint : sketch.constraints()) {
    if (constraint.type != ConstraintType::Angle) continue;

    if (constraint.firstGeometry == kInvalidGeometryId ||
        constraint.secondGeometry == kInvalidGeometryId ||
        !sketch.lineIndex(constraint.firstGeometry) ||
        !sketch.lineIndex(constraint.secondGeometry) ||
        constraint.firstGeometry == constraint.secondGeometry ||
        constraint.value <= 0.0 || constraint.value >= 180.0) {
      ++result.invalidReferences;
      continue;
    }

    const auto hasOrthogonalConstraint =
        [&sketch](GeometryId id) {
          return std::any_of(
              sketch.constraints().begin(), sketch.constraints().end(),
              [id](const Constraint& item) {
                return item.firstGeometry == id &&
                       (item.type == ConstraintType::Horizontal ||
                        item.type == ConstraintType::Vertical);
              });
        };

    const bool firstOrthogonal =
        hasOrthogonalConstraint(constraint.firstGeometry);
    const bool secondOrthogonal =
        hasOrthogonalConstraint(constraint.secondGeometry);

    // H/V is a stronger orientation constraint than Angle in this simple
    // sequential solver. If the second line is H/V-fixed, rotate the first
    // line instead. If both are fixed, keep both H/V constraints intact.
    if (firstOrthogonal && secondOrthogonal) {
      continue;
    }

    const GeometryId referenceGeometry =
        secondOrthogonal && !firstOrthogonal
            ? constraint.secondGeometry
            : constraint.firstGeometry;
    const GeometryId movingGeometry =
        secondOrthogonal && !firstOrthogonal
            ? constraint.firstGeometry
            : constraint.secondGeometry;

    if (sketch.setLineAngleByIds(referenceGeometry,
                                 movingGeometry,
                                 constraint.value))
      ++result.applied;
    else
      ++result.invalidReferences;
  }

  return result;
}

}  // namespace solidar::sketch