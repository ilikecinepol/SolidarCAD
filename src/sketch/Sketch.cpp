#include "sketch/Sketch.h"

#include "sketch/SketchConstraintDiagnostics.h"
#include "sketch/SketchSolver.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace solidar::sketch {

namespace {
std::atomic_size_t fullSketchCopyCount{};
std::atomic_size_t deltaJournalBeginCount{};
std::atomic_bool failNextNestedConstraintJournal{};

int pointReferenceSourceCount(PointReference reference) noexcept {
  return (reference.origin ? 1 : 0) +
         (reference.elementCenterId != 0 ? 1 : 0) +
         (reference.lineId != kInvalidGeometryId ? 1 : 0) +
         (reference.circleId != kInvalidGeometryId ? 1 : 0) +
         (reference.arcId != kInvalidGeometryId ? 1 : 0);
}

bool emptyPointReference(PointReference reference) noexcept {
  return pointReferenceSourceCount(reference) == 0;
}

bool validPointReferenceForConstraint(const Sketch& sketch,
                                      PointReference reference) {
  return pointReferenceSourceCount(reference) == 1 &&
         (reference.origin || sketch.referencedPoint(reference).has_value());
}

bool samePointReference(PointReference first, PointReference second) noexcept {
  return first.lineId == second.lineId && first.start == second.start &&
         first.circleId == second.circleId &&
         first.elementCenterId == second.elementCenterId &&
         first.arcId == second.arcId && first.origin == second.origin;
}

bool equivalentConstraint(const Constraint& first,
                          const Constraint& second) noexcept {
  if (first.type != second.type ||
      std::abs(first.value - second.value) > 1e-9)
    return false;
  const bool sameGeometryOrder =
      first.firstGeometry == second.firstGeometry &&
      first.secondGeometry == second.secondGeometry;
  const bool reverseGeometryOrder =
      first.firstGeometry == second.secondGeometry &&
      first.secondGeometry == second.firstGeometry;
  const bool samePointOrder =
      samePointReference(first.firstPoint, second.firstPoint) &&
      samePointReference(first.secondPoint, second.secondPoint);
  const bool reversePointOrder =
      samePointReference(first.firstPoint, second.secondPoint) &&
      samePointReference(first.secondPoint, second.firstPoint);
  switch (first.type) {
    case ConstraintType::Parallel:
    case ConstraintType::Perpendicular:
    case ConstraintType::Equal:
    case ConstraintType::Angle:
    case ConstraintType::LineDistance:
      return (sameGeometryOrder || reverseGeometryOrder) && samePointOrder;
    case ConstraintType::Coincident:
    case ConstraintType::Distance:
    case ConstraintType::DistanceX:
    case ConstraintType::DistanceY:
      return sameGeometryOrder && (samePointOrder || reversePointOrder);
    default:
      return sameGeometryOrder && samePointOrder;
  }
}

bool supportedConstraintSchema(const Sketch& sketch,
                               const Constraint& constraint) {
  const auto first = sketch.geometryLocation(constraint.firstGeometry);
  const auto second = sketch.geometryLocation(constraint.secondGeometry);
  const bool firstPoint = !emptyPointReference(constraint.firstPoint);
  const bool secondPoint = !emptyPointReference(constraint.secondPoint);
  const bool noPoints = !firstPoint && !secondPoint;
  const bool distinct = first && second &&
                        constraint.firstGeometry != constraint.secondGeometry;
  const auto firstIs = [&](GeometryKind kind) {
    return first && first->kind == kind;
  };
  const auto secondIs = [&](GeometryKind kind) {
    return second && second->kind == kind;
  };

  switch (constraint.type) {
    case ConstraintType::Horizontal:
    case ConstraintType::Vertical:
      return firstIs(GeometryKind::Line) && !second && noPoints;
    case ConstraintType::Length:
      return firstIs(GeometryKind::Line) && !second && noPoints &&
             constraint.value > 0.0;
    case ConstraintType::Radius:
    case ConstraintType::Diameter:
      return firstIs(GeometryKind::Circle) && !second && noPoints &&
             constraint.value > 0.0;
    case ConstraintType::Lock:
      return first.has_value() && !second && noPoints;
    case ConstraintType::Parallel:
    case ConstraintType::Perpendicular:
      return firstIs(GeometryKind::Line) && secondIs(GeometryKind::Line) &&
             distinct && noPoints;
    case ConstraintType::Angle:
      return firstIs(GeometryKind::Line) && secondIs(GeometryKind::Line) &&
             distinct && noPoints && constraint.value > 0.0 &&
             constraint.value < 180.0;
    case ConstraintType::LineDistance:
      return firstIs(GeometryKind::Line) && secondIs(GeometryKind::Line) &&
             distinct && noPoints && constraint.value > 0.0;
    case ConstraintType::Equal:
      return distinct && noPoints &&
             ((firstIs(GeometryKind::Line) && secondIs(GeometryKind::Line)) ||
              (firstIs(GeometryKind::Circle) &&
               secondIs(GeometryKind::Circle)));
    case ConstraintType::Tangent:
      return firstIs(GeometryKind::Line) && distinct && noPoints &&
             (secondIs(GeometryKind::Circle) || secondIs(GeometryKind::Arc));
    case ConstraintType::Coincident:
      return !first && !second && firstPoint && secondPoint;
    case ConstraintType::Distance:
    case ConstraintType::DistanceX:
    case ConstraintType::DistanceY:
      return !first && !second && firstPoint && secondPoint &&
             constraint.value > 0.0;
    case ConstraintType::Midpoint:
    case ConstraintType::PointOnLine:
      return firstIs(GeometryKind::Line) && !second && !firstPoint &&
             secondPoint;
    case ConstraintType::PointOnCircle:
      return firstIs(GeometryKind::Circle) && !second && !firstPoint &&
             secondPoint;
    case ConstraintType::PointOnArc:
      return firstIs(GeometryKind::Arc) && !second && !firstPoint &&
             secondPoint;
    case ConstraintType::PointOnXAxis:
    case ConstraintType::PointOnYAxis:
      return !first && !second && !firstPoint && secondPoint;
  }
  return false;
}

std::vector<GeometryId> referencedGeometryIds(const Sketch& sketch,
                                              const Constraint& constraint) {
  std::vector<GeometryId> result;
  const auto add = [&result](GeometryId id) {
    if (id != kInvalidGeometryId &&
        std::find(result.begin(), result.end(), id) == result.end())
      result.push_back(id);
  };
  add(constraint.firstGeometry);
  add(constraint.secondGeometry);
  for (const auto point : {constraint.firstPoint, constraint.secondPoint}) {
    add(point.lineId);
    add(point.circleId);
    add(point.arcId);
    if (point.elementCenterId != 0) {
      for (std::size_t i = 0; i < sketch.lines().size(); ++i)
        if (sketch.lines()[i].elementId == point.elementCenterId)
          add(sketch.lineId(i));
    }
  }
  return result;
}

[[nodiscard]] std::optional<DimensionId> firstFreeDimensionId(
    const std::vector<Dimension>& dimensions, DimensionId start,
    DimensionId additionallyReserved = kInvalidDimensionId) noexcept {
  DimensionId candidate = start == kInvalidDimensionId ? DimensionId{1} : start;
  // There are at most dimensions.size()+1 occupied values to inspect (the
  // optional reserved ID is the not-yet-inserted dimension). Pigeonhole
  // guarantees a free value within the following bounded scan.
  for (std::size_t attempts = 0; attempts <= dimensions.size() + 1;
       ++attempts) {
    const bool occupied = candidate == additionallyReserved ||
        std::any_of(dimensions.begin(), dimensions.end(),
                    [candidate](const Dimension& dimension) {
                      return dimension.id == candidate;
                    });
    if (!occupied) return candidate;
    candidate = candidate == std::numeric_limits<DimensionId>::max()
        ? DimensionId{1}
        : candidate + 1;
  }
  return std::nullopt;
}
}

Point arcStartPoint(const Arc& arc) noexcept {
  return {arc.center.xMm + arc.radiusMm * std::cos(arc.startAngleRad),
          arc.center.yMm + arc.radiusMm * std::sin(arc.startAngleRad)};
}

Point arcEndPoint(const Arc& arc) noexcept {
  const double angle = arc.startAngleRad + arc.sweepAngleRad;
  return {arc.center.xMm + arc.radiusMm * std::cos(angle),
          arc.center.yMm + arc.radiusMm * std::sin(angle)};
}

namespace {

// First safe Arc constraint primitive: keep the Arc shape rigid and translate
// the whole curve when one of its endpoint references has to move. This avoids
// rebuilding radius/sweep inside the sequential constraint solver.
bool moveArcEndpointRigid(Arc& arc, bool start, Point target) {
  const Point current = start ? arcStartPoint(arc) : arcEndPoint(arc);
  const double dx = target.xMm - current.xMm;
  const double dy = target.yMm - current.yMm;
  if (std::abs(dx) <= 1e-12 && std::abs(dy) <= 1e-12) return true;
  arc.center.xMm += dx;
  arc.center.yMm += dy;
  return true;
}

// Reshape an arc by moving one endpoint while keeping the center, radius and
// the opposite endpoint fixed. Used by interactive endpoint dragging (unlike
// moveArcEndpointRigid, which translates the whole curve for the solver).
bool moveArcEndpointReshape(Arc& arc, bool start, Point target) {
  const double dx = target.xMm - arc.center.xMm;
  const double dy = target.yMm - arc.center.yMm;
  if (std::hypot(dx, dy) <= 1e-12) return false;

  const double targetAngle = std::atan2(dy, dx);
  constexpr double kTwoPi = 6.28318530717958647692;
  const auto normalizeSweep = [kTwoPi](double sweep) {
    while (sweep <= 1e-9) sweep += kTwoPi;
    while (sweep >= kTwoPi - 1e-9) sweep -= kTwoPi;
    return sweep;
  };

  if (start) {
    const double endAngle = arc.startAngleRad + arc.sweepAngleRad;
    arc.startAngleRad = targetAngle;
    arc.sweepAngleRad = normalizeSweep(endAngle - targetAngle);
  } else {
    arc.sweepAngleRad = normalizeSweep(targetAngle - arc.startAngleRad);
  }
  return true;
}

// Move one Arc endpoint exactly onto a constraint target while leaving the
// opposite endpoint fixed. Preserve the included angle, so the Arc keeps its
// overall shape while its radius and centre adapt to the new chord.
bool moveArcEndpointForConstraint(Arc& arc, bool start, Point target) {
  const Point fixed = start ? arcEndPoint(arc) : arcStartPoint(arc);
  const Point newStart = start ? target : fixed;
  const Point newEnd = start ? fixed : target;
  const double chordX = newEnd.xMm - newStart.xMm;
  const double chordY = newEnd.yMm - newStart.yMm;
  const double chordLength = std::hypot(chordX, chordY);
  if (chordLength <= 1e-12) return false;

  constexpr double kTwoPi = 6.28318530717958647692;
  const double sweep = std::clamp(arc.sweepAngleRad, 1e-9,
                                  kTwoPi - 1e-9);
  const double halfSweep = sweep * 0.5;
  const double sinHalfSweep = std::sin(halfSweep);
  if (std::abs(sinHalfSweep) <= 1e-12) return false;

  const double radius = chordLength / (2.0 * sinHalfSweep);
  const Point midpoint{(newStart.xMm + newEnd.xMm) * 0.5,
                       (newStart.yMm + newEnd.yMm) * 0.5};
  const double centerOffset = radius * std::cos(halfSweep);
  const double leftNormalX = -chordY / chordLength;
  const double leftNormalY = chordX / chordLength;

  arc.center = {midpoint.xMm + leftNormalX * centerOffset,
                midpoint.yMm + leftNormalY * centerOffset};
  arc.radiusMm = radius;
  arc.startAngleRad =
      std::atan2(newStart.yMm - arc.center.yMm,
                 newStart.xMm - arc.center.xMm);
  arc.sweepAngleRad = sweep;
  return true;
}

}  // namespace

Sketch::Sketch() { clear(); }

Sketch::Sketch(const Sketch& other)
    : widthMm_(other.widthMm_),
      heightMm_(other.heightMm_),
      lines_(other.lines_), circles_(other.circles_), arcs_(other.arcs_),
      lineIds_(other.lineIds_), circleIds_(other.circleIds_),
      arcIds_(other.arcIds_), dimensions_(other.dimensions_),
      constraints_(other.constraints_),
      centerNodeElementIds_(other.centerNodeElementIds_),
      nextElementId_(other.nextElementId_),
      nextGeometryId_(other.nextGeometryId_),
      nextConstraintId_(other.nextConstraintId_),
      nextDimensionId_(other.nextDimensionId_),
      structureIndexesDirty_(other.structureIndexesDirty_),
      connectivityDirty_(other.connectivityDirty_),
      geometryIndex_(other.geometryIndex_),
      constraintIndex_(other.constraintIndex_),
      connectivity_(other.connectivity_),
      geometryConstraints_(other.geometryConstraints_),
      lastSolvedFingerprint_(other.lastSolvedFingerprint_),
      hasLastSolvedFingerprint_(other.hasLastSolvedFingerprint_),
      lastSolveConverged_(other.lastSolveConverged_),
      lastSolveViolatedConstraints_(other.lastSolveViolatedConstraints_),
      lastSolveMaxNormalizedResidual_(other.lastSolveMaxNormalizedResidual_),
      lastSolveUnsupported_(other.lastSolveUnsupported_),
      lastSolveInvalidReferences_(other.lastSolveInvalidReferences_) {
  ++fullSketchCopyCount;
}

Sketch& Sketch::operator=(const Sketch& other) {
  if (this == &other) return *this;
  ++fullSketchCopyCount;
  widthMm_ = other.widthMm_; heightMm_ = other.heightMm_;
  lines_ = other.lines_; circles_ = other.circles_; arcs_ = other.arcs_;
  lineIds_ = other.lineIds_; circleIds_ = other.circleIds_;
  arcIds_ = other.arcIds_; dimensions_ = other.dimensions_;
  constraints_ = other.constraints_;
  centerNodeElementIds_ = other.centerNodeElementIds_;
  nextElementId_ = other.nextElementId_;
  nextGeometryId_ = other.nextGeometryId_;
  nextConstraintId_ = other.nextConstraintId_;
  nextDimensionId_ = other.nextDimensionId_;
  structureIndexesDirty_ = other.structureIndexesDirty_;
  connectivityDirty_ = other.connectivityDirty_;
  geometryIndex_ = other.geometryIndex_; constraintIndex_ = other.constraintIndex_;
  connectivity_ = other.connectivity_;
  geometryConstraints_ = other.geometryConstraints_;
  lastSolvedFingerprint_ = other.lastSolvedFingerprint_;
  hasLastSolvedFingerprint_ = other.hasLastSolvedFingerprint_;
  lastSolveConverged_ = other.lastSolveConverged_;
  lastSolveViolatedConstraints_ = other.lastSolveViolatedConstraints_;
  lastSolveMaxNormalizedResidual_ = other.lastSolveMaxNormalizedResidual_;
  lastSolveUnsupported_ = other.lastSolveUnsupported_;
  lastSolveInvalidReferences_ = other.lastSolveInvalidReferences_;
  deltaJournals_.clear();
  return *this;
}

void Sketch::resetFullCopyCountForTesting() noexcept {
  fullSketchCopyCount.store(0);
}

std::size_t Sketch::fullCopyCountForTesting() noexcept {
  return fullSketchCopyCount.load();
}

void Sketch::resetDeltaJournalBeginCountForTesting() noexcept {
  deltaJournalBeginCount.store(0);
}

std::size_t Sketch::deltaJournalBeginCountForTesting() noexcept {
  return deltaJournalBeginCount.load();
}

void Sketch::failNextNestedConstraintJournalForTesting() noexcept {
  failNextNestedConstraintJournal.store(true);
}

bool Sketch::deltaJournalActive() const noexcept {
  return !deltaJournals_.empty();
}

std::size_t Sketch::deltaJournalDepth() const noexcept {
  return deltaJournals_.size();
}

bool Sketch::rollbackDeltaJournalsToDepth(std::size_t depth) noexcept {
  if (depth > deltaJournals_.size()) return false;
  bool success = true;
  while (deltaJournals_.size() > depth) {
    try {
      static_cast<void>(cancelDeltaJournal());
    } catch (...) {
      // cancelDeltaJournal removes the top frame before applying its inverse.
      // Keep unwinding any remaining controller-owned frames so a failed
      // rollback can never orphan a journal and wedge subsequent history.
      success = false;
    }
  }
  return success;
}

std::vector<ConstraintId> Sketch::invalidReferenceConstraintIds() const {
  std::unordered_map<GeometryId, GeometryKind> geometry;
  geometry.reserve(lineIds_.size() + circleIds_.size() + arcIds_.size());
  for (const auto id : lineIds_) geometry.emplace(id, GeometryKind::Line);
  for (const auto id : circleIds_) geometry.emplace(id, GeometryKind::Circle);
  for (const auto id : arcIds_) geometry.emplace(id, GeometryKind::Arc);
  const auto validPoint = [this, &geometry](const PointReference& point) {
    const unsigned sourceCount = static_cast<unsigned>(point.origin) +
        static_cast<unsigned>(point.lineId != kInvalidGeometryId) +
        static_cast<unsigned>(point.circleId != kInvalidGeometryId) +
        static_cast<unsigned>(point.arcId != kInvalidGeometryId) +
        static_cast<unsigned>(point.elementCenterId != 0);
    if (sourceCount > 1) return false;
    const auto hasKind = [&geometry](GeometryId id, GeometryKind kind) {
      const auto found = geometry.find(id);
      return found != geometry.end() && found->second == kind;
    };
    return (point.lineId == kInvalidGeometryId ||
            hasKind(point.lineId, GeometryKind::Line)) &&
           (point.circleId == kInvalidGeometryId ||
            hasKind(point.circleId, GeometryKind::Circle)) &&
           (point.arcId == kInvalidGeometryId ||
            hasKind(point.arcId, GeometryKind::Arc)) &&
           (point.elementCenterId == 0 ||
            std::find(centerNodeElementIds_.begin(),
                      centerNodeElementIds_.end(), point.elementCenterId) !=
                centerNodeElementIds_.end());
  };
  std::vector<ConstraintId> result;
  for (const auto& item : constraints_)
    if ((item.firstGeometry != kInvalidGeometryId &&
         !geometry.contains(item.firstGeometry)) ||
        (item.secondGeometry != kInvalidGeometryId &&
         !geometry.contains(item.secondGeometry)) ||
        !validPoint(item.firstPoint) || !validPoint(item.secondPoint))
      result.push_back(item.id);
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

void Sketch::beginDeltaJournal() {
  ++deltaJournalBeginCount;
  std::vector<std::vector<std::size_t>> parentDimensionTokens;
  parentDimensionTokens.reserve(deltaJournals_.size());
  for (const auto& active : deltaJournals_)
    parentDimensionTokens.push_back(active.dimensionTokens);
  deltaJournals_.emplace_back();
  auto& journal = deltaJournals_.back();
  journal.parentDimensionTokensBefore = std::move(parentDimensionTokens);
  journal.beforeLineIds = lineIds_;
  journal.beforeCircleIds = circleIds_;
  journal.beforeArcIds = arcIds_;
  journal.beforeConstraintIds.reserve(constraints_.size());
  for (const auto& constraint : constraints_)
    journal.beforeConstraintIds.push_back(constraint.id);
  journal.beforeDimensionCount = dimensions_.size();
  journal.nextDimensionToken = dimensions_.size();
  journal.dimensionTokens.resize(dimensions_.size());
  std::iota(journal.dimensionTokens.begin(), journal.dimensionTokens.end(),
            std::size_t{0});
  journal.beforeNextElementId = nextElementId_;
  journal.beforeNextGeometryId = nextGeometryId_;
  journal.beforeNextConstraintId = nextConstraintId_;
  journal.beforeNextDimensionId = nextDimensionId_;
  journal.beforeSemanticFingerprint = semanticFingerprint();
  journal.beforeInvalidConstraintIds = invalidReferenceConstraintIds();
}

void Sketch::clear() {
  if (!deltaJournals_.empty()) {
    for (const auto id : lineIds_) journalCaptureGeometry(id);
    for (const auto id : circleIds_) journalCaptureGeometry(id);
    for (const auto id : arcIds_) journalCaptureGeometry(id);
    for (const auto& item : constraints_) journalCaptureConstraint(item.id);
    journalCaptureAllDimensions();
    journalCaptureCenters();
  }
  invalidateStructureIndexes();
  centerNodeElementIds_.clear();
  lines_.clear();
  circles_.clear();
  arcs_.clear();
  lineIds_.clear();
  circleIds_.clear();
  arcIds_.clear();
  dimensions_.clear();
  for (auto& journal : deltaJournals_) journal.dimensionTokens.clear();
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
  for (std::size_t index = 0; index < lines_.size(); ++index)
    journalRecordAddedGeometry(lineIds_[index], GeometryKind::Line, index);
  invalidateStructureIndexes();
  updateBounds();
}

void Sketch::addLine(Point start, Point end) {
  if (start.xMm == end.xMm && start.yMm == end.yMm) return;
  lines_.push_back({start, end, nextElementId_++});
  lineIds_.push_back(nextGeometryId_++);
  journalRecordAddedGeometry(lineIds_.back(), GeometryKind::Line,
                             lines_.size() - 1);
  invalidateStructureIndexes();
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
  journalRecordAddedGeometry(lineIds_.back(), GeometryKind::Line,
                             lines_.size() - 1);
  invalidateStructureIndexes();

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
  journalRecordAddedGeometry(firstLineId, GeometryKind::Line,
                             lines_.size() - 4);
  journalRecordAddedGeometry(secondLineId, GeometryKind::Line,
                             lines_.size() - 3);
  journalRecordAddedGeometry(thirdLineId, GeometryKind::Line,
                             lines_.size() - 2);
  journalRecordAddedGeometry(fourthLineId, GeometryKind::Line,
                             lines_.size() - 1);
  invalidateStructureIndexes();

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
  journalRecordAddedGeometry(firstLineId, GeometryKind::Line,
                             lines_.size() - 4);
  journalRecordAddedGeometry(secondLineId, GeometryKind::Line,
                             lines_.size() - 3);
  journalRecordAddedGeometry(thirdLineId, GeometryKind::Line,
                             lines_.size() - 2);
  journalRecordAddedGeometry(fourthLineId, GeometryKind::Line,
                             lines_.size() - 1);
  invalidateStructureIndexes();

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
  journalRecordAddedGeometry(circleIds_.back(), GeometryKind::Circle,
                             circles_.size() - 1);
  invalidateStructureIndexes();
  updateBounds();
}

void Sketch::addArc(Point center, double radiusMm, double startAngleRad,
                    double sweepAngleRad, bool dashed) {
  constexpr double kTwoPi = 6.28318530717958647692;
  if (!std::isfinite(center.xMm) || !std::isfinite(center.yMm) ||
      !std::isfinite(radiusMm) || !std::isfinite(startAngleRad) ||
      !std::isfinite(sweepAngleRad) || radiusMm <= 0.0 ||
      sweepAngleRad <= 1e-9 || sweepAngleRad >= kTwoPi - 1e-9)
    return;

  arcs_.push_back({center, radiusMm, startAngleRad, sweepAngleRad, dashed});
  arcIds_.push_back(nextGeometryId_++);
  journalRecordAddedGeometry(arcIds_.back(), GeometryKind::Arc,
                             arcs_.size() - 1);
  invalidateStructureIndexes();
  updateBounds();
}

void Sketch::removeLine(std::size_t index) {
  // LOCK CONSTRAINT: locked geometry cannot be deleted.
  if (index < lineIds_.size() &&
      isGeometryLocked(lineIds_[index]))
    return;
  if (index >= lines_.size()) return;

  const GeometryId removedId = lineIds_[index];
  journalCaptureComponents({removedId});
  journalCaptureAllDimensions();
  journalCaptureCenters();
  const std::size_t removedElementId = lines_[index].elementId;
  lines_.erase(lines_.begin() + index);
  lineIds_.erase(lineIds_.begin() + index);

  const bool invalidatedElementCenter =
      hasElementCenterNode(removedElementId) &&
      std::count_if(lines_.begin(), lines_.end(),
                    [removedElementId](const Line& line) {
                      return line.elementId == removedElementId;
                    }) != 4;
  if (invalidatedElementCenter)
    std::erase(centerNodeElementIds_, removedElementId);

  std::vector<std::size_t> dimensionsToRemove;
  for (std::size_t dimensionIndex = 0;
       dimensionIndex < dimensions_.size(); ++dimensionIndex) {
    const auto& dimension = dimensions_[dimensionIndex];
    const bool remove = [removedId, invalidatedElementCenter,
                         removedElementId](const Dimension& dimension) {
    if (dimension.kind == DimensionKind::LineLength)
      return dimension.geometryId == removedId;
    if (dimension.kind == DimensionKind::PointDistance ||
        dimension.kind == DimensionKind::PointDistanceX ||
        dimension.kind == DimensionKind::PointDistanceY)
      return dimension.firstPoint.lineId == removedId ||
             dimension.secondPoint.lineId == removedId ||
             (invalidatedElementCenter &&
              (dimension.firstPoint.elementCenterId == removedElementId ||
               dimension.secondPoint.elementCenterId == removedElementId));
    if (dimension.kind == DimensionKind::LineAngle ||
        dimension.kind == DimensionKind::LineDistance)
      return dimension.geometryId == removedId ||
             dimension.secondPoint.lineId == removedId;
    return false;
    }(dimension);
    if (remove) dimensionsToRemove.push_back(dimensionIndex);
  }
  for (auto it = dimensionsToRemove.rbegin();
       it != dimensionsToRemove.rend(); ++it)
    static_cast<void>(removeDimension(*it));

  std::erase_if(constraints_,
                [removedId, invalidatedElementCenter,
                 removedElementId](const Constraint& constraint) {
    return constraint.firstGeometry == removedId ||
           constraint.secondGeometry == removedId ||
           constraint.firstPoint.lineId == removedId ||
           constraint.secondPoint.lineId == removedId ||
           (invalidatedElementCenter &&
            (constraint.firstPoint.elementCenterId == removedElementId ||
             constraint.secondPoint.elementCenterId == removedElementId));
  });

  invalidateStructureIndexes();
  updateBounds();
}

void Sketch::removeCircle(std::size_t index) {
  // LOCK CONSTRAINT: locked geometry cannot be deleted.
  if (index < circleIds_.size() &&
      isGeometryLocked(circleIds_[index]))
    return;
  if (index >= circles_.size()) return;

  const GeometryId removedId = circleIds_[index];
  journalCaptureComponents({removedId});
  journalCaptureAllDimensions();
  circles_.erase(circles_.begin() + index);
  circleIds_.erase(circleIds_.begin() + index);

  std::vector<std::size_t> dimensionsToRemove;
  for (std::size_t dimensionIndex = 0;
       dimensionIndex < dimensions_.size(); ++dimensionIndex) {
    const auto& dimension = dimensions_[dimensionIndex];
    const bool remove = [removedId](const Dimension& dimension) {
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
    }(dimension);
    if (remove) dimensionsToRemove.push_back(dimensionIndex);
  }
  for (auto it = dimensionsToRemove.rbegin();
       it != dimensionsToRemove.rend(); ++it)
    static_cast<void>(removeDimension(*it));

  std::erase_if(constraints_, [removedId](const Constraint& constraint) {
    return constraint.firstGeometry == removedId ||
           constraint.secondGeometry == removedId ||
           constraint.firstPoint.circleId == removedId ||
           constraint.secondPoint.circleId == removedId;
  });

  invalidateStructureIndexes();
  updateBounds();
}

void Sketch::removeArc(std::size_t index) {
  if (index < arcIds_.size() && isGeometryLocked(arcIds_[index]))
    return;
  if (index >= arcs_.size()) return;

  const GeometryId removedId = arcIds_[index];
  journalCaptureComponents({removedId});
  journalCaptureAllDimensions();
  arcs_.erase(arcs_.begin() + index);
  arcIds_.erase(arcIds_.begin() + index);

  std::vector<std::size_t> dimensionsToRemove;
  for (std::size_t dimensionIndex = 0;
       dimensionIndex < dimensions_.size(); ++dimensionIndex) {
    const auto& dimension = dimensions_[dimensionIndex];
    if (dimension.geometryId == removedId ||
        ((dimension.kind == DimensionKind::PointDistance ||
          dimension.kind == DimensionKind::PointDistanceX ||
          dimension.kind == DimensionKind::PointDistanceY) &&
         (dimension.firstPoint.arcId == removedId ||
          dimension.secondPoint.arcId == removedId)))
      dimensionsToRemove.push_back(dimensionIndex);
  }
  for (auto it = dimensionsToRemove.rbegin();
       it != dimensionsToRemove.rend(); ++it)
    static_cast<void>(removeDimension(*it));

  std::erase_if(constraints_, [removedId](const Constraint& constraint) {
    return constraint.firstGeometry == removedId ||
           constraint.secondGeometry == removedId ||
           constraint.firstPoint.arcId == removedId ||
           constraint.secondPoint.arcId == removedId;
  });

  invalidateStructureIndexes();
  updateBounds();
}

void Sketch::removeElement(std::size_t elementId) {
  // LOCK CONSTRAINT: one Lock freezes the whole CAD element.
  if (isElementLocked(elementId))
    return;
  std::vector<GeometryId> journalSeeds;
  for (std::size_t index = 0; index < lines_.size(); ++index)
    if (lines_[index].elementId == elementId)
      journalSeeds.push_back(lineIds_[index]);
  journalCaptureComponents(journalSeeds);
  journalCaptureAllDimensions();
  journalCaptureCenters();
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

    std::vector<std::size_t> dimensionsToRemove;
    for (std::size_t index = 0; index < dimensions_.size(); ++index) {
      const auto& dimension = dimensions_[index];
      if (dimension.kind == DimensionKind::LineLength)
        { if (wasRemoved(dimension.geometryId)) dimensionsToRemove.push_back(index); }
      else if (dimension.kind == DimensionKind::PointDistance ||
               dimension.kind == DimensionKind::PointDistanceX ||
               dimension.kind == DimensionKind::PointDistanceY) {
        if (wasRemoved(dimension.firstPoint.lineId) ||
            wasRemoved(dimension.secondPoint.lineId))
          dimensionsToRemove.push_back(index);
      } else if (dimension.kind == DimensionKind::LineAngle ||
                 dimension.kind == DimensionKind::LineDistance) {
        if (wasRemoved(dimension.geometryId) ||
            wasRemoved(dimension.secondPoint.lineId))
          dimensionsToRemove.push_back(index);
      }
    }
    for (auto it = dimensionsToRemove.rbegin();
         it != dimensionsToRemove.rend(); ++it)
      static_cast<void>(removeDimension(*it));

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
  std::vector<std::size_t> centeredDimensionsToRemove;
  for (std::size_t index = 0; index < dimensions_.size(); ++index)
    if (dimensions_[index].firstPoint.elementCenterId == elementId ||
        dimensions_[index].secondPoint.elementCenterId == elementId)
      centeredDimensionsToRemove.push_back(index);
  for (auto it = centeredDimensionsToRemove.rbegin();
       it != centeredDimensionsToRemove.rend(); ++it)
    static_cast<void>(removeDimension(*it));

  std::erase_if(constraints_, [elementId](const Constraint& constraint) {
    return constraint.firstPoint.elementCenterId == elementId ||
           constraint.secondPoint.elementCenterId == elementId;
  });
  std::erase(centerNodeElementIds_, elementId);
  invalidateStructureIndexes();
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
    journalCaptureCenters();
  if (lineCount == 4)
    centerNodeElementIds_.push_back(elementId);
  invalidateStructureIndexes();
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
  // LOCK CONSTRAINT: direct drag may never move a locked element.
  if (isElementLocked(elementId))
    return;
  if (dxMm == 0.0 && dyMm == 0.0) return;

  std::vector<GeometryId> movedIds;
  for (std::size_t index = 0; index < lines_.size(); ++index) {
    if (lines_[index].elementId == elementId)
      movedIds.push_back(lineIds_[index]);
  }
  journalCaptureComponents(movedIds);

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
  (void)BasicSketchSolver::solveStableComponent(*this, movedIds);
  updateBounds();
}

void Sketch::translateSelection(
    const std::vector<std::size_t>& elementIds,
    const std::vector<GeometryId>& circleIds,
    const std::vector<GeometryId>& arcIds,
    double dxMm, double dyMm) {
  // LOCK CONSTRAINT: mixed selections do not partially move.
  for (std::size_t index = 0; index < lines_.size(); ++index) {
    if (std::find(elementIds.begin(), elementIds.end(),
                  lines_[index].elementId) != elementIds.end() &&
        isGeometryLocked(lineIds_[index]))
      return;
  }
  for (const auto id : circleIds) {
    if (isGeometryLocked(id))
      return;
  }
  for (const auto id : arcIds) {
    if (isGeometryLocked(id))
      return;
  }
  if (dxMm == 0.0 && dyMm == 0.0) return;
  if (elementIds.empty() && circleIds.empty() && arcIds.empty()) return;
  std::vector<GeometryId> journalSeeds = circleIds;
  journalSeeds.insert(journalSeeds.end(), arcIds.begin(), arcIds.end());
  for (std::size_t index = 0; index < lines_.size(); ++index)
    if (std::find(elementIds.begin(), elementIds.end(),
                  lines_[index].elementId) != elementIds.end())
      journalSeeds.push_back(lineIds_[index]);
  journalCaptureComponents(journalSeeds);

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

  const auto arcSelected =
      [&arcIds](GeometryId id) {
        return std::find(arcIds.begin(), arcIds.end(), id) != arcIds.end();
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

        if (first.arcId != kInvalidGeometryId ||
            second.arcId != kInvalidGeometryId) {
          return first.arcId != kInvalidGeometryId &&
                 second.arcId != kInvalidGeometryId &&
                 first.arcId == second.arcId &&
                 first.start == second.start;
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

  for (std::size_t index = 0; index < arcs_.size(); ++index) {
    const GeometryId id = arcIds_[index];
    if (!arcSelected(id)) continue;
    PointReference endpoint;
    endpoint.arcId = id;
    endpoint.start = true;
    addReference(endpoint);
    endpoint.start = false;
    addReference(endpoint);
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

  // LOCK CONSTRAINT: Coincident expansion reaches a locked reference.
  // Reject the complete drag rather than moving the lock or breaking the link.
  if (std::any_of(
          movedReferences.begin(), movedReferences.end(),
          [this](PointReference reference) {
            return isPointReferenceLocked(reference);
          }))
    return;

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


  for (std::size_t index = 0; index < arcs_.size(); ++index) {
    const GeometryId id = arcIds_[index];
    PointReference start;
    start.arcId = id;
    start.start = true;
    PointReference end = start;
    end.start = false;
    const bool moveStart = referenceMoves(start);
    const bool moveEnd = referenceMoves(end);

    if (arcSelected(id) || (moveStart && moveEnd)) {
      arcs_[index].center.xMm += dxMm;
      arcs_[index].center.yMm += dyMm;
    } else if (moveStart) {
      const Point current = arcStartPoint(arcs_[index]);
      (void)moveArcEndpointReshape(
          arcs_[index], true,
          {current.xMm + dxMm, current.yMm + dyMm});
    } else if (moveEnd) {
      const Point current = arcEndPoint(arcs_[index]);
      (void)moveArcEndpointReshape(
          arcs_[index], false,
          {current.xMm + dxMm, current.yMm + dyMm});
    }
  }

  std::vector<GeometryId> dirtyIds;
  for (std::size_t index = 0; index < lines_.size(); ++index)
    if (elementSelected(lines_[index].elementId)) dirtyIds.push_back(lineIds_[index]);
  dirtyIds.insert(dirtyIds.end(), circleIds.begin(), circleIds.end());
  dirtyIds.insert(dirtyIds.end(), arcIds.begin(), arcIds.end());
  (void)BasicSketchSolver::solveStableComponent(*this, dirtyIds);
  updateBounds();
}

void Sketch::translateLinesByIds(const std::vector<GeometryId>& lineIds,
                                 double dxMm, double dyMm) {
  if (lineIds.empty() || (dxMm == 0.0 && dyMm == 0.0)) return;
  for (const auto id : lineIds)
    if (isGeometryLocked(id)) return;
  journalCaptureComponents(lineIds);

  bool changed = false;
  for (std::size_t index = 0; index < lines_.size(); ++index) {
    if (std::find(lineIds.begin(), lineIds.end(), lineIds_[index]) ==
        lineIds.end())
      continue;
    lines_[index].start.xMm += dxMm;
    lines_[index].start.yMm += dyMm;
    lines_[index].end.xMm += dxMm;
    lines_[index].end.yMm += dyMm;
    changed = true;
  }
  if (!changed) return;
  (void)BasicSketchSolver::solveStableComponent(*this, lineIds);
  updateBounds();
}

void Sketch::setElementDashed(std::size_t elementId, bool dashed) {
  // Locked/reference geometry cannot change its construction style.
  if (isElementLocked(elementId))
    return;
  for (std::size_t index = 0; index < lines_.size(); ++index) {
    if (lines_[index].elementId == elementId) {
      journalCaptureGeometry(lineIds_[index]);
      lines_[index].dashed = dashed;
    }
  }
}

void Sketch::setLineDashedById(GeometryId id, bool dashed) {
  const auto index = lineIndex(id);
  if (!index || isGeometryLocked(id)) return;
  journalCaptureGeometry(id);
  lines_[*index].dashed = dashed;
}

void Sketch::setCircleDashed(std::size_t index, bool dashed) {
  if (index < circleIds_.size() &&
      isGeometryLocked(circleIds_[index]))
    return;
  if (index < circles_.size()) {
    journalCaptureGeometry(circleIds_[index]);
    circles_[index].dashed = dashed;
  }
}

void Sketch::translateCircle(std::size_t index, double dxMm, double dyMm) {
  if (index < circleIds_.size() &&
      isGeometryLocked(circleIds_[index]))
    return;
  if (index >= circles_.size()) return;
  journalCaptureComponents({circleIds_[index]});
  circles_[index].center.xMm += dxMm;
  circles_[index].center.yMm += dyMm;
  updateBounds();
}

void Sketch::setCircleDashedById(GeometryId id, bool dashed) {
  const auto index = circleIndex(id);
  if (index) setCircleDashed(*index, dashed);
}

void Sketch::setArcDashedById(GeometryId id, bool dashed) {
  const auto index = arcIndex(id);
  if (!index || isGeometryLocked(id)) return;
  journalCaptureGeometry(id);
  arcs_[*index].dashed = dashed;
}

void Sketch::translateCircleById(GeometryId id, double dxMm, double dyMm) {
  if (id == kInvalidGeometryId || !circleIndex(id)) return;

  PointReference centerReference;
  centerReference.circleId = id;

  // Move the circle center together with every Coincident-connected
  // endpoint/center. This also re-runs the active solver afterwards.
  (void)translatePoint(centerReference, dxMm, dyMm);
}

void Sketch::translateArcById(GeometryId id, double dxMm, double dyMm) {
  const auto index = arcIndex(id);
  if (!index || isGeometryLocked(id)) return;
  journalCaptureComponents({id});
  arcs_[*index].center.xMm += dxMm;
  arcs_[*index].center.yMm += dyMm;
  (void)BasicSketchSolver::solveStableComponent(*this, {id});
  updateBounds();
}

bool Sketch::moveArcEndpointReshapeById(GeometryId id, bool start,
                                        Point target) {
  const auto index = arcIndex(id);
  if (!index || isGeometryLocked(id)) return false;
  journalCaptureComponents({id});
  if (!moveArcEndpointReshape(arcs_[*index], start, target)) return false;
  (void)BasicSketchSolver::solveStableComponent(*this, {id});
  updateBounds();
  return true;
}

bool Sketch::setLineLengthById(GeometryId id, double lengthMm) {
  const auto index = lineIndex(id);
  if (!index || lengthMm <= 0.0) return false;
  journalCaptureComponents({id});
  // LOCK CONSTRAINT: locked line length is immutable.
  // Re-applying the already satisfied value remains idempotently successful.
  if (isGeometryLocked(id)) {
    const auto& locked = lines_[*index];
    const double current =
        std::hypot(locked.end.xMm - locked.start.xMm,
                   locked.end.yMm - locked.start.yMm);
    return std::abs(current - lengthMm) <= 1e-7;
  }

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
  journalCaptureComponents({id});
  // LOCK CONSTRAINT: locked circle diameter is immutable.
  if (const auto lockedIndex = circleIndex(id);
      lockedIndex && isGeometryLocked(id)) {
    return std::abs(circles_[*lockedIndex].radiusMm * 2.0 -
                    diameterMm) <= 1e-7;
  }

  const auto index = circleIndex(id);
  return index ? setCircleDiameter(*index, diameterMm) : false;
}

bool Sketch::setLineHorizontalById(GeometryId id) {
  // LOCK CONSTRAINT: locked H/V geometry is immutable.
  if (isGeometryLocked(id)) {
    const auto lockedIndex = lineIndex(id);
    if (!lockedIndex) return false;
    return std::abs(lines_[*lockedIndex].end.yMm - lines_[*lockedIndex].start.yMm) <= 1e-7;
  }

  const auto index = lineIndex(id);
  if (!index) return false;

  const Point oldEnd = lines_[*index].end;
  const Point newEnd{oldEnd.xMm, lines_[*index].start.yMm};

  const auto same = [](Point first, Point second) {
    return std::hypot(first.xMm - second.xMm, first.yMm - second.yMm) <= 1e-7;
  };

  for (std::size_t candidate = 0; candidate < lines_.size(); ++candidate)
    if (same(lines_[candidate].start, oldEnd) ||
        same(lines_[candidate].end, oldEnd))
      journalCaptureGeometry(lineIds_[candidate]);

  for (auto& line : lines_) {
    if (same(line.start, oldEnd)) line.start = newEnd;
    if (same(line.end, oldEnd)) line.end = newEnd;
  }
  updateBounds();
  return true;
}

bool Sketch::setLineVerticalById(GeometryId id) {
  // LOCK CONSTRAINT: locked H/V geometry is immutable.
  if (isGeometryLocked(id)) {
    const auto lockedIndex = lineIndex(id);
    if (!lockedIndex) return false;
    return std::abs(lines_[*lockedIndex].end.xMm - lines_[*lockedIndex].start.xMm) <= 1e-7;
  }

  const auto index = lineIndex(id);
  if (!index) return false;

  const Point oldEnd = lines_[*index].end;
  const Point newEnd{lines_[*index].start.xMm, oldEnd.yMm};

  const auto same = [](Point first, Point second) {
    return std::hypot(first.xMm - second.xMm, first.yMm - second.yMm) <= 1e-7;
  };

  for (std::size_t candidate = 0; candidate < lines_.size(); ++candidate)
    if (same(lines_[candidate].start, oldEnd) ||
        same(lines_[candidate].end, oldEnd))
      journalCaptureGeometry(lineIds_[candidate]);

  for (auto& line : lines_) {
    if (same(line.start, oldEnd)) line.start = newEnd;
    if (same(line.end, oldEnd)) line.end = newEnd;
  }
  updateBounds();
  return true;
}

bool Sketch::setLinesParallelByIds(GeometryId firstId,
                                   GeometryId secondId) {
  journalCaptureComponents({firstId, secondId});
  const auto firstIndex = lineIndex(firstId);
  const auto secondIndex = lineIndex(secondId);
  // LOCK CONSTRAINT: locked parallel operand is always the reference.
  if (isGeometryLocked(secondId)) {
    if (isGeometryLocked(firstId))
      return false;
    return setLinesParallelByIds(secondId, firstId);
  }

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
bool Sketch::setParallelLineDistanceByIds(
    GeometryId referenceId, GeometryId movingId, double distanceMm) {
  journalCaptureComponents({referenceId, movingId});
  if (referenceId == kInvalidGeometryId ||
      movingId == kInvalidGeometryId ||
      referenceId == movingId ||
      !std::isfinite(distanceMm) || distanceMm <= 0.0)
    return false;

  // LOCK CONSTRAINT: locked spacing operand is the reference.
  if (isGeometryLocked(movingId)) {
    if (isGeometryLocked(referenceId))
      return false;
    return setParallelLineDistanceByIds(
        movingId, referenceId, distanceMm);
  }

  const auto referenceIndex = lineIndex(referenceId);
  const auto movingIndex = lineIndex(movingId);
  if (!referenceIndex || !movingIndex) return false;

  const Line reference = lines_[*referenceIndex];
  const Line moving = lines_[*movingIndex];
  const double rx = reference.end.xMm - reference.start.xMm;
  const double ry = reference.end.yMm - reference.start.yMm;
  const double mx = moving.end.xMm - moving.start.xMm;
  const double my = moving.end.yMm - moving.start.yMm;
  const double rl = std::hypot(rx, ry);
  const double ml = std::hypot(mx, my);
  if (rl <= 1e-9 || ml <= 1e-9) return false;

  const double parallelResidual =
      std::abs(rx * my - ry * mx) / (rl * ml);
  if (parallelResidual > 1e-6) return false;

  const double nx = -ry / rl;
  const double ny = rx / rl;
  const double currentSigned =
      (moving.start.xMm - reference.start.xMm) * nx +
      (moving.start.yMm - reference.start.yMm) * ny;
  const double targetSigned =
      currentSigned < 0.0 ? -distanceMm : distanceMm;
  const double delta = targetSigned - currentSigned;
  if (std::abs(delta) <= 1e-10) return true;

  const std::size_t movingElement = moving.elementId;
  for (auto& line : lines_) {
    if (line.elementId != movingElement) continue;
    line.start.xMm += nx * delta;
    line.start.yMm += ny * delta;
    line.end.xMm += nx * delta;
    line.end.yMm += ny * delta;
  }

  // Preserve Coincident neighbours of a moving loose line. This is important
  // for profiles such as the user's internal horizontal segment connected to
  // a vertical and a diagonal: changing its spacing stretches those neighbour
  // segments instead of opening gaps.
  const auto belongsToMoving =
      [this, movingElement](PointReference reference) {
        if (reference.lineId == kInvalidGeometryId) return false;
        const auto index = lineIndex(reference.lineId);
        return index && lines_[*index].elementId == movingElement;
      };

  const auto setLooseExternalPoint =
      [this](PointReference reference, Point target) {
        if (reference.lineId == kInvalidGeometryId) return;
        const auto index = lineIndex(reference.lineId);
        if (!index) return;
        const std::size_t element = lines_[*index].elementId;
        const auto members = static_cast<std::size_t>(std::count_if(
            lines_.begin(), lines_.end(),
            [element](const Line& line) {
              return line.elementId == element;
            }));
        // Never deform a rectangle/composite to satisfy spacing of a loose line.
        if (members != 1) return;
        Point& point = reference.start ? lines_[*index].start
                                       : lines_[*index].end;
        point = target;
      };

  for (const auto& constraint : constraints_) {
    if (constraint.type != ConstraintType::Coincident) continue;
    const bool firstMoves = belongsToMoving(constraint.firstPoint);
    const bool secondMoves = belongsToMoving(constraint.secondPoint);
    if (firstMoves == secondMoves) continue;
    const auto movedReference =
        firstMoves ? constraint.firstPoint : constraint.secondPoint;
    const auto externalReference =
        firstMoves ? constraint.secondPoint : constraint.firstPoint;
    if (const auto point = referencedPoint(movedReference))
      setLooseExternalPoint(externalReference, *point);
  }

  updateBounds();
  return true;
}

bool Sketch::setLineAngleByIds(GeometryId firstId, GeometryId secondId,
                               double angleDegrees) {
  journalCaptureComponents({firstId, secondId});
  const auto firstIndex = lineIndex(firstId);
  const auto secondIndex = lineIndex(secondId);
  // LOCK CONSTRAINT: locked angle operand is always the reference.
  if (isGeometryLocked(secondId)) {
    if (isGeometryLocked(firstId))
      return false;
    return setLineAngleByIds(secondId, firstId, angleDegrees);
  }

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
  bool pivotOnFirstBody = false;
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
      pivotOnFirstBody = true;

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

  // When the angular vertex lies on the BODY of the reference line, that
  // endpoint is allowed to slide along its PointOnLine carrier. For a
  // perpendicular relation, project the opposite endpoint onto the carrier
  // instead of rotating the opposite endpoint around a frozen foot. This is
  // essential for the common projected-geometry chain:
  // circle -> sized line -> projected straight edge.
  if (pivotOnFirstBody && std::abs(angleDegrees - 90.0) <= 1e-9) {
    const Point freePoint = pivotAtStart ? oldEnd : oldStart;
    const double carrierDx = first.end.xMm - first.start.xMm;
    const double carrierDy = first.end.yMm - first.start.yMm;
    const double carrierLengthSquared =
        carrierDx * carrierDx + carrierDy * carrierDy;
    if (carrierLengthSquared <= 1e-12) return false;
    const double parameter =
        ((freePoint.xMm - first.start.xMm) * carrierDx +
         (freePoint.yMm - first.start.yMm) * carrierDy) /
        carrierLengthSquared;
    const Point newPivot{
        first.start.xMm + parameter * carrierDx,
        first.start.yMm + parameter * carrierDy};

    if (pivotAtStart)
      second.start = newPivot;
    else
      second.end = newPivot;

    for (std::size_t lineIndexValue = 0;
         lineIndexValue < lines_.size(); ++lineIndexValue) {
      if (lineIndexValue == *secondIndex) continue;
      auto& line = lines_[lineIndexValue];
      if (same(line.start, pivot)) line.start = newPivot;
      if (same(line.end, pivot)) line.end = newPivot;
    }
    updateBounds();
    return true;
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

GeometryId Sketch::arcId(std::size_t index) const noexcept {
  return index < arcIds_.size() ? arcIds_[index] : kInvalidGeometryId;
}

std::optional<std::size_t> Sketch::lineIndex(GeometryId id) const noexcept {
  if (id == kInvalidGeometryId) return std::nullopt;
  try {
    rebuildIdentityIndexes();
    const auto found = geometryIndex_.find(id);
    if (found != geometryIndex_.end() &&
        found->second.kind == GeometryKind::Line)
      return found->second.index;
  } catch (...) {
    const auto found = std::find(lineIds_.begin(), lineIds_.end(), id);
    if (found != lineIds_.end())
      return static_cast<std::size_t>(std::distance(lineIds_.begin(), found));
  }
  return std::nullopt;
}

std::optional<std::size_t> Sketch::circleIndex(GeometryId id) const noexcept {
  if (id == kInvalidGeometryId) return std::nullopt;
  try {
    rebuildIdentityIndexes();
    const auto found = geometryIndex_.find(id);
    if (found != geometryIndex_.end() &&
        found->second.kind == GeometryKind::Circle)
      return found->second.index;
  } catch (...) {
    const auto found = std::find(circleIds_.begin(), circleIds_.end(), id);
    if (found != circleIds_.end())
      return static_cast<std::size_t>(std::distance(circleIds_.begin(), found));
  }
  return std::nullopt;
}

std::optional<std::size_t> Sketch::arcIndex(GeometryId id) const noexcept {
  if (id == kInvalidGeometryId) return std::nullopt;
  try {
    rebuildIdentityIndexes();
    const auto found = geometryIndex_.find(id);
    if (found != geometryIndex_.end() &&
        found->second.kind == GeometryKind::Arc)
      return found->second.index;
  } catch (...) {
    const auto found = std::find(arcIds_.begin(), arcIds_.end(), id);
    if (found != arcIds_.end())
      return static_cast<std::size_t>(std::distance(arcIds_.begin(), found));
  }
  return std::nullopt;
}

std::optional<GeometryLocation> Sketch::geometryLocation(
    GeometryId id) const noexcept {
  if (id == kInvalidGeometryId) return std::nullopt;
  try {
    rebuildIdentityIndexes();
    const auto found = geometryIndex_.find(id);
    if (found != geometryIndex_.end()) return found->second;
  } catch (...) {
  }
  return std::nullopt;
}

std::optional<std::size_t> Sketch::constraintIndex(ConstraintId id) const noexcept {
  if (id == kInvalidConstraintId) return std::nullopt;
  try {
    rebuildIdentityIndexes();
    const auto found = constraintIndex_.find(id);
    if (found != constraintIndex_.end()) return found->second;
  } catch (...) {
    const auto found = std::find_if(
        constraints_.begin(), constraints_.end(),
        [id](const Constraint& item) { return item.id == id; });
    if (found != constraints_.end())
      return static_cast<std::size_t>(std::distance(constraints_.begin(), found));
  }
  return std::nullopt;
}

std::optional<std::size_t> Sketch::dimensionIndex(DimensionId id) const noexcept {
  if (id == kInvalidDimensionId) return std::nullopt;
  const auto found = std::find_if(
      dimensions_.begin(), dimensions_.end(),
      [id](const Dimension& item) { return item.id == id; });
  if (found == dimensions_.end()) return std::nullopt;
  return static_cast<std::size_t>(std::distance(dimensions_.begin(), found));
}

void Sketch::invalidateStructureIndexes() noexcept {
  structureIndexesDirty_ = true;
  connectivityDirty_ = true;
  hasLastSolvedFingerprint_ = false;
}

void Sketch::rebuildIdentityIndexes() const {
  if (!structureIndexesDirty_) return;
  geometryIndex_.clear();
  constraintIndex_.clear();
  const std::size_t geometryCount =
      lineIds_.size() + circleIds_.size() + arcIds_.size();
  geometryIndex_.reserve(geometryCount);
  constraintIndex_.reserve(constraints_.size());

  const auto addGeometry = [this](GeometryId id, GeometryKind kind,
                                  std::size_t index) {
    if (id == kInvalidGeometryId) return;
    geometryIndex_.insert_or_assign(id, GeometryLocation{kind, index});
  };
  for (std::size_t i = 0; i < lineIds_.size(); ++i)
    addGeometry(lineIds_[i], GeometryKind::Line, i);
  for (std::size_t i = 0; i < circleIds_.size(); ++i)
    addGeometry(circleIds_[i], GeometryKind::Circle, i);
  for (std::size_t i = 0; i < arcIds_.size(); ++i)
    addGeometry(arcIds_[i], GeometryKind::Arc, i);
  for (std::size_t index = 0; index < constraints_.size(); ++index)
    constraintIndex_.insert_or_assign(constraints_[index].id, index);
  structureIndexesDirty_ = false;
  connectivityDirty_ = true;
}

void Sketch::rebuildStructureIndexes() const {
  rebuildIdentityIndexes();
  if (!connectivityDirty_) return;
  connectivity_.clear();
  geometryConstraints_.clear();
  const std::size_t geometryCount = geometryIndex_.size();
  connectivity_.reserve(geometryCount);
  geometryConstraints_.reserve(geometryCount);
  for (const auto& [id, location] : geometryIndex_) {
    static_cast<void>(location);
    connectivity_.try_emplace(id);
    geometryConstraints_.try_emplace(id);
  }

  const auto connect = [this](GeometryId first, GeometryId second) {
    if (first == second || first == kInvalidGeometryId ||
        second == kInvalidGeometryId || !geometryIndex_.contains(first) ||
        !geometryIndex_.contains(second))
      return;
    connectivity_[first].push_back(second);
    connectivity_[second].push_back(first);
  };

  std::unordered_map<std::size_t, std::vector<GeometryId>> elementMembers;
  for (std::size_t i = 0; i < lines_.size() && i < lineIds_.size(); ++i)
    elementMembers[lines_[i].elementId].push_back(lineIds_[i]);
  for (const auto& [elementId, members] : elementMembers) {
    static_cast<void>(elementId);
    for (std::size_t i = 1; i < members.size(); ++i)
      connect(members.front(), members[i]);
  }

  // Several interactive mutators preserve an existing CAD junction by
  // moving every point that is coincident within the model tolerance.  That
  // implicit coupling is part of the solve component too; otherwise a local
  // solve can mutate geometry that was omitted from its journal/snapshot.
  struct PointOwner {
    Point point;
    GeometryId id{kInvalidGeometryId};
  };
  std::vector<PointOwner> points;
  points.reserve(lines_.size() * 2 + circles_.size());
  for (std::size_t i = 0; i < lines_.size() && i < lineIds_.size(); ++i) {
    points.push_back({lines_[i].start, lineIds_[i]});
    points.push_back({lines_[i].end, lineIds_[i]});
  }
  for (std::size_t i = 0; i < circles_.size() && i < circleIds_.size(); ++i)
    points.push_back({circles_[i].center, circleIds_[i]});
  std::sort(points.begin(), points.end(), [](const auto& first,
                                             const auto& second) {
    if (first.point.xMm != second.point.xMm)
      return first.point.xMm < second.point.xMm;
    if (first.point.yMm != second.point.yMm)
      return first.point.yMm < second.point.yMm;
    return first.id < second.id;
  });
  constexpr double kJunctionTolerance = 1e-7;
  for (std::size_t i = 0; i < points.size(); ++i) {
    for (std::size_t j = i + 1; j < points.size(); ++j) {
      if (points[j].point.xMm - points[i].point.xMm > kJunctionTolerance)
        break;
      if (std::hypot(points[j].point.xMm - points[i].point.xMm,
                     points[j].point.yMm - points[i].point.yMm) <=
          kJunctionTolerance)
        connect(points[i].id, points[j].id);
    }
  }

  const auto appendReference = [&elementMembers](
                                   std::vector<GeometryId>& ids,
                                   const PointReference& reference) {
    if (reference.lineId != kInvalidGeometryId) ids.push_back(reference.lineId);
    if (reference.circleId != kInvalidGeometryId) ids.push_back(reference.circleId);
    if (reference.arcId != kInvalidGeometryId) ids.push_back(reference.arcId);
    if (reference.elementCenterId != 0) {
      const auto found = elementMembers.find(reference.elementCenterId);
      if (found != elementMembers.end())
        ids.insert(ids.end(), found->second.begin(), found->second.end());
    }
    // The origin is an immutable anchor, deliberately not a graph vertex:
    // two otherwise-independent constraints to the datum stay independent.
  };

  for (std::size_t index = 0; index < constraints_.size(); ++index) {
    const auto& constraint = constraints_[index];
    std::vector<GeometryId> ids;
    if (constraint.firstGeometry != kInvalidGeometryId)
      ids.push_back(constraint.firstGeometry);
    if (constraint.secondGeometry != kInvalidGeometryId)
      ids.push_back(constraint.secondGeometry);
    appendReference(ids, constraint.firstPoint);
    appendReference(ids, constraint.secondPoint);
    std::erase_if(ids, [this](GeometryId id) {
      return id == kInvalidGeometryId || !geometryIndex_.contains(id);
    });
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (const auto id : ids)
      geometryConstraints_[id].push_back(constraint.id);
    for (std::size_t i = 1; i < ids.size(); ++i) connect(ids.front(), ids[i]);
  }

  for (auto& [id, adjacent] : connectivity_) {
    static_cast<void>(id);
    std::sort(adjacent.begin(), adjacent.end());
    adjacent.erase(std::unique(adjacent.begin(), adjacent.end()), adjacent.end());
  }
  connectivityDirty_ = false;
}

ConstraintComponent Sketch::connectedComponent(
    const std::vector<GeometryId>& seeds) const {
  rebuildStructureIndexes();
  ConstraintComponent result;
  std::vector<GeometryId> orderedSeeds = seeds;
  std::sort(orderedSeeds.begin(), orderedSeeds.end());
  orderedSeeds.erase(std::unique(orderedSeeds.begin(), orderedSeeds.end()),
                     orderedSeeds.end());
  std::queue<GeometryId> pending;
  std::unordered_map<GeometryId, bool> visited;
  std::unordered_set<ConstraintId> componentConstraints;
  for (const auto seed : orderedSeeds) {
    if (!geometryIndex_.contains(seed) || visited.contains(seed)) continue;
    visited.emplace(seed, true);
    pending.push(seed);
  }
  while (!pending.empty()) {
    const auto id = pending.front();
    pending.pop();
    result.geometryIds.push_back(id);
    if (const auto found = geometryConstraints_.find(id);
        found != geometryConstraints_.end())
      componentConstraints.insert(found->second.begin(), found->second.end());
    if (const auto found = connectivity_.find(id); found != connectivity_.end()) {
      for (const auto adjacent : found->second) {
        if (visited.emplace(adjacent, true).second) pending.push(adjacent);
      }
    }
  }
  std::sort(result.geometryIds.begin(), result.geometryIds.end());
  // Constraint order is persisted and may select a deterministic branch in
  // an underconstrained sequential solve.  IDs are membership keys only.
  for (const auto& constraint : constraints_)
    if (componentConstraints.contains(constraint.id))
      result.constraintIds.push_back(constraint.id);
  return result;
}

std::vector<ConstraintComponent> Sketch::constraintComponents() const {
  rebuildStructureIndexes();
  std::vector<GeometryId> ids;
  ids.reserve(geometryIndex_.size());
  for (const auto& [id, location] : geometryIndex_) {
    static_cast<void>(location);
    ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  std::unordered_map<GeometryId, bool> consumed;
  std::vector<ConstraintComponent> result;
  for (const auto id : ids) {
    if (consumed.contains(id)) continue;
    auto component = connectedComponent({id});
    for (const auto member : component.geometryIds) consumed.emplace(member, true);
    result.push_back(std::move(component));
  }
  return result;
}

std::size_t Sketch::ownedBytes() const noexcept {
  std::size_t bytes = lines_.capacity() * sizeof(Line) +
         circles_.capacity() * sizeof(Circle) +
         arcs_.capacity() * sizeof(Arc) +
         lineIds_.capacity() * sizeof(GeometryId) +
         circleIds_.capacity() * sizeof(GeometryId) +
         arcIds_.capacity() * sizeof(GeometryId) +
         dimensions_.capacity() * sizeof(Dimension) +
         constraints_.capacity() * sizeof(Constraint) +
         centerNodeElementIds_.capacity() * sizeof(std::size_t);
  const auto mapBytes = [](const auto& map) {
    return map.bucket_count() * sizeof(void*) +
           map.size() * (sizeof(typename std::decay_t<decltype(map)>::value_type) +
                         2 * sizeof(void*));
  };
  bytes += mapBytes(geometryIndex_) + mapBytes(constraintIndex_) +
           mapBytes(connectivity_) + mapBytes(geometryConstraints_);
  for (const auto& [id, adjacent] : connectivity_) {
    static_cast<void>(id);
    bytes += adjacent.capacity() * sizeof(GeometryId);
  }
  for (const auto& [id, constraints] : geometryConstraints_) {
    static_cast<void>(id);
    bytes += constraints.capacity() * sizeof(ConstraintId);
  }
  return bytes;
}

std::size_t Sketch::ownedAllocationBlocks() const noexcept {
  std::size_t blocks = 0;
  const auto vectorBlock = [&blocks](const auto& values) {
    if (values.capacity() != 0) ++blocks;
  };
  vectorBlock(lines_); vectorBlock(circles_); vectorBlock(arcs_);
  vectorBlock(lineIds_); vectorBlock(circleIds_); vectorBlock(arcIds_);
  vectorBlock(dimensions_); vectorBlock(constraints_);
  vectorBlock(centerNodeElementIds_);
  const auto mapBlocks = [&blocks](const auto& map) {
    if (map.bucket_count() != 0) ++blocks;
    blocks += map.size();
  };
  mapBlocks(geometryIndex_); mapBlocks(constraintIndex_);
  mapBlocks(connectivity_); mapBlocks(geometryConstraints_);
  for (const auto& [id, adjacent] : connectivity_) {
    static_cast<void>(id); vectorBlock(adjacent);
  }
  for (const auto& [id, constraints] : geometryConstraints_) {
    static_cast<void>(id); vectorBlock(constraints);
  }
  return blocks;
}

void Sketch::journalCaptureGeometry(GeometryId id) {
  if (deltaJournals_.empty() || id == kInvalidGeometryId) return;
  const auto already = [id](const auto& records) {
    return std::any_of(records.begin(), records.end(),
                       [id](const auto& item) { return item.id == id; });
  };
  const auto location = geometryLocation(id);
  if (!location) return;
  for (auto& journal : deltaJournals_) {
    if (already(journal.lines) || already(journal.circles) ||
        already(journal.arcs)) continue;
    switch (location->kind) {
      case GeometryKind::Line:
        journal.lines.push_back({id, location->index, lines_[location->index]});
        break;
      case GeometryKind::Circle:
        journal.circles.push_back(
            {id, location->index, circles_[location->index]});
        break;
      case GeometryKind::Arc:
        journal.arcs.push_back({id, location->index, arcs_[location->index]});
        break;
    }
  }
}

void Sketch::journalCapturePoint(PointReference reference) {
  std::vector<GeometryId> seeds;
  if (reference.lineId != kInvalidGeometryId) seeds.push_back(reference.lineId);
  if (reference.circleId != kInvalidGeometryId)
    seeds.push_back(reference.circleId);
  if (reference.arcId != kInvalidGeometryId) seeds.push_back(reference.arcId);
  if (reference.elementCenterId != 0)
    for (std::size_t index = 0; index < lines_.size(); ++index)
      if (lines_[index].elementId == reference.elementCenterId)
        seeds.push_back(lineIds_[index]);
  journalCaptureComponents(seeds);
}

void Sketch::journalCaptureConstraint(ConstraintId id) {
  if (deltaJournals_.empty() || id == kInvalidConstraintId) return;
  const auto index = constraintIndex(id);
  if (!index) return;
  for (auto& journal : deltaJournals_) {
    if (std::any_of(journal.constraints.begin(), journal.constraints.end(),
                    [id](const auto& item) { return item.id == id; }))
      continue;
    journal.constraints.push_back({id, *index, constraints_[*index]});
  }
}

void Sketch::journalCaptureComponents(const std::vector<GeometryId>& seeds) {
  if (deltaJournals_.empty()) return;
  for (const auto seed : seeds) {
    if (seed == kInvalidGeometryId) continue;
    const auto component = connectedComponent({seed});
    if (component.geometryIds.empty()) {
      journalCaptureGeometry(seed);
      continue;
    }
    for (const auto id : component.geometryIds) journalCaptureGeometry(id);
    for (const auto id : component.constraintIds) journalCaptureConstraint(id);
  }
}

void Sketch::journalCaptureDimension(std::size_t index) {
  if (deltaJournals_.empty() || index >= dimensions_.size()) return;
  for (auto& journal : deltaJournals_) {
    if (index >= journal.dimensionTokens.size()) continue;
    const std::size_t token = journal.dimensionTokens[index];
    if (std::any_of(journal.dimensions.begin(), journal.dimensions.end(),
                    [token](const auto& item) {
                      return item.token == token;
                    }))
      continue;
    const bool existedBefore = token < journal.beforeDimensionCount;
    journal.dimensions.push_back(
        {token, existedBefore ? std::optional<std::size_t>{token}
                              : std::nullopt,
         existedBefore ? std::optional<Dimension>{dimensions_[index]}
                       : std::nullopt});
  }
}

void Sketch::journalCaptureAllDimensions() {
  if (deltaJournals_.empty()) return;
  for (std::size_t index = 0; index < dimensions_.size(); ++index)
    journalCaptureDimension(index);
}

void Sketch::journalCaptureCenters() {
  for (auto& journal : deltaJournals_)
    if (!journal.centersBefore) journal.centersBefore = centerNodeElementIds_;
}

void Sketch::journalRecordAddedGeometry(GeometryId id, GeometryKind kind,
                                        std::size_t index) {
  for (auto& journal : deltaJournals_) {
    switch (kind) {
      case GeometryKind::Line:
        journal.lines.push_back({id, index, std::nullopt}); break;
      case GeometryKind::Circle:
        journal.circles.push_back({id, index, std::nullopt}); break;
      case GeometryKind::Arc:
        journal.arcs.push_back({id, index, std::nullopt}); break;
    }
  }
}

void Sketch::journalRecordAddedConstraint(ConstraintId id,
                                          std::size_t index) {
  for (auto& journal : deltaJournals_)
    journal.constraints.push_back({id, index, std::nullopt});
}

void Sketch::journalRecordAddedDimension(std::size_t index) {
  for (auto& journal : deltaJournals_) {
    const std::size_t token = journal.nextDimensionToken++;
    const std::size_t insertion = std::min(index, journal.dimensionTokens.size());
    journal.dimensionTokens.insert(
        journal.dimensionTokens.begin() + static_cast<std::ptrdiff_t>(insertion),
        token);
    journal.dimensions.push_back({token, std::nullopt, std::nullopt});
  }
}

SketchDelta Sketch::finishDeltaJournal() {
  if (deltaJournals_.empty()) return {};
  DeltaJournalState journal = std::move(deltaJournals_.back());
  deltaJournals_.pop_back();
  SketchDelta delta;
  const auto lineEqual = [](const Line& a, const Line& b) {
    return a.start.xMm == b.start.xMm && a.start.yMm == b.start.yMm &&
           a.end.xMm == b.end.xMm && a.end.yMm == b.end.yMm &&
           a.elementId == b.elementId && a.dashed == b.dashed;
  };
  const auto circleEqual = [](const Circle& a, const Circle& b) {
    return a.center.xMm == b.center.xMm && a.center.yMm == b.center.yMm &&
           a.radiusMm == b.radiusMm && a.dashed == b.dashed;
  };
  const auto arcEqual = [](const Arc& a, const Arc& b) {
    return a.center.xMm == b.center.xMm && a.center.yMm == b.center.yMm &&
           a.radiusMm == b.radiusMm && a.startAngleRad == b.startAngleRad &&
           a.sweepAngleRad == b.sweepAngleRad && a.dashed == b.dashed;
  };
  for (const auto& item : journal.lines) {
    const auto afterIndex = lineIndex(item.id);
    const auto beforeFound = std::find(journal.beforeLineIds.begin(),
                                       journal.beforeLineIds.end(), item.id);
    const auto beforeIndex = beforeFound == journal.beforeLineIds.end()
        ? std::optional<std::size_t>{}
        : std::optional<std::size_t>{static_cast<std::size_t>(
              std::distance(journal.beforeLineIds.begin(), beforeFound))};
    std::optional<Line> after;
    if (afterIndex) after = lines_[*afterIndex];
    if (!item.before && !after) continue;
    if (item.before && after && lineEqual(*item.before, *after)) continue;
    const auto oldIndex = beforeIndex.value_or(afterIndex.value_or(0));
    const auto finalIndex = afterIndex.value_or(oldIndex);
    delta.lines.push_back({oldIndex, finalIndex, item.before, after});
    delta.lineIds.push_back({oldIndex, finalIndex,
                             item.before ? std::optional<GeometryId>{item.id}
                                         : std::nullopt,
                             after ? std::optional<GeometryId>{item.id}
                                   : std::nullopt});
  }
  for (const auto& item : journal.circles) {
    const auto afterIndex = circleIndex(item.id);
    const auto beforeFound = std::find(journal.beforeCircleIds.begin(),
                                       journal.beforeCircleIds.end(), item.id);
    const auto beforeIndex = beforeFound == journal.beforeCircleIds.end()
        ? std::optional<std::size_t>{}
        : std::optional<std::size_t>{static_cast<std::size_t>(
              std::distance(journal.beforeCircleIds.begin(), beforeFound))};
    std::optional<Circle> after;
    if (afterIndex) after = circles_[*afterIndex];
    if (!item.before && !after) continue;
    if (item.before && after && circleEqual(*item.before, *after)) continue;
    const auto oldIndex = beforeIndex.value_or(afterIndex.value_or(0));
    const auto finalIndex = afterIndex.value_or(oldIndex);
    delta.circles.push_back({oldIndex, finalIndex, item.before, after});
    delta.circleIds.push_back({oldIndex, finalIndex,
                               item.before ? std::optional<GeometryId>{item.id}
                                           : std::nullopt,
                               after ? std::optional<GeometryId>{item.id}
                                     : std::nullopt});
  }
  for (const auto& item : journal.arcs) {
    const auto afterIndex = arcIndex(item.id);
    const auto beforeFound = std::find(journal.beforeArcIds.begin(),
                                       journal.beforeArcIds.end(), item.id);
    const auto beforeIndex = beforeFound == journal.beforeArcIds.end()
        ? std::optional<std::size_t>{}
        : std::optional<std::size_t>{static_cast<std::size_t>(
              std::distance(journal.beforeArcIds.begin(), beforeFound))};
    std::optional<Arc> after;
    if (afterIndex) after = arcs_[*afterIndex];
    if (!item.before && !after) continue;
    if (item.before && after && arcEqual(*item.before, *after)) continue;
    const auto oldIndex = beforeIndex.value_or(afterIndex.value_or(0));
    const auto finalIndex = afterIndex.value_or(oldIndex);
    delta.arcs.push_back({oldIndex, finalIndex, item.before, after});
    delta.arcIds.push_back({oldIndex, finalIndex,
                            item.before ? std::optional<GeometryId>{item.id}
                                        : std::nullopt,
                            after ? std::optional<GeometryId>{item.id}
                                  : std::nullopt});
  }
  for (const auto& item : journal.constraints) {
    const auto afterIndex = constraintIndex(item.id);
    const auto beforeFound = std::find(journal.beforeConstraintIds.begin(),
                                       journal.beforeConstraintIds.end(), item.id);
    const auto beforeIndex = beforeFound == journal.beforeConstraintIds.end()
        ? std::optional<std::size_t>{}
        : std::optional<std::size_t>{static_cast<std::size_t>(
              std::distance(journal.beforeConstraintIds.begin(), beforeFound))};
    std::optional<Constraint> after;
    if (afterIndex) after = constraints_[*afterIndex];
    if (!item.before && !after) continue;
    const auto pointEqual = [](const PointReference& a,
                               const PointReference& b) {
      return a.lineId == b.lineId && a.start == b.start &&
             a.circleId == b.circleId &&
             a.elementCenterId == b.elementCenterId && a.arcId == b.arcId &&
             a.origin == b.origin;
    };
    if (item.before && after && item.before->id == after->id &&
        item.before->type == after->type &&
        item.before->firstGeometry == after->firstGeometry &&
        item.before->secondGeometry == after->secondGeometry &&
        pointEqual(item.before->firstPoint, after->firstPoint) &&
        pointEqual(item.before->secondPoint, after->secondPoint) &&
        item.before->value == after->value)
      continue;
    const auto oldIndex = beforeIndex.value_or(afterIndex.value_or(0));
    delta.constraints.push_back({oldIndex,
                                 afterIndex.value_or(oldIndex),
                                 item.before, after});
  }
  for (const auto& item : journal.dimensions) {
    std::optional<Dimension> after;
    const auto afterFound = std::find(journal.dimensionTokens.begin(),
                                      journal.dimensionTokens.end(), item.token);
    const auto afterIndex = afterFound == journal.dimensionTokens.end()
        ? std::optional<std::size_t>{}
        : std::optional<std::size_t>{static_cast<std::size_t>(
              std::distance(journal.dimensionTokens.begin(), afterFound))};
    if (afterIndex && *afterIndex < dimensions_.size())
      after = dimensions_[*afterIndex];
    if (!item.before && !after) continue;
    const auto pointEqual = [](const PointReference& a,
                               const PointReference& b) {
      return a.lineId == b.lineId && a.start == b.start &&
             a.circleId == b.circleId &&
             a.elementCenterId == b.elementCenterId && a.arcId == b.arcId &&
             a.origin == b.origin;
    };
    if (item.before && after && item.before->id == after->id &&
        item.before->kind == after->kind &&
        item.before->geometryId == after->geometryId &&
        pointEqual(item.before->firstPoint, after->firstPoint) &&
        pointEqual(item.before->secondPoint, after->secondPoint) &&
        item.before->valueMm == after->valueMm &&
        item.before->offsetMm == after->offsetMm &&
        item.before->angleRad == after->angleRad)
      continue;
    const auto oldIndex = item.beforeIndex.value_or(afterIndex.value_or(0));
    delta.dimensions.push_back(
        {oldIndex, afterIndex.value_or(oldIndex), item.before, after});
  }
  if (journal.centersBefore) {
    for (std::size_t index = 0; index < journal.centersBefore->size(); ++index) {
      const auto value = (*journal.centersBefore)[index];
      const auto found = std::find(centerNodeElementIds_.begin(),
                                   centerNodeElementIds_.end(), value);
      if (found == centerNodeElementIds_.end())
        delta.centerNodeElementIds.push_back(
            {index, index, value, std::nullopt});
    }
    for (std::size_t index = 0; index < centerNodeElementIds_.size(); ++index) {
      const auto value = centerNodeElementIds_[index];
      const auto found = std::find(journal.centersBefore->begin(),
                                   journal.centersBefore->end(), value);
      if (found == journal.centersBefore->end())
        delta.centerNodeElementIds.push_back(
            {index, index, std::nullopt, value});
    }
  }
  delta.beforeNextElementId = journal.beforeNextElementId;
  delta.afterNextElementId = nextElementId_;
  delta.beforeNextGeometryId = journal.beforeNextGeometryId;
  delta.afterNextGeometryId = nextGeometryId_;
  delta.beforeNextConstraintId = journal.beforeNextConstraintId;
  delta.afterNextConstraintId = nextConstraintId_;
  delta.beforeNextDimensionId = journal.beforeNextDimensionId;
  delta.afterNextDimensionId = nextDimensionId_;
  delta.beforeSemanticFingerprint = journal.beforeSemanticFingerprint;
  delta.afterSemanticFingerprint = semanticFingerprint();
  delta.beforeInvalidConstraintIds =
      std::move(journal.beforeInvalidConstraintIds);
  delta.afterInvalidConstraintIds = invalidReferenceConstraintIds();
  const auto vectorBytes = [](const auto& values) {
    using Value = typename std::decay_t<decltype(values)>::value_type;
    return values.capacity() * sizeof(Value);
  };
  delta.retainedBytes = vectorBytes(delta.lines) + vectorBytes(delta.circles) +
      vectorBytes(delta.arcs) + vectorBytes(delta.lineIds) +
      vectorBytes(delta.circleIds) + vectorBytes(delta.arcIds) +
      vectorBytes(delta.dimensions) + vectorBytes(delta.constraints) +
      vectorBytes(delta.centerNodeElementIds) +
      vectorBytes(delta.beforeInvalidConstraintIds) +
      vectorBytes(delta.afterInvalidConstraintIds);
  return delta;
}

SketchDelta Sketch::cancelDeltaJournal() {
  if (deltaJournals_.empty()) return {};
  const auto parentDimensionTokens =
      deltaJournals_.back().parentDimensionTokensBefore;
  auto delta = finishDeltaJournal();
  if (!delta.empty() && !applyDelta(delta, false))
    throw std::logic_error("Sketch delta journal rollback failed");
  if (parentDimensionTokens.size() != deltaJournals_.size())
    throw std::logic_error("Sketch delta journal parent mismatch");
  for (std::size_t index = 0; index < deltaJournals_.size(); ++index)
    deltaJournals_[index].dimensionTokens = parentDimensionTokens[index];
  return delta;
}

bool SketchDelta::empty() const noexcept {
  return lines.empty() && circles.empty() && arcs.empty() && lineIds.empty() &&
         circleIds.empty() && arcIds.empty() && dimensions.empty() &&
         constraints.empty() && centerNodeElementIds.empty() &&
         beforeNextElementId == afterNextElementId &&
         beforeNextGeometryId == afterNextGeometryId &&
         beforeNextConstraintId == afterNextConstraintId &&
         beforeNextDimensionId == afterNextDimensionId;
}

SketchDelta Sketch::makeDelta(const Sketch& before, const Sketch& after) {
  SketchDelta delta;
  const auto pointEqual = [](PointReference a, PointReference b) {
    return a.lineId == b.lineId && a.start == b.start &&
           a.circleId == b.circleId && a.elementCenterId == b.elementCenterId &&
           a.arcId == b.arcId && a.origin == b.origin;
  };
  const auto append = [&delta]<typename T>(
      const std::vector<T>& oldValues, const std::vector<T>& newValues,
      std::vector<IndexedValueDelta<T>>& output, auto equal) {
    const std::size_t count = std::max(oldValues.size(), newValues.size());
    for (std::size_t i = 0; i < count; ++i) {
      const bool hasOld = i < oldValues.size();
      const bool hasNew = i < newValues.size();
      if (hasOld && hasNew && equal(oldValues[i], newValues[i])) continue;
      IndexedValueDelta<T> item;
      item.beforeIndex = i;
      item.afterIndex = i;
      if (hasOld) item.before = oldValues[i];
      if (hasNew) item.after = newValues[i];
      output.push_back(std::move(item));
    }
  };
  append(before.lines_, after.lines_, delta.lines, [](const Line& a, const Line& b) {
    return a.start.xMm == b.start.xMm && a.start.yMm == b.start.yMm &&
           a.end.xMm == b.end.xMm && a.end.yMm == b.end.yMm &&
           a.elementId == b.elementId && a.dashed == b.dashed;
  });
  append(before.circles_, after.circles_, delta.circles,
         [](const Circle& a, const Circle& b) {
           return a.center.xMm == b.center.xMm && a.center.yMm == b.center.yMm &&
                  a.radiusMm == b.radiusMm && a.dashed == b.dashed;
         });
  append(before.arcs_, after.arcs_, delta.arcs, [](const Arc& a, const Arc& b) {
    return a.center.xMm == b.center.xMm && a.center.yMm == b.center.yMm &&
           a.radiusMm == b.radiusMm && a.startAngleRad == b.startAngleRad &&
           a.sweepAngleRad == b.sweepAngleRad && a.dashed == b.dashed;
  });
  const auto sameId = [](auto a, auto b) { return a == b; };
  append(before.lineIds_, after.lineIds_, delta.lineIds, sameId);
  append(before.circleIds_, after.circleIds_, delta.circleIds, sameId);
  append(before.arcIds_, after.arcIds_, delta.arcIds, sameId);
  append(before.centerNodeElementIds_, after.centerNodeElementIds_,
         delta.centerNodeElementIds, sameId);
  append(before.dimensions_, after.dimensions_, delta.dimensions,
         [&pointEqual](const Dimension& a, const Dimension& b) {
            return a.id == b.id && a.kind == b.kind &&
                   a.geometryId == b.geometryId &&
                  pointEqual(a.firstPoint, b.firstPoint) &&
                  pointEqual(a.secondPoint, b.secondPoint) &&
                  a.valueMm == b.valueMm && a.offsetMm == b.offsetMm &&
                  a.angleRad == b.angleRad;
         });
  append(before.constraints_, after.constraints_, delta.constraints,
         [&pointEqual](const Constraint& a, const Constraint& b) {
           return a.id == b.id && a.type == b.type &&
                  a.firstGeometry == b.firstGeometry &&
                  a.secondGeometry == b.secondGeometry &&
                  pointEqual(a.firstPoint, b.firstPoint) &&
                  pointEqual(a.secondPoint, b.secondPoint) && a.value == b.value;
         });
  delta.beforeNextElementId = before.nextElementId_;
  delta.afterNextElementId = after.nextElementId_;
  delta.beforeNextGeometryId = before.nextGeometryId_;
  delta.afterNextGeometryId = after.nextGeometryId_;
  delta.beforeNextConstraintId = before.nextConstraintId_;
  delta.afterNextConstraintId = after.nextConstraintId_;
  delta.beforeNextDimensionId = before.nextDimensionId_;
  delta.afterNextDimensionId = after.nextDimensionId_;
  delta.beforeSemanticFingerprint = before.semanticFingerprint();
  delta.afterSemanticFingerprint = after.semanticFingerprint();
  delta.beforeInvalidConstraintIds = before.invalidReferenceConstraintIds();
  delta.afterInvalidConstraintIds = after.invalidReferenceConstraintIds();
  const auto vectorBytes = [](const auto& values) {
    using Value = typename std::decay_t<decltype(values)>::value_type;
    return values.capacity() * sizeof(Value);
  };
  delta.retainedBytes = vectorBytes(delta.lines) +
                        vectorBytes(delta.circles) +
                        vectorBytes(delta.arcs) +
                        vectorBytes(delta.lineIds) +
                        vectorBytes(delta.circleIds) +
                        vectorBytes(delta.arcIds) +
                        vectorBytes(delta.dimensions) +
                        vectorBytes(delta.constraints) +
                        vectorBytes(delta.centerNodeElementIds) +
                        vectorBytes(delta.beforeInvalidConstraintIds) +
                        vectorBytes(delta.afterInvalidConstraintIds);
  return delta;
}

bool Sketch::applyDelta(const SketchDelta& delta, bool forward) {
  // Build every affected container off to the side.  Validation and all
  // allocations finish before the first swap, which gives Undo/Redo a strong
  // transaction guarantee without retaining a full Sketch checkpoint.
  const auto expectedSourceFingerprint =
      forward ? delta.beforeSemanticFingerprint
              : delta.afterSemanticFingerprint;
  const auto sourceNextElement =
      forward ? delta.beforeNextElementId : delta.afterNextElementId;
  const auto sourceNextGeometry =
      forward ? delta.beforeNextGeometryId : delta.afterNextGeometryId;
  const auto sourceNextConstraint =
      forward ? delta.beforeNextConstraintId : delta.afterNextConstraintId;
  const auto sourceNextDimension =
      forward ? delta.beforeNextDimensionId : delta.afterNextDimensionId;
  if (nextElementId_ != sourceNextElement ||
      nextGeometryId_ != sourceNextGeometry ||
      nextConstraintId_ != sourceNextConstraint ||
      nextDimensionId_ != sourceNextDimension ||
      semanticFingerprint() != expectedSourceFingerprint)
    return false;

  const auto plan = [forward]<typename T>(
      const std::vector<T>& current,
      const std::vector<IndexedValueDelta<T>>& changes, auto equal,
      std::optional<std::vector<T>>& result) {
    if (changes.empty()) return true;

    std::vector<std::size_t> sourceIndices;
    std::vector<std::size_t> targetIndices;
    std::size_t removalCount = 0;
    std::size_t insertionCount = 0;
    sourceIndices.reserve(changes.size());
    targetIndices.reserve(changes.size());
    for (const auto& item : changes) {
      const auto& source = forward ? item.before : item.after;
      const auto& desired = forward ? item.after : item.before;
      const std::size_t sourceIndex =
          forward ? item.beforeIndex : item.afterIndex;
      const std::size_t targetIndex =
          forward ? item.afterIndex : item.beforeIndex;
      if (source) {
        if (sourceIndex >= current.size() ||
            !equal(current[sourceIndex], *source))
          return false;
        sourceIndices.push_back(sourceIndex);
      }
      if (desired) targetIndices.push_back(targetIndex);
      if (source && !desired) ++removalCount;
      if (!source && desired) ++insertionCount;
      if (!source && !desired) return false;
    }
    std::sort(sourceIndices.begin(), sourceIndices.end());
    if (std::adjacent_find(sourceIndices.begin(), sourceIndices.end()) !=
        sourceIndices.end())
      return false;
    std::sort(targetIndices.begin(), targetIndices.end());
    if (std::adjacent_find(targetIndices.begin(), targetIndices.end()) !=
        targetIndices.end())
      return false;
    if (removalCount > current.size()) return false;
    const std::size_t finalSize =
        current.size() - removalCount + insertionCount;
    if (!targetIndices.empty() && targetIndices.back() >= finalSize)
      return false;

    std::vector<T> planned = current;
    // Replacements address the untouched source layout.
    for (const auto& item : changes) {
      const auto& source = forward ? item.before : item.after;
      const auto& desired = forward ? item.after : item.before;
      if (!source || !desired) continue;
      const std::size_t sourceIndex =
          forward ? item.beforeIndex : item.afterIndex;
      planned[sourceIndex] = *desired;
    }
    // Structural edits are deliberately independent of delta record order.
    std::vector<std::size_t> removals;
    for (const auto& item : changes) {
      const auto& source = forward ? item.before : item.after;
      const auto& desired = forward ? item.after : item.before;
      if (source && !desired)
        removals.push_back(forward ? item.beforeIndex : item.afterIndex);
    }
    std::sort(removals.rbegin(), removals.rend());
    for (const auto index : removals)
      planned.erase(planned.begin() + static_cast<std::ptrdiff_t>(index));

    std::vector<const IndexedValueDelta<T>*> insertions;
    for (const auto& item : changes) {
      const auto& source = forward ? item.before : item.after;
      const auto& desired = forward ? item.after : item.before;
      if (!source && desired) insertions.push_back(&item);
    }
    std::sort(insertions.begin(), insertions.end(), [forward](const auto* a,
                                                              const auto* b) {
      return (forward ? a->afterIndex : a->beforeIndex) <
             (forward ? b->afterIndex : b->beforeIndex);
    });
    for (const auto* item : insertions) {
      const std::size_t targetIndex =
          forward ? item->afterIndex : item->beforeIndex;
      const auto& desired = forward ? item->after : item->before;
      if (!desired || targetIndex > planned.size()) return false;
      planned.insert(planned.begin() + static_cast<std::ptrdiff_t>(targetIndex),
                     *desired);
    }
    if (planned.size() != finalSize) return false;
    // Also prove the requested final layout.  This catches stale deltas where
    // an earlier insertion/removal shifted a changed entity unexpectedly.
    for (const auto& item : changes) {
      const auto& desired = forward ? item.after : item.before;
      if (!desired) continue;
      const std::size_t targetIndex =
          forward ? item.afterIndex : item.beforeIndex;
      if (targetIndex >= planned.size() ||
          !equal(planned[targetIndex], *desired))
        return false;
    }
    result.emplace(std::move(planned));
    return true;
  };

  const auto pointEqual = [](const PointReference& a,
                             const PointReference& b) {
    return a.lineId == b.lineId && a.start == b.start &&
           a.circleId == b.circleId &&
           a.elementCenterId == b.elementCenterId && a.arcId == b.arcId &&
           a.origin == b.origin;
  };
  const auto lineEqual = [](const Line& a, const Line& b) {
    return a.start.xMm == b.start.xMm && a.start.yMm == b.start.yMm &&
           a.end.xMm == b.end.xMm && a.end.yMm == b.end.yMm &&
           a.elementId == b.elementId && a.dashed == b.dashed;
  };
  const auto circleEqual = [](const Circle& a, const Circle& b) {
    return a.center.xMm == b.center.xMm && a.center.yMm == b.center.yMm &&
           a.radiusMm == b.radiusMm && a.dashed == b.dashed;
  };
  const auto arcEqual = [](const Arc& a, const Arc& b) {
    return a.center.xMm == b.center.xMm && a.center.yMm == b.center.yMm &&
           a.radiusMm == b.radiusMm && a.startAngleRad == b.startAngleRad &&
           a.sweepAngleRad == b.sweepAngleRad && a.dashed == b.dashed;
  };
  const auto dimensionEqual = [&pointEqual](const Dimension& a,
                                             const Dimension& b) {
    return a.id == b.id && a.kind == b.kind &&
           a.geometryId == b.geometryId &&
           pointEqual(a.firstPoint, b.firstPoint) &&
           pointEqual(a.secondPoint, b.secondPoint) &&
           a.valueMm == b.valueMm && a.offsetMm == b.offsetMm &&
           a.angleRad == b.angleRad;
  };
  const auto constraintEqual = [&pointEqual](const Constraint& a,
                                              const Constraint& b) {
    return a.id == b.id && a.type == b.type &&
           a.firstGeometry == b.firstGeometry &&
           a.secondGeometry == b.secondGeometry &&
           pointEqual(a.firstPoint, b.firstPoint) &&
           pointEqual(a.secondPoint, b.secondPoint) && a.value == b.value;
  };
  const auto scalarEqual = [](const auto& a, const auto& b) { return a == b; };

  std::optional<std::vector<Line>> lines;
  std::optional<std::vector<Circle>> circles;
  std::optional<std::vector<Arc>> arcs;
  std::optional<std::vector<GeometryId>> lineIds;
  std::optional<std::vector<GeometryId>> circleIds;
  std::optional<std::vector<GeometryId>> arcIds;
  std::optional<std::vector<Dimension>> dimensions;
  std::optional<std::vector<Constraint>> constraints;
  std::optional<std::vector<std::size_t>> centers;
  try {
    if (!plan(lines_, delta.lines, lineEqual, lines) ||
        !plan(circles_, delta.circles, circleEqual, circles) ||
        !plan(arcs_, delta.arcs, arcEqual, arcs) ||
        !plan(lineIds_, delta.lineIds, scalarEqual, lineIds) ||
        !plan(circleIds_, delta.circleIds, scalarEqual, circleIds) ||
        !plan(arcIds_, delta.arcIds, scalarEqual, arcIds) ||
        !plan(dimensions_, delta.dimensions, dimensionEqual, dimensions) ||
        !plan(constraints_, delta.constraints, constraintEqual, constraints) ||
        !plan(centerNodeElementIds_, delta.centerNodeElementIds, scalarEqual,
              centers))
      return false;

    const auto& finalLines = lines ? *lines : lines_;
    const auto& finalCircles = circles ? *circles : circles_;
    const auto& finalArcs = arcs ? *arcs : arcs_;
    const auto& finalLineIds = lineIds ? *lineIds : lineIds_;
    const auto& finalCircleIds = circleIds ? *circleIds : circleIds_;
    const auto& finalArcIds = arcIds ? *arcIds : arcIds_;
    const auto& finalDimensions = dimensions ? *dimensions : dimensions_;
    const auto& finalConstraints = constraints ? *constraints : constraints_;
    const auto& finalCenters = centers ? *centers : centerNodeElementIds_;
    if (finalLines.size() != finalLineIds.size() ||
        finalCircles.size() != finalCircleIds.size() ||
        finalArcs.size() != finalArcIds.size())
      return false;

    std::unordered_map<GeometryId, GeometryKind> geometry;
    geometry.reserve(finalLineIds.size() + finalCircleIds.size() +
                     finalArcIds.size());
    const auto addIds = [&geometry](const auto& ids, GeometryKind kind) {
      for (const auto id : ids)
        if (id == kInvalidGeometryId || !geometry.emplace(id, kind).second)
          return false;
      return true;
    };
    if (!addIds(finalLineIds, GeometryKind::Line) ||
        !addIds(finalCircleIds, GeometryKind::Circle) ||
        !addIds(finalArcIds, GeometryKind::Arc))
      return false;
    std::vector<std::size_t> elementIds;
    elementIds.reserve(finalLines.size());
    for (const auto& line : finalLines) {
      if (line.elementId == 0 || !std::isfinite(line.start.xMm) ||
          !std::isfinite(line.start.yMm) ||
          !std::isfinite(line.end.xMm) || !std::isfinite(line.end.yMm) ||
          (line.start.xMm == line.end.xMm &&
           line.start.yMm == line.end.yMm))
        return false;
      elementIds.push_back(line.elementId);
    }
    for (const auto& circle : finalCircles)
      if (!std::isfinite(circle.center.xMm) ||
          !std::isfinite(circle.center.yMm) ||
          !std::isfinite(circle.radiusMm) || circle.radiusMm <= 0.0)
        return false;
    constexpr double kTwoPi = 6.28318530717958647692;
    for (const auto& arc : finalArcs)
      if (!std::isfinite(arc.center.xMm) ||
          !std::isfinite(arc.center.yMm) ||
          !std::isfinite(arc.radiusMm) ||
          !std::isfinite(arc.startAngleRad) ||
          !std::isfinite(arc.sweepAngleRad) || arc.radiusMm <= 0.0 ||
          arc.sweepAngleRad <= 1e-9 ||
          arc.sweepAngleRad >= kTwoPi - 1e-9)
        return false;
    const auto hasElement = [&elementIds](std::size_t id) {
      return id != 0 &&
             std::find(elementIds.begin(), elementIds.end(), id) !=
                 elementIds.end();
    };
    std::vector<std::size_t> uniqueCenters = finalCenters;
    std::sort(uniqueCenters.begin(), uniqueCenters.end());
    if (std::adjacent_find(uniqueCenters.begin(), uniqueCenters.end()) !=
        uniqueCenters.end())
      return false;
    for (const auto id : finalCenters) {
      if (!hasElement(id) ||
          std::count(elementIds.begin(), elementIds.end(), id) != 4)
        return false;
    }
    const auto validPoint = [&geometry, &hasElement,
                             &finalCenters](const PointReference& p) {
      const unsigned sourceCount = static_cast<unsigned>(p.origin) +
          static_cast<unsigned>(p.lineId != kInvalidGeometryId) +
          static_cast<unsigned>(p.circleId != kInvalidGeometryId) +
          static_cast<unsigned>(p.arcId != kInvalidGeometryId) +
          static_cast<unsigned>(p.elementCenterId != 0);
      if (sourceCount > 1) return false;
      if (p.lineId != kInvalidGeometryId) {
        const auto found = geometry.find(p.lineId);
        if (found == geometry.end() || found->second != GeometryKind::Line)
          return false;
      }
      if (p.circleId != kInvalidGeometryId) {
        const auto found = geometry.find(p.circleId);
        if (found == geometry.end() || found->second != GeometryKind::Circle)
          return false;
      }
      if (p.arcId != kInvalidGeometryId) {
        const auto found = geometry.find(p.arcId);
        if (found == geometry.end() || found->second != GeometryKind::Arc)
          return false;
      }
      return p.elementCenterId == 0 ||
             (hasElement(p.elementCenterId) &&
              std::find(finalCenters.begin(), finalCenters.end(),
                        p.elementCenterId) != finalCenters.end());
    };
    const auto validConstraintRefs = [](const Constraint& item,
                                        const auto& geometryMap,
                                        const auto& pointValidator) {
      return (item.firstGeometry == kInvalidGeometryId ||
              geometryMap.contains(item.firstGeometry)) &&
             (item.secondGeometry == kInvalidGeometryId ||
              geometryMap.contains(item.secondGeometry)) &&
             pointValidator(item.firstPoint) &&
             pointValidator(item.secondPoint);
    };
    auto allowedInvalid = forward ? delta.afterInvalidConstraintIds
                                  : delta.beforeInvalidConstraintIds;
    std::sort(allowedInvalid.begin(), allowedInvalid.end());
    if (std::adjacent_find(allowedInvalid.begin(), allowedInvalid.end()) !=
        allowedInvalid.end())
      return false;
    std::vector<ConstraintId> constraintIds;
    constraintIds.reserve(finalConstraints.size());
    for (const auto& item : finalConstraints) {
      if (item.id == kInvalidConstraintId ||
          static_cast<unsigned>(item.type) >
              static_cast<unsigned>(ConstraintType::PointOnYAxis) ||
          !std::isfinite(item.value))
        return false;
      if (!validConstraintRefs(item, geometry, validPoint)) {
        // Historical orphan constraints remain representable for full-solve
        // diagnostics, but only on the delta side where the journal recorded
        // them.  This permits delete/cancel/undo without allowing a crafted
        // delta to introduce a new orphan.
        if (!std::binary_search(allowedInvalid.begin(), allowedInvalid.end(),
                                item.id))
          return false;
      }
      constraintIds.push_back(item.id);
    }
    std::sort(constraintIds.begin(), constraintIds.end());
    if (std::adjacent_find(constraintIds.begin(), constraintIds.end()) !=
        constraintIds.end())
      return false;
    for (const auto id : allowedInvalid) {
      const auto found = std::find_if(
          finalConstraints.begin(), finalConstraints.end(),
          [id](const Constraint& item) { return item.id == id; });
      if (found == finalConstraints.end() ||
          validConstraintRefs(*found, geometry, validPoint))
        return false;
    }
    std::vector<DimensionId> dimensionIds;
    dimensionIds.reserve(finalDimensions.size());
    for (const auto& item : finalDimensions) {
      if (static_cast<unsigned>(item.kind) >
              static_cast<unsigned>(DimensionKind::LineDistance) ||
          item.id == kInvalidDimensionId ||
          !std::isfinite(item.valueMm) || !std::isfinite(item.offsetMm) ||
          !std::isfinite(item.angleRad) || item.valueMm <= 0.0 ||
          (item.geometryId != kInvalidGeometryId &&
           !geometry.contains(item.geometryId)) ||
          !validPoint(item.firstPoint) || !validPoint(item.secondPoint))
        return false;
      dimensionIds.push_back(item.id);
    }
    std::sort(dimensionIds.begin(), dimensionIds.end());
    if (std::adjacent_find(dimensionIds.begin(), dimensionIds.end()) !=
        dimensionIds.end())
      return false;

    const auto finalNextElement =
        forward ? delta.afterNextElementId : delta.beforeNextElementId;
    const auto finalNextGeometry =
        forward ? delta.afterNextGeometryId : delta.beforeNextGeometryId;
    const auto finalNextConstraint =
        forward ? delta.afterNextConstraintId : delta.beforeNextConstraintId;
    const auto finalNextDimension =
        forward ? delta.afterNextDimensionId : delta.beforeNextDimensionId;
    const auto maxElement = elementIds.empty()
        ? std::size_t{0}
        : *std::max_element(elementIds.begin(), elementIds.end());
    const auto maxGeometry = geometry.empty()
        ? GeometryId{0}
        : std::max({finalLineIds.empty() ? GeometryId{0}
                                        : *std::max_element(finalLineIds.begin(),
                                                            finalLineIds.end()),
                    finalCircleIds.empty() ? GeometryId{0}
                                          : *std::max_element(finalCircleIds.begin(),
                                                              finalCircleIds.end()),
                    finalArcIds.empty() ? GeometryId{0}
                                       : *std::max_element(finalArcIds.begin(),
                                                           finalArcIds.end())});
    const auto maxConstraint = constraintIds.empty()
        ? ConstraintId{0}
        : constraintIds.back();
    if (finalNextElement == 0 || finalNextElement <= maxElement ||
        finalNextGeometry == kInvalidGeometryId ||
        finalNextGeometry <= maxGeometry ||
        finalNextConstraint == kInvalidConstraintId ||
        finalNextConstraint <= maxConstraint ||
        finalNextDimension == kInvalidDimensionId ||
        std::binary_search(dimensionIds.begin(), dimensionIds.end(),
                           finalNextDimension))
      return false;

    const auto projectedFingerprint = [&] {
      std::uint64_t hash = 1469598103934665603ULL;
      const auto mix = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
      };
      const auto mixDouble = [&mix](double value) {
        if (value == 0.0) value = 0.0;
        mix(std::bit_cast<std::uint64_t>(value));
      };
      const auto mixPoint = [&mix, &mixDouble](const PointReference& point) {
        mix(point.lineId); mix(point.start ? 1U : 0U); mix(point.circleId);
        mix(point.elementCenterId); mix(point.arcId);
        mix(point.origin ? 1U : 0U);
      };
      mix(finalLines.size());
      for (std::size_t index = 0; index < finalLines.size(); ++index) {
        const auto& line = finalLines[index];
        mix(finalLineIds[index]);
        mixDouble(line.start.xMm); mixDouble(line.start.yMm);
        mixDouble(line.end.xMm); mixDouble(line.end.yMm);
        mix(line.elementId); mix(line.dashed ? 1U : 0U);
      }
      mix(finalCircles.size());
      for (std::size_t index = 0; index < finalCircles.size(); ++index) {
        const auto& circle = finalCircles[index];
        mix(finalCircleIds[index]);
        mixDouble(circle.center.xMm); mixDouble(circle.center.yMm);
        mixDouble(circle.radiusMm); mix(circle.dashed ? 1U : 0U);
      }
      mix(finalArcs.size());
      for (std::size_t index = 0; index < finalArcs.size(); ++index) {
        const auto& arc = finalArcs[index];
        mix(finalArcIds[index]);
        mixDouble(arc.center.xMm); mixDouble(arc.center.yMm);
        mixDouble(arc.radiusMm); mixDouble(arc.startAngleRad);
        mixDouble(arc.sweepAngleRad); mix(arc.dashed ? 1U : 0U);
      }
      mix(finalCenters.size());
      for (const auto id : finalCenters) mix(id);
      mix(finalDimensions.size());
      for (const auto& dimension : finalDimensions) {
        mix(dimension.id);
        mix(static_cast<std::uint64_t>(dimension.kind));
        mix(dimension.geometryId); mixPoint(dimension.firstPoint);
        mixPoint(dimension.secondPoint); mixDouble(dimension.valueMm);
        mixDouble(dimension.offsetMm); mixDouble(dimension.angleRad);
      }
      mix(finalConstraints.size());
      for (const auto& constraint : finalConstraints) {
        mix(constraint.id); mix(static_cast<std::uint64_t>(constraint.type));
        mix(constraint.firstGeometry); mix(constraint.secondGeometry);
        mixPoint(constraint.firstPoint); mixPoint(constraint.secondPoint);
        mixDouble(constraint.value);
      }
      mix(finalNextElement); mix(finalNextGeometry); mix(finalNextConstraint);
      mix(finalNextDimension);
      return hash;
    };
    const auto expectedTargetFingerprint =
        forward ? delta.afterSemanticFingerprint
                : delta.beforeSemanticFingerprint;
    if (projectedFingerprint() != expectedTargetFingerprint) return false;

    if (lines) lines_.swap(*lines);
    if (circles) circles_.swap(*circles);
    if (arcs) arcs_.swap(*arcs);
    if (lineIds) lineIds_.swap(*lineIds);
    if (circleIds) circleIds_.swap(*circleIds);
    if (arcIds) arcIds_.swap(*arcIds);
    if (dimensions) dimensions_.swap(*dimensions);
    if (constraints) constraints_.swap(*constraints);
    if (centers) centerNodeElementIds_.swap(*centers);
    nextElementId_ = finalNextElement;
    nextGeometryId_ = finalNextGeometry;
    nextConstraintId_ = finalNextConstraint;
    nextDimensionId_ = finalNextDimension;
  } catch (...) {
    return false;
  }
  invalidateStructureIndexes();
  updateBounds();
  return true;
}

bool Sketch::semanticallyEqual(const Sketch& other) const noexcept {
  const auto pointEqual = [](const PointReference& a,
                             const PointReference& b) {
    return a.lineId == b.lineId && a.start == b.start &&
           a.circleId == b.circleId &&
           a.elementCenterId == b.elementCenterId && a.arcId == b.arcId &&
           a.origin == b.origin;
  };
  const auto lineEqual = [](const Line& a, const Line& b) {
    return a.start.xMm == b.start.xMm && a.start.yMm == b.start.yMm &&
           a.end.xMm == b.end.xMm && a.end.yMm == b.end.yMm &&
           a.elementId == b.elementId && a.dashed == b.dashed;
  };
  const auto circleEqual = [](const Circle& a, const Circle& b) {
    return a.center.xMm == b.center.xMm && a.center.yMm == b.center.yMm &&
           a.radiusMm == b.radiusMm && a.dashed == b.dashed;
  };
  const auto arcEqual = [](const Arc& a, const Arc& b) {
    return a.center.xMm == b.center.xMm && a.center.yMm == b.center.yMm &&
           a.radiusMm == b.radiusMm && a.startAngleRad == b.startAngleRad &&
           a.sweepAngleRad == b.sweepAngleRad && a.dashed == b.dashed;
  };
  const auto dimensionEqual = [&pointEqual](const Dimension& a,
                                             const Dimension& b) {
    return a.id == b.id && a.kind == b.kind &&
           a.geometryId == b.geometryId &&
           pointEqual(a.firstPoint, b.firstPoint) &&
           pointEqual(a.secondPoint, b.secondPoint) &&
           a.valueMm == b.valueMm && a.offsetMm == b.offsetMm &&
           a.angleRad == b.angleRad;
  };
  const auto constraintEqual = [&pointEqual](const Constraint& a,
                                              const Constraint& b) {
    return a.id == b.id && a.type == b.type &&
           a.firstGeometry == b.firstGeometry &&
           a.secondGeometry == b.secondGeometry &&
           pointEqual(a.firstPoint, b.firstPoint) &&
           pointEqual(a.secondPoint, b.secondPoint) && a.value == b.value;
  };
  return lines_.size() == other.lines_.size() &&
         std::equal(lines_.begin(), lines_.end(), other.lines_.begin(),
                    lineEqual) &&
         circles_.size() == other.circles_.size() &&
         std::equal(circles_.begin(), circles_.end(), other.circles_.begin(),
                    circleEqual) &&
         arcs_.size() == other.arcs_.size() &&
         std::equal(arcs_.begin(), arcs_.end(), other.arcs_.begin(),
                    arcEqual) &&
         lineIds_ == other.lineIds_ && circleIds_ == other.circleIds_ &&
         arcIds_ == other.arcIds_ &&
         dimensions_.size() == other.dimensions_.size() &&
         std::equal(dimensions_.begin(), dimensions_.end(),
                    other.dimensions_.begin(), dimensionEqual) &&
         constraints_.size() == other.constraints_.size() &&
         std::equal(constraints_.begin(), constraints_.end(),
                    other.constraints_.begin(), constraintEqual) &&
         centerNodeElementIds_ == other.centerNodeElementIds_ &&
         nextElementId_ == other.nextElementId_ &&
         nextGeometryId_ == other.nextGeometryId_ &&
         nextConstraintId_ == other.nextConstraintId_ &&
         nextDimensionId_ == other.nextDimensionId_;
}

std::uint64_t Sketch::semanticFingerprint() const noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&hash](std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  const auto mixDouble = [&mix](double value) {
    if (value == 0.0) value = 0.0;
    mix(std::bit_cast<std::uint64_t>(value));
  };
  const auto mixPoint = [&mix, &mixDouble](const PointReference& point) {
    mix(point.lineId);
    mix(point.start ? 1U : 0U);
    mix(point.circleId);
    mix(point.elementCenterId);
    mix(point.arcId);
    mix(point.origin ? 1U : 0U);
  };
  mix(lines_.size());
  for (std::size_t index = 0; index < lines_.size(); ++index) {
    mix(lineIds_[index]);
    const auto& line = lines_[index];
    mixDouble(line.start.xMm);
    mixDouble(line.start.yMm);
    mixDouble(line.end.xMm);
    mixDouble(line.end.yMm);
    mix(line.elementId);
    mix(line.dashed ? 1U : 0U);
  }
  mix(circles_.size());
  for (std::size_t index = 0; index < circles_.size(); ++index) {
    mix(circleIds_[index]);
    const auto& circle = circles_[index];
    mixDouble(circle.center.xMm);
    mixDouble(circle.center.yMm);
    mixDouble(circle.radiusMm);
    mix(circle.dashed ? 1U : 0U);
  }
  mix(arcs_.size());
  for (std::size_t index = 0; index < arcs_.size(); ++index) {
    mix(arcIds_[index]);
    const auto& arc = arcs_[index];
    mixDouble(arc.center.xMm);
    mixDouble(arc.center.yMm);
    mixDouble(arc.radiusMm);
    mixDouble(arc.startAngleRad);
    mixDouble(arc.sweepAngleRad);
    mix(arc.dashed ? 1U : 0U);
  }
  mix(centerNodeElementIds_.size());
  for (const auto id : centerNodeElementIds_) mix(id);
  mix(dimensions_.size());
  for (const auto& dimension : dimensions_) {
    mix(dimension.id);
    mix(static_cast<std::uint64_t>(dimension.kind));
    mix(dimension.geometryId);
    mixPoint(dimension.firstPoint);
    mixPoint(dimension.secondPoint);
    mixDouble(dimension.valueMm);
    mixDouble(dimension.offsetMm);
    mixDouble(dimension.angleRad);
  }
  mix(constraints_.size());
  for (const auto& constraint : constraints_) {
    mix(constraint.id);
    mix(static_cast<std::uint64_t>(constraint.type));
    mix(constraint.firstGeometry);
    mix(constraint.secondGeometry);
    mixPoint(constraint.firstPoint);
    mixPoint(constraint.secondPoint);
    mixDouble(constraint.value);
  }
  mix(nextElementId_);
  mix(nextGeometryId_);
  mix(nextConstraintId_);
  mix(nextDimensionId_);
  return hash;
}

std::uint64_t Sketch::solverFingerprint() const noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&hash](std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  for (const auto id : lineIds_) mix(id);
  for (const auto& line : lines_) {
    mix(std::bit_cast<std::uint64_t>(line.start.xMm));
    mix(std::bit_cast<std::uint64_t>(line.start.yMm));
    mix(std::bit_cast<std::uint64_t>(line.end.xMm));
    mix(std::bit_cast<std::uint64_t>(line.end.yMm));
    mix(line.elementId);
  }
  for (std::size_t i = 0; i < circles_.size(); ++i) {
    mix(circleIds_[i]);
    mix(std::bit_cast<std::uint64_t>(circles_[i].center.xMm));
    mix(std::bit_cast<std::uint64_t>(circles_[i].center.yMm));
    mix(std::bit_cast<std::uint64_t>(circles_[i].radiusMm));
  }
  for (std::size_t i = 0; i < arcs_.size(); ++i) {
    mix(arcIds_[i]);
    mix(std::bit_cast<std::uint64_t>(arcs_[i].center.xMm));
    mix(std::bit_cast<std::uint64_t>(arcs_[i].center.yMm));
    mix(std::bit_cast<std::uint64_t>(arcs_[i].radiusMm));
    mix(std::bit_cast<std::uint64_t>(arcs_[i].startAngleRad));
    mix(std::bit_cast<std::uint64_t>(arcs_[i].sweepAngleRad));
  }
  for (const auto elementId : centerNodeElementIds_) mix(elementId);
  const auto mixPoint = [&mix](const PointReference& point) {
    mix(point.lineId);
    mix(point.start ? 1U : 0U);
    mix(point.circleId);
    mix(point.elementCenterId);
    mix(point.arcId);
    mix(point.origin ? 1U : 0U);
  };
  for (const auto& constraint : constraints_) {
    mix(constraint.id);
    mix(static_cast<std::uint64_t>(constraint.type));
    mix(constraint.firstGeometry);
    mix(constraint.secondGeometry);
    mixPoint(constraint.firstPoint);
    mixPoint(constraint.secondPoint);
    mix(std::bit_cast<std::uint64_t>(constraint.value));
  }
  return hash;
}

bool Sketch::isGeometryLocked(GeometryId id) const noexcept {
  if (id == kInvalidGeometryId) return false;

  if (const auto line = lineIndex(id)) {
    const std::size_t elementId = lines_[*line].elementId;
    for (const auto& constraint : constraints_) {
      if (constraint.type != ConstraintType::Lock ||
          constraint.firstGeometry == kInvalidGeometryId)
        continue;

      const auto lockedLine = lineIndex(constraint.firstGeometry);
      if (lockedLine &&
          lines_[*lockedLine].elementId == elementId)
        return true;
    }
    return false;
  }

  if (circleIndex(id) || arcIndex(id)) {
    return std::any_of(
        constraints_.begin(), constraints_.end(),
        [id](const Constraint& constraint) {
          return constraint.type == ConstraintType::Lock &&
                 constraint.firstGeometry == id;
        });
  }

  return false;
}

bool Sketch::isElementLocked(std::size_t elementId) const noexcept {
  for (std::size_t index = 0; index < lines_.size(); ++index) {
    if (lines_[index].elementId == elementId &&
        isGeometryLocked(lineIds_[index]))
      return true;
  }
  return false;
}

bool Sketch::isPointReferenceLocked(
    PointReference reference) const noexcept {
  if (reference.origin) return true;
  if (reference.elementCenterId != 0)
    return isElementLocked(reference.elementCenterId);
  if (reference.circleId != kInvalidGeometryId)
    return isGeometryLocked(reference.circleId);
  if (reference.arcId != kInvalidGeometryId)
    return isGeometryLocked(reference.arcId);
  return isGeometryLocked(reference.lineId);
}

void Sketch::restoreLockedGeometryFrom(const Sketch& baseline) {
  for (const auto& constraint : constraints_) {
    if (constraint.type != ConstraintType::Lock ||
        constraint.firstGeometry == kInvalidGeometryId)
      continue;

    if (const auto lockedLine = lineIndex(constraint.firstGeometry)) {
      const std::size_t elementId = lines_[*lockedLine].elementId;

      for (std::size_t index = 0; index < lines_.size(); ++index) {
        if (lines_[index].elementId != elementId) continue;

        const GeometryId id = lineIds_[index];
        const auto source = baseline.lineIndex(id);
        if (!source) continue;

        lines_[index].start = baseline.lines_[*source].start;
        lines_[index].end = baseline.lines_[*source].end;
      }
      continue;
    }

    if (const auto lockedCircle =
            circleIndex(constraint.firstGeometry)) {
      const auto source =
          baseline.circleIndex(constraint.firstGeometry);
      if (!source) continue;

      circles_[*lockedCircle].center =
          baseline.circles_[*source].center;
      circles_[*lockedCircle].radiusMm =
          baseline.circles_[*source].radiusMm;
      continue;
    }

    if (const auto lockedArc = arcIndex(constraint.firstGeometry)) {
      const auto source = baseline.arcIndex(constraint.firstGeometry);
      if (!source) continue;

      arcs_[*lockedArc] = baseline.arcs_[*source];
    }
  }

  updateBounds();
}

std::optional<Point> Sketch::referencedPoint(
    PointReference reference) const noexcept {
  if (reference.origin) return Point{0.0, 0.0};
  if (reference.elementCenterId != 0)
    return elementCenterPoint(reference.elementCenterId);

  if (reference.circleId != kInvalidGeometryId) {
    const auto index = circleIndex(reference.circleId);
    if (!index) return std::nullopt;
    return circles_[*index].center;
  }

  if (reference.arcId != kInvalidGeometryId) {
    const auto index = arcIndex(reference.arcId);
    if (!index) return std::nullopt;
    return reference.start ? arcStartPoint(arcs_[*index])
                           : arcEndPoint(arcs_[*index]);
  }

  const auto index = lineIndex(reference.lineId);
  if (!index) return std::nullopt;
  const auto& line = lines_[*index];
  return reference.start ? line.start : line.end;
}
bool Sketch::setPointsCoincident(PointReference firstReference,
                                 PointReference secondReference) {
  journalCapturePoint(firstReference);
  journalCapturePoint(secondReference);
  const auto first = referencedPoint(firstReference);
  const auto second = referencedPoint(secondReference);
  if (!first || !second) return false;
  // LOCK CONSTRAINT: locked Coincident reference is the anchor.
  const bool firstLocked =
      isPointReferenceLocked(firstReference);
  const bool secondLocked =
      isPointReferenceLocked(secondReference);

  if (secondLocked) {
    if (firstLocked) {
      return std::hypot(first->xMm - second->xMm,
                        first->yMm - second->yMm) <= 1e-7;
    }
    return setPointsCoincident(
        secondReference, firstReference);
  }

  const auto sameReference =
      [](PointReference a, PointReference b) {
        if (a.elementCenterId != 0 || b.elementCenterId != 0)
          return a.elementCenterId != 0 &&
                 a.elementCenterId == b.elementCenterId;

        if (a.circleId != kInvalidGeometryId ||
            b.circleId != kInvalidGeometryId)
          return a.circleId != kInvalidGeometryId &&
                 a.circleId == b.circleId;

        if (a.arcId != kInvalidGeometryId ||
            b.arcId != kInvalidGeometryId)
          return a.arcId != kInvalidGeometryId &&
                 b.arcId != kInvalidGeometryId &&
                 a.arcId == b.arcId && a.start == b.start;

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

  // Never collapse both endpoints of one Arc onto each other.
  if (firstReference.arcId != kInvalidGeometryId &&
      firstReference.arcId == secondReference.arcId &&
      firstReference.start != secondReference.start)
    return false;

  const Point target = *first;
  const Point oldSecond = *second;

  if (std::hypot(target.xMm - oldSecond.xMm,
                 target.yMm - oldSecond.yMm) <= 1e-9)
    return true;

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
  } else if (secondReference.arcId != kInvalidGeometryId) {
    const auto index = arcIndex(secondReference.arcId);
    if (!index) return false;
    if (!moveArcEndpointForConstraint(arcs_[*index], secondReference.start,
                                      target))
      return false;
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
  journalCaptureComponents({lineIdValue});
  journalCapturePoint(pointReference);
  const auto carrierIndex = lineIndex(lineIdValue);
  const auto point = referencedPoint(pointReference);

  // LOCK CONSTRAINT: the constrained point itself is locked.
  // A locked carrier remains a valid reference.
  if (isPointReferenceLocked(pointReference))
    return false;

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
  } else if (pointReference.arcId != kInvalidGeometryId) {
    const auto arc = arcIndex(pointReference.arcId);
    if (!arc) return false;
    if (!moveArcEndpointRigid(arcs_[*arc], pointReference.start, target)) return false;
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

bool Sketch::setPointToMidpoint(GeometryId lineIdValue,
                                PointReference pointReference) {
  journalCaptureComponents({lineIdValue});
  journalCapturePoint(pointReference);
  const auto carrierIndex = lineIndex(lineIdValue);
  const auto point = referencedPoint(pointReference);

  // LOCK CONSTRAINT: the constrained point itself is locked.
  if (isPointReferenceLocked(pointReference))
    return false;

  if (!carrierIndex || !point) return false;

  const auto& carrier = lines_[*carrierIndex];
  const Point target{
      (carrier.start.xMm + carrier.end.xMm) * 0.5,
      (carrier.start.yMm + carrier.end.yMm) * 0.5};

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
  } else if (pointReference.arcId != kInvalidGeometryId) {
    const auto arc = arcIndex(pointReference.arcId);
    if (!arc) return false;
    if (!moveArcEndpointRigid(arcs_[*arc], pointReference.start, target))
      return false;
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

bool Sketch::setPointOnXAxis(PointReference pointReference) {
  journalCapturePoint(pointReference);
  const auto point = referencedPoint(pointReference);
  if (!point) return false;
  return translatePoint(pointReference, 0.0, -point->yMm);
}

bool Sketch::setPointOnYAxis(PointReference pointReference) {
  journalCapturePoint(pointReference);
  const auto point = referencedPoint(pointReference);
  if (!point) return false;
  return translatePoint(pointReference, -point->xMm, 0.0);
}

bool Sketch::setPointOnCircle(GeometryId circleIdValue,
                              PointReference pointReference) {
  journalCaptureComponents({circleIdValue});
  journalCapturePoint(pointReference);
  const auto carrierIndex = circleIndex(circleIdValue);
  const auto point = referencedPoint(pointReference);

  // LOCK CONSTRAINT: the constrained point itself is locked.
  // A locked carrier remains a valid reference.
  if (isPointReferenceLocked(pointReference))
    return false;

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
  } else if (pointReference.arcId != kInvalidGeometryId) {
    const auto movingArc = arcIndex(pointReference.arcId);
    if (!movingArc) return false;
    if (!moveArcEndpointRigid(arcs_[*movingArc], pointReference.start, target)) return false;
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

bool Sketch::setPointOnArc(GeometryId arcIdValue,
                           PointReference pointReference) {
  journalCaptureComponents({arcIdValue});
  journalCapturePoint(pointReference);
  const auto carrierIndex = arcIndex(arcIdValue);
  const auto point = referencedPoint(pointReference);
  if (!carrierIndex || !point) return false;

  const Arc& carrier = arcs_[*carrierIndex];
  constexpr double kTwoPi = 6.28318530717958647692;
  if (!std::isfinite(carrier.center.xMm) ||
      !std::isfinite(carrier.center.yMm) ||
      !std::isfinite(carrier.radiusMm) ||
      !std::isfinite(carrier.startAngleRad) ||
      !std::isfinite(carrier.sweepAngleRad) ||
      carrier.radiusMm <= 1e-9 ||
      carrier.sweepAngleRad <= 1e-9 ||
      carrier.sweepAngleRad >= kTwoPi - 1e-9)
    return false;

  // An endpoint already belonging to the carrier is tautologically valid.
  if (pointReference.arcId == arcIdValue)
    return true;

  const auto normalizeAngle = [](double angle) {
    constexpr double twoPi = 6.28318530717958647692;
    angle = std::fmod(angle, twoPi);
    if (angle < 0.0) angle += twoPi;
    return angle;
  };

  const Point oldPoint = *point;
  const Point startPoint = arcStartPoint(carrier);
  const Point endPoint = arcEndPoint(carrier);

  double dx = oldPoint.xMm - carrier.center.xMm;
  double dy = oldPoint.yMm - carrier.center.yMm;
  const double radialLength = std::hypot(dx, dy);

  Point target = startPoint;

  if (radialLength > 1e-9) {
    const double candidateAngle =
        normalizeAngle(std::atan2(dy, dx));
    const double startAngle =
        normalizeAngle(carrier.startAngleRad);
    const double delta =
        normalizeAngle(candidateAngle - startAngle);

    if (delta <= carrier.sweepAngleRad + 1e-12) {
      target = {
          carrier.center.xMm +
              dx / radialLength * carrier.radiusMm,
          carrier.center.yMm +
              dy / radialLength * carrier.radiusMm};
    } else {
      const double startDistance =
          std::hypot(oldPoint.xMm - startPoint.xMm,
                     oldPoint.yMm - startPoint.yMm);
      const double endDistance =
          std::hypot(oldPoint.xMm - endPoint.xMm,
                     oldPoint.yMm - endPoint.yMm);
      target = startDistance <= endDistance
                   ? startPoint
                   : endPoint;
    }
  }

  const double moveX = target.xMm - oldPoint.xMm;
  const double moveY = target.yMm - oldPoint.yMm;

  // A locked point may satisfy the relation, but must never be moved.
  if (isPointReferenceLocked(pointReference))
    return std::hypot(moveX, moveY) <= 1e-7;

  if (pointReference.elementCenterId != 0) {
    for (auto& line : lines_) {
      if (line.elementId != pointReference.elementCenterId)
        continue;
      line.start.xMm += moveX;
      line.start.yMm += moveY;
      line.end.xMm += moveX;
      line.end.yMm += moveY;
    }
  } else if (pointReference.circleId != kInvalidGeometryId) {
    const auto movingCircle = circleIndex(pointReference.circleId);
    if (!movingCircle) return false;
    circles_[*movingCircle].center = target;
  } else if (pointReference.arcId != kInvalidGeometryId) {
    // Move the other arc rigidly. Do not reshape either arc from PointOnArc.
    const auto movingArc = arcIndex(pointReference.arcId);
    if (!movingArc) return false;
    arcs_[*movingArc].center.xMm += moveX;
    arcs_[*movingArc].center.yMm += moveY;
  } else {
    const auto same = [](Point first, Point second) {
      return std::hypot(first.xMm - second.xMm,
                        first.yMm - second.yMm) <= 1e-7;
    };

    // Preserve the existing endpoint-cluster behaviour used by PointOnCircle.
    for (auto& line : lines_) {
      if (same(line.start, oldPoint)) line.start = target;
      if (same(line.end, oldPoint)) line.end = target;
    }

    for (auto& circle : circles_) {
      if (same(circle.center, oldPoint))
        circle.center = target;
    }
  }

  // IMPORTANT: do not call translatePoint() here. translatePoint() invokes
  // solveStable(), while this mutator is itself called from the solver.
  updateBounds();
  return true;
}

bool Sketch::setCircleTangentToLine(GeometryId lineIdValue,
                                    GeometryId circleIdValue) {
  journalCaptureComponents({lineIdValue, circleIdValue});
  const auto lineIndexValue = lineIndex(lineIdValue);
  const auto circleIndexValue = circleIndex(circleIdValue);

  if (!lineIndexValue || !circleIndexValue)
    return false;

  const Line line = lines_[*lineIndexValue];
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

  const auto referencesCircleCenter =
      [circleIdValue](PointReference reference) {
        return !reference.origin && reference.elementCenterId == 0 &&
               reference.circleId == circleIdValue;
      };
  const bool circleCenterAnchored =
      std::any_of(constraints_.begin(), constraints_.end(),
                  [&referencesCircleCenter](const Constraint& constraint) {
                    return referencesCircleCenter(constraint.firstPoint) ||
                           referencesCircleCenter(constraint.secondPoint);
                  });

  // A projected circle and a circle whose centre already has a positional
  // constraint are reference geometry for a later tangency. Moving such a
  // circle only makes a following datum/point pass move it back, leaving a
  // visible Tangent entry whose geometry is no longer tangent. Reconstruct
  // the line instead and preserve the earlier centre constraint.
  if (isGeometryLocked(circleIdValue) || circleCenterAnchored) {
    const auto referencesLineEndpoint =
        [lineIdValue](PointReference reference) {
          return !reference.origin && reference.elementCenterId == 0 &&
                 reference.circleId == kInvalidGeometryId &&
                 reference.arcId == kInvalidGeometryId &&
                 reference.lineId == lineIdValue;
        };
    std::optional<bool> attachedEndpoint;
    std::optional<bool> straightEndpoint;
    GeometryId straightCarrierId = kInvalidGeometryId;
    std::optional<double> drivingLength;

    for (const auto& constraint : constraints_) {
      if (constraint.type == ConstraintType::PointOnCircle &&
          constraint.firstGeometry == circleIdValue &&
          referencesLineEndpoint(constraint.secondPoint)) {
        attachedEndpoint = constraint.secondPoint.start;
      } else if (constraint.type == ConstraintType::PointOnLine &&
                 constraint.firstGeometry != kInvalidGeometryId &&
                 referencesLineEndpoint(constraint.secondPoint)) {
        straightEndpoint = constraint.secondPoint.start;
        straightCarrierId = constraint.firstGeometry;
      } else if (constraint.type == ConstraintType::Length &&
                 constraint.firstGeometry == lineIdValue &&
                 std::isfinite(constraint.value) && constraint.value > 0.0) {
        drivingLength = constraint.value;
      } else if (constraint.type == ConstraintType::Distance &&
                 referencesLineEndpoint(constraint.firstPoint) &&
                 referencesLineEndpoint(constraint.secondPoint) &&
                 constraint.firstPoint.start != constraint.secondPoint.start &&
                 std::isfinite(constraint.value) && constraint.value > 0.0) {
        drivingLength = constraint.value;
      }
    }

    bool requiresPerpendicular = false;
    if (straightCarrierId != kInvalidGeometryId) {
      for (const auto& constraint : constraints_) {
        const bool samePair =
            (constraint.firstGeometry == straightCarrierId &&
             constraint.secondGeometry == lineIdValue) ||
            (constraint.secondGeometry == straightCarrierId &&
             constraint.firstGeometry == lineIdValue);
        if (!samePair)
          continue;
        if (constraint.type == ConstraintType::Perpendicular ||
            (constraint.type == ConstraintType::Angle &&
             std::abs(constraint.value - 90.0) <= 1e-7)) {
          requiresPerpendicular = true;
          break;
        }
      }
    }

    // LENGTH + TANGENCY ON PROJECTED REFERENCES
    //
    // Let Q be the endpoint on the straight carrier and P the tangent point.
    // |QP| = L and CP = r imply |CQ| = sqrt(L^2 + r^2). Intersect that
    // auxiliary circle with the straight projection, then construct the two
    // tangent points from every valid Q. This solves the coupled constraints
    // in one step instead of making the sequential passes overwrite each
    // other. If a 90-degree relation already exists, keep only that branch.
    if (drivingLength && attachedEndpoint && straightEndpoint &&
        *attachedEndpoint != *straightEndpoint &&
        straightCarrierId != kInvalidGeometryId &&
        isGeometryLocked(straightCarrierId)) {
      const auto straightIndex = lineIndex(straightCarrierId);
      if (straightIndex) {
        const Line straight = lines_[*straightIndex];
        const double sx = straight.end.xMm - straight.start.xMm;
        const double sy = straight.end.yMm - straight.start.yMm;
        const double straightLength = std::hypot(sx, sy);
        const double tangentLength = *drivingLength;
        const double auxiliaryRadius =
            std::hypot(tangentLength, circle.radiusMm);

        if (straightLength > 1e-9 && auxiliaryRadius > circle.radiusMm) {
          const double ux = sx / straightLength;
          const double uy = sy / straightLength;
          const double centerDx = circle.center.xMm - straight.start.xMm;
          const double centerDy = circle.center.yMm - straight.start.yMm;
          const double centerAlong = centerDx * ux + centerDy * uy;
          const double centerNormal = -centerDx * uy + centerDy * ux;
          double remaining = auxiliaryRadius * auxiliaryRadius -
                             centerNormal * centerNormal;

          struct TangentLineCandidate {
            Point start;
            Point end;
            double movement{};
          };
          std::optional<TangentLineCandidate> best;

          if (remaining >= -1e-8) {
            remaining = std::max(0.0, remaining);
            const double alongDelta = std::sqrt(remaining);
            const double auxiliarySquared =
                auxiliaryRadius * auxiliaryRadius;
            const double alongFactor =
                tangentLength * tangentLength / auxiliarySquared;
            const double perpendicularFactor =
                circle.radiusMm * tangentLength / auxiliarySquared;

            for (const double alongSide : {-1.0, 1.0}) {
              const double along = centerAlong + alongSide * alongDelta;
              if (along < -1e-7 || along > straightLength + 1e-7)
                continue;
              const Point onStraight{
                  straight.start.xMm + ux * along,
                  straight.start.yMm + uy * along};
              const double vx = circle.center.xMm - onStraight.xMm;
              const double vy = circle.center.yMm - onStraight.yMm;

              for (const double tangentSide : {-1.0, 1.0}) {
                const Point onCircle{
                    onStraight.xMm + alongFactor * vx -
                        tangentSide * perpendicularFactor * vy,
                    onStraight.yMm + alongFactor * vy +
                        tangentSide * perpendicularFactor * vx};
                const double candidateDx = onCircle.xMm - onStraight.xMm;
                const double candidateDy = onCircle.yMm - onStraight.yMm;
                if (requiresPerpendicular &&
                    std::abs(candidateDx * ux + candidateDy * uy) > 1e-5)
                  continue;

                const Point candidateStart =
                    *straightEndpoint ? onStraight : onCircle;
                const Point candidateEnd =
                    *straightEndpoint ? onCircle : onStraight;
                const double movement =
                    std::hypot(candidateStart.xMm - line.start.xMm,
                               candidateStart.yMm - line.start.yMm) +
                    std::hypot(candidateEnd.xMm - line.end.xMm,
                               candidateEnd.yMm - line.end.yMm);
                if (!best || movement < best->movement) {
                  best = TangentLineCandidate{
                      candidateStart, candidateEnd, movement};
                }
              }
            }
          }

          if (best) {
            lines_[*lineIndexValue].start = best->start;
            lines_[*lineIndexValue].end = best->end;
            updateBounds();
            return true;
          }
        }
      }
    }

    // TANGENCY WITH AN ENDPOINT ON A STRAIGHT CARRIER
    //
    // Keep the endpoint Q on its earlier PointOnLine carrier and construct
    // the tangent point P analytically. This is the common edit path when a
    // user drags the base of an already tangent line. A simple normal shift
    // would pull Q off the carrier; the next PointOnLine pass would then undo
    // the tangency while the constraint remained listed in the UI.
    if (!drivingLength && attachedEndpoint && straightEndpoint &&
        *attachedEndpoint != *straightEndpoint &&
        straightCarrierId != kInvalidGeometryId) {
      const auto straightIndex = lineIndex(straightCarrierId);
      if (straightIndex) {
        const Line straight = lines_[*straightIndex];
        const double sx = straight.end.xMm - straight.start.xMm;
        const double sy = straight.end.yMm - straight.start.yMm;
        const double straightLength = std::hypot(sx, sy);
        if (straightLength > 1e-9) {
          const double ux = sx / straightLength;
          const double uy = sy / straightLength;
          const Point currentBase =
              *straightEndpoint ? line.start : line.end;
          const Point currentContact =
              *attachedEndpoint ? line.start : line.end;
          const double baseAlong = std::clamp(
              (currentBase.xMm - straight.start.xMm) * ux +
                  (currentBase.yMm - straight.start.yMm) * uy,
              0.0, straightLength);
          Point onStraight{straight.start.xMm + ux * baseAlong,
                           straight.start.yMm + uy * baseAlong};
          std::optional<Point> onCircle;

          if (requiresPerpendicular) {
            const double centerDx =
                circle.center.xMm - straight.start.xMm;
            const double centerDy =
                circle.center.yMm - straight.start.yMm;
            const double centerAlong = centerDx * ux + centerDy * uy;
            double bestMovement = std::numeric_limits<double>::infinity();
            for (const double branch : {-1.0, 1.0}) {
              const double along = centerAlong + branch * circle.radiusMm;
              if (along < -1e-7 || along > straightLength + 1e-7)
                continue;
              const Point candidateBase{
                  straight.start.xMm + ux * along,
                  straight.start.yMm + uy * along};
              const Point candidateContact{
                  circle.center.xMm + branch * circle.radiusMm * ux,
                  circle.center.yMm + branch * circle.radiusMm * uy};
              const double movement =
                  std::hypot(candidateBase.xMm - currentBase.xMm,
                             candidateBase.yMm - currentBase.yMm) +
                  std::hypot(candidateContact.xMm - currentContact.xMm,
                             candidateContact.yMm - currentContact.yMm);
              if (movement < bestMovement) {
                bestMovement = movement;
                onStraight = candidateBase;
                onCircle = candidateContact;
              }
            }
          } else {
            const double vx = circle.center.xMm - onStraight.xMm;
            const double vy = circle.center.yMm - onStraight.yMm;
            const double distanceSquared = vx * vx + vy * vy;
            const double radiusSquared = circle.radiusMm * circle.radiusMm;
            if (distanceSquared > radiusSquared + 1e-9) {
              const double tangentLength =
                  std::sqrt(distanceSquared - radiusSquared);
              const double alongFactor =
                  (distanceSquared - radiusSquared) / distanceSquared;
              const double perpendicularFactor =
                  circle.radiusMm * tangentLength / distanceSquared;
              const Point candidates[2]{
                  {onStraight.xMm + alongFactor * vx -
                       perpendicularFactor * vy,
                   onStraight.yMm + alongFactor * vy +
                       perpendicularFactor * vx},
                  {onStraight.xMm + alongFactor * vx +
                       perpendicularFactor * vy,
                   onStraight.yMm + alongFactor * vy -
                       perpendicularFactor * vx}};
              onCircle =
                  std::hypot(candidates[0].xMm - currentContact.xMm,
                             candidates[0].yMm - currentContact.yMm) <=
                          std::hypot(candidates[1].xMm - currentContact.xMm,
                                     candidates[1].yMm - currentContact.yMm)
                      ? candidates[0]
                      : candidates[1];
            }
          }

          if (onCircle) {
            Line& solvedLine = lines_[*lineIndexValue];
            solvedLine.start = *straightEndpoint ? onStraight : *onCircle;
            solvedLine.end = *straightEndpoint ? *onCircle : onStraight;
            updateBounds();
            return true;
          }
        }
      }
    }

    const double targetSignedDistance = side * circle.radiusMm;
    const double normalShift =
        signedDistance - targetSignedDistance;

    if (isGeometryLocked(lineIdValue))
      return std::abs(normalShift) <= 1e-7;

    const std::size_t elementId = line.elementId;
    bool moved = false;

    for (std::size_t index = 0; index < lines_.size(); ++index) {
      if (lines_[index].elementId != elementId)
        continue;
      if (isGeometryLocked(lineIds_[index]))
        return false;

      lines_[index].start.xMm += nx * normalShift;
      lines_[index].start.yMm += ny * normalShift;
      lines_[index].end.xMm += nx * normalShift;
      lines_[index].end.yMm += ny * normalShift;
      moved = true;
    }

    if (!moved)
      return false;

    // If one endpoint is explicitly attached to this projected circle, put
    // that endpoint on the exact tangent foot immediately. Letting the normal
    // PointOnCircle pass project it radially makes the line non-tangent again
    // and the two sequential passes otherwise chase each other indefinitely.
    if (attachedEndpoint) {
      Line& movedLine = lines_[*lineIndexValue];
      const double movedDx = movedLine.end.xMm - movedLine.start.xMm;
      const double movedDy = movedLine.end.yMm - movedLine.start.yMm;
      const double movedLengthSquared =
          movedDx * movedDx + movedDy * movedDy;
      if (movedLengthSquared <= 1e-12)
        return false;
      const double contactParameter =
          ((circle.center.xMm - movedLine.start.xMm) * movedDx +
           (circle.center.yMm - movedLine.start.yMm) * movedDy) /
          movedLengthSquared;
      const Point contact{
          movedLine.start.xMm + movedDx * contactParameter,
          movedLine.start.yMm + movedDy * contactParameter};
      if (*attachedEndpoint)
        movedLine.start = contact;
      else
        movedLine.end = contact;
    }

    updateBounds();
    return true;
  }

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
bool Sketch::setArcTangentToLine(GeometryId lineIdValue,
                                 GeometryId arcIdValue) {
  journalCaptureComponents({lineIdValue, arcIdValue});
  if (isGeometryLocked(arcIdValue)) return false;
  const auto lineIndexValue = lineIndex(lineIdValue);
  const auto arcIndexValue = arcIndex(arcIdValue);
  if (!lineIndexValue || !arcIndexValue) return false;

  const Line& line = lines_[*lineIndexValue];
  Arc& arc = arcs_[*arcIndexValue];
  if (!std::isfinite(arc.radiusMm) || arc.radiusMm <= 1e-9 ||
      !std::isfinite(arc.startAngleRad) ||
      !std::isfinite(arc.sweepAngleRad) || arc.sweepAngleRad <= 1e-9)
    return false;

  const double dx = line.end.xMm - line.start.xMm;
  const double dy = line.end.yMm - line.start.yMm;
  const double lengthSquared = dx * dx + dy * dy;
  if (lengthSquared <= 1e-12) return false;

  const double length = std::sqrt(lengthSquared);
  const double nx = -dy / length;
  const double ny = dx / length;
  const double rawT =
      ((arc.center.xMm - line.start.xMm) * dx +
       (arc.center.yMm - line.start.yMm) * dy) /
      lengthSquared;
  const double t = std::clamp(rawT, 0.0, 1.0);
  const Point contact{line.start.xMm + dx * t,
                      line.start.yMm + dy * t};
  const double signedDistance =
      (arc.center.xMm - line.start.xMm) * nx +
      (arc.center.yMm - line.start.yMm) * ny;
  const double side = signedDistance < 0.0 ? -1.0 : 1.0;

  arc.center.xMm = contact.xMm + side * nx * arc.radiusMm;
  arc.center.yMm = contact.yMm + side * ny * arc.radiusMm;

  // Tangency belongs to the finite Arc span. Preserve its sweep and rotate
  // the rigid curve only when the contact direction is currently outside it.
  constexpr double kTwoPi = 6.28318530717958647692;
  const auto normalize = [kTwoPi](double angle) {
    angle = std::fmod(angle, kTwoPi);
    if (angle < 0.0) angle += kTwoPi;
    return angle;
  };
  const double contactAngle =
      std::atan2(contact.yMm - arc.center.yMm,
                 contact.xMm - arc.center.xMm);
  const double offset = normalize(contactAngle - arc.startAngleRad);
  if (offset > arc.sweepAngleRad + 1e-9)
    arc.startAngleRad = contactAngle - arc.sweepAngleRad * 0.5;

  updateBounds();
  return true;
}
bool Sketch::translatePoint(PointReference reference, double dxMm,
                            double dyMm) {
  journalCapturePoint(reference);
  if (!referencedPoint(reference)) return false;
  if (reference.origin)
    return std::abs(dxMm) <= 1e-12 && std::abs(dyMm) <= 1e-12;
  if (dxMm == 0.0 && dyMm == 0.0) return true;

  // CRASH-FREE 04: COMPLETE POINTREFERENCE IDENTITY
  //
  // A virtual element centre is its own reference class. Never compare two
  // centre references through their default invalid lineId values.
  const auto sameReference =
      [](PointReference first,
         PointReference second) {
        if (first.origin || second.origin)
          return first.origin && second.origin;
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

        if (first.arcId != kInvalidGeometryId ||
            second.arcId != kInvalidGeometryId) {
          return first.arcId != kInvalidGeometryId &&
                 second.arcId != kInvalidGeometryId &&
                 first.arcId == second.arcId &&
                 first.start == second.start;
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

  // Coordinate-equal line endpoints and circle centres are an implicit CAD
  // junction in the connectivity graph. Expand the same cluster here before
  // applying an interactive point translation; otherwise component capture
  // would correctly include a circle while the mutator left its centre (or
  // its neighbouring line endpoint) behind.
  const auto appendRawJunction = [&connectedPoints, &sameReference](
                                     PointReference candidate) {
    if (std::none_of(connectedPoints.begin(), connectedPoints.end(),
                     [candidate, &sameReference](PointReference item) {
                       return sameReference(item, candidate);
                     }))
      connectedPoints.push_back(candidate);
  };
  constexpr double kJunctionTolerance = 1e-7;
  for (std::size_t connectedIndex = 0;
       connectedIndex < connectedPoints.size(); ++connectedIndex) {
    const PointReference currentReference = connectedPoints[connectedIndex];
    if (currentReference.origin || currentReference.elementCenterId != 0 ||
        currentReference.arcId != kInvalidGeometryId)
      continue;
    const auto current = referencedPoint(currentReference);
    if (!current) continue;
    const auto coincides = [current](Point candidate) {
      return std::hypot(candidate.xMm - current->xMm,
                        candidate.yMm - current->yMm) <=
             kJunctionTolerance;
    };
    for (std::size_t lineIndexValue = 0;
         lineIndexValue < lines_.size(); ++lineIndexValue) {
      if (coincides(lines_[lineIndexValue].start)) {
        PointReference candidate;
        candidate.lineId = lineIds_[lineIndexValue];
        candidate.start = true;
        appendRawJunction(candidate);
      }
      if (coincides(lines_[lineIndexValue].end)) {
        PointReference candidate;
        candidate.lineId = lineIds_[lineIndexValue];
        candidate.start = false;
        appendRawJunction(candidate);
      }
    }
    for (std::size_t circleIndexValue = 0;
         circleIndexValue < circles_.size(); ++circleIndexValue) {
      if (!coincides(circles_[circleIndexValue].center)) continue;
      PointReference candidate;
      candidate.circleId = circleIds_[circleIndexValue];
      appendRawJunction(candidate);
    }
  }

  // LOCK CONSTRAINT: connected point cluster contains a lock.
  if (std::any_of(
          connectedPoints.begin(), connectedPoints.end(),
          [this](PointReference item) {
            return isPointReferenceLocked(item);
          }))
    return false;

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

    if (pointReference.arcId != kInvalidGeometryId) {
      const auto index = arcIndex(pointReference.arcId);
      if (!index) continue;
      const auto current = referencedPoint(pointReference);
      if (!current) continue;
      const Point target{current->xMm + dxMm,
                         current->yMm + dyMm};
      (void)moveArcEndpointRigid(arcs_[*index], pointReference.start, target);
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
        pointReference.arcId !=
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

  std::vector<GeometryId> dirtyIds = movedLineIds;
  for (const auto pointReference : connectedPoints) {
    if (pointReference.circleId != kInvalidGeometryId)
      dirtyIds.push_back(pointReference.circleId);
    if (pointReference.arcId != kInvalidGeometryId)
      dirtyIds.push_back(pointReference.arcId);
    if (pointReference.elementCenterId != 0) {
      for (std::size_t index = 0; index < lines_.size(); ++index)
        if (lines_[index].elementId == pointReference.elementCenterId)
          dirtyIds.push_back(lineIds_[index]);
    }
  }
  (void)BasicSketchSolver::solveStableComponent(*this, dirtyIds);
  updateBounds();
  return true;
}
bool Sketch::setLineLength(std::size_t index, double lengthMm) {
  if (index < lineIds_.size()) journalCaptureComponents({lineIds_[index]});
  if (index >= lines_.size() || lengthMm <= 0.0) return false;
  // LOCK CONSTRAINT: direct indexed length edit is blocked.
  if (index < lineIds_.size() &&
      isGeometryLocked(lineIds_[index])) {
    const auto& locked = lines_[index];
    const double current =
        std::hypot(locked.end.xMm - locked.start.xMm,
                   locked.end.yMm - locked.start.yMm);
    return std::abs(current - lengthMm) <= 1e-7;
  }
  const Point start = lines_[index].start;
  const Point oldEnd = lines_[index].end;
  const double dx = oldEnd.xMm - start.xMm;
  const double dy = oldEnd.yMm - start.yMm;
  const double oldLength = std::hypot(dx, dy);
  if (oldLength <= 1e-9) return false;
  const GeometryId lineIdValue = lineIds_[index];
  const auto referencesEndpoint =
      [lineIdValue](PointReference reference, bool startPoint) {
        return !reference.origin && reference.elementCenterId == 0 &&
               reference.circleId == kInvalidGeometryId &&
               reference.arcId == kInvalidGeometryId &&
               reference.lineId == lineIdValue &&
               reference.start == startPoint;
      };

  // PROJECTED CARRIER + PROJECTED CIRCLE DIMENSION
  //
  // With a perpendicular relation already present, changing the length of a
  // line between a straight projection and a circular projection generally
  // requires sliding the WHOLE line along the straight carrier. Resizing one
  // endpoint and projecting it back cannot find that valid branch and makes
  // the transactional dimension appear to do nothing.
  struct ProjectedAnchor {
    GeometryId geometryId{kInvalidGeometryId};
    bool atStart{};
  };
  std::optional<ProjectedAnchor> straightAnchor;
  std::optional<ProjectedAnchor> circleAnchor;

  for (const auto& constraint : constraints_) {
    if (constraint.type == ConstraintType::PointOnLine &&
        constraint.firstGeometry != kInvalidGeometryId &&
        (referencesEndpoint(constraint.secondPoint, true) ||
         referencesEndpoint(constraint.secondPoint, false))) {
      straightAnchor = ProjectedAnchor{
          constraint.firstGeometry, constraint.secondPoint.start};
    } else if (constraint.type == ConstraintType::PointOnCircle &&
               constraint.firstGeometry != kInvalidGeometryId &&
               (referencesEndpoint(constraint.secondPoint, true) ||
                referencesEndpoint(constraint.secondPoint, false))) {
      circleAnchor = ProjectedAnchor{
          constraint.firstGeometry, constraint.secondPoint.start};
    }
  }

  // TANGENT + PERPENDICULAR + LENGTH WITH A DATUM-CONSTRAINED CIRCLE
  //
  // A centre on only one datum axis is not fully fixed: it may still slide
  // along that axis. When the line already has PointOnLine, PointOnCircle,
  // Tangent and Perpendicular constraints, a new length must consume that
  // remaining degree of freedom instead of moving one endpoint and letting
  // the older constraints undo the size on the following solver pass.
  if (straightAnchor && circleAnchor &&
      straightAnchor->atStart != circleAnchor->atStart &&
      isGeometryLocked(straightAnchor->geometryId) &&
      !isGeometryLocked(circleAnchor->geometryId)) {
    const auto carrierIndex = lineIndex(straightAnchor->geometryId);
    const auto movableCircleIndex = circleIndex(circleAnchor->geometryId);
    const bool perpendicular = std::any_of(
        constraints_.begin(), constraints_.end(),
        [lineIdValue, &straightAnchor](const Constraint& constraint) {
          const bool samePair =
              (constraint.firstGeometry == straightAnchor->geometryId &&
               constraint.secondGeometry == lineIdValue) ||
              (constraint.secondGeometry == straightAnchor->geometryId &&
               constraint.firstGeometry == lineIdValue);
          return samePair &&
                 (constraint.type == ConstraintType::Perpendicular ||
                  (constraint.type == ConstraintType::Angle &&
                   std::abs(constraint.value - 90.0) <= 1e-7));
        });
    const bool tangent = std::any_of(
        constraints_.begin(), constraints_.end(),
        [lineIdValue, &circleAnchor](const Constraint& constraint) {
          if (constraint.type != ConstraintType::Tangent)
            return false;
          return (constraint.firstGeometry == lineIdValue &&
                  constraint.secondGeometry == circleAnchor->geometryId) ||
                 (constraint.secondGeometry == lineIdValue &&
                  constraint.firstGeometry == circleAnchor->geometryId);
        });

    bool centerOnXAxis = false;
    bool centerOnYAxis = false;
    bool otherCenterPosition = false;
    std::vector<GeometryId> centerCarrierIds;
    const auto referencesCircleCenter =
        [&circleAnchor](PointReference reference) {
          return !reference.origin && reference.elementCenterId == 0 &&
                 reference.circleId == circleAnchor->geometryId;
        };
    for (const auto& constraint : constraints_) {
      if (!referencesCircleCenter(constraint.firstPoint) &&
          !referencesCircleCenter(constraint.secondPoint))
        continue;
      if (constraint.type == ConstraintType::PointOnXAxis)
        centerOnXAxis = true;
      else if (constraint.type == ConstraintType::PointOnYAxis)
        centerOnYAxis = true;
      else if (constraint.type == ConstraintType::PointOnLine &&
               constraint.firstGeometry != kInvalidGeometryId &&
               referencesCircleCenter(constraint.secondPoint))
        centerCarrierIds.push_back(constraint.firstGeometry);
      else
        otherCenterPosition = true;
    }

    const bool exactlyOneFreeAxis =
        centerOnXAxis != centerOnYAxis && !otherCenterPosition;
    if (carrierIndex && movableCircleIndex && perpendicular && tangent &&
        exactlyOneFreeAxis) {
      const Line carrier = lines_[*carrierIndex];
      const Circle oldCircle = circles_[*movableCircleIndex];
      const double carrierDx = carrier.end.xMm - carrier.start.xMm;
      const double carrierDy = carrier.end.yMm - carrier.start.yMm;
      const double carrierLength = std::hypot(carrierDx, carrierDy);
      if (carrierLength > 1e-9 && oldCircle.radiusMm > 1e-9) {
        const double ux = carrierDx / carrierLength;
        const double uy = carrierDy / carrierLength;
        const double nx = -uy;
        const double ny = ux;
        const Point currentBase =
            straightAnchor->atStart ? start : oldEnd;
        const Point currentContact =
            circleAnchor->atStart ? start : oldEnd;
        const double currentAlong = std::clamp(
            (currentBase.xMm - carrier.start.xMm) * ux +
                (currentBase.yMm - carrier.start.yMm) * uy,
            0.0, carrierLength);

        struct AxisCandidate {
          Point base;
          Point contact;
          Point center;
          double movement{};
        };
        std::optional<AxisCandidate> best;

        for (const double normalSide : {-1.0, 1.0}) {
          for (const double tangentSide : {-1.0, 1.0}) {
            double along = currentAlong;
            const double axisDirection = centerOnYAxis ? ux : uy;
            const double axisConstant =
                (centerOnYAxis ? carrier.start.xMm : carrier.start.yMm) +
                (centerOnYAxis ? nx : ny) * normalSide * lengthMm +
                (centerOnYAxis ? ux : uy) * tangentSide *
                    oldCircle.radiusMm;
            if (std::abs(axisDirection) > 1e-9)
              along = -axisConstant / axisDirection;
            else if (std::abs(axisConstant) > 1e-7)
              continue;

            if (along < -1e-7 || along > carrierLength + 1e-7)
              continue;
            along = std::clamp(along, 0.0, carrierLength);

            const Point base{carrier.start.xMm + ux * along,
                             carrier.start.yMm + uy * along};
            const Point contact{
                base.xMm + nx * normalSide * lengthMm,
                base.yMm + ny * normalSide * lengthMm};
            const Point center{
                contact.xMm + ux * tangentSide * oldCircle.radiusMm,
                contact.yMm + uy * tangentSide * oldCircle.radiusMm};
            if ((centerOnYAxis && std::abs(center.xMm) > 1e-6) ||
                (centerOnXAxis && std::abs(center.yMm) > 1e-6))
              continue;

            const bool onEveryCenterCarrier = std::all_of(
                centerCarrierIds.begin(), centerCarrierIds.end(),
                [this, center](GeometryId carrierId) {
                  const auto centerCarrierIndex = lineIndex(carrierId);
                  if (!centerCarrierIndex) return false;
                  const Line& centerCarrier = lines_[*centerCarrierIndex];
                  const double dx =
                      centerCarrier.end.xMm - centerCarrier.start.xMm;
                  const double dy =
                      centerCarrier.end.yMm - centerCarrier.start.yMm;
                  const double lengthSquared = dx * dx + dy * dy;
                  if (lengthSquared <= 1e-12) return false;
                  const double parameter =
                      ((center.xMm - centerCarrier.start.xMm) * dx +
                       (center.yMm - centerCarrier.start.yMm) * dy) /
                      lengthSquared;
                  if (parameter < -1e-7 || parameter > 1.0 + 1e-7)
                    return false;
                  const Point projected{
                      centerCarrier.start.xMm + parameter * dx,
                      centerCarrier.start.yMm + parameter * dy};
                  return std::hypot(center.xMm - projected.xMm,
                                    center.yMm - projected.yMm) <= 1e-6;
                });
            if (!onEveryCenterCarrier)
              continue;

            const double movement =
                std::hypot(base.xMm - currentBase.xMm,
                           base.yMm - currentBase.yMm) +
                std::hypot(contact.xMm - currentContact.xMm,
                           contact.yMm - currentContact.yMm) +
                std::hypot(center.xMm - oldCircle.center.xMm,
                           center.yMm - oldCircle.center.yMm);
            if (!best || movement < best->movement)
              best = AxisCandidate{base, contact, center, movement};
          }
        }

        if (best) {
          lines_[index].start =
              straightAnchor->atStart ? best->base : best->contact;
          lines_[index].end =
              straightAnchor->atStart ? best->contact : best->base;
          circles_[*movableCircleIndex].center = best->center;
          updateBounds();
          return true;
        }
      }
    }
  }

  if (straightAnchor && circleAnchor &&
      straightAnchor->atStart != circleAnchor->atStart &&
      isGeometryLocked(straightAnchor->geometryId) &&
      isGeometryLocked(circleAnchor->geometryId)) {
    const auto carrierIndex = lineIndex(straightAnchor->geometryId);
    const auto projectedCircleIndex =
        circleIndex(circleAnchor->geometryId);
    const bool perpendicular = std::any_of(
        constraints_.begin(), constraints_.end(),
        [lineIdValue, &straightAnchor](const Constraint& constraint) {
          if (constraint.type != ConstraintType::Perpendicular)
            return false;
          return (constraint.firstGeometry == straightAnchor->geometryId &&
                  constraint.secondGeometry == lineIdValue) ||
                 (constraint.secondGeometry == straightAnchor->geometryId &&
                  constraint.firstGeometry == lineIdValue);
        });

    if (carrierIndex && projectedCircleIndex && perpendicular) {
      const Line carrier = lines_[*carrierIndex];
      const Circle projectedCircle = circles_[*projectedCircleIndex];
      const double carrierDx = carrier.end.xMm - carrier.start.xMm;
      const double carrierDy = carrier.end.yMm - carrier.start.yMm;
      const double carrierLength = std::hypot(carrierDx, carrierDy);

      if (carrierLength > 1e-9 && projectedCircle.radiusMm > 1e-9) {
        const double ux = carrierDx / carrierLength;
        const double uy = carrierDy / carrierLength;
        const double nx = -uy;
        const double ny = ux;
        const double centerDx =
            projectedCircle.center.xMm - carrier.start.xMm;
        const double centerDy =
            projectedCircle.center.yMm - carrier.start.yMm;
        const double centerAlong = centerDx * ux + centerDy * uy;
        const double centerNormal = centerDx * nx + centerDy * ny;

        struct Candidate {
          Point start;
          Point end;
          double movement{};
        };
        std::optional<Candidate> best;

        for (const double normalSide : {-1.0, 1.0}) {
          const double endpointNormal = normalSide * lengthMm;
          const double normalDelta = endpointNormal - centerNormal;
          double remaining = projectedCircle.radiusMm *
                                 projectedCircle.radiusMm -
                             normalDelta * normalDelta;
          if (remaining < -1e-8)
            continue;
          remaining = std::max(0.0, remaining);
          const double alongDelta = std::sqrt(remaining);

          for (const double alongSide : {-1.0, 1.0}) {
            const double along = centerAlong + alongSide * alongDelta;
            if (along < -1e-7 || along > carrierLength + 1e-7)
              continue;

            const Point onStraight{
                carrier.start.xMm + ux * along,
                carrier.start.yMm + uy * along};
            const Point onCircle{
                onStraight.xMm + nx * endpointNormal,
                onStraight.yMm + ny * endpointNormal};
            const Point candidateStart =
                straightAnchor->atStart ? onStraight : onCircle;
            const Point candidateEnd =
                straightAnchor->atStart ? onCircle : onStraight;
            const double movement =
                std::hypot(candidateStart.xMm - start.xMm,
                           candidateStart.yMm - start.yMm) +
                std::hypot(candidateEnd.xMm - oldEnd.xMm,
                           candidateEnd.yMm - oldEnd.yMm);

            if (!best || movement < best->movement)
              best = Candidate{candidateStart, candidateEnd, movement};
          }
        }

        if (best) {
          lines_[index].start = best->start;
          lines_[index].end = best->end;
          updateBounds();
          return true;
        }
      }
    }
  }

  const auto endpointAnchorRank =
      [this, &referencesEndpoint](bool startPoint) {
        int rank = 0;
        for (const auto& constraint : constraints_) {
          if (!referencesEndpoint(constraint.secondPoint, startPoint) &&
              !referencesEndpoint(constraint.firstPoint, startPoint))
            continue;

          if (constraint.type == ConstraintType::PointOnLine)
            rank = std::max(rank, 30);
          else if (constraint.type == ConstraintType::PointOnXAxis ||
                   constraint.type == ConstraintType::PointOnYAxis ||
                   constraint.type == ConstraintType::Midpoint)
            rank = std::max(rank, 35);
          else if (constraint.type == ConstraintType::PointOnCircle ||
                   constraint.type == ConstraintType::PointOnArc)
            rank = std::max(rank, 20);
          else if (constraint.type == ConstraintType::Coincident) {
            const PointReference other =
                referencesEndpoint(constraint.firstPoint, startPoint)
                    ? constraint.secondPoint
                    : constraint.firstPoint;
            rank = std::max(rank,
                            isPointReferenceLocked(other) ? 40 : 10);
          }
        }
        return rank;
      };

  // A projected straight carrier is a stronger positional anchor than a
  // point that may slide around a circle/arc. Preserve that carrier endpoint
  // and resize the opposite end. The previous unconditional start pivot made
  // Length and PointOnLine fight forever and the transactional add was
  // correctly rejected as unsatisfied.
  const bool moveStart =
      endpointAnchorRank(false) > endpointAnchorRank(true);
  const Point oldMoving = moveStart ? start : oldEnd;
  const Point newMoving =
      moveStart
          ? Point{oldEnd.xMm - dx / oldLength * lengthMm,
                  oldEnd.yMm - dy / oldLength * lengthMm}
          : Point{start.xMm + dx / oldLength * lengthMm,
                  start.yMm + dy / oldLength * lengthMm};
  const auto same = [](Point a, Point b) {
    return std::hypot(a.xMm - b.xMm, a.yMm - b.yMm) <= 1e-7;
  };
  const double moveX = newMoving.xMm - oldMoving.xMm;
  const double moveY = newMoving.yMm - oldMoving.yMm;

  struct JunctionMove {
    Point from;
    Point to;
  };

  std::vector<JunctionMove> junctionMoves{{oldMoving, newMoving}};
  const auto addJunctionMove = [&junctionMoves](Point from, Point to) {
    const auto existing = std::find_if(
        junctionMoves.begin(), junctionMoves.end(),
        [from](const JunctionMove& item) {
          return std::hypot(item.from.xMm - from.xMm,
                            item.from.yMm - from.yMm) <= 1e-12;
        });
    if (existing == junctionMoves.end())
      junctionMoves.push_back({from, to});
  };
  const auto preservesTranslationAxis =
      [this, moveX, moveY](GeometryId id) {
        constexpr double tolerance = 1e-9;
        const bool horizontalMove =
            std::abs(moveX) > tolerance && std::abs(moveY) <= tolerance;
        const bool verticalMove =
            std::abs(moveY) > tolerance && std::abs(moveX) <= tolerance;
        return std::any_of(
            constraints_.begin(), constraints_.end(),
            [id, horizontalMove, verticalMove](const Constraint& item) {
              return item.firstGeometry == id &&
                     ((horizontalMove &&
                       item.type == ConstraintType::Vertical) ||
                      (verticalMove &&
                       item.type == ConstraintType::Horizontal));
            });
      };

  // Moving one end of a dimensioned horizontal segment must translate an
  // attached Vertical line in X (and symmetrically for Vertical/Horizontal).
  // Otherwise the next sequential H/V pass anchors that neighbour at its
  // stored start point and silently moves the junction back. Which end was
  // drawn first must not decide whether the dimension can be applied.
  for (std::size_t moveIndex = 0; moveIndex < junctionMoves.size();
       ++moveIndex) {
    const auto movement = junctionMoves[moveIndex];
    for (std::size_t lineIndexValue = 0; lineIndexValue < lines_.size();
         ++lineIndexValue) {
      const auto& line = lines_[lineIndexValue];
      if (same(line.start, movement.from))
        addJunctionMove(line.start, movement.to);
      if (same(line.end, movement.from))
        addJunctionMove(line.end, movement.to);
      if (lineIds_[lineIndexValue] == lineIdValue ||
          !preservesTranslationAxis(lineIds_[lineIndexValue]))
        continue;
      if (same(line.start, movement.from)) {
        addJunctionMove(
            line.end,
            Point{line.end.xMm + moveX, line.end.yMm + moveY});
      } else if (same(line.end, movement.from)) {
        addJunctionMove(
            line.start,
            Point{line.start.xMm + moveX, line.start.yMm + moveY});
      }
    }
    for (const auto& circle : circles_)
      if (same(circle.center, movement.from))
        addJunctionMove(circle.center, movement.to);
  }

  const auto movedPoint = [&junctionMoves, &same](Point candidate) {
    const auto found = std::find_if(
        junctionMoves.begin(), junctionMoves.end(),
        [candidate, &same](const JunctionMove& item) {
          return same(item.from, candidate);
        });
    return found == junctionMoves.end() ? candidate : found->to;
  };

  for (auto& line : lines_) {
    line.start = movedPoint(line.start);
    line.end = movedPoint(line.end);
  }
  for (auto& circle : circles_)
    circle.center = movedPoint(circle.center);
  updateBounds();
  return true;
}

bool Sketch::setPointDistance(PointReference firstReference,
                              PointReference secondReference,
                              double distanceMm) {
  journalCapturePoint(firstReference);
  journalCapturePoint(secondReference);
  const auto isPlainLineEndpoint = [](PointReference reference) {
    return !reference.origin && reference.elementCenterId == 0 &&
           reference.circleId == kInvalidGeometryId &&
           reference.arcId == kInvalidGeometryId &&
           reference.lineId != kInvalidGeometryId;
  };
  // AutoDimension represents a direct line-length click as the distance
  // between that line's two endpoints. Route it through the line-length
  // primitive so endpoint mobility (notably projected carrier vs circle/arc)
  // is handled consistently with an explicit Length constraint.
  if (distanceMm > 0.0 && isPlainLineEndpoint(firstReference) &&
      isPlainLineEndpoint(secondReference) &&
      firstReference.lineId == secondReference.lineId &&
      firstReference.start != secondReference.start)
    return setLineLengthById(firstReference.lineId, distanceMm);

  const auto rectangleElementForPoint =
      [this](PointReference reference) -> std::optional<std::size_t> {
    if (reference.elementCenterId != 0 ||
        reference.circleId != kInvalidGeometryId ||
        reference.lineId == kInvalidGeometryId)
      return std::nullopt;

    const auto index = lineIndex(reference.lineId);
    if (!index) return std::nullopt;

    const std::size_t elementId = lines_[*index].elementId;
    const std::size_t count = static_cast<std::size_t>(std::count_if(
        lines_.begin(), lines_.end(), [elementId](const Line& line) {
          return line.elementId == elementId;
        }));
    return count == 4 ? std::optional<std::size_t>{elementId}
                      : std::nullopt;
  };

  const auto firstRectangle = rectangleElementForPoint(firstReference);
  const auto secondRectangle = rectangleElementForPoint(secondReference);

  // Click order is not a mobility rule. When an unconstrained standalone
  // endpoint is paired with a composite rectangle point, keep the composite
  // as the reference and move the endpoint with the smaller CAD footprint.
  if (!isPointReferenceLocked(firstReference) &&
      !isPointReferenceLocked(secondReference) && !firstRectangle &&
      secondRectangle) {
    return setPointDistance(secondReference, firstReference, distanceMm);
  }

  const auto first = referencedPoint(firstReference);
  const auto second = referencedPoint(secondReference);
  // LOCK CONSTRAINT: keep the locked point as the reference.
  if (isPointReferenceLocked(secondReference)) {
    if (isPointReferenceLocked(firstReference))
      return false;
    return setPointDistance(secondReference, firstReference, distanceMm);
  }
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

  // Two aligned distances from the opposite endpoints of one locked
  // reference line to opposite vertices of a free axis-aligned rectangle
  // determine that rectangle's position and size. A sole external distance
  // continues through the existing rigid-translation path below.
  if (isPointReferenceLocked(firstReference) && secondRectangle &&
      !isElementLocked(*secondRectangle) &&
      firstReference.elementCenterId == 0 &&
      firstReference.circleId == kInvalidGeometryId &&
      firstReference.lineId != kInvalidGeometryId) {
    constexpr double tolerance = 1e-7;
    const bool xAxis = std::abs(dy) <= tolerance &&
                       std::abs(dx) > tolerance;
    const bool yAxis = std::abs(dx) <= tolerance &&
                       std::abs(dy) > tolerance;
    const auto externalLine = lineIndex(firstReference.lineId);

    bool referenceAligned = false;
    if (externalLine) {
      const Line& line = lines_[*externalLine];
      const double lineDx = std::abs(line.end.xMm - line.start.xMm);
      const double lineDy = std::abs(line.end.yMm - line.start.yMm);
      referenceAligned = (xAxis && lineDy <= tolerance &&
                          lineDx > tolerance) ||
                         (yAxis && lineDx <= tolerance &&
                          lineDy > tolerance);
    }

    std::vector<std::size_t> rectangleLines;
    bool rectangleAxisAligned = xAxis || yAxis;
    for (std::size_t index = 0; index < lines_.size(); ++index) {
      if (lines_[index].elementId != *secondRectangle)
        continue;
      rectangleLines.push_back(index);
      const double lineDx =
          std::abs(lines_[index].end.xMm - lines_[index].start.xMm);
      const double lineDy =
          std::abs(lines_[index].end.yMm - lines_[index].start.yMm);
      if (lineDx > tolerance && lineDy > tolerance)
        rectangleAxisAligned = false;
    }

    const auto pointBelongsToRectangle =
        [this, elementId = *secondRectangle](PointReference reference) {
      if (reference.elementCenterId != 0 ||
          reference.circleId != kInvalidGeometryId ||
          reference.lineId == kInvalidGeometryId)
        return false;
      const auto index = lineIndex(reference.lineId);
      return index && lines_[*index].elementId == elementId;
    };

    bool axisSizeDriven = false;
    for (const auto& constraint : constraints_) {
      const auto constrainedLine = lineIndex(constraint.firstGeometry);
      if (constraint.type == ConstraintType::Length && constrainedLine &&
          lines_[*constrainedLine].elementId == *secondRectangle) {
        const Line& line = lines_[*constrainedLine];
        const double lineDx = std::abs(line.end.xMm - line.start.xMm);
        const double lineDy = std::abs(line.end.yMm - line.start.yMm);
        if ((xAxis && lineDx > lineDy + tolerance) ||
            (yAxis && lineDy > lineDx + tolerance)) {
          axisSizeDriven = true;
          break;
        }
      }

      if (!pointBelongsToRectangle(constraint.firstPoint) ||
          !pointBelongsToRectangle(constraint.secondPoint))
        continue;

      const auto constrainedFirst = referencedPoint(constraint.firstPoint);
      const auto constrainedSecond = referencedPoint(constraint.secondPoint);
      if (!constrainedFirst || !constrainedSecond)
        continue;

      const double internalDx =
          std::abs(constrainedSecond->xMm - constrainedFirst->xMm);
      const double internalDy =
          std::abs(constrainedSecond->yMm - constrainedFirst->yMm);
      if ((xAxis &&
           (constraint.type == ConstraintType::DistanceX ||
            (constraint.type == ConstraintType::Distance &&
             internalDx > tolerance && internalDy <= tolerance))) ||
          (yAxis &&
           (constraint.type == ConstraintType::DistanceY ||
            (constraint.type == ConstraintType::Distance &&
             internalDy > tolerance && internalDx <= tolerance)))) {
        axisSizeDriven = true;
        break;
      }
    }

    struct AxisTarget {
      double oldCoordinate{};
      double newCoordinate{};
    };
    std::vector<AxisTarget> targets;
    bool hasOppositeGap = false;
    const double currentRectangleCoordinate =
        xAxis ? second->xMm : second->yMm;

    if (referenceAligned && rectangleAxisAligned &&
        rectangleLines.size() == 4 && !axisSizeDriven) {
      for (const auto& constraint : constraints_) {
        if (constraint.type != ConstraintType::Distance ||
            !std::isfinite(constraint.value) || constraint.value <= 0.0)
          continue;

        const bool firstIsRectangle =
            pointBelongsToRectangle(constraint.firstPoint);
        const bool secondIsRectangle =
            pointBelongsToRectangle(constraint.secondPoint);
        if (firstIsRectangle == secondIsRectangle)
          continue;

        const PointReference rectangleReference =
            firstIsRectangle ? constraint.firstPoint : constraint.secondPoint;
        const PointReference externalReference =
            firstIsRectangle ? constraint.secondPoint : constraint.firstPoint;
        if (externalReference.elementCenterId != 0 ||
            externalReference.circleId != kInvalidGeometryId ||
            externalReference.lineId != firstReference.lineId ||
            !isPointReferenceLocked(externalReference))
          continue;

        const auto rectanglePoint = referencedPoint(rectangleReference);
        const auto externalPoint = referencedPoint(externalReference);
        if (!rectanglePoint || !externalPoint)
          continue;

        const double gapDx = rectanglePoint->xMm - externalPoint->xMm;
        const double gapDy = rectanglePoint->yMm - externalPoint->yMm;
        if ((xAxis && (std::abs(gapDy) > tolerance ||
                       std::abs(gapDx) <= tolerance)) ||
            (yAxis && (std::abs(gapDx) > tolerance ||
                       std::abs(gapDy) <= tolerance)))
          continue;

        const double oldCoordinate =
            xAxis ? rectanglePoint->xMm : rectanglePoint->yMm;
        const double externalCoordinate =
            xAxis ? externalPoint->xMm : externalPoint->yMm;
        const double signedGap = oldCoordinate - externalCoordinate;
        const double newCoordinate =
            externalCoordinate + std::copysign(constraint.value, signedGap);

        if (externalReference.start != firstReference.start &&
            std::abs(oldCoordinate - currentRectangleCoordinate) > tolerance)
          hasOppositeGap = true;

        const auto existing = std::find_if(
            targets.begin(), targets.end(),
            [oldCoordinate](const AxisTarget& target) {
              return std::abs(target.oldCoordinate - oldCoordinate) <=
                     tolerance;
            });
        if (existing != targets.end()) {
          if (std::abs(existing->newCoordinate - newCoordinate) > tolerance)
            return false;
        } else {
          targets.push_back({oldCoordinate, newCoordinate});
        }
      }
    }

    if (hasOppositeGap && targets.size() >= 2) {
      for (const std::size_t index : rectangleLines) {
        auto& line = lines_[index];
        for (const auto& target : targets) {
          double& startCoordinate = xAxis ? line.start.xMm : line.start.yMm;
          double& endCoordinate = xAxis ? line.end.xMm : line.end.yMm;
          if (std::abs(startCoordinate - target.oldCoordinate) <= tolerance)
            startCoordinate = target.newCoordinate;
          if (std::abs(endCoordinate - target.oldCoordinate) <= tolerance)
            endCoordinate = target.newCoordinate;
        }
      }
      updateBounds();
      return true;
    }

    // Once a rectangle is already positioned by multiple external driving
    // dimensions, another aligned gap must use the remaining row/column DOF
    // instead of translating the complete composite and disturbing its
    // established placement. The stored constraint remains Euclidean
    // Distance; the axis-specific mutator is only a motion candidate.
    std::size_t externalDrivingCount = 0;
    for (const auto& constraint : constraints_) {
      if (constraint.type != ConstraintType::Distance &&
          constraint.type != ConstraintType::DistanceX &&
          constraint.type != ConstraintType::DistanceY)
        continue;
      const bool firstIsRectangle =
          pointBelongsToRectangle(constraint.firstPoint);
      const bool secondIsRectangle =
          pointBelongsToRectangle(constraint.secondPoint);
      if (firstIsRectangle != secondIsRectangle)
        ++externalDrivingCount;
    }

    if (rectangleAxisAligned && rectangleLines.size() == 4 &&
        !axisSizeDriven && externalDrivingCount >= 3) {
      if (xAxis)
        return setPointDistanceX(firstReference, secondReference, distanceMm);
      if (yAxis)
        return setPointDistanceY(firstReference, secondReference, distanceMm);
    }
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

bool Sketch::setPointDistanceX(
    PointReference firstReference,
    PointReference secondReference,
    double distanceMm) {
  journalCapturePoint(firstReference);
  journalCapturePoint(secondReference);
  const auto first = referencedPoint(firstReference);
  const auto second = referencedPoint(secondReference);

  if (!first || !second ||
      !std::isfinite(distanceMm) ||
      distanceMm <= 0.0)
    return false;

  // A horizontal segment dimensioned between its own endpoints is also its
  // line length. Route it through the length mutator so endpoint mobility is
  // resolved from PointOnLine/Coincident anchors instead of always moving the
  // second endpoint (which can be attached to a locked projected carrier).
  const bool ownLineEndpoints =
      !firstReference.origin && !secondReference.origin &&
      firstReference.elementCenterId == 0 &&
      secondReference.elementCenterId == 0 &&
      firstReference.circleId == kInvalidGeometryId &&
      secondReference.circleId == kInvalidGeometryId &&
      firstReference.arcId == kInvalidGeometryId &&
      secondReference.arcId == kInvalidGeometryId &&
      firstReference.lineId != kInvalidGeometryId &&
      firstReference.lineId == secondReference.lineId &&
      firstReference.start != secondReference.start;
  if (ownLineEndpoints &&
      std::abs(second->yMm - first->yMm) <= 1e-7)
    return setLineLengthById(firstReference.lineId, distanceMm);

  const auto rectangleElementForPoint =
      [this](PointReference reference)
          -> std::optional<std::size_t> {
    if (reference.elementCenterId != 0 ||
        reference.circleId != kInvalidGeometryId ||
        reference.lineId == kInvalidGeometryId)
      return std::nullopt;

    const auto index = lineIndex(reference.lineId);
    if (!index)
      return std::nullopt;

    const std::size_t elementId =
        lines_[*index].elementId;

    std::size_t count = 0;
    for (const auto& line : lines_) {
      if (line.elementId == elementId)
        ++count;
    }

    return count == 4
               ? std::optional<std::size_t>{elementId}
               : std::nullopt;
  };

  const auto pointBelongsToElement =
      [this](PointReference reference,
             std::size_t elementId) {
    if (reference.elementCenterId != 0 ||
        reference.circleId != kInvalidGeometryId ||
        reference.lineId == kInvalidGeometryId)
      return false;

    const auto index = lineIndex(reference.lineId);
    return index &&
           lines_[*index].elementId == elementId;
  };

  const auto firstRectangle =
      rectangleElementForPoint(firstReference);
  const auto secondRectangle =
      rectangleElementForPoint(secondReference);

  const double dx = second->xMm - first->xMm;
  const double direction = dx < 0.0 ? -1.0 : 1.0;
  const Point movedSecond{
      first->xMm + direction * distanceMm,
      second->yMm};
  const double moveSecondX =
      movedSecond.xMm - second->xMm;

  // Internal dimension of one rectangle = explicit driving width.
  if (firstRectangle && secondRectangle &&
      *firstRectangle == *secondRectangle) {
    if (isElementLocked(*secondRectangle))
      return false;

    const double oldSecondX = second->xMm;

    for (auto& line : lines_) {
      if (line.elementId != *secondRectangle)
        continue;

      if (std::abs(line.start.xMm - oldSecondX) <= 1e-7)
        line.start.xMm += moveSecondX;

      if (std::abs(line.end.xMm - oldSecondX) <= 1e-7)
        line.end.xMm += moveSecondX;
    }

    updateBounds();
    return true;
  }

  // COUPLED EXTERNAL RECTANGLE X GAPS V4
  //
  // AutoDimension can present either operand order. More importantly, when a
  // second external gap is entered, the NEW constraint has not yet been
  // appended to constraints_. Solve the existing gaps + the current request
  // together before addConstraint() validates the complete system.
  if (static_cast<bool>(firstRectangle) !=
      static_cast<bool>(secondRectangle)) {
    const std::size_t elementId =
        firstRectangle ? *firstRectangle
                       : *secondRectangle;

    const PointReference currentRectangleReference =
        firstRectangle ? firstReference
                       : secondReference;
    const PointReference currentExternalReference =
        firstRectangle ? secondReference
                       : firstReference;

    if (isPointReferenceLocked(currentExternalReference) &&
        !isElementLocked(elementId)) {
      std::vector<std::size_t> members;
      for (std::size_t index = 0;
           index < lines_.size(); ++index) {
        if (lines_[index].elementId == elementId)
          members.push_back(index);
      }

      bool axisAligned = members.size() == 4;
      for (const auto index : members) {
        const double lineDx =
            std::abs(lines_[index].end.xMm -
                     lines_[index].start.xMm);
        const double lineDy =
            std::abs(lines_[index].end.yMm -
                     lines_[index].start.yMm);

        if (lineDx > 1e-7 && lineDy > 1e-7) {
          axisAligned = false;
          break;
        }
      }

      const auto lineBelongsToElement =
          [this, elementId](GeometryId id) {
        const auto index = lineIndex(id);
        return index &&
               lines_[*index].elementId == elementId;
      };

      bool widthDriven = false;

      for (const auto& constraint : constraints_) {
        if (constraint.type == ConstraintType::Length &&
            lineBelongsToElement(
                constraint.firstGeometry)) {
          const auto index =
              lineIndex(constraint.firstGeometry);

          if (index) {
            const double lineDx =
                std::abs(lines_[*index].end.xMm -
                         lines_[*index].start.xMm);
            const double lineDy =
                std::abs(lines_[*index].end.yMm -
                         lines_[*index].start.yMm);

            if (lineDx > lineDy + 1e-7) {
              widthDriven = true;
              break;
            }
          }
        }

        if (constraint.type != ConstraintType::DistanceX &&
            constraint.type != ConstraintType::Distance)
          continue;

        if (!pointBelongsToElement(
                constraint.firstPoint, elementId) ||
            !pointBelongsToElement(
                constraint.secondPoint, elementId))
          continue;

        const auto p1 =
            referencedPoint(constraint.firstPoint);
        const auto p2 =
            referencedPoint(constraint.secondPoint);

        if (!p1 || !p2)
          continue;

        const double internalDx =
            std::abs(p2->xMm - p1->xMm);
        const double internalDy =
            std::abs(p2->yMm - p1->yMm);

        if ((constraint.type ==
                 ConstraintType::DistanceX &&
             internalDx > 1e-7) ||
            (constraint.type ==
                 ConstraintType::Distance &&
             internalDx > 1e-7 &&
             internalDy <= 1e-7)) {
          widthDriven = true;
          break;
        }
      }

      const auto targetForGap =
          [](double rectangleX,
             double externalX,
             double gap) {
        return rectangleX >= externalX
                   ? externalX + gap
                   : externalX - gap;
      };

      // Explicit width means the rectangle is rigid in X. Satisfy the
      // current gap by translating it as a whole; a conflicting second gap
      // will then be rejected transactionally.
      if (widthDriven || !axisAligned) {
        const auto rectanglePoint =
            referencedPoint(
                currentRectangleReference);
        const auto externalPoint =
            referencedPoint(
                currentExternalReference);

        if (!rectanglePoint || !externalPoint)
          return false;

        const double targetX =
            targetForGap(
                rectanglePoint->xMm,
                externalPoint->xMm,
                distanceMm);

        const double moveX =
            targetX - rectanglePoint->xMm;

        for (const auto index : members) {
          lines_[index].start.xMm += moveX;
          lines_[index].end.xMm += moveX;
        }

        updateBounds();
        return true;
      }

      struct GapTarget {
        double oldX{};
        double targetX{};
      };

      std::vector<GapTarget> targets;

      const auto addGapTarget =
          [this, elementId, &targets,
           &pointBelongsToElement,
           &targetForGap](
              PointReference a,
              PointReference b,
              double gap) {
        const bool aRectangle =
            pointBelongsToElement(a, elementId);
        const bool bRectangle =
            pointBelongsToElement(b, elementId);

        if (aRectangle == bRectangle)
          return true;

        const PointReference rectangleRef =
            aRectangle ? a : b;
        const PointReference externalRef =
            aRectangle ? b : a;

        if (!isPointReferenceLocked(externalRef))
          return true;

        const auto rectanglePoint =
            referencedPoint(rectangleRef);
        const auto externalPoint =
            referencedPoint(externalRef);

        if (!rectanglePoint || !externalPoint)
          return false;

        const double oldX =
            rectanglePoint->xMm;
        const double targetX =
            targetForGap(
                oldX,
                externalPoint->xMm,
                gap);

        for (const auto& existing : targets) {
          if (std::abs(existing.oldX - oldX) > 1e-7)
            continue;

          // Two dimensions attempting different positions for the SAME
          // rectangle column are a real overconstraint.
          return std::abs(
                     existing.targetX -
                     targetX) <= 1e-7;
        }

        targets.push_back({oldX, targetX});
        return true;
      };

      // Existing driving gaps.
      for (const auto& constraint : constraints_) {
        if (constraint.type != ConstraintType::DistanceX ||
            !std::isfinite(constraint.value) ||
            constraint.value <= 0.0)
          continue;

        const bool touchesRectangle =
            pointBelongsToElement(
                constraint.firstPoint, elementId) ||
            pointBelongsToElement(
                constraint.secondPoint, elementId);

        if (!touchesRectangle)
          continue;

        if (!addGapTarget(
                constraint.firstPoint,
                constraint.secondPoint,
                constraint.value))
          return false;
      }

      // Current request is intentionally included even before it becomes a
      // stored Constraint. This is what makes 7 mm + free width + 7 mm work.
      if (!addGapTarget(
              firstReference,
              secondReference,
              distanceMm))
        return false;

      if (!targets.empty()) {
        // A single datum/external gap positions the rectangle; it must not
        // consume the still-free width degree of freedom.  Only two distinct
        // constrained columns are allowed to resize it (for example left and
        // right clearances to a reference line).
        if (targets.size() == 1) {
          const double moveX =
              targets.front().targetX - targets.front().oldX;

          for (const auto index : members) {
            lines_[index].start.xMm += moveX;
            lines_[index].end.xMm += moveX;
          }

          updateBounds();
          return true;
        }

        struct EndpointSnapshot {
          double startX{};
          double endX{};
        };

        std::vector<EndpointSnapshot> before;
        before.reserve(members.size());

        for (const auto index : members) {
          before.push_back({
              lines_[index].start.xMm,
              lines_[index].end.xMm});
        }

        for (std::size_t member = 0;
             member < members.size();
             ++member) {
          auto& line =
              lines_[members[member]];

          for (const auto& target : targets) {
            if (std::abs(
                    before[member].startX -
                    target.oldX) <= 1e-7)
              line.start.xMm =
                  target.targetX;

            if (std::abs(
                    before[member].endX -
                    target.oldX) <= 1e-7)
              line.end.xMm =
                  target.targetX;
          }
        }

        updateBounds();
        return true;
      }
    }
  }

  // Generic locked reference: move the unlocked operand instead.
  if (isPointReferenceLocked(secondReference)) {
    if (isPointReferenceLocked(firstReference)) {
      return std::abs(
                 std::abs(dx) -
                 distanceMm) <= 1e-7;
    }

    return setPointDistanceX(
        secondReference,
        firstReference,
        distanceMm);
  }

  if (secondReference.elementCenterId != 0) {
    if (isElementLocked(
            secondReference.elementCenterId))
      return false;

    for (auto& line : lines_) {
      if (line.elementId !=
          secondReference.elementCenterId)
        continue;

      line.start.xMm += moveSecondX;
      line.end.xMm += moveSecondX;
    }

    updateBounds();
    return true;
  }

  if (secondReference.circleId !=
      kInvalidGeometryId) {
    if (isGeometryLocked(
            secondReference.circleId))
      return false;

    const auto circle =
        circleIndex(
            secondReference.circleId);

    if (!circle)
      return false;

    circles_[*circle].center.xMm =
        movedSecond.xMm;
    updateBounds();
    return true;
  }

  const auto secondLineIndex =
      lineIndex(secondReference.lineId);

  if (secondLineIndex) {
    const std::size_t elementId =
        lines_[*secondLineIndex].elementId;

    std::size_t count = 0;
    for (const auto& line : lines_) {
      if (line.elementId == elementId)
        ++count;
    }

    if (count == 4) {
      if (isElementLocked(elementId))
        return false;

      for (auto& line : lines_) {
        if (line.elementId != elementId)
          continue;

        line.start.xMm += moveSecondX;
        line.end.xMm += moveSecondX;
      }

      updateBounds();
      return true;
    }
  }

  const auto same = [](Point a, Point b) {
    return std::hypot(
               a.xMm - b.xMm,
               a.yMm - b.yMm) <= 1e-7;
  };

  for (auto& line : lines_) {
    if (same(line.start, *second))
      line.start = movedSecond;

    if (same(line.end, *second))
      line.end = movedSecond;
  }

  updateBounds();
  return true;
}

bool Sketch::setPointDistanceY(
    PointReference firstReference,
    PointReference secondReference,
    double distanceMm) {
  journalCapturePoint(firstReference);
  journalCapturePoint(secondReference);
  const auto first = referencedPoint(firstReference);
  const auto second = referencedPoint(secondReference);

  if (!first || !second ||
      !std::isfinite(distanceMm) ||
      distanceMm <= 0.0)
    return false;

  // Vertical own-endpoint dimensions use the same anchor-aware path as an
  // explicit line length. This preserves an endpoint constrained to a locked
  // carrier and moves the free end regardless of selection order.
  const bool ownLineEndpoints =
      !firstReference.origin && !secondReference.origin &&
      firstReference.elementCenterId == 0 &&
      secondReference.elementCenterId == 0 &&
      firstReference.circleId == kInvalidGeometryId &&
      secondReference.circleId == kInvalidGeometryId &&
      firstReference.arcId == kInvalidGeometryId &&
      secondReference.arcId == kInvalidGeometryId &&
      firstReference.lineId != kInvalidGeometryId &&
      firstReference.lineId == secondReference.lineId &&
      firstReference.start != secondReference.start;
  if (ownLineEndpoints &&
      std::abs(second->xMm - first->xMm) <= 1e-7)
    return setLineLengthById(firstReference.lineId, distanceMm);

  const auto rectangleElementForPoint =
      [this](PointReference reference)
          -> std::optional<std::size_t> {
    if (reference.elementCenterId != 0 ||
        reference.circleId != kInvalidGeometryId ||
        reference.lineId == kInvalidGeometryId)
      return std::nullopt;

    const auto index = lineIndex(reference.lineId);
    if (!index)
      return std::nullopt;

    const std::size_t elementId =
        lines_[*index].elementId;

    std::size_t count = 0;
    for (const auto& line : lines_) {
      if (line.elementId == elementId)
        ++count;
    }

    return count == 4
               ? std::optional<std::size_t>{elementId}
               : std::nullopt;
  };

  const auto pointBelongsToElement =
      [this](PointReference reference,
             std::size_t elementId) {
    if (reference.elementCenterId != 0 ||
        reference.circleId != kInvalidGeometryId ||
        reference.lineId == kInvalidGeometryId)
      return false;

    const auto index = lineIndex(reference.lineId);
    return index &&
           lines_[*index].elementId == elementId;
  };

  const auto firstRectangle =
      rectangleElementForPoint(firstReference);
  const auto secondRectangle =
      rectangleElementForPoint(secondReference);

  const double dy = second->yMm - first->yMm;
  const double direction = dy < 0.0 ? -1.0 : 1.0;
  const Point movedSecond{
      second->xMm,
      first->yMm + direction * distanceMm};
  const double moveSecondY =
      movedSecond.yMm - second->yMm;

  // Internal dimension of one rectangle = explicit driving height.
  if (firstRectangle && secondRectangle &&
      *firstRectangle == *secondRectangle) {
    if (isElementLocked(*secondRectangle))
      return false;

    const double oldSecondY = second->yMm;

    for (auto& line : lines_) {
      if (line.elementId != *secondRectangle)
        continue;

      if (std::abs(line.start.yMm - oldSecondY) <= 1e-7)
        line.start.yMm += moveSecondY;

      if (std::abs(line.end.yMm - oldSecondY) <= 1e-7)
        line.end.yMm += moveSecondY;
    }

    updateBounds();
    return true;
  }

  // COUPLED EXTERNAL RECTANGLE Y GAPS V4
  if (static_cast<bool>(firstRectangle) !=
      static_cast<bool>(secondRectangle)) {
    const std::size_t elementId =
        firstRectangle ? *firstRectangle
                       : *secondRectangle;

    const PointReference currentRectangleReference =
        firstRectangle ? firstReference
                       : secondReference;
    const PointReference currentExternalReference =
        firstRectangle ? secondReference
                       : firstReference;

    if (isPointReferenceLocked(currentExternalReference) &&
        !isElementLocked(elementId)) {
      std::vector<std::size_t> members;
      for (std::size_t index = 0;
           index < lines_.size(); ++index) {
        if (lines_[index].elementId == elementId)
          members.push_back(index);
      }

      bool axisAligned = members.size() == 4;
      for (const auto index : members) {
        const double lineDx =
            std::abs(lines_[index].end.xMm -
                     lines_[index].start.xMm);
        const double lineDy =
            std::abs(lines_[index].end.yMm -
                     lines_[index].start.yMm);

        if (lineDx > 1e-7 && lineDy > 1e-7) {
          axisAligned = false;
          break;
        }
      }

      const auto lineBelongsToElement =
          [this, elementId](GeometryId id) {
        const auto index = lineIndex(id);
        return index &&
               lines_[*index].elementId == elementId;
      };

      bool heightDriven = false;

      for (const auto& constraint : constraints_) {
        if (constraint.type == ConstraintType::Length &&
            lineBelongsToElement(
                constraint.firstGeometry)) {
          const auto index =
              lineIndex(constraint.firstGeometry);

          if (index) {
            const double lineDx =
                std::abs(lines_[*index].end.xMm -
                         lines_[*index].start.xMm);
            const double lineDy =
                std::abs(lines_[*index].end.yMm -
                         lines_[*index].start.yMm);

            if (lineDy > lineDx + 1e-7) {
              heightDriven = true;
              break;
            }
          }
        }

        if (constraint.type != ConstraintType::DistanceY &&
            constraint.type != ConstraintType::Distance)
          continue;

        if (!pointBelongsToElement(
                constraint.firstPoint, elementId) ||
            !pointBelongsToElement(
                constraint.secondPoint, elementId))
          continue;

        const auto p1 =
            referencedPoint(constraint.firstPoint);
        const auto p2 =
            referencedPoint(constraint.secondPoint);

        if (!p1 || !p2)
          continue;

        const double internalDx =
            std::abs(p2->xMm - p1->xMm);
        const double internalDy =
            std::abs(p2->yMm - p1->yMm);

        if ((constraint.type ==
                 ConstraintType::DistanceY &&
             internalDy > 1e-7) ||
            (constraint.type ==
                 ConstraintType::Distance &&
             internalDy > 1e-7 &&
             internalDx <= 1e-7)) {
          heightDriven = true;
          break;
        }
      }

      const auto targetForGap =
          [](double rectangleY,
             double externalY,
             double gap) {
        return rectangleY >= externalY
                   ? externalY + gap
                   : externalY - gap;
      };

      if (heightDriven || !axisAligned) {
        const auto rectanglePoint =
            referencedPoint(
                currentRectangleReference);
        const auto externalPoint =
            referencedPoint(
                currentExternalReference);

        if (!rectanglePoint || !externalPoint)
          return false;

        const double targetY =
            targetForGap(
                rectanglePoint->yMm,
                externalPoint->yMm,
                distanceMm);

        const double moveY =
            targetY - rectanglePoint->yMm;

        for (const auto index : members) {
          lines_[index].start.yMm += moveY;
          lines_[index].end.yMm += moveY;
        }

        updateBounds();
        return true;
      }

      struct GapTarget {
        double oldY{};
        double targetY{};
      };

      std::vector<GapTarget> targets;

      const auto addGapTarget =
          [this, elementId, &targets,
           &pointBelongsToElement,
           &targetForGap](
              PointReference a,
              PointReference b,
              double gap) {
        const bool aRectangle =
            pointBelongsToElement(a, elementId);
        const bool bRectangle =
            pointBelongsToElement(b, elementId);

        if (aRectangle == bRectangle)
          return true;

        const PointReference rectangleRef =
            aRectangle ? a : b;
        const PointReference externalRef =
            aRectangle ? b : a;

        if (!isPointReferenceLocked(externalRef))
          return true;

        const auto rectanglePoint =
            referencedPoint(rectangleRef);
        const auto externalPoint =
            referencedPoint(externalRef);

        if (!rectanglePoint || !externalPoint)
          return false;

        const double oldY =
            rectanglePoint->yMm;
        const double targetY =
            targetForGap(
                oldY,
                externalPoint->yMm,
                gap);

        for (const auto& existing : targets) {
          if (std::abs(existing.oldY - oldY) > 1e-7)
            continue;

          return std::abs(
                     existing.targetY -
                     targetY) <= 1e-7;
        }

        targets.push_back({oldY, targetY});
        return true;
      };

      for (const auto& constraint : constraints_) {
        if (constraint.type != ConstraintType::DistanceY ||
            !std::isfinite(constraint.value) ||
            constraint.value <= 0.0)
          continue;

        const bool touchesRectangle =
            pointBelongsToElement(
                constraint.firstPoint, elementId) ||
            pointBelongsToElement(
                constraint.secondPoint, elementId);

        if (!touchesRectangle)
          continue;

        if (!addGapTarget(
                constraint.firstPoint,
                constraint.secondPoint,
                constraint.value))
          return false;
      }

      if (!addGapTarget(
              firstReference,
              secondReference,
              distanceMm))
        return false;

      if (!targets.empty()) {
        // One external Y gap defines position, not height.  Keep the whole
        // rectangle rigid until distinct constraints address both rows.
        if (targets.size() == 1) {
          const double moveY =
              targets.front().targetY - targets.front().oldY;

          for (const auto index : members) {
            lines_[index].start.yMm += moveY;
            lines_[index].end.yMm += moveY;
          }

          updateBounds();
          return true;
        }

        struct EndpointSnapshot {
          double startY{};
          double endY{};
        };

        std::vector<EndpointSnapshot> before;
        before.reserve(members.size());

        for (const auto index : members) {
          before.push_back({
              lines_[index].start.yMm,
              lines_[index].end.yMm});
        }

        for (std::size_t member = 0;
             member < members.size();
             ++member) {
          auto& line =
              lines_[members[member]];

          for (const auto& target : targets) {
            if (std::abs(
                    before[member].startY -
                    target.oldY) <= 1e-7)
              line.start.yMm =
                  target.targetY;

            if (std::abs(
                    before[member].endY -
                    target.oldY) <= 1e-7)
              line.end.yMm =
                  target.targetY;
          }
        }

        updateBounds();
        return true;
      }
    }
  }

  if (isPointReferenceLocked(secondReference)) {
    if (isPointReferenceLocked(firstReference)) {
      return std::abs(
                 std::abs(dy) -
                 distanceMm) <= 1e-7;
    }

    return setPointDistanceY(
        secondReference,
        firstReference,
        distanceMm);
  }

  if (secondReference.elementCenterId != 0) {
    if (isElementLocked(
            secondReference.elementCenterId))
      return false;

    for (auto& line : lines_) {
      if (line.elementId !=
          secondReference.elementCenterId)
        continue;

      line.start.yMm += moveSecondY;
      line.end.yMm += moveSecondY;
    }

    updateBounds();
    return true;
  }

  if (secondReference.circleId !=
      kInvalidGeometryId) {
    if (isGeometryLocked(
            secondReference.circleId))
      return false;

    const auto circle =
        circleIndex(
            secondReference.circleId);

    if (!circle)
      return false;

    circles_[*circle].center.yMm =
        movedSecond.yMm;
    updateBounds();
    return true;
  }

  const auto secondLineIndex =
      lineIndex(secondReference.lineId);

  if (secondLineIndex) {
    const std::size_t elementId =
        lines_[*secondLineIndex].elementId;

    std::size_t count = 0;
    for (const auto& line : lines_) {
      if (line.elementId == elementId)
        ++count;
    }

    if (count == 4) {
      if (isElementLocked(elementId))
        return false;

      for (auto& line : lines_) {
        if (line.elementId != elementId)
          continue;

        line.start.yMm += moveSecondY;
        line.end.yMm += moveSecondY;
      }

      updateBounds();
      return true;
    }
  }

  const auto same = [](Point a, Point b) {
    return std::hypot(
               a.xMm - b.xMm,
               a.yMm - b.yMm) <= 1e-7;
  };

  for (auto& line : lines_) {
    if (same(line.start, *second))
      line.start = movedSecond;

    if (same(line.end, *second))
      line.end = movedSecond;
  }

  updateBounds();
  return true;
}
bool Sketch::setCircleDiameter(std::size_t index, double diameterMm) {
  if (index < circleIds_.size())
    journalCaptureComponents({circleIds_[index]});
  if (index >= circles_.size() || diameterMm <= 0.0) return false;
  // LOCK CONSTRAINT: direct indexed diameter edit is blocked.
  if (index < circleIds_.size() &&
      isGeometryLocked(circleIds_[index]))
    return std::abs(circles_[index].radiusMm * 2.0 -
                    diameterMm) <= 1e-7;
  circles_[index].radiusMm = diameterMm * 0.5;
  updateBounds();
  return true;
}

void Sketch::addDimension(Dimension dimension) { storeDimension(dimension); }

void Sketch::storeDimension(const Dimension& dimension) {
  if (dimension.valueMm > 0.0) {
    Dimension stored = dimension;
    const bool needsGeneratedId =
        stored.id == kInvalidDimensionId || dimensionIndex(stored.id);
    if (needsGeneratedId) {
      const auto generated = firstFreeDimensionId(dimensions_, nextDimensionId_);
      if (!generated) return;
      stored.id = *generated;
    }
    DimensionId nextStart = nextDimensionId_;
    if (stored.id >= nextStart) {
      nextStart = stored.id == std::numeric_limits<DimensionId>::max()
          ? DimensionId{1}
          : stored.id + 1;
    }
    const auto next = firstFreeDimensionId(dimensions_, nextStart, stored.id);
    if (!next) return;
    journalRecordAddedDimension(dimensions_.size());
    dimensions_.push_back(std::move(stored));
    nextDimensionId_ = *next;
  }
}

void Sketch::clearDimensions() {
  while (!dimensions_.empty())
    static_cast<void>(removeDimension(dimensions_.size() - 1));
}

bool Sketch::removeDimension(std::size_t index) {
  if (index >= dimensions_.size()) return false;
  for (const auto& journal : deltaJournals_)
    if (journal.dimensionTokens.size() != dimensions_.size())
      return false;
  journalCaptureDimension(index);
  dimensions_.erase(dimensions_.begin() + static_cast<std::ptrdiff_t>(index));
  for (auto& journal : deltaJournals_)
    journal.dimensionTokens.erase(
        journal.dimensionTokens.begin() + static_cast<std::ptrdiff_t>(index));
  return true;
}

bool Sketch::setDimensionPlacement(std::size_t index, double offsetMm,
                                   double angleRad) {
  auto& dimensions = dimensions_;
  if (index >= dimensions.size()) return false;
  journalCaptureDimension(index);
  dimensions[index].offsetMm = offsetMm;
  dimensions[index].angleRad = angleRad;
  return true;
}

bool Sketch::setDimensionValue(std::size_t index, double valueMm) {
  auto& dimensions = dimensions_;
  if (index >= dimensions.size() || valueMm <= 0.0) return false;
  journalCaptureDimension(index);
  dimensions[index].valueMm = valueMm;
  return true;
}

ConstraintId Sketch::addConstraint(
    Constraint constraint) {
  // TRANSACTIONAL CONSTRAINT ADD
  //
  // A new user constraint may move geometry, but it may NOT make an older
  // previously-satisfied constraint false. If the complete system cannot be
  // solved, restore both geometry and the old constraint set.
  //
  // Explicit IDs are used by project loading; that path stays permissive so
  // old files can still be opened and diagnosed.
  const bool transactional =
      constraint.id == kInvalidConstraintId;

  ConstraintDiagnostics before;
  std::vector<ConstraintId> previouslySatisfied;

  if (transactional) {
    beginDeltaJournal();
    if (failNextNestedConstraintJournal.exchange(false))
      throw std::runtime_error("injected nested journal failure");
    before = analyzeConstraintSystem(*this, false);
    for (const auto& old : constraints_)
      if (!hasConstraintViolation(before, old.id))
        previouslySatisfied.push_back(old.id);
  }

  journalCaptureComponents(
      {constraint.firstGeometry, constraint.secondGeometry});
  journalCapturePoint(constraint.firstPoint);
  journalCapturePoint(constraint.secondPoint);

  if (constraint.id == kInvalidConstraintId)
    constraint.id = nextConstraintId_++;
  else
    nextConstraintId_ =
        std::max(nextConstraintId_,
                 constraint.id + 1);

  const ConstraintId addedId =
      constraint.id;

  constraints_.push_back(constraint);
  journalRecordAddedConstraint(addedId, constraints_.size() - 1);
  invalidateStructureIndexes();
  (void)BasicSketchSolver::solveStable(*this);

  if (transactional) {
    const auto after =
        analyzeConstraintSystem(*this, false);

    bool oldConstraintBroken = false;

    for (const auto oldId : previouslySatisfied) {
      if (hasConstraintViolation(after, oldId)) {
        oldConstraintBroken = true;
        break;
      }
    }

    if (hasConstraintViolation(
            after, addedId) ||
        oldConstraintBroken) {
      static_cast<void>(cancelDeltaJournal());
      return kInvalidConstraintId;
    }
    static_cast<void>(finishDeltaJournal());
  }

  return addedId;
}

ConstraintApplyResult Sketch::tryApplyConstraint(Constraint constraint,
                                                 bool commitRedundant) {
  ConstraintApplyResult result;
  const auto reject = [&result](ConstraintApplyStatus status,
                                std::string diagnostic) {
    result.status = status;
    result.diagnostic = std::move(diagnostic);
    return result;
  };

  const int type = static_cast<int>(constraint.type);
  if (type < static_cast<int>(ConstraintType::Horizontal) ||
      type > static_cast<int>(ConstraintType::PointOnYAxis) ||
      !std::isfinite(constraint.value))
    return reject(ConstraintApplyStatus::Unsupported,
                  "Unsupported constraint type or value");

  if ((constraint.firstGeometry != kInvalidGeometryId &&
       !geometryLocation(constraint.firstGeometry)) ||
      (constraint.secondGeometry != kInvalidGeometryId &&
       !geometryLocation(constraint.secondGeometry)) ||
      (!emptyPointReference(constraint.firstPoint) &&
       !validPointReferenceForConstraint(*this, constraint.firstPoint)) ||
      (!emptyPointReference(constraint.secondPoint) &&
       !validPointReferenceForConstraint(*this, constraint.secondPoint)) ||
      (constraint.id != kInvalidConstraintId &&
       constraintIndex(constraint.id)))
    return reject(ConstraintApplyStatus::InvalidReference,
                  "Constraint contains a stale or malformed reference");

  if (!supportedConstraintSchema(*this, constraint))
    return reject(ConstraintApplyStatus::Unsupported,
                  "Constraint geometry combination is not supported");

  const auto seeds = referencedGeometryIds(*this, constraint);
  if (seeds.empty())
    return reject(ConstraintApplyStatus::InvalidReference,
                  "Constraint does not reference sketch geometry");

  const auto duplicate = std::find_if(
      constraints_.begin(), constraints_.end(),
      [&constraint](const Constraint& old) {
        return equivalentConstraint(old, constraint);
      });
  if (duplicate != constraints_.end()) {
    result.status = ConstraintApplyStatus::Redundant;
    result.relatedConstraintIds = {duplicate->id};
    result.diagnostic = "Equivalent constraint already exists";
    return result;
  }

  const auto affectedBefore = connectedComponent(seeds);
  result.relatedConstraintIds = affectedBefore.constraintIds;
  const auto sameGeometryPair = [&constraint](const Constraint& old) {
    return (old.firstGeometry == constraint.firstGeometry &&
            old.secondGeometry == constraint.secondGeometry) ||
           (old.firstGeometry == constraint.secondGeometry &&
            old.secondGeometry == constraint.firstGeometry);
  };
  for (const auto& old : constraints_) {
    bool conflicts = false;
    if (old.firstGeometry == constraint.firstGeometry) {
      conflicts =
          (constraint.type == ConstraintType::Horizontal &&
           old.type == ConstraintType::Vertical) ||
          (constraint.type == ConstraintType::Vertical &&
           old.type == ConstraintType::Horizontal);
    }
    if (sameGeometryPair(old)) {
      if (constraint.type == ConstraintType::Parallel)
        conflicts = conflicts || old.type == ConstraintType::Perpendicular ||
                    (old.type == ConstraintType::Angle &&
                     std::abs(old.value) > 1e-9 &&
                     std::abs(old.value - 180.0) > 1e-9);
      else if (constraint.type == ConstraintType::Perpendicular)
        conflicts = conflicts || old.type == ConstraintType::Parallel ||
                    (old.type == ConstraintType::Angle &&
                     std::abs(old.value - 90.0) > 1e-9);
      else if (constraint.type == ConstraintType::Angle)
        conflicts = conflicts ||
                    (old.type == ConstraintType::Parallel &&
                     std::abs(constraint.value) > 1e-9 &&
                     std::abs(constraint.value - 180.0) > 1e-9) ||
                    (old.type == ConstraintType::Perpendicular &&
                     std::abs(constraint.value - 90.0) > 1e-9);
    }
    if (conflicts) {
      result.status = ConstraintApplyStatus::Conflicting;
      result.relatedConstraintIds = {old.id};
      result.diagnostic =
          "Constraint conflicts with an existing orientation relationship";
      return result;
    }
  }
  const auto before = analyzeConstraintSystem(*this, true);
  std::vector<ConstraintId> previouslySatisfied;
  for (const auto& old : constraints_)
    if (!hasConstraintViolation(before, old.id))
      previouslySatisfied.push_back(old.id);

  const auto originalFingerprint = semanticFingerprint();
  beginDeltaJournal();
  if (failNextNestedConstraintJournal.exchange(false))
    throw std::runtime_error("injected nested journal failure");
  journalCaptureComponents(seeds);
  journalCapturePoint(constraint.firstPoint);
  journalCapturePoint(constraint.secondPoint);

  if (constraint.id == kInvalidConstraintId)
    constraint.id = nextConstraintId_++;
  else
    nextConstraintId_ = std::max(nextConstraintId_, constraint.id + 1);
  result.constraintId = constraint.id;
  constraints_.push_back(constraint);
  journalRecordAddedConstraint(constraint.id, constraints_.size() - 1);
  invalidateStructureIndexes();

  const SolveResult solved =
      BasicSketchSolver::solveStableComponent(*this, seeds);
  const auto after = analyzeConstraintSystem(*this, true);
  bool oldConstraintBroken = false;
  for (const auto id : previouslySatisfied) {
    if (hasConstraintViolation(after, id)) {
      oldConstraintBroken = true;
      break;
    }
  }

  ConstraintApplyStatus failure = ConstraintApplyStatus::Accepted;
  std::string diagnostic;
  if (solved.invalidReferences != 0) {
    failure = ConstraintApplyStatus::InvalidReference;
    diagnostic = "Solver rejected a constraint reference";
  } else if (solved.unsupported != 0) {
    failure = ConstraintApplyStatus::Unsupported;
    diagnostic = "Solver does not support this constraint";
  } else if (hasConstraintViolation(after, constraint.id) ||
             oldConstraintBroken) {
    failure = ConstraintApplyStatus::Conflicting;
    diagnostic = "Constraint conflicts with existing dependencies";
  } else if (!solved.converged) {
    failure = ConstraintApplyStatus::SolverFailed;
    diagnostic = "Constraint solver did not converge";
  } else if (after.equationRank <= before.equationRank) {
    failure = ConstraintApplyStatus::Redundant;
    diagnostic = "Constraint does not add an independent equation";
  }

  if (failure == ConstraintApplyStatus::Redundant && commitRedundant) {
    static_cast<void>(finishDeltaJournal());
    updateBounds();
    result.status = failure;
    result.diagnostic = std::move(diagnostic);
    return result;
  }

  if (failure != ConstraintApplyStatus::Accepted) {
    static_cast<void>(cancelDeltaJournal());
    result.constraintId = kInvalidConstraintId;
    result.status = failure;
    result.diagnostic = std::move(diagnostic);
    if (semanticFingerprint() != originalFingerprint)
      result.diagnostic += "; rollback verification failed";
    return result;
  }

  static_cast<void>(finishDeltaJournal());
  updateBounds();
  result.status = ConstraintApplyStatus::Accepted;
  result.diagnostic = "Constraint accepted";
  return result;
}

bool Sketch::restoreConstraints(std::vector<Constraint> constraints) {
  for (const auto& constraint : constraints) {
    const int type = static_cast<int>(constraint.type);
    if (type < static_cast<int>(ConstraintType::Horizontal) ||
        type > static_cast<int>(ConstraintType::PointOnYAxis))
      return false;
  }
  Sketch geometrySnapshot = *this;
  constraints_ = std::move(constraints);
  invalidateStructureIndexes();
  nextConstraintId_ = 1;
  for (const auto& constraint : constraints_)
    nextConstraintId_ = std::max(nextConstraintId_, constraint.id + 1);
  const SolveResult solved = BasicSketchSolver::solveStable(*this);
  updateBounds();
  // The persistence boundary validates every reference before this call.
  // Solver counters also cover valid diagnostic states: locked no-ops and
  // incompatible known relationships. Preserve such sketches without a
  // partially applied solve; the editor can surface their diagnostics.
  if (!solved.converged || solved.violatedConstraints != 0 ||
      solved.unsupported != 0 || solved.invalidReferences != 0) {
    // A conflicting/over-constrained sketch is still valid editable CAD data:
    // preserve its serialized geometry and constraints for diagnostics instead
    // of committing a partially converged solver state.
    auto unresolvedConstraints = std::move(constraints_);
    const ConstraintId restoredNextConstraintId = nextConstraintId_;
    *this = std::move(geometrySnapshot);
    constraints_ = std::move(unresolvedConstraints);
    invalidateStructureIndexes();
    nextConstraintId_ = restoredNextConstraintId;
    updateBounds();
  }
  return true;
}

bool Sketch::setConstraintValue(ConstraintId id, double value) {
  if (id == kInvalidConstraintId || !std::isfinite(value) || value <= 0.0)
    return false;
  const auto found = std::find_if(
      constraints_.begin(), constraints_.end(),
      [id](const Constraint& constraint) { return constraint.id == id; });
  if (found == constraints_.end()) return false;

  beginDeltaJournal();
  journalCaptureComponents(
      {found->firstGeometry, found->secondGeometry});
  journalCapturePoint(found->firstPoint);
  journalCapturePoint(found->secondPoint);
  journalCaptureConstraint(id);
  const auto before = analyzeConstraintSystem(*this, false);
  std::vector<ConstraintId> previouslySatisfied;
  for (const auto& old : constraints_)
    if (!hasConstraintViolation(before, old.id))
      previouslySatisfied.push_back(old.id);
  found->value = value;
  hasLastSolvedFingerprint_ = false;
  (void)BasicSketchSolver::solveStable(*this);
  const auto after = analyzeConstraintSystem(*this, false);

  bool previouslyValidConstraintBroke = false;
  for (const auto oldId : previouslySatisfied) {
    if (hasConstraintViolation(after, oldId)) {
      previouslyValidConstraintBroke = true;
      break;
    }
  }
  if (hasConstraintViolation(after, id) || previouslyValidConstraintBroke) {
    static_cast<void>(cancelDeltaJournal());
    return false;
  }
  static_cast<void>(finishDeltaJournal());
  updateBounds();
  return true;
}

bool Sketch::removeConstraint(ConstraintId id) {
  journalCaptureConstraint(id);
  const auto oldSize = constraints_.size();
  std::erase_if(constraints_, [id](const Constraint& constraint) {
    return constraint.id == id;
  });
  if (constraints_.size() != oldSize) invalidateStructureIndexes();
  return constraints_.size() != oldSize;
}

void Sketch::clearConstraints() {
  for (const auto& item : constraints_) journalCaptureConstraint(item.id);
  constraints_.clear();
  invalidateStructureIndexes();
}

const std::vector<Constraint>& Sketch::constraints() const noexcept {
  return constraints_;
}

double Sketch::widthMm() const noexcept { return widthMm_; }
double Sketch::heightMm() const noexcept { return heightMm_; }
const std::vector<Line>& Sketch::lines() const noexcept { return lines_; }
const std::vector<Circle>& Sketch::circles() const noexcept { return circles_; }
const std::vector<Arc>& Sketch::arcs() const noexcept { return arcs_; }
const std::vector<Dimension>& Sketch::dimensions() const {
  return dimensions_;
}

void Sketch::updateBounds() noexcept {
  hasLastSolvedFingerprint_ = false;
  connectivityDirty_ = true;
  if (lines_.empty() && circles_.empty() && arcs_.empty()) {
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

  constexpr double kPi = 3.14159265358979323846;
  const auto normalizedAngle = [](double angle) {
    constexpr double twoPi = 6.28318530717958647692;
    angle = std::fmod(angle, twoPi);
    if (angle < 0.0) angle += twoPi;
    return angle;
  };
  const auto angleOnArc = [&](double angle, const Arc& arc) {
    const double start = normalizedAngle(arc.startAngleRad);
    const double delta = normalizedAngle(normalizedAngle(angle) - start);
    return delta <= arc.sweepAngleRad + 1e-12;
  };

  for (const auto& arc : arcs_) {
    include(arcStartPoint(arc));
    include(arcEndPoint(arc));
    for (const double angle : {0.0, 0.5 * kPi, kPi, 1.5 * kPi}) {
      if (!angleOnArc(angle, arc)) continue;
      include({arc.center.xMm + arc.radiusMm * std::cos(angle),
               arc.center.yMm + arc.radiusMm * std::sin(angle)});
    }
  }
  widthMm_ = maxX - minX;
  heightMm_ = maxY - minY;
}

bool Sketch::isClosed() const noexcept {
  const auto samePoint = [](Point first, Point second) {
    // Match the wire builder's modelling tolerance. Interactive snapping and
    // imported decimal coordinates may leave sub-micron endpoint noise that
    // is topologically closed for OCCT and must not be rejected here first.
    return std::abs(first.xMm - second.xMm) <= 1e-6 &&
           std::abs(first.yMm - second.yMm) <= 1e-6;
  };

  std::vector<std::pair<Point, Point>> edges;
  for (const auto& line : lines_) {
    if (!line.dashed) edges.push_back({line.start, line.end});
  }
  for (const auto& arc : arcs_) {
    if (!arc.dashed) edges.push_back({arcStartPoint(arc), arcEndPoint(arc)});
  }

  const std::size_t solidCircles = static_cast<std::size_t>(std::count_if(
      circles_.begin(), circles_.end(),
      [](const Circle& circle) { return !circle.dashed; }));

  if (edges.empty()) return solidCircles == 1;
  if (solidCircles != 0) return false;

  // Every vertex of one or several independent closed loops has degree two.
  for (const auto& edge : edges) {
    for (const Point vertex : {edge.first, edge.second}) {
      std::size_t degree = 0;
      for (const auto& candidate : edges) {
        if (samePoint(vertex, candidate.first)) ++degree;
        if (samePoint(vertex, candidate.second)) ++degree;
      }
      if (degree != 2) return false;
    }
  }
  return true;
}

}  // namespace solidar::sketch
