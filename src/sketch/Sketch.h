#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace solidar::sketch {

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

struct PointReference {
  std::size_t lineIndex{};
  bool start{true};
};

enum class DimensionKind { LineLength, PointDistance, CircleDiameter };

struct Dimension {
  DimensionKind kind{DimensionKind::LineLength};
  std::size_t geometryIndex{};
  PointReference firstPoint{};
  PointReference secondPoint{};
  double valueMm{};
  double offsetMm{4.0};
  double angleRad{};
};

class Sketch final {
 public:
  Sketch();

  void clear();
  void setRectangle(double widthMm, double heightMm);
  void addLine(Point start, Point end);
  void addRectangle(Point firstCorner, Point oppositeCorner);
  void addRectangle(Point first, Point second, Point third, Point fourth);
  void addCircle(Point center, double radiusMm);
  void removeLine(std::size_t index);
  void removeCircle(std::size_t index);
  void removeElement(std::size_t elementId);
  void translateElement(std::size_t elementId, double dxMm, double dyMm);
  void setElementDashed(std::size_t elementId, bool dashed);
  void setCircleDashed(std::size_t index, bool dashed);
  void translateCircle(std::size_t index, double dxMm, double dyMm);
  bool setLineLength(std::size_t index, double lengthMm);
  bool setPointDistance(PointReference first, PointReference second,
                        double distanceMm);
  bool setCircleDiameter(std::size_t index, double diameterMm);
  // Kept for binary compatibility with partially rebuilt Qt Creator targets.
  void addDimension(Dimension dimension);
  void storeDimension(const Dimension& dimension);
  void clearDimensions();
  bool setDimensionPlacement(std::size_t index, double offsetMm,
                             double angleRad);
  bool setDimensionValue(std::size_t index, double valueMm);
  [[nodiscard]] double widthMm() const noexcept;
  [[nodiscard]] double heightMm() const noexcept;
  [[nodiscard]] const std::vector<Line>& lines() const noexcept;
  [[nodiscard]] const std::vector<Circle>& circles() const noexcept;
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
  std::size_t nextElementId_{1};
};

}  // namespace solidar::sketch
