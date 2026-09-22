#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace solidar::sketch {

using GeometryId = std::uint64_t;
inline constexpr GeometryId kInvalidGeometryId = 0;

using ConstraintId = std::uint64_t;
inline constexpr ConstraintId kInvalidConstraintId = 0;

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
  Midpoint};

struct Constraint {
  ConstraintId id{kInvalidConstraintId};
  ConstraintType type{ConstraintType::Horizontal};
  GeometryId firstGeometry{kInvalidGeometryId};
  GeometryId secondGeometry{kInvalidGeometryId};
  PointReference firstPoint{};
  PointReference secondPoint{};
  double value{};
};

class Sketch final {
 public:
  Sketch();

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
  void setElementDashed(std::size_t elementId, bool dashed);
  void setCircleDashed(std::size_t index, bool dashed);
  void translateCircle(std::size_t index, double dxMm, double dyMm);
  void setCircleDashedById(GeometryId id, bool dashed);
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
  void clearDimensions();
  bool setDimensionPlacement(std::size_t index, double offsetMm,
                             double angleRad);
  bool setDimensionValue(std::size_t index, double valueMm);

  ConstraintId addConstraint(Constraint constraint);
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
  void updateBounds() noexcept;
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
};

}  // namespace solidar::sketch
