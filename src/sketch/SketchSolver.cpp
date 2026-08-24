#include "sketch/SketchSolver.h"

#include "sketch/Sketch.h"

#include <algorithm>
#include <cmath>
#include <limits>

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
            !std::isfinite(constraint.value) ||
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
        // PointReference can denote a line endpoint, a circle center or a
        // virtual element center. Ask Sketch to validate the reference rather
        // than assuming lineId is always populated.
        if (!sketch.referencedPoint(constraint.firstPoint) ||
            !sketch.referencedPoint(constraint.secondPoint) ||
            !std::isfinite(constraint.value) ||
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
            !std::isfinite(constraint.value) ||
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

      case ConstraintType::Tangent:
        // Applied in the final tangency stabilization pass.
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
        !std::isfinite(constraint.value) ||
        constraint.value <= 0.0 || constraint.value >= 180.0) {
      ++result.invalidReferences;
      continue;
    }

    // ANGLE MOBILITY SCORE
    //
    // Angle is a relative constraint. Do not blindly rotate the second
    // selected line: prefer moving the less constrained / smaller CAD object.
    // A four-line rectangle is treated as one composite object and is much
    // "heavier" than a standalone line.
    const auto elementGeometryIds =
        [&sketch](GeometryId id) {
          std::vector<GeometryId> ids;

          const auto index = sketch.lineIndex(id);
          if (!index)
            return ids;

          const std::size_t elementId =
              sketch.lines()[*index].elementId;

          for (std::size_t lineIndex = 0;
               lineIndex < sketch.lines().size();
               ++lineIndex) {
            if (sketch.lines()[lineIndex].elementId == elementId) {
              const auto memberId = sketch.lineId(lineIndex);
              if (memberId != kInvalidGeometryId)
                ids.push_back(memberId);
            }
          }

          return ids;
        };

    const auto containsGeometry =
        [](const std::vector<GeometryId>& ids, GeometryId id) {
          return std::find(ids.begin(), ids.end(), id) != ids.end();
        };

    const auto hasOrthogonalConstraint =
        [&sketch, &elementGeometryIds, &containsGeometry](GeometryId id) {
          const auto ids = elementGeometryIds(id);
          if (ids.empty())
            return false;

          return std::any_of(
              sketch.constraints().begin(), sketch.constraints().end(),
              [&ids, &containsGeometry](const Constraint& item) {
                return containsGeometry(ids, item.firstGeometry) &&
                       (item.type == ConstraintType::Horizontal ||
                        item.type == ConstraintType::Vertical);
              });
        };

    const auto mobilityScore =
        [&sketch, &elementGeometryIds, &containsGeometry,
         &constraint](GeometryId id) {
          const auto ids = elementGeometryIds(id);
          if (ids.empty())
            return std::numeric_limits<int>::max();

          int score = 0;

          // Composite CAD elements should remain the reference when compared
          // with a simple line. This is the dominant term.
          if (ids.size() > 1)
            score += static_cast<int>((ids.size() - 1) * 100);

          for (const auto& item : sketch.constraints()) {
            // Do not count the Angle currently being solved against itself.
            if (item.id == constraint.id)
              continue;

            const bool touchesGeometry =
                containsGeometry(ids, item.firstGeometry) ||
                containsGeometry(ids, item.secondGeometry);

            const bool touchesPoint =
                containsGeometry(ids, item.firstPoint.lineId) ||
                containsGeometry(ids, item.secondPoint.lineId);

            if (!touchesGeometry && !touchesPoint)
              continue;

            switch (item.type) {
              case ConstraintType::Horizontal:
              case ConstraintType::Vertical:
                score += 1000;
                break;

              case ConstraintType::Distance:
              case ConstraintType::DistanceX:
              case ConstraintType::DistanceY:
              case ConstraintType::Length:
                score += 40;
                break;

              case ConstraintType::Coincident:
              case ConstraintType::PointOnLine:
              case ConstraintType::PointOnCircle:
                score += 25;
                break;

              case ConstraintType::Parallel:
              case ConstraintType::Perpendicular:
              case ConstraintType::Equal:
              case ConstraintType::Angle:
                score += 15;
                break;

              default:
                score += 5;
                break;
            }
          }

          return score;
        };

    const bool firstOrthogonal =
        hasOrthogonalConstraint(constraint.firstGeometry);
    const bool secondOrthogonal =
        hasOrthogonalConstraint(constraint.secondGeometry);

    // Absolute orientation remains stronger than Angle.
    if (firstOrthogonal && secondOrthogonal) {
      continue;
    }

    GeometryId referenceGeometry = constraint.firstGeometry;
    GeometryId movingGeometry = constraint.secondGeometry;

    if (firstOrthogonal && !secondOrthogonal) {
      referenceGeometry = constraint.firstGeometry;
      movingGeometry = constraint.secondGeometry;
    } else if (secondOrthogonal && !firstOrthogonal) {
      referenceGeometry = constraint.secondGeometry;
      movingGeometry = constraint.firstGeometry;
    } else {
      const int firstScore =
          mobilityScore(constraint.firstGeometry);
      const int secondScore =
          mobilityScore(constraint.secondGeometry);

      // Lower score = easier to move. On an exact tie preserve historical
      // behaviour and rotate the second selected line.
      if (firstScore < secondScore) {
        referenceGeometry = constraint.secondGeometry;
        movingGeometry = constraint.firstGeometry;
      }
    }

    if (sketch.setLineAngleByIds(referenceGeometry,
                                 movingGeometry,
                                 constraint.value))
      ++result.applied;
    else
      ++result.invalidReferences;
  }

  // CRASH-FREE 11: COMPOSITE RELATIONSHIP GUARD
  //
  // Primitive orientation setters may only rotate a standalone line relative
  // to a composite side. Two different composite elements need a future
  // whole-element rigid transform solver; never deform one rectangle side.
  const auto compositeLineElement =
      [&sketch](GeometryId id)
          -> std::optional<std::size_t> {
        const auto index =
            sketch.lineIndex(id);

        if (!index)
          return std::nullopt;

        const std::size_t elementId =
            sketch.lines()[*index].elementId;

        std::size_t count = 0;

        for (const auto& line :
             sketch.lines()) {
          if (line.elementId == elementId)
            ++count;
        }

        if (count <= 1)
          return std::nullopt;

        return elementId;
      };
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
  // CRASH-FREE 10: GROUPED MULTI-TANGENT SOLVER
  //
  // Sequentially projecting one circle onto Tangent A, then B, then C makes
  // the last constraint win. Instead, collect every Tangent that targets the
  // same circle and solve that circle as one system.
  std::vector<GeometryId> processedTangentCircles;

  const auto alreadyProcessedTangentCircle =
      [&processedTangentCircles](GeometryId id) {
        return std::find(
                   processedTangentCircles.begin(),
                   processedTangentCircles.end(),
                   id) !=
               processedTangentCircles.end();
      };

  for (const auto& seed :
       sketch.constraints()) {
    if (seed.type != ConstraintType::Tangent)
      continue;

    const GeometryId circleId =
        seed.secondGeometry;

    if (circleId == kInvalidGeometryId ||
        alreadyProcessedTangentCircle(circleId))
      continue;

    processedTangentCircles.push_back(circleId);

    const auto circleIndex =
        sketch.circleIndex(circleId);

    if (!circleIndex) {
      ++result.invalidReferences;
      continue;
    }

    const auto& circle =
        sketch.circles()[*circleIndex];

    if (!std::isfinite(circle.radiusMm) ||
        circle.radiusMm <= 1e-9) {
      ++result.invalidReferences;
      continue;
    }

    std::vector<GeometryId> carrierIds;

    for (const auto& item :
         sketch.constraints()) {
      if (item.type != ConstraintType::Tangent ||
          item.secondGeometry != circleId)
        continue;

      if (item.firstGeometry ==
              kInvalidGeometryId ||
          !sketch.lineIndex(item.firstGeometry)) {
        ++result.invalidReferences;
        continue;
      }

      if (std::find(
              carrierIds.begin(),
              carrierIds.end(),
              item.firstGeometry) ==
          carrierIds.end()) {
        carrierIds.push_back(
            item.firstGeometry);
      }
    }

    if (carrierIds.empty())
      continue;

    if (carrierIds.size() == 1) {
      if (sketch.setCircleTangentToLine(
              carrierIds.front(),
              circleId))
        ++result.applied;
      else
        ++result.invalidReferences;

      continue;
    }

    struct TangentCandidate {
      Point center;
      double residual{};
      double finitePenalty{};
      double movement{};
    };

    std::optional<TangentCandidate>
        bestCandidate;

    const auto evaluateCandidate =
        [&sketch,
         &carrierIds,
         &circle](Point candidate) {
          TangentCandidate scored;
          scored.center = candidate;

          scored.movement =
              std::hypot(
                  candidate.xMm -
                      circle.center.xMm,
                  candidate.yMm -
                      circle.center.yMm);

          for (const auto lineId :
               carrierIds) {
            const auto index =
                sketch.lineIndex(lineId);

            if (!index) {
              scored.residual += 1e9;
              continue;
            }

            const auto& line =
                sketch.lines()[*index];

            const double dx =
                line.end.xMm -
                line.start.xMm;
            const double dy =
                line.end.yMm -
                line.start.yMm;
            const double lengthSquared =
                dx * dx + dy * dy;

            if (lengthSquared <= 1e-12) {
              scored.residual += 1e9;
              continue;
            }

            const double length =
                std::sqrt(lengthSquared);
            const double nx =
                -dy / length;
            const double ny =
                dx / length;

            const double signedDistance =
                (candidate.xMm -
                     line.start.xMm) *
                    nx +
                (candidate.yMm -
                     line.start.yMm) *
                    ny;

            scored.residual +=
                std::abs(
                    std::abs(signedDistance) -
                    circle.radiusMm);

            const double rawT =
                ((candidate.xMm -
                      line.start.xMm) *
                     dx +
                 (candidate.yMm -
                      line.start.yMm) *
                     dy) /
                lengthSquared;

            if (rawT < 0.0)
              scored.finitePenalty +=
                  -rawT * length;
            else if (rawT > 1.0)
              scored.finitePenalty +=
                  (rawT - 1.0) * length;
          }

          return scored;
        };

    for (std::size_t first = 0;
         first < carrierIds.size();
         ++first) {
      const auto firstIndex =
          sketch.lineIndex(
              carrierIds[first]);

      if (!firstIndex)
        continue;

      const auto& firstLine =
          sketch.lines()[*firstIndex];

      const double firstDx =
          firstLine.end.xMm -
          firstLine.start.xMm;
      const double firstDy =
          firstLine.end.yMm -
          firstLine.start.yMm;
      const double firstLength =
          std::hypot(firstDx, firstDy);

      if (firstLength <= 1e-9)
        continue;

      const double firstNx =
          -firstDy / firstLength;
      const double firstNy =
          firstDx / firstLength;

      for (std::size_t second =
               first + 1;
           second < carrierIds.size();
           ++second) {
        const auto secondIndex =
            sketch.lineIndex(
                carrierIds[second]);

        if (!secondIndex)
          continue;

        const auto& secondLine =
            sketch.lines()[*secondIndex];

        const double secondDx =
            secondLine.end.xMm -
            secondLine.start.xMm;
        const double secondDy =
            secondLine.end.yMm -
            secondLine.start.yMm;
        const double secondLength =
            std::hypot(
                secondDx,
                secondDy);

        if (secondLength <= 1e-9)
          continue;

        const double secondNx =
            -secondDy / secondLength;
        const double secondNy =
            secondDx / secondLength;

        const double determinant =
            firstNx * secondNy -
            firstNy * secondNx;

        if (std::abs(determinant) <=
            1e-10)
          continue;

        for (const double firstSide :
             {-1.0, 1.0}) {
          for (const double secondSide :
               {-1.0, 1.0}) {
            const double firstConstant =
                firstNx *
                    firstLine.start.xMm +
                firstNy *
                    firstLine.start.yMm +
                firstSide *
                    circle.radiusMm;

            const double secondConstant =
                secondNx *
                    secondLine.start.xMm +
                secondNy *
                    secondLine.start.yMm +
                secondSide *
                    circle.radiusMm;

            const Point candidate{
                (firstConstant *
                     secondNy -
                 firstNy *
                     secondConstant) /
                    determinant,
                (firstNx *
                     secondConstant -
                 firstConstant *
                     secondNx) /
                    determinant};

            if (!std::isfinite(
                    candidate.xMm) ||
                !std::isfinite(
                    candidate.yMm))
              continue;

            const auto scored =
                evaluateCandidate(candidate);

            if (!bestCandidate) {
              bestCandidate = scored;
              continue;
            }

            constexpr double
                residualTolerance = 1e-7;
            constexpr double
                finiteTolerance = 1e-7;

            const bool betterResidual =
                scored.residual <
                bestCandidate->residual -
                    residualTolerance;

            const bool equalResidual =
                std::abs(
                    scored.residual -
                    bestCandidate->residual) <=
                residualTolerance;

            const bool betterFinite =
                equalResidual &&
                scored.finitePenalty <
                    bestCandidate
                            ->finitePenalty -
                        finiteTolerance;

            const bool equalFinite =
                equalResidual &&
                std::abs(
                    scored.finitePenalty -
                    bestCandidate
                        ->finitePenalty) <=
                    finiteTolerance;

            const bool closerBranch =
                equalFinite &&
                scored.movement <
                    bestCandidate->movement;

            if (betterResidual ||
                betterFinite ||
                closerBranch)
              bestCandidate = scored;
          }
        }
      }
    }

    if (!bestCandidate) {
      bool anyApplied = false;

      for (const auto lineId :
           carrierIds) {
        anyApplied =
            sketch.setCircleTangentToLine(
                lineId,
                circleId) ||
            anyApplied;
      }

      if (anyApplied)
        ++result.applied;
      else
        ++result.invalidReferences;

      continue;
    }

    const double moveX =
        bestCandidate->center.xMm -
        circle.center.xMm;
    const double moveY =
        bestCandidate->center.yMm -
        circle.center.yMm;

    sketch.translateCircleById(
        circleId,
        moveX,
        moveY);

    ++result.applied;
  }
  // CRASH-FREE 11: FINAL COMPOSITE-SAFE EQUAL STABILIZATION
  //
  // Orientation relationships are solved after the main Equal pass. Re-apply
  // Equal here so Parallel/Perpendicular/Angle cannot leave an equal pair one
  // step out of sync. setLineLengthById() is rectangle-aware and resizes a
  // four-line composite as one rectangle rather than stretching one side.
  for (const auto& constraint :
       sketch.constraints()) {
    if (constraint.type !=
        ConstraintType::Equal)
      continue;

    if (constraint.firstGeometry ==
            kInvalidGeometryId ||
        constraint.secondGeometry ==
            kInvalidGeometryId ||
        constraint.firstGeometry ==
            constraint.secondGeometry)
      continue;

    const auto firstLineIndex =
        sketch.lineIndex(
            constraint.firstGeometry);
    const auto secondLineIndex =
        sketch.lineIndex(
            constraint.secondGeometry);

    if (firstLineIndex &&
        secondLineIndex) {
      const auto& firstLine =
          sketch.lines()[*firstLineIndex];
      const auto& secondLine =
          sketch.lines()[*secondLineIndex];

      const double firstLength =
          std::hypot(
              firstLine.end.xMm -
                  firstLine.start.xMm,
              firstLine.end.yMm -
                  firstLine.start.yMm);

      const double secondLength =
          std::hypot(
              secondLine.end.xMm -
                  secondLine.start.xMm,
              secondLine.end.yMm -
                  secondLine.start.yMm);

      if (firstLength <= 1e-9 ||
          secondLength <= 1e-9)
        continue;

      // Prefer the geometry with an explicit driving size as reference.
      const auto drivingLength =
          [&sketch](GeometryId id)
              -> std::optional<double> {
            for (const auto& item :
                 sketch.constraints()) {
              if (item.type ==
                      ConstraintType::Length &&
                  item.firstGeometry == id &&
                  std::isfinite(item.value) &&
                  item.value > 0.0)
                return item.value;

              if (item.firstPoint.lineId != id ||
                  item.secondPoint.lineId != id ||
                  item.firstPoint.start ==
                      item.secondPoint.start)
                continue;

              if ((item.type ==
                       ConstraintType::Distance ||
                   item.type ==
                       ConstraintType::DistanceX ||
                   item.type ==
                       ConstraintType::DistanceY) &&
                  std::isfinite(item.value) &&
                  item.value > 0.0)
                return item.value;
            }

            return std::nullopt;
          };

      const auto firstDriving =
          drivingLength(
              constraint.firstGeometry);
      const auto secondDriving =
          drivingLength(
              constraint.secondGeometry);

      GeometryId movingGeometry =
          constraint.secondGeometry;
      double targetLength =
          firstLength;

      if (secondDriving &&
          !firstDriving) {
        movingGeometry =
            constraint.firstGeometry;
        targetLength =
            *secondDriving;
      }
      else if (firstDriving) {
        targetLength =
            *firstDriving;
      }

      const auto movingIndex =
          sketch.lineIndex(
              movingGeometry);

      if (!movingIndex)
        continue;

      const auto& movingLine =
          sketch.lines()[*movingIndex];

      const double currentLength =
          std::hypot(
              movingLine.end.xMm -
                  movingLine.start.xMm,
              movingLine.end.yMm -
                  movingLine.start.yMm);

      if (std::abs(
              currentLength -
              targetLength) <= 1e-7)
        continue;

      (void)sketch.setLineLengthById(
          movingGeometry,
          targetLength);

      continue;
    }

    const auto firstCircleIndex =
        sketch.circleIndex(
            constraint.firstGeometry);
    const auto secondCircleIndex =
        sketch.circleIndex(
            constraint.secondGeometry);

    if (firstCircleIndex &&
        secondCircleIndex) {
      const double targetDiameter =
          sketch.circles()[*firstCircleIndex]
              .radiusMm *
          2.0;

      const double currentDiameter =
          sketch.circles()[*secondCircleIndex]
              .radiusMm *
          2.0;

      if (std::abs(
              currentDiameter -
              targetDiameter) > 1e-7)
        (void)sketch.setCircleDiameterById(
            constraint.secondGeometry,
            targetDiameter);
    }
  }
  // CRASH-FREE 09 V2: FINAL DRIVING DIMENSION STABILIZATION
  //
  // The solver is sequential. Equal/orientation/point/tangent passes may move
  // geometry after a Distance/X/Y was initially solved. Re-apply all active
  // driving dimensions at the very end. For compatible constraints this makes
  // drag converge back to the exact requested values instead of silently
  // changing an older dimension.
  constexpr int drivingDimensionIterations = 8;

  for (int iteration = 0;
       iteration < drivingDimensionIterations;
       ++iteration) {
    bool anyDrivingDimension = false;

    for (const auto& constraint :
         sketch.constraints()) {
      if (constraint.type !=
              ConstraintType::Distance &&
          constraint.type !=
              ConstraintType::DistanceX &&
          constraint.type !=
              ConstraintType::DistanceY)
        continue;

      if (!std::isfinite(constraint.value) ||
          constraint.value <= 0.0 ||
          !sketch.referencedPoint(
              constraint.firstPoint) ||
          !sketch.referencedPoint(
              constraint.secondPoint))
        continue;

      bool applied = false;

      if (constraint.type ==
          ConstraintType::Distance) {
        applied =
            sketch.setPointDistance(
                constraint.firstPoint,
                constraint.secondPoint,
                constraint.value);
      }
      else if (constraint.type ==
               ConstraintType::DistanceX) {
        applied =
            sketch.setPointDistanceX(
                constraint.firstPoint,
                constraint.secondPoint,
                constraint.value);
      }
      else if (constraint.type ==
               ConstraintType::DistanceY) {
        applied =
            sketch.setPointDistanceY(
                constraint.firstPoint,
                constraint.secondPoint,
                constraint.value);
      }

      anyDrivingDimension =
          anyDrivingDimension || applied;
    }

    if (!anyDrivingDimension)
      break;
  }
  return result;
}

}  // namespace solidar::sketch