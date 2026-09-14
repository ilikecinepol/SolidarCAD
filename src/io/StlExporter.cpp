#include "io/StlExporter.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <QFile>
#include <QLocale>
#include <QSaveFile>
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

bool finite(Vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

bool writeBrepFacets(QTextStream& stream, const TopoDS_Shape& shape,
                     qsizetype* triangleCount, QString* error) {
  if (shape.IsNull()) {
    if (error) *error = QString::fromUtf8("РўРµР»Рѕ РЅРµ СЃРѕРґРµСЂР¶РёС‚ РіРµРѕРјРµС‚СЂРёРё РґР»СЏ STL.");
    return false;
  }

  BRepCheck_Analyzer analyzer(shape);
  if (!analyzer.IsValid()) {
    if (error)
      *error = QString::fromUtf8(
          "B-Rep С‚РµР»Р° РЅРµРєРѕСЂСЂРµРєС‚РµРЅ. STL РЅРµ СЌРєСЃРїРѕСЂС‚РёСЂРѕРІР°РЅ, С‡С‚РѕР±С‹ РЅРµ СЃРѕР·РґР°РІР°С‚СЊ "
          "РїРѕРІСЂРµР¶РґС‘РЅРЅСѓСЋ СЃРµС‚РєСѓ.");
    return false;
  }

  // STL is an approximation of the exact OCCT B-Rep. 0.05 mm gives a useful
  // default for printing while the angular limit keeps curved faces smooth.
  BRepMesh_IncrementalMesh mesher(shape, 0.05, false, 0.20, true);
  if (!mesher.IsDone()) {
    if (error)
      *error = QString::fromUtf8("РќРµ СѓРґР°Р»РѕСЃСЊ РїРѕСЃС‚СЂРѕРёС‚СЊ STL-СЃРµС‚РєСѓ С‚РµР»Р°.");
    return false;
  }

  qsizetype written = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next()) {
    const TopoDS_Face face = TopoDS::Face(explorer.Current());
    TopLoc_Location location;
    const Handle(Poly_Triangulation) triangulation =
        BRep_Tool::Triangulation(face, location);
    if (triangulation.IsNull()) continue;

    const gp_Trsf transform = location.Transformation();
    for (Standard_Integer index = 1;
         index <= triangulation->NbTriangles(); ++index) {
      Standard_Integer n1 = 0;
      Standard_Integer n2 = 0;
      Standard_Integer n3 = 0;
      triangulation->Triangle(index).Get(n1, n2, n3);

      // Poly_Triangulation follows the underlying surface orientation.
      // Reverse vertex winding for reversed topological faces so STL normals
      // point outside the solid rather than producing an "inside-out" mesh.
      if (face.Orientation() == TopAbs_REVERSED) std::swap(n2, n3);

      const gp_Pnt p1 = triangulation->Node(n1).Transformed(transform);
      const gp_Pnt p2 = triangulation->Node(n2).Transformed(transform);
      const gp_Pnt p3 = triangulation->Node(n3).Transformed(transform);
      const Vec3 a{p1.X(), p1.Y(), p1.Z()};
      const Vec3 b{p2.X(), p2.Y(), p2.Z()};
      const Vec3 c{p3.X(), p3.Y(), p3.Z()};
      if (!finite(a) || !finite(b) || !finite(c)) continue;

      const Vec3 u = subtract(b, a);
      const Vec3 v = subtract(c, a);
      const Vec3 cross{u.y * v.z - u.z * v.y,
                       u.z * v.x - u.x * v.z,
                       u.x * v.y - u.y * v.x};
      const double area2 =
          cross.x * cross.x + cross.y * cross.y + cross.z * cross.z;
      if (!std::isfinite(area2) || area2 <= 1e-24) continue;

      writeTriangle(stream, a, b, c);
      ++written;
    }
  }

  if (written == 0) {
    if (error)
      *error = QString::fromUtf8(
          "OCCT РЅРµ СЃРѕР·РґР°Р» РЅРё РѕРґРЅРѕРіРѕ С‚СЂРµСѓРіРѕР»СЊРЅРёРєР° РґР»СЏ STL.");
    return false;
  }
  if (triangleCount) *triangleCount += written;
  return true;
}
}  // namespace

bool exportDocumentAsciiStl(const QString& path, const Document& document,
                            QString* error) {
  std::vector<ShapeFeature::ShapePtr> shapes;
  shapes.reserve(document.bodies().size());
  for (const Body& body : document.bodies()) {
    auto shape = body.resultShape();
    if (shape && !shape->IsNull()) shapes.push_back(std::move(shape));
  }
  if (shapes.empty()) {
    if (error)
      *error = QString::fromUtf8(
          "Р’ РґРѕРєСѓРјРµРЅС‚Рµ РЅРµС‚ РїРѕСЃС‚СЂРѕРµРЅРЅРѕРіРѕ С‚РІС‘СЂРґРѕРіРѕ С‚РµР»Р° РґР»СЏ СЌРєСЃРїРѕСЂС‚Р°.");
    return false;
  }

  // QSaveFile prevents a failed mesh/write from leaving a half-written STL.
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    if (error) *error = file.errorString();
    return false;
  }

  QTextStream stream(&file);
  // STL syntax always requires a dot as the decimal separator, regardless of
  // Windows/Russian locale.
  stream.setLocale(QLocale::c());
  stream.setRealNumberNotation(QTextStream::SmartNotation);
  stream.setRealNumberPrecision(12);
  stream << "solid SolidarCAD\n";

  qsizetype triangleCount = 0;
  for (const auto& shape : shapes) {
    if (!writeBrepFacets(stream, *shape, &triangleCount, error)) {
      file.cancelWriting();
      return false;
    }
  }

  stream << "endsolid SolidarCAD\n";
  stream.flush();
  if (stream.status() != QTextStream::Ok) {
    if (error)
      *error = QString::fromUtf8("РќРµ СѓРґР°Р»РѕСЃСЊ РїРѕР»РЅРѕСЃС‚СЊСЋ Р·Р°РїРёСЃР°С‚СЊ STL-С„Р°Р№Р».");
    file.cancelWriting();
    return false;
  }
  if (triangleCount == 0) {
    if (error) *error = QString::fromUtf8("STL РЅРµ СЃРѕРґРµСЂР¶РёС‚ С‚СЂРµСѓРіРѕР»СЊРЅРёРєРѕРІ.");
    file.cancelWriting();
    return false;
  }
  if (!file.commit()) {
    if (error) *error = file.errorString();
    return false;
  }
  return true;
}

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
  stream.setLocale(QLocale::c());
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
