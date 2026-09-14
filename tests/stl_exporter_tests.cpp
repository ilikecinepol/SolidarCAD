#include "io/StlExporter.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "model/ExtrudeFeature.h"
#include "model/FilletBuilder.h"
#include "model/FilletFeature.h"

namespace {

struct V3 {
  double x{};
  double y{};
  double z{};
};

V3 cross(V3 a, V3 b) {
  return {a.y * b.z - a.z * b.y,
          a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}

double dot(V3 a, V3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

bool readVertex(const QString& line, V3* out) {
  const QString trimmed = line.trimmed();
  if (!trimmed.startsWith(QStringLiteral("vertex "))) return false;
  const auto fields =
      trimmed.mid(7).split(QLatin1Char(' '), Qt::SkipEmptyParts);
  if (fields.size() != 3) return false;
  bool okX = false;
  bool okY = false;
  bool okZ = false;
  const double x = fields[0].toDouble(&okX);
  const double y = fields[1].toDouble(&okY);
  const double z = fields[2].toDouble(&okZ);
  if (!okX || !okY || !okZ) return false;
  *out = {x, y, z};
  return true;
}

double stlSignedVolume(const QString& path, std::size_t* facets) {
  QFile file(path);
  assert(file.open(QIODevice::ReadOnly | QIODevice::Text));
  QTextStream stream(&file);
  std::vector<V3> triangle;
  triangle.reserve(3);
  double volume6 = 0.0;
  std::size_t count = 0;
  while (!stream.atEnd()) {
    V3 vertex;
    if (!readVertex(stream.readLine(), &vertex)) continue;
    triangle.push_back(vertex);
    if (triangle.size() != 3) continue;
    volume6 += dot(triangle[0], cross(triangle[1], triangle[2]));
    triangle.clear();
    ++count;
  }
  assert(triangle.empty());
  if (facets) *facets = count;
  return std::abs(volume6 / 6.0);
}

double exactVolume(const TopoDS_Shape& shape) {
  GProp_GProps props;
  BRepGProp::VolumeProperties(shape, props);
  return props.Mass();
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir directory(
      QDir::current().filePath(QStringLiteral("stl-export-tests-XXXXXX")));
  assert(directory.isValid());

  solidar::Document document;
  auto& sketch = document.addSketch("Base");
  const auto sketchId = sketch.id;
  sketch.geometry.addRectangle({0.0, 0.0}, {40.0, 30.0});

  auto& body = document.addBody("Body");
  auto base = std::make_unique<solidar::ExtrudeFeature>(
      sketchId, 20.0, "Extrude");
  auto* basePtr = base.get();
  body.addFeature(std::move(base));
  assert(document.recompute());
  assert(body.resultShape());

  // Add a real downstream curved feature. This is the key regression: STL
  // must come from Body::resultShape(), not from the original sketch prism.
  std::size_t edgeIndex = 0;
  for (;; ++edgeIndex) {
    std::string error;
    if (solidar::buildFilletShape(*body.resultShape(), {edgeIndex}, 2.0,
                                  &error))
      break;
    assert(edgeIndex < 64);
  }
  body.addFeature(std::make_unique<solidar::FilletFeature>(
      solidar::EdgeReference{body.id(), basePtr->id(), edgeIndex}, 2.0,
      "Fillet"));
  assert(document.recompute());
  assert(body.resultShape());

  const double brepVolume = exactVolume(*body.resultShape());
  const QString path = directory.filePath(QStringLiteral("final-body.stl"));
  QString error;
  if (!solidar::io::exportDocumentAsciiStl(path, document, &error)) {
    std::fprintf(stderr, "STL export failed: %s\n",
                 error.toUtf8().constData());
    return 1;
  }

  std::size_t facets = 0;
  const double meshVolume = stlSignedVolume(path, &facets);
  assert(facets > 12);  // a plain rectangular prism has only 12 triangles
  assert(meshVolume > 0.0);
  // OCCT tessellation is approximate, but the closed oriented STL should
  // preserve final solid volume comfortably within 1%.
  const double relativeError =
      std::abs(meshVolume - brepVolume) / std::max(1.0, brepVolume);
  assert(relativeError < 0.01);

  QFile output(path);
  assert(output.open(QIODevice::ReadOnly | QIODevice::Text));
  const QByteArray bytes = output.readAll();
  assert(!bytes.contains("nan"));
  assert(!bytes.contains("inf"));
  assert(bytes.contains("solid SolidarCAD"));
  assert(bytes.contains("endsolid SolidarCAD"));
  return 0;
}