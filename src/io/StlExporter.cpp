#include "io/StlExporter.h"

#include <QFile>
#include <QTextStream>

#include <array>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

namespace solidar::io {
namespace {

struct Vec3 {
  double x{};
  double y{};
  double z{};
};

Vec3 subtract(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

Vec3 normal(Vec3 a, Vec3 b, Vec3 c) {
  const Vec3 u = subtract(b, a);
  const Vec3 v = subtract(c, a);
  Vec3 result{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z,
              u.x * v.y - u.y * v.x};
  const double length = std::sqrt(result.x * result.x + result.y * result.y +
                                  result.z * result.z);
  if (length > 1e-12) {
    result.x /= length;
    result.y /= length;
    result.z /= length;
  }
  return result;
}

Vec3 onSupport(sketch::Point point, const QString& support,
               const BoxParameters& box, QPointF offset) {
  if (support.contains("XZ") || support.contains(QString::fromUtf8("Передняя")) ||
      support.contains(QString::fromUtf8("Задняя"))) {
    const double y = support.contains(QString::fromUtf8("Задняя"))
                         ? box.depthMm * 0.5 + offset.y()
                         : support.contains(QString::fromUtf8("Передняя"))
                               ? -box.depthMm * 0.5 + offset.y()
                               : offset.y();
    return {point.xMm + offset.x(), y, point.yMm};
  }
  if (support.contains("YZ") || support.contains(QString::fromUtf8("Правая")) ||
      support.contains(QString::fromUtf8("Левая"))) {
    const double x = support.contains(QString::fromUtf8("Правая"))
                         ? box.widthMm * 0.5 + offset.x()
                         : support.contains(QString::fromUtf8("Левая"))
                               ? -box.widthMm * 0.5 + offset.x()
                               : offset.x();
    return {x, point.xMm + offset.y(), point.yMm};
  }
  const double z = support.contains(QString::fromUtf8("Верхняя"))
                       ? box.heightMm
                       : 0.0;
  return {point.xMm + offset.x(), point.yMm + offset.y(), z};
}

Vec3 extrusionDirection(const QString& support) {
  const double sign = support.contains(QStringLiteral("|NEG")) ? -1.0 : 1.0;
  if (support.contains("XZ") || support.contains(QString::fromUtf8("Передняя")) ||
      support.contains(QString::fromUtf8("Задняя")))
    return {0.0, sign, 0.0};
  if (support.contains("YZ") || support.contains(QString::fromUtf8("Правая")) ||
      support.contains(QString::fromUtf8("Левая")))
    return {sign, 0.0, 0.0};
  return {0.0, 0.0, sign};
}

Vec3 translated(Vec3 point, Vec3 direction, double distance) {
  return {point.x + direction.x * distance,
          point.y + direction.y * distance,
          point.z + direction.z * distance};
}

void writeTriangle(QTextStream& stream, Vec3 a, Vec3 b, Vec3 c) {
  const Vec3 n = normal(a, b, c);
  stream << "  facet normal " << n.x << ' ' << n.y << ' ' << n.z << '\n'
         << "    outer loop\n"
         << "      vertex " << a.x << ' ' << a.y << ' ' << a.z << '\n'
         << "      vertex " << b.x << ' ' << b.y << ' ' << b.z << '\n'
         << "      vertex " << c.x << ' ' << c.y << ' ' << c.z << '\n'
         << "    endloop\n"
         << "  endfacet\n";
}

bool same(sketch::Point a, sketch::Point b) {
  return std::hypot(a.xMm - b.xMm, a.yMm - b.yMm) < 1e-7;
}

std::vector<std::vector<sketch::Point>> contours(const sketch::Sketch& sketch) {
  std::vector<std::vector<sketch::Point>> result;
  std::vector<bool> used(sketch.lines().size(), false);
  for (std::size_t first = 0; first < sketch.lines().size(); ++first) {
    if (used[first] || sketch.lines()[first].dashed) continue;
    std::vector<sketch::Point> contour{sketch.lines()[first].start,
                                       sketch.lines()[first].end};
    used[first] = true;
    while (!same(contour.back(), contour.front())) {
      bool found = false;
      for (std::size_t index = 0; index < sketch.lines().size(); ++index) {
        if (used[index] || sketch.lines()[index].dashed) continue;
        const auto& line = sketch.lines()[index];
        if (same(line.start, contour.back()))
          contour.push_back(line.end);
        else if (same(line.end, contour.back()))
          contour.push_back(line.start);
        else
          continue;
        used[index] = true;
        found = true;
        break;
      }
      if (!found) break;
    }
    if (contour.size() >= 4 && same(contour.front(), contour.back())) {
      contour.pop_back();
      result.push_back(std::move(contour));
    }
  }
  for (const auto& circle : sketch.circles()) {
    if (circle.dashed) continue;
    std::vector<sketch::Point> contour;
    constexpr int segments = 96;
    contour.reserve(segments);
    for (int step = 0; step < segments; ++step) {
      const double angle = 2.0 * std::numbers::pi * step / segments;
      contour.push_back({circle.center.xMm + circle.radiusMm * std::cos(angle),
                         circle.center.yMm + circle.radiusMm * std::sin(angle)});
    }
    result.push_back(std::move(contour));
  }
  return result;
}

}  // namespace

bool exportAsciiStl(const QString& path, const sketch::Sketch& profile,
                    const QString& support, const BoxParameters& box,
                    QPointF bodyPosition,
                    const std::vector<SolidFeature>& features, QString* error) {
  const auto profileContours = contours(profile);
  if (profileContours.empty()) {
    if (error) *error = QString::fromUtf8("У тела нет замкнутого контура.");
    return false;
  }
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    if (error) *error = file.errorString();
    return false;
  }
  QTextStream stream(&file);
  stream.setRealNumberNotation(QTextStream::SmartNotation);
  stream.setRealNumberPrecision(12);
  stream << "solid SolidarCAD\n";
  const Vec3 direction = extrusionDirection(support);

  // A negative extrusion from the end cap is a pocket, not another solid.
  // Emit a single watertight boundary for the common circular case: the top
  // annulus replaces the original cap, and the inner wall closes at the floor.
  const auto pocket = std::find_if(
      features.begin(), features.end(), [](const SolidFeature& feature) {
        return feature.lengthMm < 0.0 && !feature.geometry.circles().empty();
      });
  if (pocket != features.end() && profileContours.size() == 1 &&
      profile.circles().size() == 1 &&
      !pocket->geometry.circles().front().dashed &&
      extrusionDirection(pocket->supportName).z == direction.z) {
    const auto innerContours = contours(pocket->geometry);
    if (!innerContours.empty() &&
        innerContours.front().size() == profileContours.front().size()) {
      const auto& outer = profileContours.front();
      const auto& inner = innerContours.front();
      std::vector<Vec3> bottom, outerTop, innerTop, innerFloor;
      bottom.reserve(outer.size());
      outerTop.reserve(outer.size());
      innerTop.reserve(inner.size());
      innerFloor.reserve(inner.size());
      for (const auto point : outer) {
        const Vec3 base = onSupport(point, support, box, bodyPosition);
        bottom.push_back(base);
        outerTop.push_back(translated(base, direction, box.heightMm));
      }
      const Vec3 pocketDirection = extrusionDirection(pocket->supportName);
      for (const auto point : inner) {
        const Vec3 base = onSupport(point, pocket->supportName, box, bodyPosition);
        innerTop.push_back(translated(base, pocketDirection, pocket->startMm));
        innerFloor.push_back(translated(base, pocketDirection,
                                        pocket->startMm + pocket->lengthMm));
      }
      for (std::size_t index = 1; index + 1 < bottom.size(); ++index) {
        writeTriangle(stream, bottom[0], bottom[index + 1], bottom[index]);
        writeTriangle(stream, innerFloor[0], innerFloor[index],
                      innerFloor[index + 1]);
      }
      for (std::size_t index = 0; index < outer.size(); ++index) {
        const std::size_t next = (index + 1) % outer.size();
        writeTriangle(stream, bottom[index], bottom[next], outerTop[next]);
        writeTriangle(stream, bottom[index], outerTop[next], outerTop[index]);
        writeTriangle(stream, outerTop[index], outerTop[next], innerTop[next]);
        writeTriangle(stream, outerTop[index], innerTop[next], innerTop[index]);
        writeTriangle(stream, innerTop[index], innerFloor[next], innerTop[next]);
        writeTriangle(stream, innerTop[index], innerFloor[index], innerFloor[next]);
      }
      stream << "endsolid SolidarCAD\n";
      if (stream.status() != QTextStream::Ok) {
        if (error)
          *error = QString::fromUtf8("Не удалось полностью записать STL-файл.");
        return false;
      }
      return true;
    }
  }

  for (const auto& contour : profileContours) {
    std::vector<Vec3> bottom;
    std::vector<Vec3> top;
    bottom.reserve(contour.size());
    top.reserve(contour.size());
    for (const auto point : contour) {
      const Vec3 base = onSupport(point, support, box, bodyPosition);
      bottom.push_back(base);
      top.push_back(translated(base, direction, box.heightMm));
    }
    for (std::size_t index = 1; index + 1 < bottom.size(); ++index) {
      writeTriangle(stream, bottom[0], bottom[index + 1], bottom[index]);
      writeTriangle(stream, top[0], top[index], top[index + 1]);
    }
    for (std::size_t index = 0; index < bottom.size(); ++index) {
      const std::size_t next = (index + 1) % bottom.size();
      writeTriangle(stream, bottom[index], bottom[next], top[next]);
      writeTriangle(stream, bottom[index], top[next], top[index]);
    }
  }
  stream << "endsolid SolidarCAD\n";
  if (stream.status() != QTextStream::Ok) {
    if (error) *error = QString::fromUtf8("Не удалось полностью записать STL-файл.");
    return false;
  }
  return true;
}

}  // namespace solidar::io
