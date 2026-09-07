#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "TestGeometryUtils.h"
#include "model/ChamferBuilder.h"
#include "model/ChamferFeature.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletBuilder.h"
#include "model/FilletFeature.h"
#include "model/TopologyReferenceResolver.h"
#include "project/ProjectFile.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return false;                                                        \
    }                                                                      \
  } while (false)

namespace {

double faceArea(const TopoDS_Face& face) {
  GProp_GProps properties;
  BRepGProp::SurfaceProperties(face, properties);
  return properties.Mass();
}

double chamferRegionArea(const TopoDS_Shape& shape) {
  double area = 0.0;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next()) {
    const auto face = TopoDS::Face(explorer.Current());
    BRepAdaptor_Surface surface(face);
    if (surface.GetType() != GeomAbs_Plane) continue;
    const auto normal = surface.Plane().Axis().Direction();
    const double vertical = std::abs(normal.Z());
    if (vertical > 1e-4 && vertical < 1.0 - 1e-4) area += faceArea(face);
  }
  return area;
}

double filletRegionArea(const TopoDS_Shape& shape) {
  double area = 0.0;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next()) {
    const auto face = TopoDS::Face(explorer.Current());
    if (BRepAdaptor_Surface(face).GetType() != GeomAbs_Plane)
      area += faceArea(face);
  }
  return area;
}

std::optional<std::size_t> bottomEdge(const TopoDS_Shape& shape,
                                      bool fillet, double size) {
  std::size_t index = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More();
       explorer.Next(), ++index) {
    BRepAdaptor_Curve curve(TopoDS::Edge(explorer.Current()));
    const auto a = curve.Value(curve.FirstParameter());
    const auto b = curve.Value(curve.LastParameter());
    if (std::abs(a.Z()) > 1e-7 || std::abs(b.Z()) > 1e-7) continue;
    std::string error;
    const auto result = fillet
        ? solidar::buildFilletShape(shape, {index}, size, &error)
        : solidar::buildChamferShape(shape, {index}, size, &error);
    if (result) return index;
  }
  return std::nullopt;
}

struct ChainResult {
  solidar::Document document;
  solidar::BodyId bodyId{};
  solidar::FeatureId edgeFeatureId{};
  solidar::FeatureId downstreamId{};
  double regionArea{};
  double finalVolume{};
};

std::optional<ChainResult> buildChain(bool fillet) {
  solidar::Document document;
  auto& baseSketch = document.addSketch("Base");
  baseSketch.geometry.addRectangle({-20.0, -15.0}, {20.0, 15.0});
  auto& body = document.addBody("Body");
  auto& base = body.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      baseSketch.id, 30.0, "Extrude 1"));
  if (!document.recompute()) return std::nullopt;

  const auto edgeIndex = bottomEdge(*body.resultShape(), fillet, 4.0);
  if (!edgeIndex) return std::nullopt;
  const auto edge = solidar::makeEdgeReference(
      *body.resultShape(), body.id(), base.id(), *edgeIndex);
  solidar::ShapeFeature* edgeFeature = nullptr;
  if (fillet) {
    edgeFeature = &body.addFeature(std::make_unique<solidar::FilletFeature>(
        edge, 4.0, "Fillet"));
  } else {
    edgeFeature = &body.addFeature(std::make_unique<solidar::ChamferFeature>(
        edge, 4.0, "Chamfer"));
  }
  if (!document.recompute()) return std::nullopt;
  const auto upstreamShape = edgeFeature->shape();
  const double regionArea =
      fillet ? filletRegionArea(*upstreamShape) : chamferRegionArea(*upstreamShape);
  if (!(regionArea > 1e-6)) return std::nullopt;
  const double upstreamVolume = solidar::test::volumeOf(*upstreamShape);

  const auto topFace = solidar::test::topPlanarFace(*upstreamShape, 30.0);
  if (!topFace) return std::nullopt;
  auto& downstreamSketch = document.addSketch("Opposite face sketch");
  const auto support = solidar::makeFaceReference(
      *upstreamShape, body.id(), edgeFeature->id(), *topFace);
  if (!document.attachSketchToFace(downstreamSketch.id, support))
    return std::nullopt;
  downstreamSketch.geometry.addRectangle({-5.0, -5.0}, {5.0, 5.0});
  auto& downstream = body.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      downstreamSketch.id, 20.0, "Extrude 2",
      solidar::ExtrudeOperation::Join, false));
  if (!document.recompute()) return std::nullopt;

  // Adding a downstream feature consumes, but never reconstructs or mutates,
  // the exact upstream B-Rep.
  if (edgeFeature->shape() != upstreamShape) return std::nullopt;
  const double finalArea = fillet ? filletRegionArea(*body.resultShape())
                                  : chamferRegionArea(*body.resultShape());
  if (!solidar::test::near(finalArea, regionArea, 1e-4)) return std::nullopt;
  const double finalVolume = solidar::test::volumeOf(*body.resultShape());
  if (!(finalVolume > upstreamVolume + 100.0)) return std::nullopt;
  std::size_t solidCount = 0;
  for (TopExp_Explorer solids(*body.resultShape(), TopAbs_SOLID); solids.More();
       solids.Next())
    ++solidCount;
  if (solidCount != 1) return std::nullopt;
  const auto bodyId = body.id();
  const auto edgeFeatureId = edgeFeature->id();
  const auto downstreamId = downstream.id();
  return ChainResult{std::move(document), bodyId, edgeFeatureId,
                     downstreamId, regionArea, finalVolume};
}

bool run() {
  auto chamfer = buildChain(false);
  CHECK(chamfer);
  const auto* chamferBody = chamfer->document.findBody(chamfer->bodyId);
  CHECK(chamferBody);
  const auto* chamferFeature = dynamic_cast<const solidar::ChamferFeature*>(
      chamferBody->features()[1].get());
  CHECK(chamferFeature);
  CHECK(chamferFeature->id() == chamfer->edgeFeatureId);
  CHECK(solidar::test::near(chamferFeature->distanceMm(), 4.0));

  auto fillet = buildChain(true);
  CHECK(fillet);
  const auto* filletBody = fillet->document.findBody(fillet->bodyId);
  CHECK(filletBody);
  const auto* filletFeature = dynamic_cast<const solidar::FilletFeature*>(
      filletBody->features()[1].get());
  CHECK(filletFeature);
  CHECK(solidar::test::near(filletFeature->radiusMm(), 4.0));

  QTemporaryDir directory(QDir::current().filePath("downstream-XXXXXX"));
  CHECK(directory.isValid());
  const QString path = directory.filePath("preservation.solidar");
  QString error;
  CHECK(solidar::project::ProjectFile::saveDocument(
      path, chamfer->document, &error));
  solidar::Document restored;
  CHECK(solidar::project::ProjectFile::loadDocument(path, &restored, &error));
  CHECK(restored.recompute());
  const auto* restoredBody = restored.findBody(chamfer->bodyId);
  CHECK(restoredBody && restoredBody->features().size() == 3);
  const auto* restoredChamfer = dynamic_cast<const solidar::ChamferFeature*>(
      restoredBody->features()[1].get());
  CHECK(restoredChamfer && restoredChamfer->id() == chamfer->edgeFeatureId);
  CHECK(solidar::test::near(restoredChamfer->distanceMm(), 4.0));
  CHECK(solidar::test::near(
      chamferRegionArea(*restoredBody->resultShape()), chamfer->regionArea,
      1e-4));
  CHECK(solidar::test::near(solidar::test::volumeOf(*restoredBody->resultShape()),
                            chamfer->finalVolume, 1e-4));
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  return run() ? EXIT_SUCCESS : EXIT_FAILURE;
}
