#include "sketch/SketchSolver.h"

#include "sketch/Sketch.h"

#include <algorithm>
#include <cmath>

namespace solidar::sketch {

SolveResult BasicSketchSolver::solve(Sketch& sketch) {
  SolveResult result;

  // Resolve point coincidence first. H/V constraints are applied afterwards,
  // so an orthogonal line can move the whole coincident endpoint cluster.
  for (const auto& constraint : sketch.constraints()) {
    if (constraint.type != ConstraintType::Coincident) continue;

    const auto validPointReference =
        [&sketch](PointReference reference) {
      return sketch.referencedPoint(reference).has_value();
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
      case ConstraintType::Perpendicular:
      case ConstraintType::Parallel:
      case ConstraintType::Equal:
        // Applied in final relationship passes below. This simple solver is
        // sequential, so relative geometry constraints must be last.
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

      case ConstraintType::PointOnLine:
        // Applied in the final relationship/stabilization passes so line
        // orientation and rectangle geometry are already settled.
        break;

      case ConstraintType::PointOnCircle:
        // Applied after carrier geometry has settled.
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

  // Perpendicular uses the same rotation primitive as Angle, but always with
  // 90 degrees. Preserve an existing H/V-fixed line by rotating the other.
  for (const auto& constraint : sketch.constraints()) {
    if (constraint.type != ConstraintType::Perpendicular) continue;

    if (constraint.firstGeometry == kInvalidGeometryId ||
        constraint.secondGeometry == kInvalidGeometryId ||
        !sketch.lineIndex(constraint.firstGeometry) ||
        !sketch.lineIndex(constraint.secondGeometry) ||
        constraint.firstGeometry == constraint.secondGeometry) {
      ++result.invalidReferences;
      continue;
    }

    const auto orthogonalType =
        [&sketch](GeometryId id)
            -> std::optional<ConstraintType> {
          for (const auto& item : sketch.constraints()) {
            if (item.firstGeometry != id) continue;
            if (item.type == ConstraintType::Horizontal ||
                item.type == ConstraintType::Vertical)
              return item.type;
          }
          return std::nullopt;
        };

    const auto firstOrthogonal =
        orthogonalType(constraint.firstGeometry);
    const auto secondOrthogonal =
        orthogonalType(constraint.secondGeometry);

    if (firstOrthogonal && secondOrthogonal) {
      if (*firstOrthogonal != *secondOrthogonal)
        ++result.applied;
      else
        ++result.unsupported;
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
                                 90.0))
      ++result.applied;
    else
      ++result.invalidReferences;
  }
  // Parallel is another relative orientation constraint. Keep an H/V-fixed
  // line as reference when only one side has an absolute orientation.
  for (const auto& constraint : sketch.constraints()) {
    if (constraint.type != ConstraintType::Parallel) continue;

    if (constraint.firstGeometry == kInvalidGeometryId ||
        constraint.secondGeometry == kInvalidGeometryId ||
        !sketch.lineIndex(constraint.firstGeometry) ||
        !sketch.lineIndex(constraint.secondGeometry) ||
        constraint.firstGeometry == constraint.secondGeometry) {
      ++result.invalidReferences;
      continue;
    }

    const auto orthogonalType =
        [&sketch](GeometryId id)
            -> std::optional<ConstraintType> {
          for (const auto& item : sketch.constraints()) {
            if (item.firstGeometry != id) continue;

            if (item.type == ConstraintType::Horizontal ||
                item.type == ConstraintType::Vertical)
              return item.type;
          }

          return std::nullopt;
        };

    const auto firstOrthogonal =
        orthogonalType(constraint.firstGeometry);
    const auto secondOrthogonal =
        orthogonalType(constraint.secondGeometry);

    if (firstOrthogonal && secondOrthogonal) {
      if (*firstOrthogonal == *secondOrthogonal)
        ++result.applied;
      else
        ++result.unsupported;
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

    if (sketch.setLinesParallelByIds(referenceGeometry,
                                     movingGeometry))
      ++result.applied;
    else
      ++result.invalidReferences;
  }
  // Equal makes two primitives of the same kind share size.
  //
  // A driving AutoDimension has priority regardless of click order. Direct
  // line dimensions are currently represented either by Length or by a
  // Distance/DistanceX/DistanceY between the two endpoints of the same line.
  for (const auto& constraint : sketch.constraints()) {
    if (constraint.type != ConstraintType::Equal) continue;

    if (constraint.firstGeometry == kInvalidGeometryId ||
        constraint.secondGeometry == kInvalidGeometryId ||
        constraint.firstGeometry == constraint.secondGeometry) {
      ++result.invalidReferences;
      continue;
    }

    const auto firstLine =
        sketch.lineIndex(constraint.firstGeometry);
    const auto secondLine =
        sketch.lineIndex(constraint.secondGeometry);

    if (firstLine && secondLine) {
      const auto drivingLineSize =
          [&sketch](GeometryId id)
              -> std::optional<double> {
        const auto index = sketch.lineIndex(id);
        if (!index) return std::nullopt;

        const auto& line = sketch.lines()[*index];
        const double dx =
            std::abs(line.end.xMm - line.start.xMm);
        const double dy =
            std::abs(line.end.yMm - line.start.yMm);

        const auto isOwnEndpointPair =
            [id](const Constraint& item) {
              if (item.firstPoint.circleId !=
                      kInvalidGeometryId ||
                  item.secondPoint.circleId !=
                      kInvalidGeometryId)
                return false;

              if (item.firstPoint.lineId != id ||
                  item.secondPoint.lineId != id)
                return false;

              return item.firstPoint.start !=
                     item.secondPoint.start;
            };

        for (const auto& item : sketch.constraints()) {
          if (item.value <= 0.0) continue;

          if (item.type == ConstraintType::Length &&
              item.firstGeometry == id)
            return item.value;

          if (!isOwnEndpointPair(item)) continue;

          if (item.type == ConstraintType::Distance)
            return item.value;

          if (item.type == ConstraintType::DistanceX &&
              dy <= 1e-7)
            return item.value;

          if (item.type == ConstraintType::DistanceY &&
              dx <= 1e-7)
            return item.value;
        }

        return std::nullopt;
      };

      const auto firstDriving =
          drivingLineSize(constraint.firstGeometry);
      const auto secondDriving =
          drivingLineSize(constraint.secondGeometry);

      if (firstDriving && secondDriving) {
        if (std::abs(*firstDriving - *secondDriving) <= 1e-7)
          ++result.applied;
        else
          ++result.unsupported;

        continue;
      }

      GeometryId referenceGeometry =
          constraint.firstGeometry;
      GeometryId movingGeometry =
          constraint.secondGeometry;
      double targetLength = 0.0;

      if (secondDriving && !firstDriving) {
        referenceGeometry =
            constraint.secondGeometry;
        movingGeometry =
            constraint.firstGeometry;
        targetLength = *secondDriving;
      } else if (firstDriving) {
        targetLength = *firstDriving;
      } else {
        const auto& reference =
            sketch.lines()[*firstLine];

        targetLength =
            std::hypot(reference.end.xMm -
                           reference.start.xMm,
                       reference.end.yMm -
                           reference.start.yMm);
      }

      if (targetLength <= 1e-9) {
        ++result.invalidReferences;
        continue;
      }

      const auto movingIndex =
          sketch.lineIndex(movingGeometry);

      if (!movingIndex) {
        ++result.invalidReferences;
        continue;
      }

      const auto& movingLine =
          sketch.lines()[*movingIndex];
      const double currentLength =
          std::hypot(movingLine.end.xMm -
                         movingLine.start.xMm,
                     movingLine.end.yMm -
                         movingLine.start.yMm);

      if (std::abs(currentLength - targetLength) <= 1e-7) {
        ++result.applied;
      } else if (sketch.setLineLengthById(
                     movingGeometry, targetLength)) {
        ++result.applied;
      } else {
        ++result.invalidReferences;
      }

      continue;
    }

    const auto firstCircle =
        sketch.circleIndex(constraint.firstGeometry);
    const auto secondCircle =
        sketch.circleIndex(constraint.secondGeometry);

    if (firstCircle && secondCircle) {
      const auto drivingCircleDiameter =
          [&sketch](GeometryId id)
              -> std::optional<double> {
        for (const auto& item : sketch.constraints()) {
          if (item.firstGeometry != id ||
              item.value <= 0.0)
            continue;

          if (item.type == ConstraintType::Diameter)
            return item.value;

          if (item.type == ConstraintType::Radius)
            return item.value * 2.0;
        }

        return std::nullopt;
      };

      const auto firstDriving =
          drivingCircleDiameter(
              constraint.firstGeometry);
      const auto secondDriving =
          drivingCircleDiameter(
              constraint.secondGeometry);

      if (firstDriving && secondDriving) {
        if (std::abs(*firstDriving - *secondDriving) <= 1e-7)
          ++result.applied;
        else
          ++result.unsupported;

        continue;
      }

      GeometryId movingGeometry =
          constraint.secondGeometry;
      double targetDiameter = 0.0;

      if (secondDriving && !firstDriving) {
        movingGeometry =
            constraint.firstGeometry;
        targetDiameter = *secondDriving;
      } else if (firstDriving) {
        targetDiameter = *firstDriving;
      } else {
        targetDiameter =
            sketch.circles()[*firstCircle].radiusMm * 2.0;
      }

      if (targetDiameter <= 1e-9) {
        ++result.invalidReferences;
        continue;
      }

      const auto movingIndex =
          sketch.circleIndex(movingGeometry);

      if (!movingIndex) {
        ++result.invalidReferences;
        continue;
      }

      const double currentDiameter =
          sketch.circles()[*movingIndex].radiusMm * 2.0;

      if (std::abs(currentDiameter - targetDiameter) <= 1e-7) {
        ++result.applied;
      } else if (sketch.setCircleDiameterById(
                     movingGeometry, targetDiameter)) {
        ++result.applied;
      } else {
        ++result.invalidReferences;
      }

      continue;
    }

    ++result.invalidReferences;
  }

  // RECTANGLE/STANDARD ORIENTATION STABILIZATION
  //
  // BasicSketchSolver is intentionally simple and sequential. Relative
  // constraints can slightly disturb a point relation that was solved earlier
  // in the same pass. A few cheap final iterations are enough for common
  // sketch chains (especially rectangles) to settle without turning this into
  // a full nonlinear solver.
  for (int stabilizationPass = 0; stabilizationPass < 8;
       ++stabilizationPass) {
    // 0) Solve Equal as connected equivalence groups.
    //
    // Equal is transitive: if A=B and B=C, all three geometries belong to
    // one component and must receive one common size in the same pass.
    // This avoids order-dependent chains where A remains one step behind C.

    const auto drivingLineSize =
        [&sketch](GeometryId id)
            -> std::optional<double> {
      const auto index = sketch.lineIndex(id);
      if (!index) return std::nullopt;

      const auto& line = sketch.lines()[*index];
      const double dx =
          std::abs(line.end.xMm - line.start.xMm);
      const double dy =
          std::abs(line.end.yMm - line.start.yMm);

      const auto isOwnEndpointPair =
          [id](const Constraint& item) {
            if (item.firstPoint.circleId !=
                    kInvalidGeometryId ||
                item.secondPoint.circleId !=
                    kInvalidGeometryId)
              return false;

            if (item.firstPoint.lineId != id ||
                item.secondPoint.lineId != id)
              return false;

            return item.firstPoint.start !=
                   item.secondPoint.start;
          };

      for (const auto& item : sketch.constraints()) {
        if (item.value <= 0.0) continue;

        if (item.type == ConstraintType::Length &&
            item.firstGeometry == id)
          return item.value;

        if (!isOwnEndpointPair(item)) continue;

        if (item.type == ConstraintType::Distance)
          return item.value;

        if (item.type == ConstraintType::DistanceX &&
            dy <= 1e-7)
          return item.value;

        if (item.type == ConstraintType::DistanceY &&
            dx <= 1e-7)
          return item.value;
      }

      return std::nullopt;
    };

    const auto drivingCircleDiameter =
        [&sketch](GeometryId id)
            -> std::optional<double> {
      for (const auto& item : sketch.constraints()) {
        if (item.firstGeometry != id ||
            item.value <= 0.0)
          continue;

        if (item.type == ConstraintType::Diameter)
          return item.value;

        if (item.type == ConstraintType::Radius)
          return item.value * 2.0;
      }

      return std::nullopt;
    };

    std::vector<GeometryId> processedEqualLines;
    std::vector<GeometryId> processedEqualCircles;

    const auto alreadyProcessed =
        [](const std::vector<GeometryId>& values,
           GeometryId id) {
          return std::find(values.begin(), values.end(), id) !=
                 values.end();
        };

    // Build and solve every connected Equal component of lines.
    for (const auto& seed : sketch.constraints()) {
      if (seed.type != ConstraintType::Equal)
        continue;

      if (!sketch.lineIndex(seed.firstGeometry) ||
          !sketch.lineIndex(seed.secondGeometry))
        continue;

      if (alreadyProcessed(processedEqualLines,
                           seed.firstGeometry))
        continue;

      std::vector<GeometryId> group{
          seed.firstGeometry,
          seed.secondGeometry};

      bool expanded = true;
      while (expanded) {
        expanded = false;

        for (const auto& item : sketch.constraints()) {
          if (item.type != ConstraintType::Equal)
            continue;

          if (!sketch.lineIndex(item.firstGeometry) ||
              !sketch.lineIndex(item.secondGeometry))
            continue;

          const bool hasFirst =
              std::find(group.begin(), group.end(),
                        item.firstGeometry) != group.end();
          const bool hasSecond =
              std::find(group.begin(), group.end(),
                        item.secondGeometry) != group.end();

          if (hasFirst && !hasSecond) {
            group.push_back(item.secondGeometry);
            expanded = true;
          } else if (!hasFirst && hasSecond) {
            group.push_back(item.firstGeometry);
            expanded = true;
          }
        }
      }

      for (const auto id : group)
        processedEqualLines.push_back(id);

      std::optional<double> targetLength;
      bool conflictingDriving = false;

      for (const auto id : group) {
        const auto driving = drivingLineSize(id);
        if (!driving) continue;

        if (!targetLength) {
          targetLength = *driving;
        } else if (std::abs(*targetLength - *driving) > 1e-7) {
          conflictingDriving = true;
          break;
        }
      }

      if (conflictingDriving)
        continue;

      if (!targetLength && !group.empty()) {
        const auto index = sketch.lineIndex(group.front());
        if (index) {
          const auto& line = sketch.lines()[*index];
          targetLength =
              std::hypot(line.end.xMm - line.start.xMm,
                         line.end.yMm - line.start.yMm);
        }
      }

      if (!targetLength || *targetLength <= 1e-9)
        continue;

      for (const auto id : group) {
        const auto index = sketch.lineIndex(id);
        if (!index) continue;

        const auto& line = sketch.lines()[*index];
        const double currentLength =
            std::hypot(line.end.xMm - line.start.xMm,
                       line.end.yMm - line.start.yMm);

        // Equal must be idempotent. Re-applying the same size can still
        // move an endpoint because setLineLengthById preserves one pivot.
        // Do not touch geometry that already satisfies the constraint.
        if (std::abs(currentLength - *targetLength) <= 1e-7)
          continue;

        (void)sketch.setLineLengthById(id, *targetLength);
      }
    }

    // Build and solve every connected Equal component of circles.
    for (const auto& seed : sketch.constraints()) {
      if (seed.type != ConstraintType::Equal)
        continue;

      if (!sketch.circleIndex(seed.firstGeometry) ||
          !sketch.circleIndex(seed.secondGeometry))
        continue;

      if (alreadyProcessed(processedEqualCircles,
                           seed.firstGeometry))
        continue;

      std::vector<GeometryId> group{
          seed.firstGeometry,
          seed.secondGeometry};

      bool expanded = true;
      while (expanded) {
        expanded = false;

        for (const auto& item : sketch.constraints()) {
          if (item.type != ConstraintType::Equal)
            continue;

          if (!sketch.circleIndex(item.firstGeometry) ||
              !sketch.circleIndex(item.secondGeometry))
            continue;

          const bool hasFirst =
              std::find(group.begin(), group.end(),
                        item.firstGeometry) != group.end();
          const bool hasSecond =
              std::find(group.begin(), group.end(),
                        item.secondGeometry) != group.end();

          if (hasFirst && !hasSecond) {
            group.push_back(item.secondGeometry);
            expanded = true;
          } else if (!hasFirst && hasSecond) {
            group.push_back(item.firstGeometry);
            expanded = true;
          }
        }
      }

      for (const auto id : group)
        processedEqualCircles.push_back(id);

      std::optional<double> targetDiameter;
      bool conflictingDriving = false;

      for (const auto id : group) {
        const auto driving = drivingCircleDiameter(id);
        if (!driving) continue;

        if (!targetDiameter) {
          targetDiameter = *driving;
        } else if (std::abs(*targetDiameter - *driving) > 1e-7) {
          conflictingDriving = true;
          break;
        }
      }

      if (conflictingDriving)
        continue;

      if (!targetDiameter && !group.empty()) {
        const auto index = sketch.circleIndex(group.front());
        if (index)
          targetDiameter =
              sketch.circles()[*index].radiusMm * 2.0;
      }

      if (!targetDiameter || *targetDiameter <= 1e-9)
        continue;

      for (const auto id : group) {
        const auto index = sketch.circleIndex(id);
        if (!index) continue;

        const double currentDiameter =
            sketch.circles()[*index].radiusMm * 2.0;

        if (std::abs(currentDiameter - *targetDiameter) <= 1e-7)
          continue;

        (void)sketch.setCircleDiameterById(
            id, *targetDiameter);
      }
    }

    // 1) Restore coincident corners.
    for (const auto& constraint : sketch.constraints()) {
      if (constraint.type != ConstraintType::Coincident) continue;

      const auto validPointReference =
          [&sketch](PointReference reference) {
            return sketch.referencedPoint(reference).has_value();
          };

      if (!validPointReference(constraint.firstPoint) ||
          !validPointReference(constraint.secondPoint))
        continue;

      (void)sketch.setPointsCoincident(constraint.firstPoint,
                                       constraint.secondPoint);
    }

    // 2) Re-apply absolute H/V orientation.
    for (const auto& constraint : sketch.constraints()) {
      if (constraint.firstGeometry == kInvalidGeometryId ||
          !sketch.lineIndex(constraint.firstGeometry))
        continue;

      if (constraint.type == ConstraintType::Horizontal)
        (void)sketch.setLineHorizontalById(constraint.firstGeometry);
      else if (constraint.type == ConstraintType::Vertical)
        (void)sketch.setLineVerticalById(constraint.firstGeometry);
    }

    // 3) Re-apply parallel pairs. Preserve an H/V-fixed side as reference.
    for (const auto& constraint : sketch.constraints()) {
      if (constraint.type != ConstraintType::Parallel) continue;

      if (constraint.firstGeometry == kInvalidGeometryId ||
          constraint.secondGeometry == kInvalidGeometryId ||
          constraint.firstGeometry == constraint.secondGeometry ||
          !sketch.lineIndex(constraint.firstGeometry) ||
          !sketch.lineIndex(constraint.secondGeometry))
        continue;

      const auto hasAbsoluteOrientation =
          [&sketch](GeometryId id) {
            return std::any_of(
                sketch.constraints().begin(), sketch.constraints().end(),
                [id](const Constraint& item) {
                  return item.firstGeometry == id &&
                         (item.type == ConstraintType::Horizontal ||
                          item.type == ConstraintType::Vertical);
                });
          };

      const bool firstFixed =
          hasAbsoluteOrientation(constraint.firstGeometry);
      const bool secondFixed =
          hasAbsoluteOrientation(constraint.secondGeometry);

      if (firstFixed && secondFixed)
        continue;

      const GeometryId referenceGeometry =
          secondFixed && !firstFixed
              ? constraint.secondGeometry
              : constraint.firstGeometry;
      const GeometryId movingGeometry =
          secondFixed && !firstFixed
              ? constraint.firstGeometry
              : constraint.secondGeometry;

      (void)sketch.setLinesParallelByIds(referenceGeometry,
                                         movingGeometry);
    }

    // 4) Re-apply perpendicular pairs using the same H/V priority rule.
    for (const auto& constraint : sketch.constraints()) {
      if (constraint.type != ConstraintType::Perpendicular) continue;

      if (constraint.firstGeometry == kInvalidGeometryId ||
          constraint.secondGeometry == kInvalidGeometryId ||
          constraint.firstGeometry == constraint.secondGeometry ||
          !sketch.lineIndex(constraint.firstGeometry) ||
          !sketch.lineIndex(constraint.secondGeometry))
        continue;

      const auto hasAbsoluteOrientation =
          [&sketch](GeometryId id) {
            return std::any_of(
                sketch.constraints().begin(), sketch.constraints().end(),
                [id](const Constraint& item) {
                  return item.firstGeometry == id &&
                         (item.type == ConstraintType::Horizontal ||
                          item.type == ConstraintType::Vertical);
                });
          };

      const bool firstFixed =
          hasAbsoluteOrientation(constraint.firstGeometry);
      const bool secondFixed =
          hasAbsoluteOrientation(constraint.secondGeometry);

      if (firstFixed && secondFixed)
        continue;

      const GeometryId referenceGeometry =
          secondFixed && !firstFixed
              ? constraint.secondGeometry
              : constraint.firstGeometry;
      const GeometryId movingGeometry =
          secondFixed && !firstFixed
              ? constraint.firstGeometry
              : constraint.secondGeometry;

      (void)sketch.setLineAngleByIds(referenceGeometry,
                                     movingGeometry,
                                     90.0);
    }

    // 5) Finally project PointOnLine references onto their finite carriers.
    for (const auto& constraint : sketch.constraints()) {
      if (constraint.type != ConstraintType::PointOnLine)
        continue;

      if (constraint.firstGeometry == kInvalidGeometryId ||
          !sketch.lineIndex(constraint.firstGeometry) ||
          !sketch.referencedPoint(constraint.secondPoint))
        continue;

      (void)sketch.setPointOnLine(constraint.firstGeometry,
                                  constraint.secondPoint);
    }
  }
  // POINT-ON-LINE FINAL PASS
  // Keep constrained points on finite segments after all other geometry
  // relations have settled.
  for (const auto& constraint : sketch.constraints()) {
    if (constraint.type != ConstraintType::PointOnLine)
      continue;

    if (constraint.firstGeometry == kInvalidGeometryId ||
        !sketch.lineIndex(constraint.firstGeometry) ||
        !sketch.referencedPoint(constraint.secondPoint)) {
      ++result.invalidReferences;
      continue;
    }

    if (sketch.setPointOnLine(constraint.firstGeometry,
                              constraint.secondPoint))
      ++result.applied;
    else
      ++result.invalidReferences;
  }
  // POINT-ON-CIRCLE FINAL PASS
  // PointOnCircle is not tangency: only the selected point is projected
  // onto the current circumference.
  for (const auto& constraint : sketch.constraints()) {
    if (constraint.type != ConstraintType::PointOnCircle)
      continue;

    if (constraint.firstGeometry == kInvalidGeometryId ||
        !sketch.circleIndex(constraint.firstGeometry) ||
        !sketch.referencedPoint(constraint.secondPoint)) {
      ++result.invalidReferences;
      continue;
    }

    if (sketch.setPointOnCircle(constraint.firstGeometry,
                                constraint.secondPoint))
      ++result.applied;
    else
      ++result.invalidReferences;
  }
  return result;
}

}  // namespace solidar::sketch