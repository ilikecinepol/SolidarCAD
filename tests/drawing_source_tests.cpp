#include "TestAssertions.h"

#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS_Shape.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "drawing/DrawingSource.h"
#include "drawing/EskdRenderer.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/ProductDocument.h"
#include "project/ProjectFile.h"

namespace solidar {

class ComponentDrawingValidationTestAccess final {
 public:
  static void replaceSources(ComponentDrawing& drawing,
                             std::vector<BodyId> sourceBodyIds) {
    drawing.sourceBodyIds_ = std::move(sourceBodyIds);
  }
  static void invalidateId(ComponentDrawing& drawing) {
    drawing.id_ = kInvalidDrawingId;
  }
};

}  // namespace solidar

namespace {

solidar::ExtrudeFeature* addExtrudedBody(solidar::Document& document,
                                         std::string name, double xOffset) {
  auto& sketch = document.addSketch(name + " sketch");
  sketch.geometry.addRectangle({xOffset, 0.0}, {xOffset + 20.0, 12.0});
  auto& body = document.addBody(std::move(name));
  auto feature = std::make_unique<solidar::ExtrudeFeature>(
      sketch.id, 8.0, "Extrude");
  auto* result = feature.get();
  body.addFeature(std::move(feature));
  return result;
}

double heightOf(const TopoDS_Shape& shape) {
  Bnd_Box bounds;
  BRepBndLib::Add(shape, bounds);
  double xMin{}, yMin{}, zMin{}, xMax{}, yMax{}, zMax{};
  bounds.Get(xMin, yMin, zMin, xMax, yMax, zMax);
  return zMax - zMin;
}

}  // namespace

QImage renderDrawing(const solidar::sketch::Sketch& sketch,
                     const TopoDS_Shape& shape) {
  QImage image(800, 1100, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  solidar::drawing::EskdRenderer::renderA4(
      painter, QRectF(0.0, 0.0, image.width(), image.height()), sketch, {},
      &shape);
  return image;
}

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);
  solidar::Document document;
  auto* firstFeature = addExtrudedBody(document, "Body A", 0.0);
  addExtrudedBody(document, "Body B", 40.0);
  CHECK(document.recompute());

  const auto source = solidar::drawing::collectDrawingSource(document);
  CHECK(source.succeeded());
  CHECK(source.shape);
  CHECK(!source.shape->IsNull());
  CHECK(source.bodyCount == 2);
  CHECK(source.solidCount == 2);

  // Recollecting after a model update must not retain the old B-Rep. Also
  // cover the required three-independent-body drawing source.
  firstFeature->setLengthMm(15.0);
  addExtrudedBody(document, "Body C", 80.0);
  CHECK(document.recompute());
  const auto updatedSource = solidar::drawing::collectDrawingSource(document);
  CHECK(updatedSource.succeeded());
  CHECK(updatedSource.bodyCount == 3);
  CHECK(updatedSource.solidCount == 3);
  CHECK(heightOf(*updatedSource.shape) > 14.9);
  CHECK(heightOf(*source.shape) < 9.0);

  const auto firstBodyId = document.bodies()[0].id();
  const auto secondBodyId = document.bodies()[1].id();
  const auto thirdBodyId = document.bodies()[2].id();
  const solidar::ComponentDrawing allBodiesDrawing(
      solidar::DrawingId{1}, "all bodies",
      solidar::ComponentDrawingPaper::A4,
      solidar::ComponentDrawingOrientation::Portrait, 1.0, {});
  const auto allBodiesSource =
      solidar::drawing::collectDrawingSource(allBodiesDrawing, document);
  CHECK(allBodiesSource.succeeded());
  CHECK(allBodiesSource.bodyCount == 3);
  CHECK(allBodiesSource.solidCount == 3);

  const solidar::ComponentDrawing specificDrawing(
      solidar::DrawingId{2}, "specific body",
      solidar::ComponentDrawingPaper::A4,
      solidar::ComponentDrawingOrientation::Landscape, 2.0,
      {firstBodyId});
  const auto specificSource =
      solidar::drawing::collectDrawingSource(specificDrawing, document);
  CHECK(specificSource.succeeded());
  CHECK(specificSource.bodyCount == 1);
  CHECK(specificSource.solidCount == 1);
  CHECK(heightOf(*specificSource.shape) > 14.9);

  const solidar::ComponentDrawing multipleDrawing(
      solidar::DrawingId{3}, "multiple bodies",
      solidar::ComponentDrawingPaper::A4,
      solidar::ComponentDrawingOrientation::Portrait, 1.0,
      {thirdBodyId, firstBodyId});
  CHECK(multipleDrawing.sourceBodyIds() ==
        std::vector<solidar::BodyId>({firstBodyId, thirdBodyId}));
  const auto multipleSource =
      solidar::drawing::collectDrawingSource(multipleDrawing, document);
  CHECK(multipleSource.succeeded());
  CHECK(multipleSource.bodyCount == 2);
  CHECK(multipleSource.solidCount == 2);

  solidar::Document foreignDocument;
  foreignDocument.addBody(secondBodyId + 100, "foreign");
  const auto missingSource =
      solidar::drawing::collectDrawingSource(specificDrawing, foreignDocument);
  CHECK(!missingSource.succeeded());
  CHECK(missingSource.status ==
        solidar::drawing::DrawingSourceStatus::MissingSourceBody);
  CHECK(!missingSource.shape);

  auto invalidDrawing = specificDrawing;
  solidar::ComponentDrawingValidationTestAccess::replaceSources(
      invalidDrawing, {firstBodyId, firstBodyId});
  const auto invalidSource =
      solidar::drawing::collectDrawingSource(invalidDrawing, document);
  CHECK(!invalidSource.succeeded());
  CHECK(invalidSource.status ==
        solidar::drawing::DrawingSourceStatus::InvalidDrawing);
  CHECK(!invalidSource.shape);
  auto invalidMetadata = allBodiesDrawing;
  solidar::ComponentDrawingValidationTestAccess::invalidateId(
      invalidMetadata);
  const auto invalidMetadataSource =
      solidar::drawing::collectDrawingSource(invalidMetadata, document);
  CHECK(!invalidMetadataSource.succeeded());
  CHECK(invalidMetadataSource.status ==
        solidar::drawing::DrawingSourceStatus::InvalidDrawing);
  CHECK(!invalidMetadataSource.shape);

  QTemporaryDir temporary(QDir::current().filePath(
      QStringLiteral("drawing-source-tests-XXXXXX")));
  CHECK(temporary.isValid());
  const QString path = temporary.filePath("multi-body.solidar");
  QString error;
  CHECK(solidar::project::ProjectFile::saveDocument(path, document, &error));
  solidar::Document restored;
  CHECK(solidar::project::ProjectFile::loadDocument(path, &restored, &error));
  const auto restoredSource =
      solidar::drawing::collectDrawingSource(restored);
  CHECK(restoredSource.succeeded());
  CHECK(restoredSource.bodyCount == 3);
  CHECK(restoredSource.solidCount == 3);
  CHECK(heightOf(*restoredSource.shape) > 14.9);

  // Valid curved solids may contain degenerated edges without a 3D curve.
  // Rendering must skip those safely, preserve analytic circles/arcs, and
  // continue drawing the model and its annotations. Once a model source is
  // available, a differently sized support sketch must not be overlaid or
  // alter the drawing dimensions.
  const TopoDS_Shape sphere = BRepPrimAPI_MakeSphere(10.0).Shape();
  solidar::sketch::Sketch firstDrawing;
  firstDrawing.setRectangle(10.0, 10.0);
  solidar::sketch::Sketch secondDrawing;
  secondDrawing.setRectangle(25.0, 10.0);
  const QImage firstImage = renderDrawing(firstDrawing, sphere);
  const QImage secondImage = renderDrawing(secondDrawing, sphere);
  CHECK(!firstImage.isNull());
  CHECK(!secondImage.isNull());
  CHECK(firstImage == secondImage);
  return EXIT_SUCCESS;
}
