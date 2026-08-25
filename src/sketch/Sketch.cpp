#include "sketch/Sketch.h"

#include "sketch/SketchSolver.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace solidar::sketch {

Sketch::Sketch() { clear(); }

void Sketch::clear() {
  centerNodeElementIds_.clear();
  lines_.clear();
  circles_.clear();
  lineIds_.clear();
  circleIds_.clear();
  dimensions_.clear();
  constraints_.clear();
  widthMm_ = 0.0;
  heightMm_ = 0.0;
}

void Sketch::setRectangle(double widthMm, double heightMm) {
  if (widthMm <= 0.0 || heightMm <= 0.0)
    throw std::invalid_argument("Sketch dimensions must be positive");

  clear();
  const double halfWidth = widthMm * 0.5;
  const double halfHeight = heightMm * 0.5;
  const Point bottomLeft{-halfWidth, -halfHeight};
  const Point bottomRight{halfWidth, -halfHeight};
  const Point topRight{halfWidth, halfHeight};
  const Point topLeft{-halfWidth, halfHeight};
  const auto elementId = nextElementId_++;
  lines_ = {{bottomLeft, bottomRight, elementId},
            {bottomRight, topRight, elementId},
            {topRight, topLeft, elementId},
            {topLeft, bottomLeft, elementId}};
  lineIds_.clear();
  lineIds_.reserve(lines_.size());
  for (std::size_t index = 0; index < lines_.size(); ++index)
    lineIds_.push_back(nextGeometryId_++);
  updateBounds();
}

void Sketch::addLine(Point start, Point end) {
  if (start.xMm == end.xMm && start.yMm == end.yMm) return;
  lines_.push_back({start, end, nextElementId_++});
  lineIds_.push_back(nextGeometryId_++);
  updateBounds();
}

void Sketch::addLine(Point start, Point end, std::size_t elementId) {
  if (start.xMm == end.xMm && start.yMm == end.yMm) return;

  // elementId is a group/composite identifier, not a GeometryId.
  // Serialized rectangles intentionally reuse the same elementId on
  // multiple line primitives.
  if (elementId == 0) {
    addLine(start, end);
    return;
  }

  lines_.push_back({start, end, elementId});
  lineIds_.push_back(nextGeometryId_++);

  // Prevent subsequently created elements from reusing a restored ID.
  nextElementId_ = std::max(nextElementId_, elementId + 1);
  updateBounds();
}

void Sketch::addRectangle(Point firstCorner, Point oppositeCorner) {
  if (std::abs(firstCorner.xMm - oppositeCorner.xMm) < 1e-9 ||
      std::abs(firstCorner.yMm - oppositeCorner.yMm) < 1e-9)
    return;

  const Point second{oppositeCorner.xMm, firstCorner.yMm};
  const Point fourth{firstCorner.xMm, oppositeCorner.yMm};
  const auto elementId = nextElementId_++;

  const GeometryId firstLineId = nextGeometryId_++;
  const GeometryId secondLineId = nextGeometryId_++;
  const GeometryId thirdLineId = nextGeometryId_++;
  const GeometryId fourthLineId = nextGeometryId_++;

  lines_.push_back({firstCorner, second, elementId});
  lineIds_.push_back(firstLineId);
  lines_.push_back({second, oppositeCorner, elementId});
  lineIds_.push_back(secondLineId);
  lines_.push_back({oppositeCorner, fourth, elementId});
  lineIds_.push_back(thirdLineId);
  lines_.push_back({fourth, firstCorner, elementId});
  lineIds_.push_back(fourthLineId);

  const auto addCornerCoincident =
      [this](GeometryId firstId, bool firstStart,
             GeometryId secondId, bool secondStart) {
        Constraint constraint;
        constraint.type = ConstraintType::Coincident;
        constraint.firstPoint = PointReference{firstId, firstStart};
        constraint.secondPoint = PointReference{secondId, secondStart};
        addConstraint(constraint);
      };

  // Four persistent CAD corners.
  addCornerCoincident(firstLineId, false, secondLineId, true);
  addCornerCoincident(secondLineId, false, thirdLineId, true);
  addCornerCoincident(thirdLineId, false, fourthLineId, true);
  addCornerCoincident(fourthLineId, false, firstLineId, true);

  // Minimal non-redundant rectangle orientation system.
  // Opposite sides have equal lengths. Together with the four connected
  // corners and one right angle, this defines a rectangle without explicit
  // Parallel constraints.
  Constraint firstEqual;
  firstEqual.type = ConstraintType::Equal;
  firstEqual.firstGeometry = firstLineId;
  firstEqual.secondGeometry = thirdLineId;
  addConstraint(firstEqual);

  Constraint secondEqual;
  secondEqual.type = ConstraintType::Equal;
  secondEqual.firstGeometry = secondLineId;
  secondEqual.secondGeometry = fourthLineId;
  addConstraint(secondEqual);

  // Keep explicit parallel relationships as well. They are mathematically
  // redundant with the rectangle system, but make the current sequential
  // solver much more stable during interactive dragging.
  Constraint firstParallel;
  firstParallel.type = ConstraintType::Parallel;
  firstParallel.firstGeometry = firstLineId;
  firstParallel.secondGeometry = thirdLineId;
  addConstraint(firstParallel);

  Constraint secondParallel;
  secondParallel.type = ConstraintType::Parallel;
  secondParallel.firstGeometry = secondLineId;
  secondParallel.secondGeometry = fourthLineId;
  addConstraint(secondParallel);

  Constraint perpendicular;
  perpendicular.type = ConstraintType::Perpendicular;
  perpendicular.firstGeometry = firstLineId;
  perpendicular.secondGeometry = secondLineId;
  addConstraint(perpendicular);

  // A standard two-point rectangle is axis-aligned. Keep that absolute
  // orientation while allowing width and height to change independently.
  Constraint horizontal;
  horizontal.type = ConstraintType::Horizontal;
  horizontal.firstGeometry = firstLineId;
  addConstraint(horizontal);

  Constraint vertical;
  vertical.type = ConstraintType::Vertical;
  vertical.firstGeometry = secondLineId;
  addConstraint(vertical);

  updateBounds();
}
void Sketch::addRectangle(Point first, Point second, Point third, Point fourth) {
  const auto elementId = nextElementId_++;

  const GeometryId firstLineId = nextGeometryId_++;
  const GeometryId secondLineId = nextGeometryId_++;
  const GeometryId thirdLineId = nextGeometryId_++;
  const GeometryId fourthLineId = nextGeometryId_++;

  lines_.push_back({first, second, elementId});
  lineIds_.push_back(firstLineId);
  lines_.push_back({second, third, elementId});
  lineIds_.push_back(secondLineId);
  lines_.push_back({third, fourth, elementId});
  lineIds_.push_back(thirdLineId);
  lines_.push_back({fourth, first, elementId});
  lineIds_.push_back(fourthLineId);

  const auto addCornerCoincident =
      [this](GeometryId firstId, bool firstStart,
             GeometryId secondId, bool secondStart) {
        Constraint constraint;
        constraint.type = ConstraintType::Coincident;
        constraint.firstPoint = PointReference{firstId, firstStart};
        constraint.secondPoint = PointReference{secondId, secondStart};
        addConstraint(constraint);
      };

  addCornerCoincident(firstLineId, false, secondLineId, true);
  addCornerCoincident(secondLineId, false, thirdLineId, true);
  addCornerCoincident(thirdLineId, false, fourthLineId, true);
  addCornerCoincident(fourthLineId, false, firstLineId, true);

  // Opposite sides have equal lengths. Together with the four connected
  // corners and one right angle, this defines a rectangle without explicit
  // Parallel constraints.
  Constraint firstEqual;
  firstEqual.type = ConstraintType::Equal;
  firstEqual.firstGeometry = firstLineId;
  firstEqual.secondGeometry = thirdLineId;
  addConstraint(firstEqual);

  Constraint secondEqual;
  secondEqual.type = ConstraintType::Equal;
  secondEqual.firstGeometry = secondLineId;
  secondEqual.secondGeometry = fourthLineId;
  addConstraint(secondEqual);

  // Keep explicit parallel relationships as well. They are mathematically
  // redundant with the rectangle system, but make the current sequential
  // solver much more stable during interactive dragging.
  Constraint firstParallel;
  firstParallel.type = ConstraintType::Parallel;
  firstParallel.firstGeometry = firstLineId;
  firstParallel.secondGeometry = thirdLineId;
  addConstraint(firstParallel);

  Constraint secondParallel;
  secondParallel.type = ConstraintType::Parallel;
  secondParallel.firstGeometry = secondLineId;
  secondParallel.secondGeometry = fourthLineId;
  addConstraint(secondParallel);

  Constraint perpendicular;
  perpendicular.type = ConstraintType::Perpendicular;
  perpendicular.firstGeometry = firstLineId;
  perpendicular.secondGeometry = secondLineId;
  addConstraint(perpendicular);

  updateBounds();
}
void Sketch::addCircle(Point center, double radiusMm) {
  if (radiusMm <= 0.0) return;
  circles_.push_back({center, radiusMm});
  circleIds_.push_back(nextGeometryId_++);
  updateBounds();
}

void Sketch::removeLine(std::size_t index) {
  if (index >= lines_.size()) return;

  const GeometryId removedId = lineIds_[index];
  lines_.erase(lines_.begin() + index);
  lineIds_.erase(lineIds_.begin() + index);

  auto& dimensions = dimensions_;
  std::erase_if(dimensions, [removedId](const Dimension& dimension) {
    if (dimension.kind == DimensionKind::LineLength)
      return dimension.geometryId == removedId;
    if (dimension.kind == DimensionKind::PointDistance ||
        dimension.kind == DimensionKind::PointDistanceX ||
        dimension.kind == DimensionKind::PointDistanceY)
      return dimension.firstPoint.lineId == removedId ||
             dimension.secondPoint.lineId == removedId;
    if (dimension.kind == DimensionKind::LineAngle)
      return dimension.geometryId == removedId ||
             dimension.secondPoint.lineId == removedId;
    return false;
  });

  std::erase_if(constraints_, [removedId](const Constraint& constraint) {
    return constraint.firstGeometry == removedId ||
           constraint.secondGeometry == removedId ||
           constraint.firstPoint.lineId == removedId ||
           constraint.secondPoint.lineId == removedId;
  });

  updateBounds();
}

void Sketch::removeCircle(std::size_t index) {
  if (index >= circles_.size()) return;

  const GeometryId removedId = circleIds_[index];
  circles_.erase(circles_.begin() + index);
  circleIds_.erase(circleIds_.begin() + index);

  auto& dimensions = dimensions_;
  std::erase_if(dimensions, [removedId](const Dimension& dimension) {
    if (dimension.kind == DimensionKind::CircleDiameter)
      return dimension.geometryId == removedId;

    // Point-to-point dimensions may reference a circle center through
    // PointReference::circleId. Remove them together with the circle so no
    // stale point reference survives deletion.
    if (dimension.kind == DimensionKind::PointDistance ||
        dimension.kind == DimensionKind::PointDistanceX ||
        dimension.kind == DimensionKind::PointDistanceY)
      return dimension.firstPoint.circleId == removedId ||
             dimension.secondPoint.circleId == removedId;

    return false;
  });

  std::erase_if(constraints_, [removedId](const Constraint& constraint) {
    return constraint.firstGeometry == removedId ||
           constraint.secondGeometry == removedId ||
           constraint.firstPoint.circleId == removedId ||
           constraint.secondPoint.circleId == removedId;
  });

  updateBounds();
}

void Sketch::removeElement(std::size_t elementId) {
  std::vector<GeometryId> removedIds;
  for (std::size_t index = lines_.size(); index > 0; --index) {
    const std::size_t current = index - 1;
    if (lines_[current].elementId != elementId) continue;
    removedIds.push_back(lineIds_[current]);
    lines_.erase(lines_.begin() + current);
    lineIds_.erase(lineIds_.begin() + current);
  }

  if (!removedIds.empty()) {
    const auto wasRemoved = [&removedIds](GeometryId id) {
      return std::find(removedIds.begin(), removedIds.end(), id) !=
             removedIds.end();
    };

    auto& dimensions = dimensions_;
    std::erase_if(dimensions, [&wasRemoved](const Dimension& dimension) {
      if (dimension.kind == DimensionKind::LineLength)
        return wasRemoved(dimension.geometryId);
      if (dimension.kind == DimensionKind::PointDistance ||
          dimension.kind == DimensionKind::PointDistanceX ||
          dimension.kind == DimensionKind::PointDistanceY)
        return wasRemoved(dimension.firstPoint.lineId) ||
               wasRemoved(dimension.secondPoint.lineId);
      if (dimension.kind == DimensionKind::LineAngle)
        return wasRemoved(dimension.geometryId) ||
               wasRemoved(dimension.secondPoint.lineId);
      return false;
    });

    std::erase_if(constraints_, [&wasRemoved](const Constraint& constraint) {
      return wasRemoved(constraint.firstGeometry) ||
             wasRemoved(constraint.secondGeometry) ||
             wasRemoved(constraint.firstPoint.lineId) ||
             wasRemoved(constraint.secondPoint.lineId);
    });
  }

  // A centered composite element exposes a virtual PointReference identified
  // by elementCenterId rather than by a GeometryId. Clean those references
  // explicitly when the owning element is deleted.
  std::erase_if(dimensions_, [elementId](const Dimension& dimension) {
    return dimension.firstPoint.elementCenterId == elementId ||
           dimension.secondPoint.elementCenterId == elementId;
  });

  std::erase_if(constraints_, [elementId](const Constraint& constraint) {
    return constraint.firstPoint.elementCenterId == elementId ||
           constraint.secondPoint.elementCenterId == elementId;
  });
  std::erase(centerNodeElementIds_, elementId);
  updateBounds();
}

void Sketch::markElementCenterNode(std::size_t elementId) {
  if (elementId == 0 || hasElementCenterNode(elementId))
    return;

  std::size_t lineCount = 0;
  for (const auto& line : lines_) {
    if (line.elementId == elementId)
      ++lineCount;
  }

  // Current rectangle elements are exactly four perimeter lines.
  if (lineCount == 4)
    centerNodeElementIds_.push_back(elementId);
}

bool Sketch::hasElementCenterNode(std::size_t elementId) const noexcept {
  return std::find(centerNodeElementIds_.begin(),
                   centerNodeElementIds_.end(),
                   elementId) != centerNodeElementIds_.end();
}

std::optional<Point> Sketch::elementCenterPoint(
    std::size_t elementId) const noexcept {
  if (!hasElementCenterNode(elementId))
    return std::nullopt;

  std::vector<const Line*> elementLines;
  elementLines.reserve(4);

  for (const auto& line : lines_) {
    if (line.elementId == elementId)
      elementLines.push_back(&line);
  }

  if (elementLines.size() != 4)
    return std::nullopt;

  // Average the four perimeter starts. Rectangle lines are stored in
  // perimeter order, so these are exactly the four rectangle vertices.
  Point center{};

  for (const auto* line : elementLines) {
    center.xMm += line->start.xMm;
    center.yMm += line->start.yMm;
  }

  center.xMm *= 0.25;
  center.yMm *= 0.25;
  return center;
}

const std::vector<std::size_t>&
Sketch::centerNodeElementIds() const noexcept {
  return centerNodeElementIds_;
}
void Sketch::translateElement(std::size_t elementId, double dxMm,
                              double dyMm) {
  if (dxMm == 0.0 && dyMm == 0.0) return;

  std::vector<GeometryId> movedIds;
  for (std::size_t index = 0; index < lines_.size(); ++index) {
    if (lines_[index].elementId == elementId)
      movedIds.push_back(lineIds_[index]);
  }

  const auto isMoved = [&movedIds](GeometryId id) {
    return std::find(movedIds.begin(), movedIds.end(), id) != movedIds.end();
  };

  // If the selected element has Coincident constraints, remember the
  // connected endpoint(s) outside the selected element. They should follow
  // the drag instead of being left behind.
  std::vector<PointReference> connectedExternalPoints;
  for (const auto& constraint : constraints_) {
    if (constraint.type != ConstraintType::Coincident) continue;

    const bool firstMoved = isMoved(constraint.firstPoint.lineId);
    const bool secondMoved = isMoved(constraint.secondPoint.lineId);
    if (firstMoved == secondMoved) continue;

    const PointReference external =
        firstMoved ? constraint.secondPoint : constraint.firstPoint;

    const auto duplicate = std::find_if(
        connectedExternalPoints.begin(), connectedExternalPoints.end(),
        [external](const PointReference& item) {
          if (external.circleId != kInvalidGeometryId ||
              item.circleId != kInvalidGeometryId)
            return external.circleId != kInvalidGeometryId &&
                   item.circleId == external.circleId;

          return item.lineId == external.lineId &&
                 item.start == external.start;
        });
    if (duplicate == connectedExternalPoints.end())
      connectedExternalPoints.push_back(external);
  }

  // Capture the old coordinates of those external endpoint clusters before
  // the selected element is moved.
  struct ConnectedPointMove {
    PointReference reference;
    Point oldPoint;
    Point newPoint;
  };
  std::vector<ConnectedPointMove> connectedMoves;
  connectedMoves.reserve(connectedExternalPoints.size());
  for (const auto reference : connectedExternalPoints) {
    const auto point = referencedPoint(reference);
    if (!point) continue;
    connectedMoves.push_back(
        {reference, *point,
         {point->xMm + dxMm, point->yMm + dyMm}});
  }

  // Points constrained to a moved carrier line belong to that moving frame.
  // Move them by the same delta before the final projection pass.
  std::vector<PointReference> pointOnLineFollowers;

  const auto samePointReference =
      [](PointReference first, PointReference second) {
        if (first.circleId != kInvalidGeometryId ||
            second.circleId != kInvalidGeometryId) {
          return first.circleId != kInvalidGeometryId &&
                 first.circleId == second.circleId;
        }

        return first.lineId == second.lineId &&
               first.start == second.start;
      };

  for (const auto& constraint : constraints_) {
    if (constraint.type != ConstraintType::PointOnLine ||
        !isMoved(constraint.firstGeometry))
      continue;

    const auto duplicate = std::any_of(
        pointOnLineFollowers.begin(),
        pointOnLineFollowers.end(),
        [&constraint, &samePointReference](PointReference item) {
          return samePointReference(item, constraint.secondPoint);
        });

    if (!duplicate)
      pointOnLineFollowers.push_back(constraint.secondPoint);
  }

  for (auto& line : lines_) {
    if (line.elementId != elementId) continue;
    line.start.xMm += dxMm;
    line.start.yMm += dyMm;
    line.end.xMm += dxMm;
    line.end.yMm += dyMm;
  }

  for (const auto follower : pointOnLineFollowers) {
    if (follower.circleId != kInvalidGeometryId) {
      const auto circle = circleIndex(follower.circleId);
      if (!circle) continue;
      circles_[*circle].center.xMm += dxMm;
      circles_[*circle].center.yMm += dyMm;
      continue;
    }

    const auto followerIndex = lineIndex(follower.lineId);
    if (!followerIndex || isMoved(follower.lineId)) continue;

    Point& followerPoint =
        follower.start ? lines_[*followerIndex].start
                       : lines_[*followerIndex].end;
    followerPoint.xMm += dxMm;
    followerPoint.yMm += dyMm;
  }

  const auto same = [](Point first, Point second) {
    return std::hypot(first.xMm - second.xMm,
                      first.yMm - second.yMm) <= 1e-7;
  };

  // Move only the connected endpoint cluster of the neighbouring geometry.
  // The rest of that neighbouring line stays where it was, so it stretches /
  // rotates naturally while remaining connected.
  for (const auto& move : connectedMoves) {
    if (move.reference.circleId != kInvalidGeometryId) {
      const auto circle = circleIndex(move.reference.circleId);
      if (circle && same(circles_[*circle].center, move.oldPoint))
        circles_[*circle].center = move.newPoint;
      continue;
    }

    for (std::size_t index = 0; index < lines_.size(); ++index) {
      if (isMoved(lineIds_[index])) continue;
      auto& line = lines_[index];
      if (same(line.start, move.oldPoint)) line.start = move.newPoint;
      if (same(line.end, move.oldPoint)) line.end = move.newPoint;
    }
  }

  // Re-apply all active constraints after interactive geometry movement.
  (void)BasicSketchSolver::solve(*this);
  updateBounds();
}

void Sketch::translateSelection(
    const std::vector<std::size_t>& elementIds,
    const std::vector<GeometryId>& circleIds,
    double dxMm, double dyMm) {
  if (dxMm == 0.0 && dyMm == 0.0) return;
  if (elementIds.empty() && circleIds.empty()) return;

  const auto elementSelected =
      [&elementIds](std::size_t elementId) {
        return std::find(
                   elementIds.begin(),
                   elementIds.end(),
                   elementId) !=
               elementIds.end();
      };

  const auto circleSelected =
      [&circleIds](GeometryId id) {
        return std::find(
                   circleIds.begin(),
                   circleIds.end(),
                   id) !=
               circleIds.end();
      };

  // CRASH-FREE 04: COMPLETE POINTREFERENCE IDENTITY IN GROUP DRAG
  const auto sameReference =
      [](PointReference first,
         PointReference second) {
        if (first.elementCenterId != 0 ||
            second.elementCenterId != 0) {
          return first.elementCenterId != 0 &&
                 second.elementCenterId != 0 &&
                 first.elementCenterId ==
                     second.elementCenterId;
        }

        if (first.circleId != kInvalidGeometryId ||
            second.circleId != kInvalidGeometryId) {
          return first.circleId != kInvalidGeometryId &&
                 second.circleId != kInvalidGeometryId &&
                 first.circleId == second.circleId;
        }

        if (first.lineId == kInvalidGeometryId ||
            second.lineId == kInvalidGeometryId)
          return false;

        return first.lineId == second.lineId &&
               first.start == second.start;
      };

  std::vector<PointReference> movedReferences;

  const auto addReference =
      [&movedReferences,
       &sameReference](PointReference reference) {
        const bool exists =
            std::any_of(
                movedReferences.begin(),
                movedReferences.end(),
                [reference,
                 &sameReference](
                    PointReference item) {
                  return sameReference(
                      item,
                      reference);
                });

        if (!exists)
          movedReferences.push_back(reference);
      };

  for (std::size_t index = 0;
       index < lines_.size();
       ++index) {
    if (!elementSelected(
            lines_[index].elementId))
      continue;

    const GeometryId id =
        lineIds_[index];

    if (id == kInvalidGeometryId)
      continue;

    addReference(PointReference{id, true});
    addReference(PointReference{id, false});

    if (hasElementCenterNode(
            lines_[index].elementId)) {
      PointReference center;
      center.elementCenterId =
          lines_[index].elementId;
      addReference(center);
    }
  }

  for (std::size_t index = 0;
       index < circles_.size();
       ++index) {
    const GeometryId id =
        circleIds_[index];

    if (!circleSelected(id))
      continue;

    PointReference center;
    center.circleId = id;
    addReference(center);
  }

  bool expanded = true;

  while (expanded) {
    expanded = false;

    for (const auto& constraint :
         constraints_) {
      if (constraint.type !=
          ConstraintType::Coincident)
        continue;

      const auto first =
          constraint.firstPoint;
      const auto second =
          constraint.secondPoint;

      if (!referencedPoint(first) ||
          !referencedPoint(second))
        continue;

      const bool hasFirst =
          std::any_of(
              movedReferences.begin(),
              movedReferences.end(),
              [first,
               &sameReference](
                  PointReference item) {
                return sameReference(
                    item,
                    first);
              });

      const bool hasSecond =
          std::any_of(
              movedReferences.begin(),
              movedReferences.end(),
              [second,
               &sameReference](
                  PointReference item) {
                return sameReference(
                    item,
                    second);
              });

      if (hasFirst && !hasSecond) {
        addReference(second);
        expanded = true;
      }
      else if (hasSecond && !hasFirst) {
        addReference(first);
        expanded = true;
      }
    }
  }

  const auto referenceMoves =
      [&movedReferences,
       &sameReference](
          PointReference reference) {
        return std::any_of(
            movedReferences.begin(),
            movedReferences.end(),
            [reference,
             &sameReference](
                PointReference item) {
              return sameReference(
                  item,
                  reference);
            });
      };

  std::vector<std::size_t>
      centerMovedElements;

  for (const auto reference :
       movedReferences) {
    if (reference.elementCenterId == 0)
      continue;

    if (std::find(
            centerMovedElements.begin(),
            centerMovedElements.end(),
            reference.elementCenterId) ==
        centerMovedElements.end())
      centerMovedElements.push_back(
          reference.elementCenterId);
  }

  const auto centerElementMoves =
      [&centerMovedElements](
          std::size_t elementId) {
        return std::find(
                   centerMovedElements.begin(),
                   centerMovedElements.end(),
                   elementId) !=
               centerMovedElements.end();
      };

  for (std::size_t index = 0;
       index < lines_.size();
       ++index) {
    auto& line = lines_[index];
    const GeometryId id =
        lineIds_[index];

    if (elementSelected(line.elementId) ||
        centerElementMoves(line.elementId)) {
      line.start.xMm += dxMm;
      line.start.yMm += dyMm;
      line.end.xMm += dxMm;
      line.end.yMm += dyMm;
      continue;
    }

    if (referenceMoves(
            PointReference{id, true})) {
      line.start.xMm += dxMm;
      line.start.yMm += dyMm;
    }

    if (referenceMoves(
            PointReference{id, false})) {
      line.end.xMm += dxMm;
      line.end.yMm += dyMm;
    }
  }

  for (std::size_t index = 0;
       index < circles_.size();
       ++index) {
    const GeometryId id =
        circleIds_[index];

    PointReference center;
    center.circleId = id;

    if (circleSelected(id) ||
        referenceMoves(center)) {
      circles_[index].center.xMm += dxMm;
      circles_[index].center.yMm += dyMm;
    }
  }

  (void)BasicSketchSolver::solve(*this);
  updateBounds();
}
void Sketch::setElementDashed(std::size_t elementId, bool dashed) {
  for (auto& line : lines_) {
    if (line.elementId == elementId) line.dashed = dashed;
  }
}

void Sketch::setCircleDashed(std::size_t index, bool dashed) {
  if (index < circles_.size()) circles_[index].dashed = dashed;
}

void Sketch::translateCircle(std::size_t index, double dxMm, double dyMm) {
  if (index >= circles_.size()) return;
  circles_[index].center.xMm += dxMm;
  circles_[index].center.yMm += dyMm;
  updateBounds();
}

void Sketch::setCircleDashedById(GeometryId id, bool dashed) {
  const auto index = circleIndex(id);
  if (index) setCircleDashed(*index, dashed);
}

void Sketch::translateCircleById(GeometryId id, double dxMm, double dyMm) {
  if (id == kInvalidGeometryId || !circleIndex(id)) return;

  PointReference centerReference;
  centerReference.circleId = id;

  // Move the circle center together with every Coincident-connected
  // endpoint/center. This also re-runs the active solver afterwards.
  (void)translatePoint(centerReference, dxMm, dyMm);
}

bool Sketch::setLineLengthById(GeometryId id, double lengthMm) {
  const auto index = lineIndex(id);
  if (!index || lengthMm <= 0.0) return false;

  const std::size_t elementId = lines_[*index].elementId;

  std::vector<std::size_t> elementLines;
  for (std::size_t lineIndexValue = 0;
       lineIndexValue < lines_.size(); ++lineIndexValue) {
    if (lines_[lineIndexValue].elementId == elementId)
      elementLines.push_back(lineIndexValue);
  }

  // A four-line composite element is a rectangle in the current Sketch
  // model. Resize the whole rectangle instead of stretching one primitive.
  if (elementLines.size() == 4) {
    const auto found =
        std::find(elementLines.begin(), elementLines.end(), *index);
    if (found == elementLines.end()) return false;

    const std::size_t side =
        static_cast<std::size_t>(
            std::distance(elementLines.begin(), found));

    const std::size_t i0 = elementLines[side];
    const std::size_t i1 = elementLines[(side + 1) % 4];
    const std::size_t i2 = elementLines[(side + 2) % 4];
    const std::size_t i3 = elementLines[(side + 3) % 4];

    const Line& selected = lines_[i0];
    const Line& adjacent = lines_[i1];

    double ux = selected.end.xMm - selected.start.xMm;
    double uy = selected.end.yMm - selected.start.yMm;
    const double selectedLength = std::hypot(ux, uy);
    if (selectedLength <= 1e-9) return false;

    ux /= selectedLength;
    uy /= selectedLength;

    double vx = adjacent.end.xMm - adjacent.start.xMm;
    double vy = adjacent.end.yMm - adjacent.start.yMm;

    // Orthogonalize the second rectangle axis. This also repairs a slightly
    // distorted rectangle instead of preserving its accumulated skew.
    const double projection = vx * ux + vy * uy;
    vx -= projection * ux;
    vy -= projection * uy;

    double adjacentLength = std::hypot(vx, vy);
    if (adjacentLength <= 1e-9) {
      // Preserve the handedness using the raw adjacent direction.
      const double rawX = adjacent.end.xMm - adjacent.start.xMm;
      const double rawY = adjacent.end.yMm - adjacent.start.yMm;
      const double cross = ux * rawY - uy * rawX;
      vx = cross < 0.0 ? uy : -uy;
      vy = cross < 0.0 ? -ux : ux;
      adjacentLength = std::hypot(
          adjacent.end.xMm - adjacent.start.xMm,
          adjacent.end.yMm - adjacent.start.yMm);
    } else {
      vx /= adjacentLength;
      vy /= adjacentLength;

      // Use the actual adjacent side length, not the orthogonal projection
      // length, as the preserved second rectangle dimension.
      adjacentLength = std::hypot(
          adjacent.end.xMm - adjacent.start.xMm,
          adjacent.end.yMm - adjacent.start.yMm);
    }

    if (adjacentLength <= 1e-9) return false;

    // Compute the element centre from all eight stored endpoints. Averaging
    // makes the operation stable even if Coincident is off by a tiny epsilon.
    Point center{};
    for (const auto lineIndexValue : elementLines) {
      center.xMm += lines_[lineIndexValue].start.xMm;
      center.yMm += lines_[lineIndexValue].start.yMm;
      center.xMm += lines_[lineIndexValue].end.xMm;
      center.yMm += lines_[lineIndexValue].end.yMm;
    }
    center.xMm /= 8.0;
    center.yMm /= 8.0;

    const double halfU = lengthMm * 0.5;
    const double halfV = adjacentLength * 0.5;

    const Point p0{center.xMm - ux * halfU - vx * halfV,
                   center.yMm - uy * halfU - vy * halfV};
    const Point p1{center.xMm + ux * halfU - vx * halfV,
                   center.yMm + uy * halfU - vy * halfV};
    const Point p2{center.xMm + ux * halfU + vx * halfV,
                   center.yMm + uy * halfU + vy * halfV};
    const Point p3{center.xMm - ux * halfU + vx * halfV,
                   center.yMm - uy * halfU + vy * halfV};

    lines_[i0].start = p0;
    lines_[i0].end = p1;
    lines_[i1].start = p1;
    lines_[i1].end = p2;
    lines_[i2].start = p2;
    lines_[i2].end = p3;
    lines_[i3].start = p3;
    lines_[i3].end = p0;

    updateBounds();
    return true;
  }

  return setLineLength(*index, lengthMm);
}

bool Sketch::setCircleDiameterById(GeometryId id, double diameterMm) {
  const auto index = circleIndex(id);
  return index ? setCircleDiameter(*index, diameterMm) : false;
}

bool Sketch::setLineHorizontalById(GeometryId id) {
  const auto index = lineIndex(id);
  if (!index) return false;

  const Point oldEnd = lines_[*index].end;
  const Point newEnd{oldEnd.xMm, lines_[*index].start.yMm};

  const auto same = [](Point first, Point second) {
    return std::hypot(first.xMm - second.xMm, first.yMm - second.yMm) <= 1e-7;
  };

  for (auto& line : lines_) {
    if (same(line.start, oldEnd)) line.start = newEnd;
    if (same(line.end, oldEnd)) line.end = newEnd;
  }
  updateBounds();
  return true;
}

bool Sketch::setLineVerticalById(GeometryId id) {
  const auto index = lineIndex(id);
  if (!index) return false;

  const Point oldEnd = lines_[*index].end;
  const Point newEnd{lines_[*index].start.xMm, oldEnd.yMm};

  const auto same = [](Point first, Point second) {
    return std::hypot(first.xMm - second.xMm, first.yMm - second.yMm) <= 1e-7;
  };

  for (auto& line : lines_) {
    if (same(line.start, oldEnd)) line.start = newEnd;
    if (same(line.end, oldEnd)) line.end = newEnd;
  }
  updateBounds();
  return true;
}

bool Sketch::setLinesParallelByIds(GeometryId firstId,
                                   GeometryId secondId) {
  const auto firstIndex = lineIndex(firstId);
  const auto secondIndex = lineIndex(secondId);

  if (!firstIndex || !secondIndex || firstId == secondId)
    return false;

  // CRASH-FREE 11: COMPOSITE-SAFE PARALLEL DISPATCH
  //
  // Parallel is a relative orientation constraint. Rotating only one side of
  // a rectangle tears the composite apart. If exactly one operand is a
  // composite, always rotate the standalone operand. If both operands belong
  // to different composites, reject this primitive operation rather than
  // deform either element.
  const auto elementLineCount =
      [this](std::size_t lineIndexValue) {
        if (lineIndexValue >= lines_.size())
          return std::size_t{0};

        const std::size_t elementId =
            lines_[lineIndexValue].elementId;

        std::size_t count = 0;

        for (const auto& line : lines_) {
          if (line.elementId == elementId)
            ++count;
        }

        return count;
      };

  const std::size_t firstElementCount =
      elementLineCount(*firstIndex);
  const std::size_t secondElementCount =
      elementLineCount(*secondIndex);

  const bool firstComposite =
      firstElementCount > 1;
  const bool secondComposite =
      secondElementCount > 1;

  const std::size_t firstElementId =
      lines_[*firstIndex].elementId;
  const std::size_t secondElementId =
      lines_[*secondIndex].elementId;

  if (firstElementId != secondElementId) {
    if (!firstComposite && secondComposite) {
      // Reverse the relationship: keep the rectangle side as reference and
      // rotate the standalone line instead.
      return setLinesParallelByIds(
          secondId,
          firstId);
    }

    if (firstComposite && secondComposite) {
      // Rotating one side of either composite would destroy its shape.
      return false;
    }
  }

  const auto& first = lines_[*firstIndex];
  auto& second = lines_[*secondIndex];

  const double firstDx = first.end.xMm - first.start.xMm;
  const double firstDy = first.end.yMm - first.start.yMm;
  const double firstLength = std::hypot(firstDx, firstDy);

  const double secondDx = second.end.xMm - second.start.xMm;
  const double secondDy = second.end.yMm - second.start.yMm;
  const double secondLength = std::hypot(secondDx, secondDy);

  if (firstLength <= 1e-9 || secondLength <= 1e-9)
    return false;

  const auto same = [](Point a, Point b) {
    return std::hypot(a.xMm - b.xMm,
                      a.yMm - b.yMm) <= 1e-7;
  };

  // Preserve a shared CAD vertex when the two lines are connected.
  bool pivotAtStart = true;
  Point pivot = second.start;

  if (same(second.start, first.start) ||
      same(second.start, first.end)) {
    pivotAtStart = true;
    pivot = second.start;
  } else if (same(second.end, first.start) ||
             same(second.end, first.end)) {
    pivotAtStart = false;
    pivot = second.end;
  }

  double ux = firstDx / firstLength;
  double uy = firstDy / firstLength;

  // Choose the parallel direction closest to the current orientation of the
  // moving line, avoiding an unnecessary 180-degree flip.
  const double dot = secondDx * ux + secondDy * uy;
  if (dot < 0.0) {
    ux = -ux;
    uy = -uy;
  }

  const Point oldMovingPoint =
      pivotAtStart ? second.end : second.start;

  const Point newMovingPoint =
      pivotAtStart
          ? Point{pivot.xMm + ux * secondLength,
                  pivot.yMm + uy * secondLength}
          : Point{pivot.xMm - ux * secondLength,
                  pivot.yMm - uy * secondLength};

  if (pivotAtStart)
    second.end = newMovingPoint;
  else
    second.start = newMovingPoint;

  // Preserve legacy behaviour for endpoint clusters that currently share the
  // same coordinate, including geometry without an explicit Coincident yet.
  for (std::size_t index = 0; index < lines_.size(); ++index) {
    if (index == *secondIndex) continue;

    auto& line = lines_[index];
    if (same(line.start, oldMovingPoint))
      line.start = newMovingPoint;
    if (same(line.end, oldMovingPoint))
      line.end = newMovingPoint;
  }

  updateBounds();
  return true;
}
bool Sketch::setLineAngleByIds(GeometryId firstId, GeometryId secondId,
                               double angleDegrees) {
  const auto firstIndex = lineIndex(firstId);
  const auto secondIndex = lineIndex(secondId);

  if (!firstIndex || !secondIndex || firstId == secondId ||
      angleDegrees <= 0.0 || angleDegrees >= 180.0)
    return false;
  // COMPOSITE-SAFE ANGLE DISPATCH
  //
  // This primitive rotates only the SECOND line. That is safe for a
  // standalone line, but rotating one side of a rectangle as if it were an
  // independent primitive tears the composite apart.
  //
  // If exactly one operand belongs to a multi-line element, always rotate
  // the standalone/smaller operand. If both operands are different composite
  // elements, reject the primitive operation instead of corrupting geometry.
  const auto elementLineCount =
      [this](std::size_t lineIndexValue) {
        if (lineIndexValue >= lines_.size())
          return std::size_t{0};

        const std::size_t elementId =
            lines_[lineIndexValue].elementId;

        std::size_t count = 0;
        for (const auto& line : lines_) {
          if (line.elementId == elementId)
            ++count;
        }

        return count;
      };

  const std::size_t firstElementLineCount =
      elementLineCount(*firstIndex);
  const std::size_t secondElementLineCount =
      elementLineCount(*secondIndex);

  const bool firstComposite =
      firstElementLineCount > 1;
  const bool secondComposite =
      secondElementLineCount > 1;

  const std::size_t firstElementId =
      lines_[*firstIndex].elementId;
  const std::size_t secondElementId =
      lines_[*secondIndex].elementId;

  // Internal rectangle constraints (two sides of the SAME composite) still
  // use the normal primitive implementation below.
  if (firstElementId != secondElementId) {
    if (!firstComposite && secondComposite) {
      // Solver chose the rectangle side as the moving operand. Reverse the
      // relationship so the standalone line rotates instead.
      return setLineAngleByIds(secondId, firstId, angleDegrees);
    }

    if (firstComposite && secondComposite) {
      // Rotating one primitive of either composite would be destructive.
      return false;
    }
  }

  const Line& first = lines_[*firstIndex];
  Line& second = lines_[*secondIndex];

  const Point oldStart = second.start;
  const Point oldEnd = second.end;

  const auto same = [](Point a, Point b) {
    return std::hypot(a.xMm - b.xMm,
                      a.yMm - b.yMm) <= 1e-7;
  };

  // Determine the real shared CAD vertex when the lines touch.
  //
  // The shared vertex is the hinge. It must stay fixed; only the opposite
  // endpoint of the moving line is allowed to rotate.
  bool pivotAtStart = true;
  bool hasSharedPivot = false;
  Point pivot = oldStart;
  Point firstRayEnd = first.end;

  if (same(oldStart, first.start)) {
    pivotAtStart = true;
    hasSharedPivot = true;
    pivot = oldStart;
    firstRayEnd = first.end;
  } else if (same(oldStart, first.end)) {
    pivotAtStart = true;
    hasSharedPivot = true;
    pivot = oldStart;
    firstRayEnd = first.start;
  } else if (same(oldEnd, first.start)) {
    pivotAtStart = false;
    hasSharedPivot = true;
    pivot = oldEnd;
    firstRayEnd = first.end;
  } else if (same(oldEnd, first.end)) {
    pivotAtStart = false;
    hasSharedPivot = true;
    pivot = oldEnd;
    firstRayEnd = first.start;
  }

  // CRASH-FREE 16: FIX ANGLE ENDPOINT PIVOT
  //
  // A CAD angle is frequently created where an endpoint of one line touches
  // the BODY of another line. That is a real angular vertex even though the
  // two primitives do not share endpoint coordinates. Keep that endpoint
  // fixed and rotate only the opposite/free endpoint.
  if (!hasSharedPivot) {
    const auto pointOnFiniteSegment =
        [](Point point, const Line& carrier) {
          const double dx =
              carrier.end.xMm -
              carrier.start.xMm;
          const double dy =
              carrier.end.yMm -
              carrier.start.yMm;
          const double lengthSquared =
              dx * dx + dy * dy;

          if (lengthSquared <= 1e-12)
            return false;

          const double t =
              ((point.xMm - carrier.start.xMm) * dx +
               (point.yMm - carrier.start.yMm) * dy) /
              lengthSquared;

          constexpr double parameterTolerance =
              1e-7;

          if (t < -parameterTolerance ||
              t > 1.0 + parameterTolerance)
            return false;

          const double clampedT =
              std::clamp(t, 0.0, 1.0);

          const Point projected{
              carrier.start.xMm +
                  clampedT * dx,
              carrier.start.yMm +
                  clampedT * dy};

          const double carrierLength =
              std::sqrt(lengthSquared);

          // Scale tolerance slightly with geometry size, while keeping a
          // strict absolute floor for normal millimetre-sized sketches.
          const double distanceTolerance =
              std::max(
                  1e-7,
                  carrierLength * 1e-8);

          return std::hypot(
                     point.xMm - projected.xMm,
                     point.yMm - projected.yMm) <=
                 distanceTolerance;
        };

    const bool startOnFirst =
        pointOnFiniteSegment(
            oldStart,
            first);
    const bool endOnFirst =
        pointOnFiniteSegment(
            oldEnd,
            first);

    if (startOnFirst != endOnFirst) {
      pivotAtStart =
          startOnFirst;

      pivot =
          pivotAtStart
              ? oldStart
              : oldEnd;

      hasSharedPivot = true;

      // The reference line may continue on both sides of an interior pivot.
      // Use the endpoint that points most strongly toward the current moving
      // ray. This keeps the visible angular branch stable instead of flipping
      // by 180 degrees when the carrier is crossed.
      const Point movingEnd =
          pivotAtStart
              ? oldEnd
              : oldStart;

      const double movingDx =
          movingEnd.xMm -
          pivot.xMm;
      const double movingDy =
          movingEnd.yMm -
          pivot.yMm;

      const double startDx =
          first.start.xMm -
          pivot.xMm;
      const double startDy =
          first.start.yMm -
          pivot.yMm;
      const double endDx =
          first.end.xMm -
          pivot.xMm;
      const double endDy =
          first.end.yMm -
          pivot.yMm;

      const double startLength =
          std::hypot(startDx, startDy);
      const double endLength =
          std::hypot(endDx, endDy);

      double startScore =
          -std::numeric_limits<double>::infinity();
      double endScore =
          -std::numeric_limits<double>::infinity();

      if (startLength > 1e-9) {
        startScore =
            (movingDx * startDx +
             movingDy * startDy) /
            startLength;
      }

      if (endLength > 1e-9) {
        endScore =
            (movingDx * endDx +
             movingDy * endDy) /
            endLength;
      }

      firstRayEnd =
          endScore >= startScore
              ? first.end
              : first.start;
    }
  }

  // Truly disconnected lines have no CAD hinge. Preserve the historical
  // fallback only for that case.
  if (!hasSharedPivot) {
    pivotAtStart = true;
    pivot = oldStart;
    firstRayEnd = {
        pivot.xMm +
            (first.end.xMm -
             first.start.xMm),
        pivot.yMm +
            (first.end.yMm -
             first.start.yMm)};
  }

  const Point secondRayEnd =
      pivotAtStart ? oldEnd : oldStart;

  const double firstDx = firstRayEnd.xMm - pivot.xMm;
  const double firstDy = firstRayEnd.yMm - pivot.yMm;
  const double secondDx = secondRayEnd.xMm - pivot.xMm;
  const double secondDy = secondRayEnd.yMm - pivot.yMm;

  const double firstLength = std::hypot(firstDx, firstDy);
  const double secondLength = std::hypot(secondDx, secondDy);

  if (firstLength <= 1e-9 || secondLength <= 1e-9)
    return false;

  constexpr double pi = 3.14159265358979323846;
  const double targetOffset = angleDegrees * pi / 180.0;

  const double referenceAngle =
      std::atan2(firstDy, firstDx);
  const double currentAngle =
      std::atan2(secondDy, secondDx);

  // Both +angle and -angle satisfy an unsigned CAD angle. Pick the solution
  // that is nearest to the line's current orientation. This prevents a small
  // correction from unexpectedly turning into a ~180-degree flip.
  const auto wrappedDelta = [pi](double from, double to) {
    double delta = to - from;

    while (delta > pi)
      delta -= 2.0 * pi;
    while (delta < -pi)
      delta += 2.0 * pi;

    return delta;
  };

  const double candidatePositive =
      referenceAngle + targetOffset;
  const double candidateNegative =
      referenceAngle - targetOffset;

  const double positiveMove =
      std::abs(wrappedDelta(currentAngle, candidatePositive));
  const double negativeMove =
      std::abs(wrappedDelta(currentAngle, candidateNegative));

  const double targetAngle =
      positiveMove <= negativeMove
          ? candidatePositive
          : candidateNegative;

  const Point oldMoving =
      pivotAtStart ? oldEnd : oldStart;

  const Point newMoving{
      pivot.xMm + std::cos(targetAngle) * secondLength,
      pivot.yMm + std::sin(targetAngle) * secondLength};

  // Move only the free endpoint. The pivot/opposite endpoint is deliberately
  // untouched. Existing geometry coincident with the moving endpoint follows
  // that endpoint as one CAD vertex cluster.
  if (pivotAtStart)
    second.end = newMoving;
  else
    second.start = newMoving;

  for (std::size_t index = 0; index < lines_.size(); ++index) {
    if (index == *secondIndex)
      continue;

    auto& line = lines_[index];

    if (same(line.start, oldMoving))
      line.start = newMoving;
    if (same(line.end, oldMoving))
      line.end = newMoving;
  }

  for (auto& circle : circles_) {
    if (same(circle.center, oldMoving))
      circle.center = newMoving;
  }

  updateBounds();
  return true;
}
GeometryId Sketch::lineId(std::size_t index) const noexcept {
  return index < lineIds_.size() ? lineIds_[index] : kInvalidGeometryId;
}

GeometryId Sketch::circleId(std::size_t index) const noexcept {
  return index < circleIds_.size() ? circleIds_[index] : kInvalidGeometryId;
}

std::optional<std::size_t> Sketch::lineIndex(GeometryId id) const noexcept {
  if (id == kInvalidGeometryId) return std::nullopt;
  const auto found = std::find(lineIds_.begin(), lineIds_.end(), id);
  if (found == lineIds_.end()) return std::nullopt;
  return static_cast<std::size_t>(std::distance(lineIds_.begin(), found));
}

std::optional<std::size_t> Sketch::circleIndex(GeometryId id) const noexcept {
  if (id == kInvalidGeometryId) return std::nullopt;
  const auto found = std::find(circleIds_.begin(), circleIds_.end(), id);
  if (found == circleIds_.end()) return std::nullopt;
  return static_cast<std::size_t>(std::distance(circleIds_.begin(), found));
}

std::optional<Point> Sketch::referencedPoint(
    PointReference reference) const noexcept {
  if (reference.elementCenterId != 0)
    return elementCenterPoint(reference.elementCenterId);

  if (reference.circleId != kInvalidGeometryId) {
    const auto index = circleIndex(reference.circleId);
    if (!index) return std::nullopt;
    return circles_[*index].center;
  }

  const auto index = lineIndex(reference.lineId);
  if (!index) return std::nullopt;
  const auto& line = lines_[*index];
  return reference.start ? line.start : line.end;
}
bool Sketch::setPointsCoincident(PointReference firstReference,
                                 PointReference secondReference) {
  const auto first = referencedPoint(firstReference);
  const auto second = referencedPoint(secondReference);
  if (!first || !second) return false;

  const auto sameReference =
      [](PointReference a, PointReference b) {
        if (a.elementCenterId != 0 || b.elementCenterId != 0)
          return a.elementCenterId != 0 &&
                 a.elementCenterId == b.elementCenterId;

        if (a.circleId != kInvalidGeometryId ||
            b.circleId != kInvalidGeometryId)
          return a.circleId != kInvalidGeometryId &&
                 a.circleId == b.circleId;

        return a.lineId == b.lineId &&
               a.start == b.start;
      };

  if (sameReference(firstReference, secondReference))
    return true;

  // Coinciding both ends of one line would collapse it.
  if (firstReference.elementCenterId == 0 &&
      secondReference.elementCenterId == 0 &&
      firstReference.circleId == kInvalidGeometryId &&
      secondReference.circleId == kInvalidGeometryId &&
      firstReference.lineId != kInvalidGeometryId &&
      firstReference.lineId == secondReference.lineId)
    return false;

  const Point target = *first;
  const Point oldSecond = *second;

  // A virtual rectangle center is not an independently movable coordinate.
  // Moving it means translating the complete owning rectangle.
  if (secondReference.elementCenterId != 0) {
    const double dx = target.xMm - oldSecond.xMm;
    const double dy = target.yMm - oldSecond.yMm;

    for (auto& line : lines_) {
      if (line.elementId != secondReference.elementCenterId)
        continue;

      line.start.xMm += dx;
      line.start.yMm += dy;
      line.end.xMm += dx;
      line.end.yMm += dy;
    }

    updateBounds();
    return true;
  }

  if (secondReference.circleId != kInvalidGeometryId) {
    const auto index = circleIndex(secondReference.circleId);
    if (!index) return false;
    circles_[*index].center = target;
  } else {
    const auto same = [](Point a, Point b) {
      return std::hypot(a.xMm - b.xMm,
                        a.yMm - b.yMm) <= 1e-7;
    };

    // Preserve the existing behavior for endpoint clusters that currently
    // share the same geometric coordinate.
    for (auto& line : lines_) {
      if (same(line.start, oldSecond)) line.start = target;
      if (same(line.end, oldSecond)) line.end = target;
    }
  }

  updateBounds();
  return true;
}
bool Sketch::setPointOnLine(GeometryId lineIdValue,
                            PointReference pointReference) {
  const auto carrierIndex = lineIndex(lineIdValue);
  const auto point = referencedPoint(pointReference);

  if (!carrierIndex || !point) return false;

  // A line endpoint cannot be constrained onto its own carrier segment:
  // that would be tautological and interferes with normal endpoint editing.
  if (pointReference.circleId == kInvalidGeometryId &&
      pointReference.lineId == lineIdValue)
    return false;

  const auto& carrier = lines_[*carrierIndex];
  const double dx = carrier.end.xMm - carrier.start.xMm;
  const double dy = carrier.end.yMm - carrier.start.yMm;
  const double lengthSquared = dx * dx + dy * dy;

  if (lengthSquared <= 1e-12) return false;

  const double rawT =
      ((point->xMm - carrier.start.xMm) * dx +
       (point->yMm - carrier.start.yMm) * dy) /
      lengthSquared;

  // PointOnLine means point on the finite CAD segment, not on an infinite
  // mathematical line. Clamp to the two endpoints.
  const double t = std::clamp(rawT, 0.0, 1.0);

  const Point target{
      carrier.start.xMm + dx * t,
      carrier.start.yMm + dy * t};

  const Point oldPoint = *point;

  if (pointReference.elementCenterId != 0) {
    const double moveX = target.xMm - oldPoint.xMm;
    const double moveY = target.yMm - oldPoint.yMm;

    for (auto& line : lines_) {
      if (line.elementId != pointReference.elementCenterId)
        continue;

      line.start.xMm += moveX;
      line.start.yMm += moveY;
      line.end.xMm += moveX;
      line.end.yMm += moveY;
    }
  } else if (pointReference.circleId != kInvalidGeometryId) {
    const auto circle = circleIndex(pointReference.circleId);
    if (!circle) return false;
    circles_[*circle].center = target;
  } else {
    const auto same = [](Point first, Point second) {
      return std::hypot(first.xMm - second.xMm,
                        first.yMm - second.yMm) <= 1e-7;
    };

    // Keep the complete coincident CAD vertex cluster together.
    for (auto& line : lines_) {
      if (same(line.start, oldPoint)) line.start = target;
      if (same(line.end, oldPoint)) line.end = target;
    }

    for (auto& circle : circles_) {
      if (same(circle.center, oldPoint))
        circle.center = target;
    }
  }

  updateBounds();
  return true;
}
bool Sketch::setPointOnCircle(GeometryId circleIdValue,
                              PointReference pointReference) {
  const auto carrierIndex = circleIndex(circleIdValue);
  const auto point = referencedPoint(pointReference);

  if (!carrierIndex || !point) return false;

  // The centre of a circle cannot belong to its own circumference.
  if (pointReference.circleId == circleIdValue)
    return false;

  const Circle& carrier = circles_[*carrierIndex];
  if (carrier.radiusMm <= 1e-9) return false;

  const Point oldPoint = *point;

  double dx = oldPoint.xMm - carrier.center.xMm;
  double dy = oldPoint.yMm - carrier.center.yMm;
  double length = std::hypot(dx, dy);

  // There is no radial direction if the point is exactly at the centre.
  // Choose +X deterministically for that degenerate initial case.
  if (length <= 1e-9) {
    dx = 1.0;
    dy = 0.0;
    length = 1.0;
  }

  const Point target{
      carrier.center.xMm + dx / length * carrier.radiusMm,
      carrier.center.yMm + dy / length * carrier.radiusMm};

  const double moveX = target.xMm - oldPoint.xMm;
  const double moveY = target.yMm - oldPoint.yMm;

  if (pointReference.elementCenterId != 0) {
    // Rectangle centre is derived geometry: translate the whole rectangle.
    for (auto& line : lines_) {
      if (line.elementId != pointReference.elementCenterId)
        continue;

      line.start.xMm += moveX;
      line.start.yMm += moveY;
      line.end.xMm += moveX;
      line.end.yMm += moveY;
    }
  } else if (pointReference.circleId != kInvalidGeometryId) {
    const auto movingCircle =
        circleIndex(pointReference.circleId);
    if (!movingCircle) return false;

    circles_[*movingCircle].center = target;
  } else {
    const auto same = [](Point first, Point second) {
      return std::hypot(first.xMm - second.xMm,
                        first.yMm - second.yMm) <= 1e-7;
    };

    // Keep a geometrically shared vertex together.
    for (auto& line : lines_) {
      if (same(line.start, oldPoint)) line.start = target;
      if (same(line.end, oldPoint)) line.end = target;
    }

    for (auto& circle : circles_) {
      if (same(circle.center, oldPoint))
        circle.center = target;
    }
  }

  updateBounds();
  return true;
}
bool Sketch::setCircleTangentToLine(GeometryId lineIdValue,
                                    GeometryId circleIdValue) {
  const auto lineIndexValue = lineIndex(lineIdValue);
  const auto circleIndexValue = circleIndex(circleIdValue);

  if (!lineIndexValue || !circleIndexValue)
    return false;

  const Line& line = lines_[*lineIndexValue];
  Circle& circle = circles_[*circleIndexValue];

  if (!std::isfinite(circle.radiusMm) ||
      circle.radiusMm <= 1e-9)
    return false;

  const double dx = line.end.xMm - line.start.xMm;
  const double dy = line.end.yMm - line.start.yMm;
  const double lengthSquared = dx * dx + dy * dy;

  if (lengthSquared <= 1e-12)
    return false;

  const double length = std::sqrt(lengthSquared);
  const double nx = -dy / length;
  const double ny = dx / length;

  const double signedDistance =
      (circle.center.xMm - line.start.xMm) * nx +
      (circle.center.yMm - line.start.yMm) * ny;

  const double side =
      signedDistance < 0.0 ? -1.0 : 1.0;

  // CRASH-FREE 04: FINITE SEGMENT TANGENCY
  //
  // Tangency belongs to the finite CAD segment. Project the circle centre
  // onto the carrier and clamp the contact parameter to [0, 1]. This keeps
  // the contact at the endpoint instead of letting the circle continue along
  // an imaginary extension of the line.
  const double rawT =
      ((circle.center.xMm - line.start.xMm) * dx +
       (circle.center.yMm - line.start.yMm) * dy) /
      lengthSquared;

  const double t =
      std::clamp(rawT, 0.0, 1.0);

  const Point contact{
      line.start.xMm + dx * t,
      line.start.yMm + dy * t};

  circle.center.xMm =
      contact.xMm + side * nx * circle.radiusMm;
  circle.center.yMm =
      contact.yMm + side * ny * circle.radiusMm;

  updateBounds();
  return true;
}
bool Sketch::translatePoint(PointReference reference, double dxMm,
                            double dyMm) {
  if (!referencedPoint(reference)) return false;
  if (dxMm == 0.0 && dyMm == 0.0) return true;

  // CRASH-FREE 04: COMPLETE POINTREFERENCE IDENTITY
  //
  // A virtual element centre is its own reference class. Never compare two
  // centre references through their default invalid lineId values.
  const auto sameReference =
      [](PointReference first,
         PointReference second) {
        if (first.elementCenterId != 0 ||
            second.elementCenterId != 0) {
          return first.elementCenterId != 0 &&
                 second.elementCenterId != 0 &&
                 first.elementCenterId ==
                     second.elementCenterId;
        }

        if (first.circleId != kInvalidGeometryId ||
            second.circleId != kInvalidGeometryId) {
          return first.circleId != kInvalidGeometryId &&
                 second.circleId != kInvalidGeometryId &&
                 first.circleId == second.circleId;
        }

        if (first.lineId == kInvalidGeometryId ||
            second.lineId == kInvalidGeometryId)
          return false;

        return first.lineId == second.lineId &&
               first.start == second.start;
      };

  std::vector<PointReference> connectedPoints{reference};
  bool changed = true;

  while (changed) {
    changed = false;

    for (const auto& constraint : constraints_) {
      if (constraint.type != ConstraintType::Coincident)
        continue;

      const auto first = constraint.firstPoint;
      const auto second = constraint.secondPoint;

      if (!referencedPoint(first) ||
          !referencedPoint(second))
        continue;

      const bool containsFirst =
          std::any_of(
              connectedPoints.begin(),
              connectedPoints.end(),
              [first, &sameReference](
                  PointReference item) {
                return sameReference(item, first);
              });

      const bool containsSecond =
          std::any_of(
              connectedPoints.begin(),
              connectedPoints.end(),
              [second, &sameReference](
                  PointReference item) {
                return sameReference(item, second);
              });

      if (containsFirst && !containsSecond) {
        connectedPoints.push_back(second);
        changed = true;
      }
      else if (containsSecond && !containsFirst) {
        connectedPoints.push_back(first);
        changed = true;
      }
    }
  }

  // CRASH-FREE 04: VIRTUAL CENTER MOVEMENT
  //
  // Virtual rectangle centres are derived coordinates. Moving one means
  // translating its whole owning element exactly once.
  std::vector<std::size_t> movedCenterElements;

  for (const auto pointReference : connectedPoints) {
    if (pointReference.elementCenterId == 0)
      continue;

    if (std::find(
            movedCenterElements.begin(),
            movedCenterElements.end(),
            pointReference.elementCenterId) !=
        movedCenterElements.end())
      continue;

    bool foundElement = false;

    for (auto& line : lines_) {
      if (line.elementId !=
          pointReference.elementCenterId)
        continue;

      foundElement = true;
      line.start.xMm += dxMm;
      line.start.yMm += dyMm;
      line.end.xMm += dxMm;
      line.end.yMm += dyMm;
    }

    if (foundElement)
      movedCenterElements.push_back(
          pointReference.elementCenterId);
  }

  for (const auto pointReference : connectedPoints) {
    if (pointReference.elementCenterId != 0)
      continue;

    if (pointReference.circleId !=
        kInvalidGeometryId) {
      const auto index =
          circleIndex(pointReference.circleId);

      if (!index)
        continue;

      circles_[*index].center.xMm += dxMm;
      circles_[*index].center.yMm += dyMm;
      continue;
    }

    const auto index =
        lineIndex(pointReference.lineId);

    if (!index)
      continue;

    // If this endpoint belongs to an element already translated through its
    // virtual centre, do not apply the same displacement twice.
    if (std::find(
            movedCenterElements.begin(),
            movedCenterElements.end(),
            lines_[*index].elementId) !=
        movedCenterElements.end())
      continue;

    Point& point =
        pointReference.start
            ? lines_[*index].start
            : lines_[*index].end;

    point.xMm += dxMm;
    point.yMm += dyMm;
  }

  std::vector<GeometryId> movedLineIds;

  for (const auto pointReference :
       connectedPoints) {
    if (pointReference.elementCenterId != 0 ||
        pointReference.circleId !=
            kInvalidGeometryId ||
        pointReference.lineId ==
            kInvalidGeometryId)
      continue;

    if (std::find(
            movedLineIds.begin(),
            movedLineIds.end(),
            pointReference.lineId) ==
        movedLineIds.end())
      movedLineIds.push_back(
          pointReference.lineId);
  }

  // Lines translated through a virtual centre are also active moved
  // geometries for Equal propagation.
  for (std::size_t index = 0;
       index < lines_.size();
       ++index) {
    if (std::find(
            movedCenterElements.begin(),
            movedCenterElements.end(),
            lines_[index].elementId) ==
        movedCenterElements.end())
      continue;

    const auto id = lineIds_[index];

    if (id != kInvalidGeometryId &&
        std::find(
            movedLineIds.begin(),
            movedLineIds.end(),
            id) ==
            movedLineIds.end())
      movedLineIds.push_back(id);
  }

  const auto hasDrivingSize =
      [this](GeometryId lineIdValue) {
        const auto lineIndexValue =
            lineIndex(lineIdValue);

        if (!lineIndexValue)
          return false;

        for (const auto& item : constraints_) {
          if (item.value <= 0.0)
            continue;

          if (item.type ==
                  ConstraintType::Length &&
              item.firstGeometry ==
                  lineIdValue)
            return true;

          if (item.firstPoint.circleId !=
                  kInvalidGeometryId ||
              item.secondPoint.circleId !=
                  kInvalidGeometryId ||
              item.firstPoint.elementCenterId != 0 ||
              item.secondPoint.elementCenterId != 0)
            continue;

          if (item.firstPoint.lineId !=
                  lineIdValue ||
              item.secondPoint.lineId !=
                  lineIdValue ||
              item.firstPoint.start ==
                  item.secondPoint.start)
            continue;

          if (item.type ==
                  ConstraintType::Distance ||
              item.type ==
                  ConstraintType::DistanceX ||
              item.type ==
                  ConstraintType::DistanceY)
            return true;
        }

        return false;
      };

  for (const auto sourceId : movedLineIds) {
    const auto sourceIndex =
        lineIndex(sourceId);

    if (!sourceIndex)
      continue;

    if (hasDrivingSize(sourceId))
      continue;

    std::vector<GeometryId> equalGroup{
        sourceId};

    bool expandedEqual = true;

    while (expandedEqual) {
      expandedEqual = false;

      for (const auto& item : constraints_) {
        if (item.type !=
            ConstraintType::Equal)
          continue;

        if (!lineIndex(item.firstGeometry) ||
            !lineIndex(item.secondGeometry))
          continue;

        const bool hasFirst =
            std::find(
                equalGroup.begin(),
                equalGroup.end(),
                item.firstGeometry) !=
            equalGroup.end();

        const bool hasSecond =
            std::find(
                equalGroup.begin(),
                equalGroup.end(),
                item.secondGeometry) !=
            equalGroup.end();

        if (hasFirst && !hasSecond) {
          equalGroup.push_back(
              item.secondGeometry);
          expandedEqual = true;
        }
        else if (!hasFirst && hasSecond) {
          equalGroup.push_back(
              item.firstGeometry);
          expandedEqual = true;
        }
      }
    }

    if (equalGroup.size() <= 1)
      continue;

    bool groupHasDriving = false;

    for (const auto groupId : equalGroup) {
      if (hasDrivingSize(groupId)) {
        groupHasDriving = true;
        break;
      }
    }

    if (groupHasDriving)
      continue;

    const auto& sourceLine =
        lines_[*sourceIndex];

    const double targetLength =
        std::hypot(
            sourceLine.end.xMm -
                sourceLine.start.xMm,
            sourceLine.end.yMm -
                sourceLine.start.yMm);

    if (targetLength <= 1e-9)
      continue;

    const std::size_t sourceElementId =
        sourceLine.elementId;

    for (const auto targetId :
         equalGroup) {
      if (targetId == sourceId)
        continue;

      const auto targetIndex =
          lineIndex(targetId);

      if (!targetIndex)
        continue;

      if (lines_[*targetIndex].elementId ==
          sourceElementId)
        continue;

      const double currentLength =
          std::hypot(
              lines_[*targetIndex].end.xMm -
                  lines_[*targetIndex].start.xMm,
              lines_[*targetIndex].end.yMm -
                  lines_[*targetIndex].start.yMm);

      if (std::abs(
              currentLength -
              targetLength) <= 1e-7)
        continue;

      (void)setLineLengthById(
          targetId,
          targetLength);
    }
  }

  (void)BasicSketchSolver::solve(*this);
  updateBounds();
  return true;
}
bool Sketch::setLineLength(std::size_t index, double lengthMm) {
  if (index >= lines_.size() || lengthMm <= 0.0) return false;
  const Point start = lines_[index].start;
  const Point oldEnd = lines_[index].end;
  const double dx = oldEnd.xMm - start.xMm;
  const double dy = oldEnd.yMm - start.yMm;
  const double oldLength = std::hypot(dx, dy);
  if (oldLength <= 1e-9) return false;
  const Point newEnd{start.xMm + dx / oldLength * lengthMm,
                     start.yMm + dy / oldLength * lengthMm};
  const auto same = [](Point a, Point b) {
    return std::hypot(a.xMm - b.xMm, a.yMm - b.yMm) <= 1e-7;
  };
  for (auto& line : lines_) {
    if (same(line.start, oldEnd)) line.start = newEnd;
    if (same(line.end, oldEnd)) line.end = newEnd;
  }
  updateBounds();
  return true;
}

bool Sketch::setPointDistance(PointReference firstReference,
                              PointReference secondReference,
                              double distanceMm) {
  const auto first = referencedPoint(firstReference);
  const auto second = referencedPoint(secondReference);
  if (!first || !second || distanceMm <= 0.0) return false;

  const double dx = second->xMm - first->xMm;
  const double dy = second->yMm - first->yMm;
  const double oldDistance = std::hypot(dx, dy);
  if (oldDistance <= 1e-9) return false;

  const Point moved{first->xMm + dx / oldDistance * distanceMm,
                    first->yMm + dy / oldDistance * distanceMm};

  const double moveX = moved.xMm - second->xMm;
  const double moveY = moved.yMm - second->yMm;

  // SAME-RECTANGLE POINT DISTANCE
  // If both selected endpoints belong to the same four-line rectangle,
  // translating the whole element cannot change their internal distance.
  // Resize the rectangle instead: move the vertex column/row that contains
  // the second point while keeping the first side anchored.
  const auto rectangleElementForPoint =
      [this](PointReference reference) -> std::optional<std::size_t> {
    if (reference.elementCenterId != 0 ||
        reference.circleId != kInvalidGeometryId ||
        reference.lineId == kInvalidGeometryId)
      return std::nullopt;

    const auto index = lineIndex(reference.lineId);
    if (!index) return std::nullopt;

    const std::size_t elementId = lines_[*index].elementId;
    std::size_t count = 0;
    for (const auto& line : lines_) {
      if (line.elementId == elementId)
        ++count;
    }

    return count == 4
               ? std::optional<std::size_t>{elementId}
               : std::nullopt;
  };

  const auto firstRectangle =
      rectangleElementForPoint(firstReference);
  const auto secondRectangle =
      rectangleElementForPoint(secondReference);

  if (firstRectangle && secondRectangle &&
      *firstRectangle == *secondRectangle) {
    const double oldSecondX = second->xMm;
    const double oldSecondY = second->yMm;

    const auto resizePoint =
        [oldSecondX, oldSecondY, moveX, moveY](Point& point) {
      if (std::abs(point.xMm - oldSecondX) <= 1e-7)
        point.xMm += moveX;
      if (std::abs(point.yMm - oldSecondY) <= 1e-7)
        point.yMm += moveY;
    };

    for (auto& line : lines_) {
      if (line.elementId != *secondRectangle)
        continue;

      resizePoint(line.start);
      resizePoint(line.end);
    }

    updateBounds();
    return true;
  }
  // A point-distance attached to a rectangle must move the complete
  // composite element. Moving only one corner destroys the rectangle and
  // makes the dimension appear to drift during subsequent solver passes.
  // CRASH-FREE 07: MOVABLE NON-LINE POINT REFERENCES
  //
  // PointReference may denote a circle centre or a virtual composite centre,
  // not only a line endpoint. A driving point-distance must move whichever
  // CAD point is the SECOND reference.
  if (secondReference.elementCenterId != 0) {
    for (auto& line : lines_) {
      if (line.elementId != secondReference.elementCenterId)
        continue;

      line.start.xMm += moveX;
      line.start.yMm += moveY;
      line.end.xMm += moveX;
      line.end.yMm += moveY;
    }

    updateBounds();
    return true;
  }

  if (secondReference.circleId != kInvalidGeometryId) {
    const auto circle =
        circleIndex(secondReference.circleId);

    if (!circle)
      return false;

    circles_[*circle].center = moved;
    updateBounds();
    return true;
  }
  if (secondReference.circleId == kInvalidGeometryId) {
    const auto secondLineIndex = lineIndex(secondReference.lineId);

    if (secondLineIndex) {
      const std::size_t elementId =
          lines_[*secondLineIndex].elementId;

      std::size_t elementLineCount = 0;
      for (const auto& line : lines_) {
        if (line.elementId == elementId)
          ++elementLineCount;
      }

      if (elementLineCount == 4) {
        for (auto& line : lines_) {
          if (line.elementId != elementId) continue;

          line.start.xMm += moveX;
          line.start.yMm += moveY;
          line.end.xMm += moveX;
          line.end.yMm += moveY;
        }

        updateBounds();
        return true;
      }
    }
  }

  const auto same = [](Point a, Point b) {
    return std::hypot(a.xMm - b.xMm, a.yMm - b.yMm) <= 1e-7;
  };

  for (auto& line : lines_) {
    if (same(line.start, *second)) line.start = moved;
    if (same(line.end, *second)) line.end = moved;
  }

  updateBounds();
  return true;
}

bool Sketch::setPointDistanceX(PointReference firstReference,
                               PointReference secondReference,
                               double distanceMm) {
  const auto first = referencedPoint(firstReference);
  const auto second = referencedPoint(secondReference);
  if (!first || !second || distanceMm <= 0.0) return false;

  const double dx = second->xMm - first->xMm;
  const double direction = dx < 0.0 ? -1.0 : 1.0;
  const Point moved{first->xMm + direction * distanceMm,
                    second->yMm};

  const double moveX = moved.xMm - second->xMm;

  // SAME-RECTANGLE X DISTANCE
  // Move only the rectangle column containing the second point. This changes
  // width while preserving the rectangular topology.
  const auto rectangleElementForPointX =
      [this](PointReference reference) -> std::optional<std::size_t> {
    if (reference.elementCenterId != 0 ||
        reference.circleId != kInvalidGeometryId ||
        reference.lineId == kInvalidGeometryId)
      return std::nullopt;

    const auto index = lineIndex(reference.lineId);
    if (!index) return std::nullopt;

    const std::size_t elementId = lines_[*index].elementId;
    std::size_t count = 0;
    for (const auto& line : lines_) {
      if (line.elementId == elementId)
        ++count;
    }

    return count == 4
               ? std::optional<std::size_t>{elementId}
               : std::nullopt;
  };

  const auto firstRectangleX =
      rectangleElementForPointX(firstReference);
  const auto secondRectangleX =
      rectangleElementForPointX(secondReference);

  if (firstRectangleX && secondRectangleX &&
      *firstRectangleX == *secondRectangleX) {
    const double oldSecondX = second->xMm;

    for (auto& line : lines_) {
      if (line.elementId != *secondRectangleX)
        continue;

      if (std::abs(line.start.xMm - oldSecondX) <= 1e-7)
        line.start.xMm += moveX;
      if (std::abs(line.end.xMm - oldSecondX) <= 1e-7)
        line.end.xMm += moveX;
    }

    updateBounds();
    return true;
  }
  // CRASH-FREE 07: X DISTANCE TO NON-LINE POINT
  if (secondReference.elementCenterId != 0) {
    for (auto& line : lines_) {
      if (line.elementId != secondReference.elementCenterId)
        continue;

      line.start.xMm += moveX;
      line.end.xMm += moveX;
    }

    updateBounds();
    return true;
  }

  if (secondReference.circleId != kInvalidGeometryId) {
    const auto circle =
        circleIndex(secondReference.circleId);

    if (!circle)
      return false;

    circles_[*circle].center.xMm = moved.xMm;
    updateBounds();
    return true;
  }
  if (secondReference.circleId == kInvalidGeometryId) {
    const auto secondLineIndex = lineIndex(secondReference.lineId);

    if (secondLineIndex) {
      const std::size_t elementId =
          lines_[*secondLineIndex].elementId;

      std::size_t elementLineCount = 0;
      for (const auto& line : lines_) {
        if (line.elementId == elementId)
          ++elementLineCount;
      }

      if (elementLineCount == 4) {
        for (auto& line : lines_) {
          if (line.elementId != elementId) continue;

          line.start.xMm += moveX;
          line.end.xMm += moveX;
        }

        updateBounds();
        return true;
      }
    }
  }

  const auto same = [](Point a, Point b) {
    return std::hypot(a.xMm - b.xMm, a.yMm - b.yMm) <= 1e-7;
  };

  for (auto& line : lines_) {
    if (same(line.start, *second)) line.start = moved;
    if (same(line.end, *second)) line.end = moved;
  }

  updateBounds();
  return true;
}

bool Sketch::setPointDistanceY(PointReference firstReference,
                               PointReference secondReference,
                               double distanceMm) {
  const auto first = referencedPoint(firstReference);
  const auto second = referencedPoint(secondReference);
  if (!first || !second || distanceMm <= 0.0) return false;

  const double dy = second->yMm - first->yMm;
  const double direction = dy < 0.0 ? -1.0 : 1.0;
  const Point moved{second->xMm,
                    first->yMm + direction * distanceMm};

  const double moveY = moved.yMm - second->yMm;

  // SAME-RECTANGLE Y DISTANCE
  // Move only the rectangle row containing the second point. This changes
  // height while preserving the rectangular topology.
  const auto rectangleElementForPointY =
      [this](PointReference reference) -> std::optional<std::size_t> {
    if (reference.elementCenterId != 0 ||
        reference.circleId != kInvalidGeometryId ||
        reference.lineId == kInvalidGeometryId)
      return std::nullopt;

    const auto index = lineIndex(reference.lineId);
    if (!index) return std::nullopt;

    const std::size_t elementId = lines_[*index].elementId;
    std::size_t count = 0;
    for (const auto& line : lines_) {
      if (line.elementId == elementId)
        ++count;
    }

    return count == 4
               ? std::optional<std::size_t>{elementId}
               : std::nullopt;
  };

  const auto firstRectangleY =
      rectangleElementForPointY(firstReference);
  const auto secondRectangleY =
      rectangleElementForPointY(secondReference);

  if (firstRectangleY && secondRectangleY &&
      *firstRectangleY == *secondRectangleY) {
    const double oldSecondY = second->yMm;

    for (auto& line : lines_) {
      if (line.elementId != *secondRectangleY)
        continue;

      if (std::abs(line.start.yMm - oldSecondY) <= 1e-7)
        line.start.yMm += moveY;
      if (std::abs(line.end.yMm - oldSecondY) <= 1e-7)
        line.end.yMm += moveY;
    }

    updateBounds();
    return true;
  }
  // CRASH-FREE 07: Y DISTANCE TO NON-LINE POINT
  if (secondReference.elementCenterId != 0) {
    for (auto& line : lines_) {
      if (line.elementId != secondReference.elementCenterId)
        continue;

      line.start.yMm += moveY;
      line.end.yMm += moveY;
    }

    updateBounds();
    return true;
  }

  if (secondReference.circleId != kInvalidGeometryId) {
    const auto circle =
        circleIndex(secondReference.circleId);

    if (!circle)
      return false;

    circles_[*circle].center.yMm = moved.yMm;
    updateBounds();
    return true;
  }
  if (secondReference.circleId == kInvalidGeometryId) {
    const auto secondLineIndex = lineIndex(secondReference.lineId);

    if (secondLineIndex) {
      const std::size_t elementId =
          lines_[*secondLineIndex].elementId;

      std::size_t elementLineCount = 0;
      for (const auto& line : lines_) {
        if (line.elementId == elementId)
          ++elementLineCount;
      }

      if (elementLineCount == 4) {
        for (auto& line : lines_) {
          if (line.elementId != elementId) continue;

          line.start.yMm += moveY;
          line.end.yMm += moveY;
        }

        updateBounds();
        return true;
      }
    }
  }

  const auto same = [](Point a, Point b) {
    return std::hypot(a.xMm - b.xMm, a.yMm - b.yMm) <= 1e-7;
  };

  for (auto& line : lines_) {
    if (same(line.start, *second)) line.start = moved;
    if (same(line.end, *second)) line.end = moved;
  }

  updateBounds();
  return true;
}
bool Sketch::setCircleDiameter(std::size_t index, double diameterMm) {
  if (index >= circles_.size() || diameterMm <= 0.0) return false;
  circles_[index].radiusMm = diameterMm * 0.5;
  updateBounds();
  return true;
}

void Sketch::addDimension(Dimension dimension) { storeDimension(dimension); }

void Sketch::storeDimension(const Dimension& dimension) {
  if (dimension.valueMm > 0.0)
    dimensions_.push_back(dimension);
}

void Sketch::clearDimensions() { dimensions_.clear(); }

bool Sketch::setDimensionPlacement(std::size_t index, double offsetMm,
                                   double angleRad) {
  auto& dimensions = dimensions_;
  if (index >= dimensions.size()) return false;
  dimensions[index].offsetMm = offsetMm;
  dimensions[index].angleRad = angleRad;
  return true;
}

bool Sketch::setDimensionValue(std::size_t index, double valueMm) {
  auto& dimensions = dimensions_;
  if (index >= dimensions.size() || valueMm <= 0.0) return false;
  dimensions[index].valueMm = valueMm;
  return true;
}

ConstraintId Sketch::addConstraint(Constraint constraint) {
  if (constraint.id == kInvalidConstraintId)
    constraint.id = nextConstraintId_++;
  else
    nextConstraintId_ = std::max(nextConstraintId_, constraint.id + 1);

  constraints_.push_back(constraint);
  (void)BasicSketchSolver::solve(*this);
  return constraint.id;
}
bool Sketch::removeConstraint(ConstraintId id) {
  const auto oldSize = constraints_.size();
  std::erase_if(constraints_, [id](const Constraint& constraint) {
    return constraint.id == id;
  });
  return constraints_.size() != oldSize;
}

void Sketch::clearConstraints() { constraints_.clear(); }

const std::vector<Constraint>& Sketch::constraints() const noexcept {
  return constraints_;
}

double Sketch::widthMm() const noexcept { return widthMm_; }
double Sketch::heightMm() const noexcept { return heightMm_; }
const std::vector<Line>& Sketch::lines() const noexcept { return lines_; }
const std::vector<Circle>& Sketch::circles() const noexcept { return circles_; }
const std::vector<Dimension>& Sketch::dimensions() const {
  return dimensions_;
}

void Sketch::updateBounds() noexcept {
  if (lines_.empty() && circles_.empty()) {
    widthMm_ = 0.0;
    heightMm_ = 0.0;
    return;
  }
  double minX = std::numeric_limits<double>::max();
  double minY = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double maxY = std::numeric_limits<double>::lowest();
  auto include = [&](Point point) {
    minX = std::min(minX, point.xMm);
    minY = std::min(minY, point.yMm);
    maxX = std::max(maxX, point.xMm);
    maxY = std::max(maxY, point.yMm);
  };
  for (const auto& line : lines_) {
    include(line.start);
    include(line.end);
  }
  for (const auto& circle : circles_) {
    include({circle.center.xMm - circle.radiusMm,
             circle.center.yMm - circle.radiusMm});
    include({circle.center.xMm + circle.radiusMm,
             circle.center.yMm + circle.radiusMm});
  }
  widthMm_ = maxX - minX;
  heightMm_ = maxY - minY;
}

bool Sketch::isClosed() const noexcept {
  if (std::none_of(lines_.begin(), lines_.end(),
                   [](const Line& line) { return !line.dashed; }))
    return false;
  const auto samePoint = [](Point first, Point second) {
    return std::abs(first.xMm - second.xMm) <= 1e-7 &&
           std::abs(first.yMm - second.yMm) <= 1e-7;
  };
  // Every vertex of one or several independent closed loops has degree two.
  // This also permits Ctrl-selection of multiple extrusion regions.
  for (const auto& line : lines_) {
    if (line.dashed) continue;
    for (const Point vertex : {line.start, line.end}) {
      std::size_t degree = 0;
      for (const auto& candidate : lines_) {
        if (candidate.dashed) continue;
        if (samePoint(vertex, candidate.start)) ++degree;
        if (samePoint(vertex, candidate.end)) ++degree;
      }
      if (degree != 2) return false;
    }
  }
  return true;
}

}  // namespace solidar::sketch
