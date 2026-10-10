#include "TestAssertions.h"

#include "io/StepExchange.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Builder.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Interface_Static.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletBuilder.h"
#include "model/FilletFeature.h"
#include "model/ImportedShapeFeature.h"
#include "project/ProjectFile.h"

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

solidar::Document documentWithShape(const TopoDS_Shape& shape,
                                    const std::string& name = "Source") {
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

std::size_t countSubshapes(const TopoDS_Shape& shape,
                           TopAbs_ShapeEnum type) {
  std::size_t count = 0;
  for (TopExp_Explorer explorer(shape, type); explorer.More(); explorer.Next())
    ++count;
  return count;
}

double volume(const TopoDS_Shape& shape) {
  GProp_GProps properties;
  BRepGProp::VolumeProperties(shape, properties);
  return properties.Mass();
}

std::array<double, 3> dimensions(const TopoDS_Shape& shape) {
  Bnd_Box bounds;
  BRepBndLib::Add(shape, bounds);
  double xMin = 0.0, yMin = 0.0, zMin = 0.0;
  double xMax = 0.0, yMax = 0.0, zMax = 0.0;
  bounds.Get(xMin, yMin, zMin, xMax, yMax, zMax);
  return {xMax - xMin, yMax - yMin, zMax - zMin};
}

bool near(double actual, double expected, double tolerance = 1e-6) {
  return std::abs(actual - expected) <= tolerance;
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

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir temporary(QDir::currentPath() +
                           QStringLiteral("/step-exchange-tests-XXXXXX"));
  CHECK(temporary.isValid());
  const QString unicodeDirectory =
      temporary.filePath(QString::fromUtf8("тестовые файлы"));
  CHECK(QDir().mkpath(unicodeDirectory));

  QString error;
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(100.0, 50.0, 25.0).Shape();
  auto boxDocument = documentWithShape(box, "Box");
  const QString boxPath = unicodeDirectory + QStringLiteral("/деталь box.step");

  // A. Exact box round-trip, including a Unicode path with spaces.
  CHECK(solidar::io::exportDocumentStep(boxPath, boxDocument, &error));
  CHECK(error.isEmpty());
  auto roundTripBox = solidar::io::readStepFile(boxPath, &error);
  CHECK(roundTripBox);
  CHECK(!roundTripBox->IsNull());
  CHECK(BRepCheck_Analyzer(*roundTripBox).IsValid());
  CHECK(countSubshapes(*roundTripBox, TopAbs_SOLID) == 1);
  const auto boxDimensions = dimensions(*roundTripBox);
  CHECK(near(boxDimensions[0], 100.0, 1e-5));
  CHECK(near(boxDimensions[1], 50.0, 1e-5));
  CHECK(near(boxDimensions[2], 25.0, 1e-5));
  CHECK(near(volume(*roundTripBox), volume(box), 1e-4));

  QFile exportedBox(boxPath);
  CHECK(exportedBox.open(QIODevice::ReadOnly));
  CHECK(exportedBox.readAll().contains("AP242"));
  exportedBox.close();

  // B. Curved B-Rep stays analytical rather than becoming a polygon mesh.
  const TopoDS_Shape cylinder = BRepPrimAPI_MakeCylinder(12.0, 40.0).Shape();
  auto cylinderDocument = documentWithShape(cylinder, "Cylinder");
  const QString cylinderPath = temporary.filePath(QStringLiteral("cylinder.stp"));
  CHECK(solidar::io::exportDocumentStep(cylinderPath, cylinderDocument, &error));
  auto roundTripCylinder = solidar::io::readStepFile(cylinderPath, &error);
  CHECK(roundTripCylinder);
  CHECK(BRepCheck_Analyzer(*roundTripCylinder).IsValid());
  bool hasAnalyticCylinder = false;
  for (TopExp_Explorer faces(*roundTripCylinder, TopAbs_FACE); faces.More();
       faces.Next()) {
    BRepAdaptor_Surface surface(TopoDS::Face(faces.Current()));
    if (surface.GetType() == GeomAbs_Cylinder) hasAnalyticCylinder = true;
  }
  CHECK(hasAnalyticCylinder);

  // C. Two independent solids remain present.
  TopoDS_Compound twoBoxes;
  BRep_Builder builder;
  builder.MakeCompound(twoBoxes);
  builder.Add(twoBoxes, BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape());
  builder.Add(twoBoxes,
              BRepPrimAPI_MakeBox(gp_Pnt(50.0, 0.0, 0.0), 5.0, 6.0, 7.0)
                  .Shape());
  auto multiDocument = documentWithShape(twoBoxes, "Two solids");
  const QString multiPath = temporary.filePath(QStringLiteral("two-solids.step"));
  CHECK(solidar::io::exportDocumentStep(multiPath, multiDocument, &error));
  auto roundTripMulti = solidar::io::readStepFile(multiPath, &error);
  CHECK(roundTripMulti);
  CHECK(BRepCheck_Analyzer(*roundTripMulti).IsValid());
  CHECK(countSubshapes(*roundTripMulti, TopAbs_SOLID) == 2);

  // Separate Bodies are emitted as separate STEP roots, and every available
  // root must be transferred on import.
  solidar::Document separateBodies;
  auto& firstBody = separateBodies.addBody("Root A");
  firstBody.addFeature(std::make_unique<solidar::ImportedShapeFeature>(
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(8.0, 9.0, 10.0).Shape()),
      "Root A"));
  auto& secondBody = separateBodies.addBody("Root B");
  secondBody.addFeature(std::make_unique<solidar::ImportedShapeFeature>(
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(gp_Pnt(30.0, 0.0, 0.0), 4.0, 5.0, 6.0)
              .Shape()),
      "Root B"));
  CHECK(separateBodies.recompute());
  const QString separateRootsPath =
      temporary.filePath(QStringLiteral("separate-roots.step"));
  CHECK(solidar::io::exportDocumentStep(separateRootsPath, separateBodies,
                                        &error));
  const auto separateRoots =
      solidar::io::readStepFile(separateRootsPath, &error);
  CHECK(separateRoots);
  CHECK(countSubshapes(*separateRoots, TopAbs_SOLID) == 2);

  // D. Invalid input fails without changing the destination document.
  const QString invalidPath = temporary.filePath(QStringLiteral("invalid.step"));
  QFile invalid(invalidPath);
  CHECK(invalid.open(QIODevice::WriteOnly));
  CHECK(invalid.write("this is not a STEP file\n") > 0);
  invalid.close();
  solidar::Document unchanged = boxDocument;
  const auto beforeBodies = unchanged.bodies().size();
  const double beforeVolume = volume(*unchanged.bodies().front().resultShape());
  CHECK(!solidar::io::importDocumentStep(invalidPath, &unchanged, {}, &error));
  CHECK(!error.isEmpty());
  CHECK(unchanged.bodies().size() == beforeBodies);
  CHECK(near(volume(*unchanged.bodies().front().resultShape()), beforeVolume));

  // E. Empty documents report a controlled export error and create no file.
  solidar::Document empty;
  const QString emptyPath = temporary.filePath(QStringLiteral("empty.step"));
  CHECK(!solidar::io::exportDocumentStep(emptyPath, empty, &error));
  CHECK(!error.isEmpty());
  CHECK(!QFile::exists(emptyPath));

  // F. Native persistence does not depend on the source STEP.
  solidar::Document imported;
  CHECK(solidar::io::importDocumentStep(boxPath, &imported,
                                        QStringLiteral("Imported STEP"),
                                        &error));
  CHECK(imported.bodies().size() == 1);
  const QString projectPath = temporary.filePath(QStringLiteral("embedded.solidar"));
  CHECK(solidar::project::ProjectFile::saveDocument(projectPath, imported,
                                                    &error));
  CHECK(QFile::remove(boxPath));
  solidar::Document restored;
  CHECK(solidar::project::ProjectFile::loadDocument(projectPath, &restored,
                                                    &error));
  CHECK(restored.bodies().size() == 1);
  const auto restoredShape = restored.bodies().front().resultShape();
  CHECK(restoredShape);
  CHECK(BRepCheck_Analyzer(*restoredShape).IsValid());
  CHECK(countSubshapes(*restoredShape, TopAbs_SOLID) == 1);
  CHECK(near(volume(*restoredShape), volume(box), 1e-4));

  // A native-restored ImportedShape remains a valid STEP export source.
  const QString restoredStepPath =
      temporary.filePath(QStringLiteral("restored-import.step"));
  CHECK(solidar::io::exportDocumentStep(restoredStepPath, restored, &error));
  auto restoredRoundTrip =
      solidar::io::readStepFile(restoredStepPath, &error);
  CHECK(restoredRoundTrip);
  CHECK(BRepCheck_Analyzer(*restoredRoundTrip).IsValid());
  CHECK(countSubshapes(*restoredRoundTrip, TopAbs_SOLID) == 1);
  CHECK(near(volume(*restoredRoundTrip), volume(box), 1e-4));

  // G. Real faces and edges remain available to picking/topology analysis.
  CHECK(countSubshapes(*roundTripBox, TopAbs_FACE) == 6);
  CHECK(countSubshapes(*roundTripBox, TopAbs_EDGE) >= 12);

  // STEP writer settings are process-global and must be restored.
  const char* schema = Interface_Static::CVal("write.step.schema");
  const std::string previousSchema = schema ? schema : "";
  CHECK(Interface_Static::SetCVal("write.step.schema", "AP203"));
  const QString schemaPath = temporary.filePath(QStringLiteral("schema.step"));
  CHECK(solidar::io::exportDocumentStep(schemaPath, boxDocument, &error));
  CHECK(std::string(Interface_Static::CVal("write.step.schema")) == "AP203");
  if (!previousSchema.empty())
    CHECK(Interface_Static::SetCVal("write.step.schema",
                                    previousSchema.c_str()));

  // H. A native parametric model exports the final downstream Body result,
  // not the intermediate Extrude shape.
  solidar::Document parametric;
  auto& profile = parametric.addSketch("STEP final-body profile");
  profile.geometry.addRectangle({0.0, 0.0}, {40.0, 30.0});
  auto& body = parametric.addBody("STEP final-body Body");
  auto base = std::make_unique<solidar::ExtrudeFeature>(
      profile.id, 20.0, "Extrude");
  const auto baseId = base->id();
  body.addFeature(std::move(base));
  CHECK(parametric.recompute());
  CHECK(body.resultShape());
  const double intermediateVolume = volume(*body.resultShape());

  std::size_t filletEdge = 0;
  for (; filletEdge < 64; ++filletEdge) {
    std::string filletError;
    if (solidar::buildFilletShape(*body.resultShape(), {filletEdge}, 2.0,
                                  &filletError))
      break;
  }
  CHECK(filletEdge < 64);
  body.addFeature(std::make_unique<solidar::FilletFeature>(
      solidar::EdgeReference{body.id(), baseId, filletEdge}, 2.0, "Fillet"));
  CHECK(parametric.recompute());
  CHECK(body.resultShape());
  const double finalVolume = volume(*body.resultShape());
  CHECK(std::abs(finalVolume - intermediateVolume) > 1e-4);

  const QString finalBodyPath =
      temporary.filePath(QStringLiteral("parametric-final-body.step"));
  CHECK(solidar::io::exportDocumentStep(finalBodyPath, parametric, &error));
  QFile finalBodyFile(finalBodyPath);
  CHECK(finalBodyFile.exists());
  CHECK(finalBodyFile.size() > 0);
  const auto finalBodyRoundTrip =
      solidar::io::readStepFile(finalBodyPath, &error);
  CHECK(finalBodyRoundTrip);
  CHECK(BRepCheck_Analyzer(*finalBodyRoundTrip).IsValid());
  CHECK(countSubshapes(*finalBodyRoundTrip, TopAbs_SOLID) == 1);
  CHECK(near(volume(*finalBodyRoundTrip), finalVolume, 1e-4));
  CHECK(std::abs(volume(*finalBodyRoundTrip) - intermediateVolume) > 1e-4);

  // I. Export is all-or-nothing: a failed active operation must not be
  // silently replaced by its retained presentation shape or omitted when
  // another Body is valid. An existing destination survives unchanged.
  auto incompleteDocument = documentWithShape(box, "Valid Body");
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
  const QByteArray sentinel("existing STEP sentinel\0bytes", 28);
  const QString incompletePath =
      temporary.filePath(QStringLiteral("incomplete-document.step"));
  CHECK(writeBytes(incompletePath, sentinel));
  CHECK(!solidar::io::exportDocumentStep(incompletePath, incompleteDocument,
                                         &error));
  CHECK(!error.isEmpty());
  CHECK(fileBytesEqual(incompletePath, sentinel));

  // A truly empty Body carries no failed history and may be omitted.
  auto documentWithEmptyBody = documentWithShape(box, "Valid with empty");
  documentWithEmptyBody.addBody("Empty Body");
  const QString emptyBodyPath =
      temporary.filePath(QStringLiteral("empty-body.step"));
  CHECK(solidar::io::exportDocumentStep(emptyBodyPath, documentWithEmptyBody,
                                        &error));
  CHECK(error.isEmpty());

  // J. A malformed but non-null OCCT face is rejected behind the adapter
  // exception boundary and cannot replace an existing destination.
  TopoDS_Face invalidFace;
  BRep_Builder invalidBuilder;
  invalidBuilder.MakeFace(invalidFace);
  CHECK(!invalidFace.IsNull());
  auto invalidFaceDocument = documentWithUncheckedShape(invalidFace);
  CHECK(invalidFaceDocument.bodies().front().resultShape());
  const QString invalidFacePath =
      temporary.filePath(QStringLiteral("invalid-face.step"));
  CHECK(writeBytes(invalidFacePath, sentinel));
  CHECK(!solidar::io::exportDocumentStep(invalidFacePath, invalidFaceDocument,
                                         &error));
  CHECK(!error.isEmpty());
  CHECK(fileBytesEqual(invalidFacePath, sentinel));

  // K. Reject oversized input before readAll() attempts to allocate it.
  const QString oversizedPath =
      temporary.filePath(QStringLiteral("oversized.step"));
  QFile oversized(oversizedPath);
  CHECK(oversized.open(QIODevice::WriteOnly));
  CHECK(oversized.resize(solidar::io::kMaximumStepFileBytes + 1));
  oversized.close();
  CHECK(!solidar::io::readStepFile(oversizedPath, &error));
  CHECK(!error.isEmpty());

  return 0;
}
