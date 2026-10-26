#include "sketch/SketchSolver.h"

#include "sketch/SketchConstraintDiagnostics.h"
#include "sketch/Sketch.h"
#include "sketch/NonlinearSketchSolver.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace solidar::sketch {

SolveResult SketchSolver::solveStableComponent(
    Sketch& sketch, const std::vector<GeometryId>& dirtyGeometry,
    int maxPasses, ConstraintId preferredConstraintId,
    bool useDirtyGeometryHints) {
  SolveResult total;
  std::vector<ConstraintComponent> components;
  std::vector<GeometryId> consumed;
  std::vector<GeometryId> orderedSeeds = dirtyGeometry;
  std::sort(orderedSeeds.begin(), orderedSeeds.end());
  orderedSeeds.erase(std::unique(orderedSeeds.begin(), orderedSeeds.end()),
                     orderedSeeds.end());
  for (const auto seed : orderedSeeds) {
    if (std::binary_search(consumed.begin(), consumed.end(), seed)) continue;
    auto component = sketch.connectedComponent({seed});
    if (component.geometryIds.empty()) continue;
    consumed.insert(consumed.end(), component.geometryIds.begin(),
                    component.geometryIds.end());
    std::sort(consumed.begin(), consumed.end());
    consumed.erase(std::unique(consumed.begin(), consumed.end()), consumed.end());
    components.push_back(std::move(component));
  }

  total.converged = true;
  total.status = SolveStatus::SolvedFullyConstrained;
  std::vector<Sketch> solvedComponents;
  solvedComponents.reserve(components.size());
  for (const auto& component : components) {
    Sketch local;
    local.clear();
    std::size_t lineCount = 0;
    std::size_t circleCount = 0;
    std::size_t arcCount = 0;
    std::size_t bezierCount = 0;
    for (const auto id : component.geometryIds) {
      const auto location = sketch.geometryLocation(id);
      if (!location) continue;
      lineCount += location->kind == GeometryKind::Line;
      circleCount += location->kind == GeometryKind::Circle;
      arcCount += location->kind == GeometryKind::Arc;
      bezierCount += location->kind == GeometryKind::Bezier;
    }
    local.lines_.reserve(lineCount); local.lineIds_.reserve(lineCount);
    local.circles_.reserve(circleCount); local.circleIds_.reserve(circleCount);
    local.arcs_.reserve(arcCount); local.arcIds_.reserve(arcCount);
    local.beziers_.reserve(bezierCount);
    local.bezierIds_.reserve(bezierCount);
    local.constraints_.reserve(component.constraintIds.size());
    local.centerNodeElementIds_.reserve(lineCount);
    local.nextElementId_ = sketch.nextElementId_;
    local.nextGeometryId_ = sketch.nextGeometryId_;
    local.nextConstraintId_ = sketch.nextConstraintId_;

    for (const auto id : component.geometryIds) {
      const auto location = sketch.geometryLocation(id);
      if (!location) continue;
      switch (location->kind) {
        case GeometryKind::Line:
          local.lines_.push_back(sketch.lines_[location->index]);
          local.lineIds_.push_back(id);
          break;
        case GeometryKind::Circle:
          local.circles_.push_back(sketch.circles_[location->index]);
          local.circleIds_.push_back(id);
          break;
        case GeometryKind::Arc:
          local.arcs_.push_back(sketch.arcs_[location->index]);
          local.arcIds_.push_back(id);
          break;
        case GeometryKind::Bezier:
          local.beziers_.push_back(sketch.beziers_[location->index]);
          local.bezierIds_.push_back(id);
          break;
      }
    }
    for (const auto elementId : sketch.centerNodeElementIds_) {
      const bool present = std::any_of(
          local.lines_.begin(), local.lines_.end(),
          [elementId](const Line& line) { return line.elementId == elementId; });
      if (present) local.centerNodeElementIds_.push_back(elementId);
    }
    for (const auto id : component.constraintIds) {
      const auto index = sketch.constraintIndex(id);
      if (index) local.constraints_.push_back(sketch.constraints_[*index]);
    }
    local.invalidateStructureIndexes();
    local.updateBounds();

    if (preferredConstraintId != kInvalidConstraintId) {
      const auto compositeElement = [&](GeometryId id)
          -> std::optional<std::size_t> {
        const auto index = local.lineIndex(id);
        if (!index) return std::nullopt;
        const auto elementId = local.lines_[*index].elementId;
        const auto count = static_cast<std::size_t>(std::count_if(
            local.lines_.begin(), local.lines_.end(),
            [elementId](const Line& line) {
              return line.elementId == elementId;
            }));
        return elementId != 0 && count >= 2 ? std::optional{elementId}
                                            : std::nullopt;
      };
      std::vector<GeometryId> fixedGeometry;
      const auto addFixed = [&](GeometryId id) {
        if (id != kInvalidGeometryId && local.geometryLocation(id))
          fixedGeometry.push_back(id);
      };
      const auto pointGeometry = [](const PointReference& point) {
        if (point.lineId != kInvalidGeometryId)
          return static_cast<GeometryId>(point.lineId);
        if (point.circleId != kInvalidGeometryId)
          return static_cast<GeometryId>(point.circleId);
        if (point.arcId != kInvalidGeometryId)
          return static_cast<GeometryId>(point.arcId);
        if (point.bezierId != kInvalidGeometryId)
          return static_cast<GeometryId>(point.bezierId);
        return kInvalidGeometryId;
      };
      const auto stablePointAnchor = [&](const PointReference& firstPoint,
                                         const PointReference& secondPoint) {
        if (firstPoint.origin) return kInvalidGeometryId;
        const GeometryId first = pointGeometry(firstPoint);
        const GeometryId second = pointGeometry(secondPoint);
        if (first != kInvalidGeometryId && first == second)
          return kInvalidGeometryId;
        if (first == kInvalidGeometryId) return second;
        if (second == kInvalidGeometryId) return first;
        const bool firstLocked = local.isGeometryLocked(first);
        const bool secondLocked = local.isGeometryLocked(second);
        if (firstLocked != secondLocked) return firstLocked ? first : second;
        const bool firstComposite = compositeElement(first).has_value();
        const bool secondComposite = compositeElement(second).has_value();
        if (firstComposite != secondComposite)
          return firstComposite ? first : second;
        // Point dimensions intentionally use the first reference as their
        // datum.  This is operand semantics, not constraint-vector priority.
        return first;
      };
      for (const auto& constraint : local.constraints_) {
        if (!participatesInConstraintSystem(constraint)) continue;
        // During constraint creation only the new relation chooses the local
        // reference side.  Letting every pre-existing relation add another
        // transient lock can freeze both peers of the new constraint.
        if (preferredConstraintId != kInvalidConstraintId &&
            constraint.id != preferredConstraintId)
          continue;
        const auto type = static_cast<ConstraintType>(constraint.type);
        const GeometryId first = constraint.firstGeometry;
        const GeometryId second = constraint.secondGeometry;
        const bool relational =
            type == ConstraintType::Parallel ||
            type == ConstraintType::Perpendicular ||
            type == ConstraintType::Equal || type == ConstraintType::Angle ||
            type == ConstraintType::LineDistance ||
            type == ConstraintType::Tangent;
        if (relational) {
          // An explicit Lock already supplies the reference frame.  Adding a
          // second transient lock to the movable peer would turn a valid new
          // relation into an artificial conflict (notably line/circle
          // tangency against projected, locked geometry).
          if (local.isGeometryLocked(first) ||
              local.isGeometryLocked(second))
            continue;
          const auto firstComposite = compositeElement(first);
          const auto secondComposite = compositeElement(second);
          if (firstComposite && secondComposite &&
              *firstComposite == *secondComposite)
            continue;
          if (firstComposite) addFixed(first);
          else if (secondComposite) addFixed(second);
          else addFixed(first);
        } else if (type == ConstraintType::Coincident ||
                   type == ConstraintType::Distance ||
                   type == ConstraintType::DistanceX ||
                   type == ConstraintType::DistanceY) {
          addFixed(stablePointAnchor(constraint.firstPoint,
                                     constraint.secondPoint));
        } else if (type == ConstraintType::PointOnLine ||
                   type == ConstraintType::PointOnCircle ||
                   type == ConstraintType::PointOnArc ||
                   type == ConstraintType::Midpoint) {
          addFixed(first);
        }
      }
      std::sort(fixedGeometry.begin(), fixedGeometry.end());
      fixedGeometry.erase(
          std::unique(fixedGeometry.begin(), fixedGeometry.end()),
          fixedGeometry.end());
      ConstraintId transientId = local.nextConstraintId_;
      for (const auto id : fixedGeometry) {
        while (local.constraintIndex(transientId)) ++transientId;
        local.constraints_.emplace_back(
            LockConstraint{id}, ConstraintMode::Driving, transientId++);
      }
    }
    SolveRequest request{local,
                         {component.geometryIds, component.constraintIds}};
    request.preferredConstraintId = preferredConstraintId;
    // The outermost open journal is the last committed/reference solution.
    // Build the backend warm start in the candidate parameter layout instead
    // of treating already-previewed coordinates as authoritative state.
    if (!sketch.deltaJournals_.empty()) {
      const auto& reference = sketch.deltaJournals_.front();
      const auto layout = ParameterLayout::build(local, component.geometryIds);
      request.previousCandidate.assign(layout.variableCount(), 0.0);
      const auto referenceValue = [&](const ParameterDescriptor& descriptor) {
        const GeometryId id = descriptor.provenance.geometryId;
        const auto line = std::find_if(
            reference.lines.begin(), reference.lines.end(),
            [id](const auto& item) { return item.id == id && item.before; });
        if (line != reference.lines.end()) {
          switch (descriptor.provenance.role) {
            case ParameterRole::LineStartX: return line->before->start.xMm;
            case ParameterRole::LineStartY: return line->before->start.yMm;
            case ParameterRole::LineEndX: return line->before->end.xMm;
            case ParameterRole::LineEndY: return line->before->end.yMm;
            default: break;
          }
        }
        const auto circle = std::find_if(
            reference.circles.begin(), reference.circles.end(),
            [id](const auto& item) { return item.id == id && item.before; });
        if (circle != reference.circles.end()) {
          switch (descriptor.provenance.role) {
            case ParameterRole::CircleCenterX: return circle->before->center.xMm;
            case ParameterRole::CircleCenterY: return circle->before->center.yMm;
            case ParameterRole::CircleRadius: return circle->before->radiusMm;
            default: break;
          }
        }
        const auto arc = std::find_if(
            reference.arcs.begin(), reference.arcs.end(),
            [id](const auto& item) { return item.id == id && item.before; });
        if (arc != reference.arcs.end()) {
          switch (descriptor.provenance.role) {
            case ParameterRole::ArcCenterX: return arc->before->center.xMm;
            case ParameterRole::ArcCenterY: return arc->before->center.yMm;
            case ParameterRole::ArcRadius: return arc->before->radiusMm;
            case ParameterRole::ArcStartAngle:
              return arc->before->startAngleRad;
            case ParameterRole::ArcSweepAngle:
              return arc->before->sweepAngleRad;
            default: break;
          }
        }
        const auto bezier = std::find_if(
            reference.beziers.begin(), reference.beziers.end(),
            [id](const auto& item) { return item.id == id && item.before; });
        if (bezier != reference.beziers.end()) {
          const int offset = static_cast<int>(descriptor.provenance.role) -
                             static_cast<int>(ParameterRole::BezierPoint0X);
          if (offset >= 0 && offset < 8)
            return offset % 2 == 0
                       ? bezier->before->points[offset / 2].xMm
                       : bezier->before->points[offset / 2].yMm;
        }
        return descriptor.value;
      };
      for (const auto& descriptor : layout.parameters())
        if (descriptor.variableIndex)
          request.previousCandidate[*descriptor.variableIndex] =
              referenceValue(descriptor);
    }
    if (preferredConstraintId != kInvalidConstraintId) {
      const auto preferred = local.constraintIndex(preferredConstraintId);
      if (preferred) {
        const auto type = static_cast<ConstraintType>(
            local.constraints_[*preferred].type);
        if (type == ConstraintType::Horizontal ||
            type == ConstraintType::Vertical) {
          const GeometryId lineId = local.constraints_[*preferred].firstGeometry;
          const auto lineIndex = local.lineIndex(lineId);
          if (lineIndex) {
            const Line& line = local.lines_[*lineIndex];
            const double dx = line.end.xMm - line.start.xMm;
            const double dy = line.end.yMm - line.start.yMm;
            const double length = std::hypot(dx, dy);
            const bool needsAxisSeed =
                length > 1e-12 &&
                ((type == ConstraintType::Vertical && std::abs(dy) <= 1e-12) ||
                 (type == ConstraintType::Horizontal &&
                  std::abs(dx) <= 1e-12));
            if (needsAxisSeed) {
              const auto layout =
                  ParameterLayout::build(local, component.geometryIds);
              if (request.previousCandidate.empty()) {
                request.previousCandidate.assign(layout.variableCount(), 0.0);
                for (const auto& descriptor : layout.parameters())
                  if (descriptor.variableIndex)
                    request.previousCandidate[*descriptor.variableIndex] =
                        descriptor.value;
              }
              const auto seed = [&](ParameterRole role, double value) {
                const auto domain = layout.parameterIndex(lineId, role);
                if (!domain) return;
                const auto variable =
                    layout.parameters()[*domain].variableIndex;
                if (variable) request.previousCandidate[*variable] = value;
              };
              if (type == ConstraintType::Vertical) {
                seed(ParameterRole::LineEndX, line.start.xMm);
                seed(ParameterRole::LineEndY,
                     line.start.yMm + std::copysign(length, dx));
              } else {
                seed(ParameterRole::LineEndX,
                     line.start.xMm + std::copysign(length, dy));
                seed(ParameterRole::LineEndY, line.start.yMm);
              }
            }
          }
        }
      }
    }
    if (useDirtyGeometryHints &&
        preferredConstraintId == kInvalidConstraintId) {
      const auto hintLayout =
          ParameterLayout::build(local, component.geometryIds);
      for (const auto& parameter : hintLayout.parameters()) {
        const bool dirty =
            std::find(dirtyGeometry.begin(), dirtyGeometry.end(),
                      parameter.provenance.geometryId) != dirtyGeometry.end();
        const bool preserveShape =
            parameter.provenance.role == ParameterRole::CircleRadius ||
            parameter.provenance.role == ParameterRole::ArcRadius ||
            parameter.provenance.role == ParameterRole::ArcStartAngle ||
            parameter.provenance.role == ParameterRole::ArcSweepAngle;
        if (parameter.variableIndex && (dirty || preserveShape))
          request.transientHints.push_back(
              {parameter.provenance, parameter.value});
      }
    }
    request.maximumIterations =
        static_cast<std::size_t>(std::max(256, maxPasses * 16));
    request.diagnoseRedundancy =
        preferredConstraintId != kInvalidConstraintId;
    request.calculateRank = !useDirtyGeometryHints ||
                            preferredConstraintId != kInvalidConstraintId;
    const SolveReport report = productionSketchSolver().solve(request);
    SolveResult result;
    result.status = report.status;
    result.converged = report.solved();
    result.passes = report.iterations;
    result.equationRank = report.equationRank;
    result.degreesOfFreedom = report.degreesOfFreedom;
    result.rankComputed = report.rankComputed;
    result.maxNormalizedResidual = report.maxNormalizedResidual;
    result.conflictingConstraintIds = report.conflictingConstraintIds;
    result.redundantConstraintIds = report.redundantConstraintIds;
    result.partiallyRedundantConstraintIds =
        report.partiallyRedundantConstraintIds;
    result.remainingDofGeometryIds = report.remainingDofGeometryIds;
    result.lockedSnapshotSize = static_cast<std::size_t>(std::count_if(
        component.geometryIds.begin(), component.geometryIds.end(),
        [&local](GeometryId id) { return local.isGeometryLocked(id); }));
    if (report.status == SolveStatus::Unsupported) ++result.unsupported;
    if (report.status == SolveStatus::Malformed) {
      const auto finitePoint = [](Point point) {
        return std::isfinite(point.xMm) && std::isfinite(point.yMm);
      };
      const bool malformedGeometry =
          std::any_of(local.lines_.begin(), local.lines_.end(),
                      [&](const Line& line) {
                        return !finitePoint(line.start) ||
                               !finitePoint(line.end) ||
                               std::hypot(line.end.xMm - line.start.xMm,
                                          line.end.yMm - line.start.yMm) <= 1e-9;
                      }) ||
          std::any_of(local.circles_.begin(), local.circles_.end(),
                      [&](const Circle& circle) {
                        return !finitePoint(circle.center) ||
                               !std::isfinite(circle.radiusMm) ||
                               circle.radiusMm <= 0.0;
                      }) ||
          std::any_of(local.arcs_.begin(), local.arcs_.end(),
                      [&](const Arc& arc) {
                        return !finitePoint(arc.center) ||
                               !std::isfinite(arc.radiusMm) ||
                               !std::isfinite(arc.startAngleRad) ||
                               !std::isfinite(arc.sweepAngleRad) ||
                               arc.radiusMm <= 0.0 ||
                               std::abs(arc.sweepAngleRad) <= 1e-12;
                      });
      if (!malformedGeometry) ++result.invalidReferences;
    }
    if (!report.solved()) result.violatedConstraints = 1;

    if (report.solved()) {
      const auto layout = ParameterLayout::build(local, component.geometryIds);
      if (layout.variableCount() != report.candidate.size()) {
        result.status = SolveStatus::BackendFailure;
        result.converged = false;
        result.violatedConstraints = 1;
      } else {
        for (const auto& parameter : layout.parameters()) {
          if (!parameter.variableIndex) continue;
          const double value = report.candidate[*parameter.variableIndex];
          const auto location =
              local.geometryLocation(parameter.provenance.geometryId);
          if (!location) {
            result.status = SolveStatus::Malformed;
            result.converged = false;
            ++result.invalidReferences;
            break;
          }
          switch (parameter.provenance.role) {
            case ParameterRole::LineStartX:
              local.lines_[location->index].start.xMm = value; break;
            case ParameterRole::LineStartY:
              local.lines_[location->index].start.yMm = value; break;
            case ParameterRole::LineEndX:
              local.lines_[location->index].end.xMm = value; break;
            case ParameterRole::LineEndY:
              local.lines_[location->index].end.yMm = value; break;
            case ParameterRole::CircleCenterX:
              local.circles_[location->index].center.xMm = value; break;
            case ParameterRole::CircleCenterY:
              local.circles_[location->index].center.yMm = value; break;
            case ParameterRole::CircleRadius:
              local.circles_[location->index].radiusMm = value; break;
            case ParameterRole::ArcCenterX:
              local.arcs_[location->index].center.xMm = value; break;
            case ParameterRole::ArcCenterY:
              local.arcs_[location->index].center.yMm = value; break;
            case ParameterRole::ArcRadius:
              local.arcs_[location->index].radiusMm = value; break;
            case ParameterRole::ArcStartAngle:
              local.arcs_[location->index].startAngleRad = value; break;
            case ParameterRole::ArcSweepAngle:
              local.arcs_[location->index].sweepAngleRad = value; break;
            default: {
              const int offset =
                  static_cast<int>(parameter.provenance.role) -
                  static_cast<int>(ParameterRole::BezierPoint0X);
              if (offset < 0 || offset >= 8) {
                result.status = SolveStatus::BackendFailure;
                result.converged = false;
                break;
              }
              auto& point = local.beziers_[location->index].points[offset / 2];
              if (offset % 2 == 0) point.xMm = value;
              else point.yMm = value;
              break;
            }
          }
        }
        local.invalidateStructureIndexes();
        local.updateBounds();
      }
    }
    ++total.componentsVisited;
    total.geometriesVisited += component.geometryIds.size();
    total.constraintsVisited += component.constraintIds.size();
    total.applied += result.applied;
    total.unsupported += result.unsupported;
    total.invalidReferences += result.invalidReferences;
    total.passes += result.passes;
    total.lockedSnapshotSize += result.lockedSnapshotSize;
    total.allocationCount += local.ownedAllocationBlocks();
    total.allocatedBytes += local.ownedBytes();
    total.peakOwnedBytes = std::max(
        total.peakOwnedBytes, sketch.ownedBytes() + local.ownedBytes());
    total.violatedConstraints += result.violatedConstraints;
    total.maxNormalizedResidual =
        std::max(total.maxNormalizedResidual, result.maxNormalizedResidual);
    total.converged = total.converged && result.converged;
    const auto statusPriority = [](SolveStatus status) {
      switch (status) {
        case SolveStatus::BackendFailure: return 8;
        case SolveStatus::Malformed: return 7;
        case SolveStatus::Unsupported: return 6;
        case SolveStatus::Conflicting: return 5;
        case SolveStatus::NonConverged: return 4;
        case SolveStatus::PartiallyRedundant: return 3;
        case SolveStatus::Redundant: return 2;
        case SolveStatus::SolvedUnderConstrained: return 1;
        case SolveStatus::SolvedFullyConstrained: return 0;
      }
      return 8;
    };
    if (statusPriority(result.status) > statusPriority(total.status))
      total.status = result.status;
    total.equationRank += result.equationRank;
    total.degreesOfFreedom += result.degreesOfFreedom;
    total.rankComputed = total.rankComputed && result.rankComputed;
    total.conflictingConstraintIds.insert(
        total.conflictingConstraintIds.end(),
        result.conflictingConstraintIds.begin(),
        result.conflictingConstraintIds.end());
    total.redundantConstraintIds.insert(
        total.redundantConstraintIds.end(),
        result.redundantConstraintIds.begin(),
        result.redundantConstraintIds.end());
    total.partiallyRedundantConstraintIds.insert(
        total.partiallyRedundantConstraintIds.end(),
        result.partiallyRedundantConstraintIds.begin(),
        result.partiallyRedundantConstraintIds.end());
    total.remainingDofGeometryIds.insert(
        total.remainingDofGeometryIds.end(),
        result.remainingDofGeometryIds.begin(),
        result.remainingDofGeometryIds.end());

    const auto finitePoint = [](Point point) {
      return std::isfinite(point.xMm) && std::isfinite(point.yMm);
    };
    for (const auto& line : local.lines_) {
      if (!finitePoint(line.start) || !finitePoint(line.end)) {
        ++total.nonFiniteGeometry;
      } else if (std::hypot(line.end.xMm - line.start.xMm,
                            line.end.yMm - line.start.yMm) <= 1e-9) {
        ++total.degenerateGeometry;
      }
    }
    for (const auto& circle : local.circles_) {
      if (!finitePoint(circle.center) || !std::isfinite(circle.radiusMm))
        ++total.nonFiniteGeometry;
      else if (circle.radiusMm <= 0.0)
        ++total.degenerateGeometry;
    }
    for (const auto& arc : local.arcs_) {
      if (!finitePoint(arc.center) || !std::isfinite(arc.radiusMm) ||
          !std::isfinite(arc.startAngleRad) ||
          !std::isfinite(arc.sweepAngleRad)) {
        ++total.nonFiniteGeometry;
      } else if (arc.radiusMm <= 0.0 ||
                 std::abs(arc.sweepAngleRad) <= 1e-12) {
        ++total.degenerateGeometry;
      }
    }
    for (const auto& bezier : local.beziers_) {
      bool finite = true;
      for (const auto point : bezier.points) {
        if (finitePoint(point)) continue;
        ++total.nonFiniteGeometry;
        finite = false;
      }
      if (finite &&
          std::hypot(bezier.points[3].xMm - bezier.points[0].xMm,
                     bezier.points[3].yMm - bezier.points[0].yMm) <= 1e-9 &&
          std::hypot(bezier.points[2].xMm - bezier.points[1].xMm,
                     bezier.points[2].yMm - bezier.points[1].yMm) <= 1e-9)
        ++total.degenerateGeometry;
    }

    const bool componentAccepted =
        result.converged && result.violatedConstraints == 0 &&
        result.invalidReferences == 0 && result.unsupported == 0 &&
        !result.deferredByActiveSolve && total.nonFiniteGeometry == 0 &&
        total.degenerateGeometry == 0;
    if (!componentAccepted) {
      total.converged = false;
      return total;
    }
    solvedComponents.push_back(std::move(local));
  }

  // Publish every component together only after all components passed the
  // residual/reference/schema/geometry audit.  A later component failure can
  // therefore never expose an earlier component's candidate geometry.
  for (const auto& local : solvedComponents) {
    for (std::size_t index = 0; index < local.lineIds_.size(); ++index)
      if (const auto target = sketch.lineIndex(local.lineIds_[index]))
        sketch.lines_[*target] = local.lines_[index];
    for (std::size_t index = 0; index < local.circleIds_.size(); ++index)
      if (const auto target = sketch.circleIndex(local.circleIds_[index]))
        sketch.circles_[*target] = local.circles_[index];
    for (std::size_t index = 0; index < local.arcIds_.size(); ++index)
      if (const auto target = sketch.arcIndex(local.arcIds_[index]))
        sketch.arcs_[*target] = local.arcs_[index];
    for (std::size_t index = 0; index < local.bezierIds_.size(); ++index)
      if (const auto target = sketch.bezierIndex(local.bezierIds_[index]))
        sketch.beziers_[*target] = local.beziers_[index];
  }
  sketch.updateBounds();
  return total;
}

SolveResult SketchSolver::translateThenSolveForBenchmark(
    Sketch& sketch, const std::vector<GeometryId>& dirtyGeometry,
    double dxMm, double dyMm) {
  // Model a real press/move/release transaction. The open journal is the
  // press-time reference consumed by solveStableComponent when it builds the
  // previous candidate; finishing it after solve models the single release
  // commit. Keeping this inside the measured helper also accounts for the
  // production journal overhead.
  sketch.beginDeltaJournal();
  for (const auto id : dirtyGeometry) {
    const auto index = sketch.lineIndex(id);
    if (!index) continue;
    sketch.journalCaptureGeometry(id);
    sketch.lines_[*index].start.xMm += dxMm;
    sketch.lines_[*index].start.yMm += dyMm;
    sketch.lines_[*index].end.xMm += dxMm;
    sketch.lines_[*index].end.yMm += dyMm;
  }
  sketch.invalidateStructureIndexes();
  sketch.updateBounds();
  auto result = solveStableComponent(sketch, dirtyGeometry, 16,
                                     kInvalidConstraintId, true);
  static_cast<void>(sketch.finishDeltaJournal());
  return result;
}

SolveResult SketchSolver::solveStable(
    Sketch& sketch, int maxPasses) {
  const std::uint64_t fingerprint = sketch.solverFingerprint();
  if (sketch.hasLastSolvedFingerprint_ &&
      sketch.lastSolvedFingerprint_ == fingerprint) {
    SolveResult cached = sketch.lastSolveResult_;
    cached.applied = 0;
    cached.passes = 0;
    cached.componentsVisited = 0;
    cached.geometriesVisited = 0;
    cached.constraintsVisited = 0;
    cached.lockedSnapshotSize = 0;
    cached.fullDiagnosticsCount = 0;
    cached.allocationCount = 0;
    cached.allocatedBytes = 0;
    cached.peakOwnedBytes = 0;
    return cached;
  }

  // Reject malformed persisted references before solving any component.
  // Otherwise a valid component could be published before an orphan
  // constraint, which belongs to no component, is discovered below.
  const auto invalidConstraintIds = sketch.invalidReferenceConstraintIds();
  if (!invalidConstraintIds.empty()) {
    SolveResult invalid;
    invalid.converged = false;
    invalid.invalidReferences = invalidConstraintIds.size();
    invalid.constraintsVisited = sketch.constraints_.size();
    return invalid;
  }

  const auto components = sketch.constraintComponents();
  std::vector<ConstraintId> visited;
  for (const auto& component : components)
    visited.insert(visited.end(), component.constraintIds.begin(),
                   component.constraintIds.end());
  std::sort(visited.begin(), visited.end());
  visited.erase(std::unique(visited.begin(), visited.end()), visited.end());
  std::size_t unvisitedConstraintCount = 0;
  for (const auto& constraint : sketch.constraints_)
    if (participatesInConstraintSystem(constraint) &&
        !std::binary_search(visited.begin(), visited.end(), constraint.id))
      ++unvisitedConstraintCount;
  if (unvisitedConstraintCount != 0) {
    SolveResult invalid;
    invalid.converged = false;
    invalid.invalidReferences = unvisitedConstraintCount;
    invalid.constraintsVisited = sketch.constraints_.size();
    return invalid;
  }

  std::vector<GeometryId> seeds;
  seeds.reserve(components.size());
  for (const auto& component : components) {
    if (!component.constraintIds.empty() && !component.geometryIds.empty())
      seeds.push_back(component.geometryIds.front());
  }
  if (seeds.empty() && sketch.constraints_.empty()) {
    sketch.lastSolvedFingerprint_ = fingerprint;
    sketch.hasLastSolvedFingerprint_ = true;
    sketch.lastSolveConverged_ = true;
    sketch.lastSolveViolatedConstraints_ = 0;
    sketch.lastSolveMaxNormalizedResidual_ = 0.0;
    sketch.lastSolveUnsupported_ = 0;
    sketch.lastSolveInvalidReferences_ = 0;
    sketch.lastSolveResult_ = {};
    return {};
  }
  SolveResult result = solveStableComponent(sketch, seeds, maxPasses,
                                            kInvalidConstraintId, false);
  const auto audit = analyzeConstraintSystem(sketch, false);
  result.converged = result.converged && !audit.conflicting;
  result.violatedConstraints = audit.violations.size();
  result.maxNormalizedResidual = audit.maxNormalizedResidual;
  if (result.converged && result.violatedConstraints == 0 &&
      result.unsupported == 0 && result.invalidReferences == 0 &&
      result.nonFiniteGeometry == 0 && result.degenerateGeometry == 0 &&
      !result.deferredByActiveSolve) {
    sketch.lastSolvedFingerprint_ = sketch.solverFingerprint();
    sketch.hasLastSolvedFingerprint_ = true;
    sketch.lastSolveConverged_ = true;
    sketch.lastSolveViolatedConstraints_ = 0;
    sketch.lastSolveMaxNormalizedResidual_ = result.maxNormalizedResidual;
    sketch.lastSolveUnsupported_ = 0;
    sketch.lastSolveInvalidReferences_ = 0;
    sketch.lastSolveResult_ = result;
  }
  return result;
}

}  // namespace solidar::sketch
