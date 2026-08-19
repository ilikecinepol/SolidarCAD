#include "sketch/SketchSolver.h"

#include "sketch/Sketch.h"

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

  return result;
}

}  // namespace solidar::sketch