#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace solidar::sketch {

using GeometryId = std::uint64_t;
inline constexpr GeometryId kInvalidGeometryId = 0;

using ConstraintId = std::uint64_t;
inline constexpr ConstraintId kInvalidConstraintId = 0;

using DimensionId = std::uint64_t;
inline constexpr DimensionId kInvalidDimensionId = 0;

struct Point {
  double xMm{};
  double yMm{};
};

struct Line {
  Point start;
  Point end;
  std::size_t elementId{};
  bool dashed{false};
};

struct Circle {
  Point center;
  double radiusMm{};
  bool dashed{false};
};

struct Arc {
  Point center;
  double radiusMm{};
  double startAngleRad{};
  double sweepAngleRad{};
  bool dashed{false};
};

[[nodiscard]] Point arcStartPoint(const Arc& arc) noexcept;
[[nodiscard]] Point arcEndPoint(const Arc& arc) noexcept;

struct PointReference {
  // Line endpoint reference. Existing code and project files use these.
  GeometryId lineId{kInvalidGeometryId};
  bool start{true};

  // Optional circle-center reference. When this is valid, the reference
  // denotes the center of that circle instead of a line endpoint.
  GeometryId circleId{kInvalidGeometryId};

  // Optional center node of a composite element (currently a rectangle
  // created with RectangleMode::FromCenter).
  std::size_t elementCenterId{0};

  // Optional arc-endpoint reference. `start` selects the first/last endpoint
  // exactly as it does for line endpoints. Kept as the final aggregate field
  // so existing PointReference initializers remain source-compatible.
  GeometryId arcId{kInvalidGeometryId};

  // Immutable sketch datum. When set, this reference denotes the origin
  // (0, 0) rather than model geometry. Axis dimensions use the origin with a
  // DistanceX/DistanceY constraint, so they participate in the normal solver.
  bool origin{false};
};

enum class DimensionKind {
  LineLength,
  PointDistance,
  CircleDiameter,
  PointDistanceX,
  PointDistanceY,
  LineAngle,
  LineDistance
};

struct Dimension {
  DimensionKind kind{DimensionKind::LineLength};
  GeometryId geometryId{kInvalidGeometryId};
  PointReference firstPoint{};
  PointReference secondPoint{};
  double valueMm{};
  double offsetMm{4.0};
  double angleRad{};
  // Persistent annotation identity. Kept last so historical aggregate
  // initializers remain source-compatible.
  DimensionId id{kInvalidDimensionId};
};

enum class ConstraintType {
  Horizontal,
  Vertical,
  Coincident,
  PointOnLine,
  Distance,
  Length,
  Radius,
  Diameter,
  Parallel,
  Perpendicular,
  Equal,
  Angle,
  DistanceX,
  DistanceY,
  PointOnCircle,
  Tangent,
  LineDistance,
  Lock,
  PointOnArc,
  Midpoint,
  PointOnXAxis,
  PointOnYAxis};

struct Constraint {
  ConstraintId id{kInvalidConstraintId};
  ConstraintType type{ConstraintType::Horizontal};
  GeometryId firstGeometry{kInvalidGeometryId};
  GeometryId secondGeometry{kInvalidGeometryId};
  PointReference firstPoint{};
  PointReference secondPoint{};
  double value{};
};

enum class GeometryKind { Line, Circle, Arc };

struct GeometryLocation {
  GeometryKind kind{GeometryKind::Line};
  std::size_t index{};
  bool operator==(const GeometryLocation&) const = default;
};

struct ConstraintComponent {
  std::vector<GeometryId> geometryIds;
  std::vector<ConstraintId> constraintIds;
};

template <typename T>
struct IndexedValueDelta {
  // Indices belong to the complete state on their respective side of the
  // transition.  Keeping both is essential when a single journal removes an
  // earlier entity and edits a later one: the entity's index is then
  // different while undoing the same delta.
  std::size_t beforeIndex{};
  std::size_t afterIndex{};
  std::optional<T> before;
  std::optional<T> after;
};

struct SketchDelta {
  std::vector<IndexedValueDelta<Line>> lines;
  std::vector<IndexedValueDelta<Circle>> circles;
  std::vector<IndexedValueDelta<Arc>> arcs;
  std::vector<IndexedValueDelta<GeometryId>> lineIds;
  std::vector<IndexedValueDelta<GeometryId>> circleIds;
  std::vector<IndexedValueDelta<GeometryId>> arcIds;
  std::vector<IndexedValueDelta<Dimension>> dimensions;
  std::vector<IndexedValueDelta<Constraint>> constraints;
  std::vector<IndexedValueDelta<std::size_t>> centerNodeElementIds;
  std::size_t beforeNextElementId{};
  std::size_t afterNextElementId{};
  GeometryId beforeNextGeometryId{};
  GeometryId afterNextGeometryId{};
  ConstraintId beforeNextConstraintId{};
  ConstraintId afterNextConstraintId{};
  DimensionId beforeNextDimensionId{};
  DimensionId afterNextDimensionId{};
  std::uint64_t beforeSemanticFingerprint{};
  std::uint64_t afterSemanticFingerprint{};
  std::vector<ConstraintId> beforeInvalidConstraintIds;
  std::vector<ConstraintId> afterInvalidConstraintIds;
  std::size_t retainedBytes{};
  [[nodiscard]] bool empty() const noexcept;
};

class Sketch final {
 public:
  Sketch();
  Sketch(const Sketch& other);
  Sketch& operator=(const Sketch& other);
  Sketch(Sketch&&) noexcept = default;
  Sketch& operator=(Sketch&&) noexcept = default;

  // Starts a mutation journal without copying the Sketch. Mutators append
  // stable-ID/range before-images as they touch entities; finish materializes
  // a reversible compact delta.
  void beginDeltaJournal();
  [[nodiscard]] SketchDelta finishDeltaJournal();
  [[nodiscard]] SketchDelta cancelDeltaJournal();
  [[nodiscard]] bool deltaJournalActive() const noexcept;
  [[nodiscard]] std::size_t deltaJournalDepth() const noexcept;
  [[nodiscard]] bool rollbackDeltaJournalsToDepth(
      std::size_t depth) noexcept;
  static void resetFullCopyCountForTesting() noexcept;
  [[nodiscard]] static std::size_t fullCopyCountForTesting() noexcept;
  static void resetDeltaJournalBeginCountForTesting() noexcept;
  [[nodiscard]] static std::size_t deltaJournalBeginCountForTesting() noexcept;
  static void failNextNestedConstraintJournalForTesting() noexcept;

  void clear();
  void setRectangle(double widthMm, double heightMm);
  void addLine(Point start, Point end);
  void addLine(Point start, Point end, std::size_t elementId);
  void addRectangle(Point firstCorner, Point oppositeCorner);
  void addRectangle(Point first, Point second, Point third, Point fourth);
  void addCircle(Point center, double radiusMm);
  void addArc(Point center, double radiusMm, double startAngleRad,
              double sweepAngleRad, bool dashed = false);
  void removeLine(std::size_t index);
  void removeCircle(std::size_t index);
  void removeArc(std::size_t index);
  void removeElement(std::size_t elementId);
  void translateElement(std::size_t elementId, double dxMm, double dyMm);

  // Optional virtual center node owned by a composite CAD element.
  // The point is derived from the element geometry, so it automatically
  // follows translation, rotation and rectangle resizing.
  void markElementCenterNode(std::size_t elementId);
  [[nodiscard]] bool hasElementCenterNode(
      std::size_t elementId) const noexcept;
  [[nodiscard]] std::optional<Point> elementCenterPoint(
      std::size_t elementId) const noexcept;
  [[nodiscard]] const std::vector<std::size_t>&
  centerNodeElementIds() const noexcept;
  void translateSelection(const std::vector<std::size_t>& elementIds,
                          const std::vector<GeometryId>& circleIds,
                          const std::vector<GeometryId>& arcIds,
                          double dxMm, double dyMm);
  void translateLinesByIds(const std::vector<GeometryId>& lineIds,
                           double dxMm, double dyMm);
  void setElementDashed(std::size_t elementId, bool dashed);
  void setLineDashedById(GeometryId id, bool dashed);
  void setCircleDashed(std::size_t index, bool dashed);
  void translateCircle(std::size_t index, double dxMm, double dyMm);
  void setCircleDashedById(GeometryId id, bool dashed);
  void setArcDashedById(GeometryId id, bool dashed);
  void translateCircleById(GeometryId id, double dxMm, double dyMm);
  void translateArcById(GeometryId id, double dxMm, double dyMm);
  bool moveArcEndpointReshapeById(GeometryId id, bool start, Point target);
  bool setLineLengthById(GeometryId id, double lengthMm);
  bool setCircleDiameterById(GeometryId id, double diameterMm);
  bool setLineHorizontalById(GeometryId id);
  bool setLineVerticalById(GeometryId id);
  bool setLinesParallelByIds(GeometryId firstId, GeometryId secondId);
  bool setParallelLineDistanceByIds(GeometryId referenceId,
                                    GeometryId movingId,
                                    double distanceMm);
  bool setLineAngleByIds(GeometryId firstId, GeometryId secondId,
                         double angleDegrees);
  bool setPointsCoincident(PointReference first, PointReference second);
  bool setPointOnLine(GeometryId lineId, PointReference pointReference);
  bool setPointOnCircle(GeometryId circleId, PointReference pointReference);
  bool setPointOnArc(GeometryId arcId, PointReference pointReference);
  bool setPointToMidpoint(GeometryId lineId, PointReference pointReference);
  bool setPointOnXAxis(PointReference pointReference);
  bool setPointOnYAxis(PointReference pointReference);

  bool setCircleTangentToLine(GeometryId lineId, GeometryId circleId);
  bool setArcTangentToLine(GeometryId lineId, GeometryId arcId);
  bool translatePoint(PointReference reference, double dxMm, double dyMm);

  [[nodiscard]] GeometryId lineId(std::size_t index) const noexcept;
  [[nodiscard]] GeometryId circleId(std::size_t index) const noexcept;
  [[nodiscard]] GeometryId arcId(std::size_t index) const noexcept;
  [[nodiscard]] std::optional<std::size_t> lineIndex(
      GeometryId id) const noexcept;
  [[nodiscard]] std::optional<std::size_t> circleIndex(
      GeometryId id) const noexcept;
  [[nodiscard]] std::optional<std::size_t> arcIndex(
      GeometryId id) const noexcept;
  [[nodiscard]] std::optional<GeometryLocation> geometryLocation(
      GeometryId id) const noexcept;
  [[nodiscard]] std::optional<std::size_t> constraintIndex(
      ConstraintId id) const noexcept;
  [[nodiscard]] std::optional<std::size_t> dimensionIndex(
      DimensionId id) const noexcept;
  [[nodiscard]] ConstraintComponent connectedComponent(
      const std::vector<GeometryId>& seeds) const;
  [[nodiscard]] std::vector<ConstraintComponent> constraintComponents() const;
  [[nodiscard]] std::size_t ownedBytes() const noexcept;
  [[nodiscard]] std::size_t ownedAllocationBlocks() const noexcept;
  // Complete persisted identity.  Unlike solverFingerprint(), this includes
  // presentation and persistence-only fields and must never be used as the
  // solver cache key.
  [[nodiscard]] std::uint64_t semanticFingerprint() const noexcept;
  [[nodiscard]] bool semanticallyEqual(const Sketch& other) const noexcept;
  [[nodiscard]] std::uint64_t solverFingerprint() const noexcept;
  [[nodiscard]] static SketchDelta makeDelta(const Sketch& before,
                                             const Sketch& after);
  bool applyDelta(const SketchDelta& delta, bool forward);

  bool setLineLength(std::size_t index, double lengthMm);
  bool setPointDistance(PointReference first, PointReference second,
                        double distanceMm);
  bool setPointDistanceX(PointReference first, PointReference second,
                         double distanceMm);
  bool setPointDistanceY(PointReference first, PointReference second,
                         double distanceMm);
  bool setCircleDiameter(std::size_t index, double diameterMm);
  // Kept for binary compatibility with partially rebuilt Qt Creator targets.
  void addDimension(Dimension dimension);
  void storeDimension(const Dimension& dimension);
  bool removeDimension(std::size_t index);
  void clearDimensions();
  bool setDimensionPlacement(std::size_t index, double offsetMm,
                             double angleRad);
  bool setDimensionValue(std::size_t index, double valueMm);

  ConstraintId addConstraint(Constraint constraint);
  // Persistence-only bulk restore: the caller validates the complete set,
  // then the solver runs once after every constraint is present.
  [[nodiscard]] bool restoreConstraints(std::vector<Constraint> constraints);
  bool setConstraintValue(ConstraintId id, double value);
  bool removeConstraint(ConstraintId id);
  void clearConstraints();
  [[nodiscard]] const std::vector<Constraint>& constraints() const noexcept;
  [[nodiscard]] bool isGeometryLocked(GeometryId id) const noexcept;
  [[nodiscard]] bool isElementLocked(std::size_t elementId) const noexcept;
  [[nodiscard]] bool isPointReferenceLocked(PointReference reference) const noexcept;
  void restoreLockedGeometryFrom(const Sketch& baseline);

  [[nodiscard]] double widthMm() const noexcept;
  [[nodiscard]] double heightMm() const noexcept;
  [[nodiscard]] const std::vector<Line>& lines() const noexcept;
  [[nodiscard]] const std::vector<Circle>& circles() const noexcept;
  [[nodiscard]] const std::vector<Arc>& arcs() const noexcept;
  [[nodiscard]] const std::vector<Dimension>& dimensions() const;
  [[nodiscard]] std::optional<Point> referencedPoint(
      PointReference reference) const noexcept;
  [[nodiscard]] bool isClosed() const noexcept;

 private:
  friend class BasicSketchSolver;
  friend class SketchTestAccess;
  void invalidateStructureIndexes() noexcept;
  void rebuildIdentityIndexes() const;
  void rebuildStructureIndexes() const;
  void updateBounds() noexcept;
  void journalCaptureGeometry(GeometryId id);
  void journalCapturePoint(PointReference reference);
  void journalCaptureComponents(const std::vector<GeometryId>& seeds);
  void journalCaptureConstraint(ConstraintId id);
  void journalCaptureDimension(std::size_t index);
  void journalCaptureAllDimensions();
  void journalCaptureCenters();
  void journalRecordAddedGeometry(GeometryId id, GeometryKind kind,
                                  std::size_t index);
  void journalRecordAddedConstraint(ConstraintId id, std::size_t index);
  void journalRecordAddedDimension(std::size_t index);
  [[nodiscard]] std::vector<ConstraintId>
  invalidReferenceConstraintIds() const;
  struct JournalLine {
    GeometryId id{kInvalidGeometryId};
    std::size_t index{};
    std::optional<Line> before;
  };
  struct JournalCircle {
    GeometryId id{kInvalidGeometryId};
    std::size_t index{};
    std::optional<Circle> before;
  };
  struct JournalArc {
    GeometryId id{kInvalidGeometryId};
    std::size_t index{};
    std::optional<Arc> before;
  };
  struct JournalConstraint {
    ConstraintId id{kInvalidConstraintId};
    std::size_t index{};
    std::optional<Constraint> before;
  };
  struct JournalDimension {
    std::size_t token{};
    std::optional<std::size_t> beforeIndex;
    std::optional<Dimension> before;
  };
  struct DeltaJournalState {
    std::vector<JournalLine> lines;
    std::vector<JournalCircle> circles;
    std::vector<JournalArc> arcs;
    std::vector<JournalConstraint> constraints;
    std::vector<JournalDimension> dimensions;
    // Stable, allocation-only transaction origin metadata. Geometry and
    // constraints already own persistent IDs; Dimensions receive journal-
    // local tokens so current vector indices may shift arbitrarily.
    std::vector<GeometryId> beforeLineIds;
    std::vector<GeometryId> beforeCircleIds;
    std::vector<GeometryId> beforeArcIds;
    std::vector<ConstraintId> beforeConstraintIds;
    std::vector<std::size_t> dimensionTokens;
    std::vector<std::vector<std::size_t>> parentDimensionTokensBefore;
    std::size_t nextDimensionToken{};
    std::size_t beforeDimensionCount{};
    std::optional<std::vector<std::size_t>> centersBefore;
    std::size_t beforeNextElementId{};
    GeometryId beforeNextGeometryId{};
    ConstraintId beforeNextConstraintId{};
    DimensionId beforeNextDimensionId{};
    std::uint64_t beforeSemanticFingerprint{};
    std::vector<ConstraintId> beforeInvalidConstraintIds;
  };
  double widthMm_{60.0};
  double heightMm_{40.0};
  std::vector<Line> lines_;
  std::vector<Circle> circles_;
  std::vector<Arc> arcs_;
  std::vector<GeometryId> lineIds_;
  std::vector<GeometryId> circleIds_;
  std::vector<GeometryId> arcIds_;
  std::vector<Dimension> dimensions_;
  std::vector<Constraint> constraints_;

  // Elements created by tools that expose a persistent virtual center node.
  // Currently used by RectangleMode::FromCenter.
  std::vector<std::size_t> centerNodeElementIds_;
  std::size_t nextElementId_{1};
  GeometryId nextGeometryId_{1};
  ConstraintId nextConstraintId_{1};
  DimensionId nextDimensionId_{1};
  mutable bool structureIndexesDirty_{true};
  mutable bool connectivityDirty_{true};
  mutable std::unordered_map<GeometryId, GeometryLocation> geometryIndex_;
  mutable std::unordered_map<ConstraintId, std::size_t> constraintIndex_;
  mutable std::unordered_map<GeometryId, std::vector<GeometryId>> connectivity_;
  mutable std::unordered_map<GeometryId, std::vector<ConstraintId>>
      geometryConstraints_;
  mutable std::uint64_t lastSolvedFingerprint_{};
  mutable bool hasLastSolvedFingerprint_{false};
  mutable bool lastSolveConverged_{true};
  mutable std::size_t lastSolveViolatedConstraints_{};
  mutable double lastSolveMaxNormalizedResidual_{};
  mutable std::size_t lastSolveUnsupported_{};
  mutable std::size_t lastSolveInvalidReferences_{};
  std::vector<DeltaJournalState> deltaJournals_;
};

}  // namespace solidar::sketch
