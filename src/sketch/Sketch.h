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
};

struct Circle {
  Point center;
  double radiusMm{};
};

class Sketch final {
 public:
  Sketch();

  void clear();
  void setRectangle(double widthMm, double heightMm);
  void addLine(Point start, Point end);
  void addRectangle(Point firstCorner, Point oppositeCorner);
  void addCircle(Point center, double radiusMm);
  void removeLine(std::size_t index);
  void removeCircle(std::size_t index);
  void removeElement(std::size_t elementId);
  void translateElement(std::size_t elementId, double dxMm, double dyMm);
  void translateCircle(std::size_t index, double dxMm, double dyMm);
  [[nodiscard]] double widthMm() const noexcept;
  [[nodiscard]] double heightMm() const noexcept;
  [[nodiscard]] const std::vector<Line>& lines() const noexcept;
  [[nodiscard]] const std::vector<Circle>& circles() const noexcept;
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
