#include "io/StlExporter.h"

#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <IMeshData_Status.hxx>
#include <Poly_Triangulation.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <QLocale>
#include <QSaveFile>
#include <QTextStream>

#include <cmath>
#include <string>
#include <vector>

#include "io/DocumentExportShapes.h"
#include "model/GeometryOperation.h"

namespace solidar::io {

bool detail::isAcceptableStlMeshingStatus(int statusFlags) noexcept {
  // ReMesh and Reused are informational in OCCT 8. Source triangulation is
  // excluded separately by the isolated copy with copyMesh=false.
  constexpr int kFatalStatusMask =
      static_cast<int>(IMeshData_OpenWire) |
      static_cast<int>(IMeshData_SelfIntersectingWire) |
      static_cast<int>(IMeshData_Failure) |
      static_cast<int>(IMeshData_UnorientedWire) |
      static_cast<int>(IMeshData_TooFewPoints) |
      static_cast<int>(IMeshData_Outdated) |
      static_cast<int>(IMeshData_UserBreak);
  return (statusFlags & kFatalStatusMask) == 0;
}

namespace {

struct Vec3 {
  double x{};
  double y{};
  double z{};
};

void setError(QString* error, const QString& message) {
  if (error) *error = message;
}

QString failureDiagnostic(const char* context, const GeometryFailure& failure) {
  QString message;
  switch (failure.kind) {
    case GeometryFailureKind::OcctException:
      message = QString::fromUtf8("Ошибка OCCT ") + QString::fromUtf8(context);
      break;
    case GeometryFailureKind::StandardException:
      message = QString::fromUtf8("Стандартное исключение ") +
                QString::fromUtf8(context);
      break;
    case GeometryFailureKind::UnknownException:
      message =
          QString::fromUtf8("Неизвестная ошибка ") + QString::fromUtf8(context);
      break;
    default:
      message = QString::fromUtf8("Ошибка ") + QString::fromUtf8(context);
      break;
  }
  message += QLatin1Char('.');
  if (!failure.detail.empty())
    message += QStringLiteral(": ") +
               QString::fromUtf8(failure.detail.data(),
                                 static_cast<qsizetype>(failure.detail.size()));
  return message;
}

bool finite(Vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

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

bool writeTriangle(QTextStream& stream, Vec3 a, Vec3 b, Vec3 c,
                   qsizetype* triangleCount, QString* error) {
  if (!finite(a) || !finite(b) || !finite(c)) {
    setError(error, QString::fromUtf8(
                        "STL-треугольник содержит нечисловые координаты."));
    return false;
  }

  const Vec3 u = subtract(b, a);
  const Vec3 v = subtract(c, a);
  const Vec3 crossProduct{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z,
                          u.x * v.y - u.y * v.x};
  const double area2 = crossProduct.x * crossProduct.x +
                       crossProduct.y * crossProduct.y +
                       crossProduct.z * crossProduct.z;
  if (!std::isfinite(area2) || area2 <= 1e-24) {
    setError(error,
             QString::fromUtf8("STL-треугольник вырожден или некорректен."));
    return false;
  }

  const Vec3 n = normal(a, b, c);
  stream << "  facet normal " << n.x << ' ' << n.y << ' ' << n.z << '\n'
         << "    outer loop\n"
         << "      vertex " << a.x << ' ' << a.y << ' ' << a.z << '\n'
         << "      vertex " << b.x << ' ' << b.y << ' ' << b.z << '\n'
         << "      vertex " << c.x << ' ' << c.y << ' ' << c.z << '\n'
         << "    endloop\n"
         << "  endfacet\n";
  if (triangleCount) ++*triangleCount;
  return true;
}

bool mapContainsAll(const TopTools_IndexedMapOfShape& expected,
                    const TopTools_IndexedMapOfShape& actual) {
  if (expected.Extent() != actual.Extent()) return false;
  for (int index = 1; index <= expected.Extent(); ++index) {
    if (!actual.Contains(expected.FindKey(index))) return false;
  }
  return true;
}

bool hasUnsupportedFreeTopology(const TopoDS_Shape& shape) {
  TopTools_IndexedMapOfShape allWires;
  TopTools_IndexedMapOfShape allEdges;
  TopTools_IndexedMapOfShape allVertices;
  TopExp::MapShapes(shape, TopAbs_WIRE, allWires);
  TopExp::MapShapes(shape, TopAbs_EDGE, allEdges);
  TopExp::MapShapes(shape, TopAbs_VERTEX, allVertices);

  TopTools_IndexedMapOfShape faceWires;
  TopTools_IndexedMapOfShape faceEdges;
  TopTools_IndexedMapOfShape faceVertices;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next()) {
    const TopoDS_Shape& face = explorer.Current();
    TopExp::MapShapes(face, TopAbs_WIRE, faceWires);
    TopExp::MapShapes(face, TopAbs_EDGE, faceEdges);
    TopExp::MapShapes(face, TopAbs_VERTEX, faceVertices);
  }

  return !mapContainsAll(allWires, faceWires) ||
         !mapContainsAll(allEdges, faceEdges) ||
         !mapContainsAll(allVertices, faceVertices);
}

bool hasAttachedTriangulation(const TopoDS_Shape& shape) {
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next()) {
    TopLoc_Location location;
    if (!BRep_Tool::Triangulation(TopoDS::Face(explorer.Current()), location)
             .IsNull())
      return true;
  }
  return false;
}

bool writeBrepFacets(QTextStream& stream, const TopoDS_Shape& shape,
                     qsizetype* triangleCount, QString* error) {
  if (shape.IsNull()) {
    setError(error, QString::fromUtf8("Тело не содержит геометрии для STL."));
    return false;
  }

  BRepCheck_Analyzer analyzer(shape);
  if (!analyzer.IsValid()) {
    setError(error,
             QString::fromUtf8(
                 "B-Rep тела некорректен. STL не экспортирован, чтобы не "
                 "создавать повреждённую сетку."));
    return false;
  }

  if (hasUnsupportedFreeTopology(shape)) {
    setError(error,
             QString::fromUtf8(
                 "STL не поддерживает свободные рёбра, каркасы или вершины "
                 "вместе с поверхностями тела."));
    return false;
  }

  // OCCT meshing attaches triangulation to a shape. Deep-copy both topology
  // and geometry and deliberately omit the source mesh so export cannot
  // mutate model-owned data or accidentally reuse stale triangulation.
  BRepBuilderAPI_Copy isolatedCopy(shape, true, false);
  if (!isolatedCopy.IsDone() || isolatedCopy.Shape().IsNull()) {
    setError(error,
             QString::fromUtf8(
                 "Не удалось создать изолированную копию тела для STL."));
    return false;
  }
  const TopoDS_Shape meshingShape = isolatedCopy.Shape();
  BRepTools::Clean(meshingShape, true);
  if (hasAttachedTriangulation(meshingShape)) {
    setError(error,
             QString::fromUtf8(
                 "Изолированная копия тела содержит устаревшую "
                 "триангуляцию."));
    return false;
  }

  BRepCheck_Analyzer copyAnalyzer(meshingShape);
  if (!copyAnalyzer.IsValid()) {
    setError(error,
             QString::fromUtf8(
                 "Изолированная B-Rep копия тела некорректна."));
    return false;
  }

  // STL is an approximation of the exact OCCT B-Rep. 0.05 mm gives a useful
  // default for printing while the angular limit keeps curved faces smooth.
  BRepMesh_IncrementalMesh mesher(meshingShape, 0.05, false, 0.20, true);
  if (!mesher.IsDone()) {
    setError(error, QString::fromUtf8("Не удалось построить STL-сетку тела."));
    return false;
  }
  const int statusFlags = mesher.GetStatusFlags();
  if (!detail::isAcceptableStlMeshingStatus(statusFlags)) {
    setError(error,
             QString::fromUtf8(
                 "OCCT сообщил о неполной или некорректной STL-сетке "
                 "(флаги %1).")
                 .arg(statusFlags));
    return false;
  }

  qsizetype written = 0;
  for (TopExp_Explorer explorer(meshingShape, TopAbs_FACE); explorer.More();
       explorer.Next()) {
    const TopoDS_Face face = TopoDS::Face(explorer.Current());
    TopLoc_Location location;
    const Handle(Poly_Triangulation) triangulation =
        BRep_Tool::Triangulation(face, location);
    if (triangulation.IsNull() || triangulation->NbTriangles() <= 0 ||
        triangulation->NbNodes() <= 0) {
      setError(error,
               QString::fromUtf8(
                   "OCCT не создал полную триангуляцию одной из граней."));
      return false;
    }

    const gp_Trsf transform = location.Transformation();
    for (int index = 1; index <= triangulation->NbTriangles(); ++index) {
      int n1 = 0;
      int n2 = 0;
      int n3 = 0;
      triangulation->Triangle(index).Get(n1, n2, n3);

      // Poly_Triangulation follows the underlying surface orientation.
      // Reverse vertex winding for reversed topological faces so STL normals
      // point outside the solid rather than producing an "inside-out" mesh.
      if (face.Orientation() == TopAbs_REVERSED) std::swap(n2, n3);

      const int nodeCount = triangulation->NbNodes();
      if (n1 < 1 || n1 > nodeCount || n2 < 1 || n2 > nodeCount || n3 < 1 ||
          n3 > nodeCount) {
        setError(error,
                 QString::fromUtf8(
                     "OCCT вернул некорректные индексы STL-треугольника."));
        return false;
      }

      const gp_Pnt p1 = triangulation->Node(n1).Transformed(transform);
      const gp_Pnt p2 = triangulation->Node(n2).Transformed(transform);
      const gp_Pnt p3 = triangulation->Node(n3).Transformed(transform);
      const Vec3 a{p1.X(), p1.Y(), p1.Z()};
      const Vec3 b{p2.X(), p2.Y(), p2.Z()};
      const Vec3 c{p3.X(), p3.Y(), p3.Z()};
      if (!writeTriangle(stream, a, b, c, &written, error)) return false;
    }
  }

  if (written == 0) {
    setError(error, QString::fromUtf8(
                        "OCCT не создал ни одного треугольника для STL."));
    return false;
  }
  if (triangleCount) *triangleCount += written;
  return true;
}

bool finishAsciiStl(QSaveFile& file, QTextStream& stream,
                    qsizetype triangleCount, QString* error) {
  stream << "endsolid SolidarCAD\n";
  stream.flush();
  if (stream.status() != QTextStream::Ok) {
    setError(error,
             QString::fromUtf8("Не удалось полностью записать STL-файл."));
    file.cancelWriting();
    return false;
  }
  if (triangleCount == 0) {
    setError(error, QString::fromUtf8("STL не содержит треугольников."));
    file.cancelWriting();
    return false;
  }
  if (!file.flush()) {
    setError(error,
             QString::fromUtf8("Не удалось полностью записать STL-файл: ") +
                 file.errorString());
    file.cancelWriting();
    return false;
  }
  if (!file.commit()) {
    setError(error,
             QString::fromUtf8("Не удалось завершить запись STL-файла: ") +
                 file.errorString());
    return false;
  }
  return true;
}
bool exportDocumentAsciiStlImpl(const QString& path, const Document& document,
                                QString* error) {
  std::vector<ShapeFeature::ShapePtr> shapes;
  if (!detail::collectDocumentExportShapes(document, &shapes, error))
    return false;

  // QSaveFile prevents a failed mesh/write from leaving a half-written STL.
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    setError(error,
             QString::fromUtf8("Не удалось открыть STL-файл для записи: ") +
                 file.errorString());
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
  return finishAsciiStl(file, stream, triangleCount, error);
}

}  // namespace

bool exportDocumentAsciiStl(const QString& path, const Document& document,
                            QString* error) {
  if (error) error->clear();
  bool exported = false;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&] { exported = exportDocumentAsciiStlImpl(path, document, error); },
      &failure);
  if (completed) return exported;
  setError(error, failureDiagnostic("при экспорте STL", failure));
  return false;
}

}  // namespace solidar::io
