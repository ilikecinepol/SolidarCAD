#include "io/StlExporter.h"
#include "io/DocumentExportShapes.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <IMeshData_Status.hxx>
#include <Poly_Triangulation.hxx>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include "TestAssertions.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "model/ExtrudeFeature.h"
#include "model/FilletBuilder.h"
#include "model/FilletFeature.h"
#include "model/ImportedShapeFeature.h"

namespace {

class PresetValidShapeFeature final : public solidar::ShapeFeature {
 public:
  explicit PresetValidShapeFeature(ShapePtr shape)
      : ShapeFeature("Preset invalid fixture") {
    setShape(std::move(shape));
    markValid();
  }

  [[nodiscard]] std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<PresetValidShapeFeature>(*this);
  }

 protected:
  bool rebuildImpl(const solidar::RebuildContext&) override {
    markValid();
    return true;
  }
};

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
  CHECK(file.open(QIODevice::ReadOnly | QIODevice::Text));
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
  CHECK(triangle.empty());
  if (facets) *facets = count;
  return std::abs(volume6 / 6.0);
}

double exactVolume(const TopoDS_Shape& shape) {
  GProp_GProps props;
  BRepGProp::VolumeProperties(shape, props);
  return props.Mass();
}

solidar::Document documentWithShape(const TopoDS_Shape& shape,
                                    const std::string& name) {
  solidar::Document document;
  auto& body = document.addBody(name);
  body.addFeature(std::make_unique<solidar::ImportedShapeFeature>(
      std::make_shared<const TopoDS_Shape>(shape), name));
  (void)document.recompute();
  return document;
}

solidar::Document documentWithUncheckedShape(const TopoDS_Shape& shape) {
  solidar::Document document;
  auto& body = document.addBody("Invalid fixture Body");
  body.addFeature(std::make_unique<PresetValidShapeFeature>(
      std::make_shared<const TopoDS_Shape>(shape)));
  return document;
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
         file.write(bytes) == bytes.size() && file.flush();
}

bool fileBytesEqual(const QString& path, const QByteArray& expected) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) && file.readAll() == expected &&
         file.error() == QFileDevice::NoError;
}

struct FaceTriangulationState {
  Handle(Poly_Triangulation) handle;
  int nodeCount{};
  int triangleCount{};
  double deflection{};
};

struct ShapeState {
  std::string bytes;
  std::vector<FaceTriangulationState> triangulations;
};

std::string brepBytes(const TopoDS_Shape& shape) {
  std::ostringstream stream;
  BRepTools::Write(shape, stream);
  return stream.str();
}

ShapeState captureShapeState(const TopoDS_Shape& shape) {
  ShapeState state;
  state.bytes = brepBytes(shape);
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next()) {
    TopLoc_Location location;
    const Handle(Poly_Triangulation) triangulation =
        BRep_Tool::Triangulation(TopoDS::Face(explorer.Current()), location);
    state.triangulations.push_back(
        {triangulation,
         triangulation.IsNull() ? 0 : triangulation->NbNodes(),
         triangulation.IsNull() ? 0 : triangulation->NbTriangles(),
         triangulation.IsNull() ? 0.0 : triangulation->Deflection()});
  }
  return state;
}

bool shapeStateMatches(const TopoDS_Shape& shape, const ShapeState& expected) {
  if (brepBytes(shape) != expected.bytes) return false;
  std::size_t faceIndex = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next(), ++faceIndex) {
    if (faceIndex >= expected.triangulations.size()) return false;
    TopLoc_Location location;
    const Handle(Poly_Triangulation) triangulation =
        BRep_Tool::Triangulation(TopoDS::Face(explorer.Current()), location);
    const auto& original = expected.triangulations[faceIndex];
    if (triangulation.get() != original.handle.get()) return false;
    if (triangulation.IsNull()) continue;
    if (triangulation->NbNodes() != original.nodeCount ||
        triangulation->NbTriangles() != original.triangleCount ||
        triangulation->Deflection() != original.deflection)
      return false;
  }
  return faceIndex == expected.triangulations.size();
}

std::size_t attachedTriangleCount(const TopoDS_Shape& shape) {
  std::size_t count = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next()) {
    TopLoc_Location location;
    const Handle(Poly_Triangulation) triangulation =
        BRep_Tool::Triangulation(TopoDS::Face(explorer.Current()), location);
    if (!triangulation.IsNull())
      count += static_cast<std::size_t>(triangulation->NbTriangles());
  }
  return count;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir directory(
      QDir::current().filePath(QStringLiteral("stl-export-tests-XXXXXX")));
  CHECK(directory.isValid());

  solidar::Document document;
  auto& sketch = document.addSketch("Base");
  const auto sketchId = sketch.id;
  sketch.geometry.addRectangle({0.0, 0.0}, {40.0, 30.0});

  auto& body = document.addBody("Body");
  auto base = std::make_unique<solidar::ExtrudeFeature>(
      sketchId, 20.0, "Extrude");
  auto* basePtr = base.get();
  body.addFeature(std::move(base));
  CHECK(document.recompute());
  CHECK(body.resultShape());

  // Public export preflight shares the complete-document validation used by
  // the exporters. Visibility is presentation-only; active Error history is
  // rejected; persistent topology indexes are not required for B-Rep export.
  QString preflightError;
  CHECK(solidar::io::hasExportableDocumentShapes(document,
                                                  &preflightError));
  body.setVisible(false);
  CHECK(solidar::io::hasExportableDocumentShapes(document,
                                                  &preflightError));
  body.setVisible(true);

  solidar::Document errorDocument = document;
  auto* errorExtrude = dynamic_cast<solidar::ExtrudeFeature*>(
      errorDocument.findFeature(basePtr->id()));
  CHECK(errorExtrude);
  errorExtrude->setLengthMm(0.0);
  CHECK(!errorDocument.recompute());
  CHECK(!solidar::io::hasExportableDocumentShapes(errorDocument,
                                                   &preflightError));
  CHECK(!preflightError.isEmpty());

  auto noTopologyDocument = documentWithUncheckedShape(
      BRepPrimAPI_MakeBox(4.0, 5.0, 6.0).Shape());
  CHECK(noTopologyDocument.bodies().front().resultShape());
  CHECK(!noTopologyDocument.bodies().front().activeFeature()
             ->lastValidTopologyIndex());
  CHECK(solidar::io::hasExportableDocumentShapes(noTopologyDocument,
                                                  &preflightError));

  // Add a real downstream curved feature. This is the key regression: STL
  // must come from Body::resultShape(), not from the original sketch prism.
  std::size_t edgeIndex = 0;
  for (;; ++edgeIndex) {
    std::string error;
    if (solidar::buildFilletShape(*body.resultShape(), {edgeIndex}, 2.0,
                                  &error))
      break;
    CHECK(edgeIndex < 64);
  }
  body.addFeature(std::make_unique<solidar::FilletFeature>(
      solidar::EdgeReference{body.id(), basePtr->id(), edgeIndex}, 2.0,
      "Fillet"));
  CHECK(document.recompute());
  CHECK(body.resultShape());

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
  CHECK(facets > 12);  // a plain rectangular prism has only 12 triangles
  CHECK(meshVolume > 0.0);
  // OCCT tessellation is approximate, but the closed oriented STL should
  // preserve final solid volume comfortably within 1%.
  const double relativeError =
      std::abs(meshVolume - brepVolume) / std::max(1.0, brepVolume);
  CHECK(relativeError < 0.01);

  QFile output(path);
  CHECK(output.open(QIODevice::ReadOnly | QIODevice::Text));
  const QByteArray bytes = output.readAll();
  CHECK(!bytes.contains("nan"));
  CHECK(!bytes.contains("inf"));
  CHECK(bytes.contains("solid SolidarCAD"));
  CHECK(bytes.contains("endsolid SolidarCAD"));

  const QByteArray sentinel("existing STL sentinel\0bytes", 27);

  // OCCT completion and status are independent. Informational remesh/reuse
  // flags are allowed, while every documented invalid-mesh flag is fatal.
  CHECK(solidar::io::detail::isAcceptableStlMeshingStatus(
      static_cast<int>(IMeshData_NoError)));
  CHECK(solidar::io::detail::isAcceptableStlMeshingStatus(
      static_cast<int>(IMeshData_ReMesh)));
  CHECK(solidar::io::detail::isAcceptableStlMeshingStatus(
      static_cast<int>(IMeshData_Reused)));
  CHECK(solidar::io::detail::isAcceptableStlMeshingStatus(
      static_cast<int>(IMeshData_ReMesh) |
      static_cast<int>(IMeshData_Reused)));
  constexpr int fatalMeshStatuses[] = {
      static_cast<int>(IMeshData_OpenWire),
      static_cast<int>(IMeshData_SelfIntersectingWire),
      static_cast<int>(IMeshData_Failure),
      static_cast<int>(IMeshData_UnorientedWire),
      static_cast<int>(IMeshData_TooFewPoints),
      static_cast<int>(IMeshData_Outdated),
      static_cast<int>(IMeshData_UserBreak),
  };
  for (const int status : fatalMeshStatuses) {
    CHECK(!solidar::io::detail::isAcceptableStlMeshingStatus(status));
    CHECK(!solidar::io::detail::isAcceptableStlMeshingStatus(
        status | static_cast<int>(IMeshData_Reused)));
  }

  // A valid Body cannot make export succeed when another non-empty Body has
  // a failed active operation, even if that operation retains a last-valid
  // shape for presentation. The prior destination must remain byte-identical.
  auto incompleteDocument = documentWithShape(
      BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape(), "Valid Body");
  auto& failedBody = incompleteDocument.addBody("Failed Body");
  auto retainedFailedFeature = std::make_unique<PresetValidShapeFeature>(
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(4.0, 5.0, 6.0).Shape()));
  auto* retainedFailedFeaturePtr = retainedFailedFeature.get();
  failedBody.addFeature(std::move(retainedFailedFeature));
  retainedFailedFeaturePtr->markBlocked("Deliberate test failure");
  CHECK(incompleteDocument.bodies().front().resultShape());
  CHECK(retainedFailedFeaturePtr->isFailed());
  CHECK(retainedFailedFeaturePtr->hasLastValidShape());
  const QString incompletePath =
      directory.filePath(QStringLiteral("incomplete-document.stl"));
  CHECK(writeBytes(incompletePath, sentinel));
  CHECK(!solidar::io::exportDocumentAsciiStl(incompletePath,
                                             incompleteDocument, &error));
  CHECK(!error.isEmpty());
  CHECK(fileBytesEqual(incompletePath, sentinel));

  // A truly empty Body has no failed history and may be omitted.
  auto documentWithEmptyBody = documentWithShape(
      BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape(), "Valid with empty");
  documentWithEmptyBody.addBody("Empty Body");
  const QString emptyBodyPath =
      directory.filePath(QStringLiteral("empty-body.stl"));
  CHECK(solidar::io::exportDocumentAsciiStl(emptyBodyPath,
                                            documentWithEmptyBody, &error));
  CHECK(error.isEmpty());

  // An edge is a valid B-Rep but has no triangles. This must be a controlled
  // whole-export failure rather than a successful empty STL.
  const TopoDS_Shape edge =
      BRepBuilderAPI_MakeEdge(gp_Pnt(0.0, 0.0, 0.0),
                              gp_Pnt(10.0, 0.0, 0.0))
          .Shape();
  CHECK(!edge.IsNull());
  CHECK(BRepCheck_Analyzer(edge).IsValid());
  auto edgeDocument = documentWithShape(edge, "Edge only");
  const QString edgePath =
      directory.filePath(QStringLiteral("edge-only.stl"));
  CHECK(writeBytes(edgePath, sentinel));
  CHECK(!solidar::io::exportDocumentAsciiStl(edgePath, edgeDocument, &error));
  CHECK(!error.isEmpty());
  CHECK(fileBytesEqual(edgePath, sentinel));

  // Meshing an already triangulated curved source must operate on a fully
  // isolated copy. Successful export may refine the STL, but cannot change
  // either the serialized source B-Rep or any attached mesh handle/state.
  TopoDS_Shape pretriangulatedCylinder =
      BRepPrimAPI_MakeCylinder(20.0, 30.0).Shape();
  BRepMesh_IncrementalMesh coarseCylinderMesh(pretriangulatedCylinder, 5.0,
                                               false, 1.0, true);
  CHECK(coarseCylinderMesh.IsDone());
  auto pretriangulatedDocument =
      documentWithShape(pretriangulatedCylinder, "Pretriangulated cylinder");
  const auto pretriangulatedSource =
      pretriangulatedDocument.bodies().front().resultShape();
  CHECK(pretriangulatedSource);
  const std::size_t coarseTriangleCount =
      attachedTriangleCount(*pretriangulatedSource);
  CHECK(coarseTriangleCount > 0);
  const ShapeState sourceBeforeSuccess =
      captureShapeState(*pretriangulatedSource);
  const QString isolatedSuccessPath =
      directory.filePath(QStringLiteral("isolated-success.stl"));
  CHECK(solidar::io::exportDocumentAsciiStl(
      isolatedSuccessPath, pretriangulatedDocument, &error));
  CHECK(error.isEmpty());
  CHECK(shapeStateMatches(*pretriangulatedSource, sourceBeforeSuccess));
  QFile isolatedSuccessFile(isolatedSuccessPath);
  CHECK(isolatedSuccessFile.open(QIODevice::ReadOnly | QIODevice::Text));
  const qsizetype refinedFacetCount =
      isolatedSuccessFile.readAll().count("  facet normal ");
  CHECK(refinedFacetCount > static_cast<qsizetype>(coarseTriangleCount));

  // STL cannot represent a free edge beside otherwise valid faces. Reject the
  // complete mixed Body, preserve the target, and leave its existing source
  // triangulation byte-for-byte and state-for-state unchanged.
  TopoDS_Shape pretriangulatedBox =
      BRepPrimAPI_MakeBox(12.0, 13.0, 14.0).Shape();
  BRepMesh_IncrementalMesh coarseBoxMesh(pretriangulatedBox, 2.0, false, 1.0,
                                         true);
  CHECK(coarseBoxMesh.IsDone());
  CHECK(attachedTriangleCount(pretriangulatedBox) > 0);
  TopoDS_Compound mixedShape;
  BRep_Builder compoundBuilder;
  compoundBuilder.MakeCompound(mixedShape);
  compoundBuilder.Add(mixedShape, pretriangulatedBox);
  compoundBuilder.Add(mixedShape, edge);
  CHECK(BRepCheck_Analyzer(mixedShape).IsValid());
  auto mixedDocument = documentWithShape(mixedShape, "Box with free edge");
  const auto mixedSource = mixedDocument.bodies().front().resultShape();
  CHECK(mixedSource);
  const ShapeState sourceBeforeFailure = captureShapeState(*mixedSource);
  const QString mixedPath =
      directory.filePath(QStringLiteral("box-with-free-edge.stl"));
  CHECK(writeBytes(mixedPath, sentinel));
  CHECK(!solidar::io::exportDocumentAsciiStl(mixedPath, mixedDocument,
                                             &error));
  CHECK(!error.isEmpty());
  CHECK(fileBytesEqual(mixedPath, sentinel));
  CHECK(shapeStateMatches(*mixedSource, sourceBeforeFailure));

  // A malformed non-null face must not throw through the STL adapter and must
  // leave an existing destination untouched.
  TopoDS_Face invalidFace;
  BRep_Builder invalidBuilder;
  invalidBuilder.MakeFace(invalidFace);
  CHECK(!invalidFace.IsNull());
  auto invalidFaceDocument = documentWithUncheckedShape(invalidFace);
  CHECK(invalidFaceDocument.bodies().front().resultShape());
  const QString invalidFacePath =
      directory.filePath(QStringLiteral("invalid-face.stl"));
  CHECK(writeBytes(invalidFacePath, sentinel));
  CHECK(!solidar::io::exportDocumentAsciiStl(invalidFacePath,
                                             invalidFaceDocument, &error));
  CHECK(!error.isEmpty());
  CHECK(fileBytesEqual(invalidFacePath, sentinel));

  return 0;
}
