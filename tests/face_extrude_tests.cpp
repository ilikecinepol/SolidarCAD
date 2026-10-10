#include "TestAssertions.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeWedge.hxx>
#include <GeomAbs_CurveType.hxx>
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "TestGeometryUtils.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/ExtrudeToolSession.h"
#include "model/FaceExtrudeBuilder.h"
#include "model/MoveFeature.h"
#include "model/TopologyReferenceResolver.h"
#include "project/ProjectFile.h"

namespace {

class TestFailure final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

using solidar::ExtrudeOperation;
using solidar::test::near;
using solidar::test::volumeOf;

TopoDS_Face faceAt(const TopoDS_Shape& shape, std::size_t wantedIndex) {
  std::size_t index = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next(), ++index)
    if (index == wantedIndex) return TopoDS::Face(explorer.Current());
  return TopoDS_Face{};
}

std::optional<std::size_t> planarFaceIndex(const TopoDS_Shape& shape,
                                           const solidar::Vector3d& normal) {
  std::size_t index = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next(), ++index) {
    const auto placement = solidar::resolveFacePlacement(shape, index);
    if (!placement.planar) continue;
    const auto n = placement.placement.normal();
    if (near(n.x, normal.x, 1e-6) && near(n.y, normal.y, 1e-6) &&
        near(n.z, normal.z, 1e-6))
      return index;
  }
  return std::nullopt;
}

std::optional<std::size_t> nonPlanarFaceIndex(const TopoDS_Shape& shape) {
  std::size_t index = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next(), ++index) {
    const auto placement = solidar::resolveFacePlacement(shape, index);
    if (!placement.planar) return index;
  }
  return std::nullopt;
}

// Finds a planar face whose outward normal is not aligned with any global axis.
std::optional<std::size_t> slopedFaceIndex(const TopoDS_Shape& shape) {
  std::size_t index = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next(), ++index) {
    const auto placement = solidar::resolveFacePlacement(shape, index);
    if (!placement.planar) continue;
    const auto n = placement.placement.normal();
    const int nonZero =
        (std::abs(n.x) > 0.1 ? 1 : 0) + (std::abs(n.y) > 0.1 ? 1 : 0) +
        (std::abs(n.z) > 0.1 ? 1 : 0);
    if (nonZero >= 2) return index;
  }
  return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  try {
    // A. Existing sketch extrude (NewBody) still works.
    {
      solidar::Document document;
      auto& sketch = document.addSketch("Rectangle");
      sketch.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
      auto& body = document.addBody();
      auto extrude = std::make_unique<solidar::ExtrudeFeature>(
          sketch.id, 50.0, "Box");
      auto* extrudePtr = extrude.get();
      body.addFeature(std::move(extrude));
      CHECK(document.recompute());
      CHECK(extrudePtr->isValid());
      CHECK(!extrudePtr->isFaceSource());
      CHECK(extrudePtr->profileSketchId() == sketch.id);
      CHECK(!extrudePtr->faceReference());
      CHECK(extrudePtr->dependsOnSketch(sketch.id));
      CHECK(!extrudePtr->dependsOnSketch(12345));
      const auto bounds = solidar::test::boundsOf(*body.resultShape());
      CHECK(near(bounds.x(), 80.0));
      CHECK(near(bounds.y(), 35.0));
      CHECK(near(bounds.z(), 50.0));
      CHECK(near(volumeOf(*body.resultShape()), 80.0 * 35.0 * 50.0, 1e-3));
    }

    // B. Top planar face Join: volume grows by faceArea * length along +Z.
    {
      const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
      const auto top = solidar::test::topPlanarFace(box, 20.0);
      CHECK(top);
      const auto reference =
          solidar::makeFaceReference(box, 1, 1, *top);
      CHECK(reference.signature);
      TopoDS_Shape result;
      solidar::FaceExtrudeGeometry geometry;
      std::string error;
      CHECK(solidar::buildExtrusionFromFace(box, reference, 10.0,
                                            ExtrudeOperation::Join, false,
                                            &result, &geometry, &error));
      const double baseVolume = 40.0 * 30.0 * 20.0;
      const double faceArea = 40.0 * 30.0;
      CHECK(near(volumeOf(result), baseVolume + faceArea * 10.0, 1e-2));
      CHECK(solidar::test::solidCount(result) == 1);
      CHECK(near(geometry.normal.X(), 0.0, 1e-6));
      CHECK(near(geometry.normal.Y(), 0.0, 1e-6));
      CHECK(near(geometry.normal.Z(), 1.0, 1e-6));
      const auto bounds = solidar::test::boundsOf(result);
      CHECK(near(bounds.maxZ, 30.0));
    }

    // C. Side face (vertical): direction is not +Z and the bbox grows along X.
    {
      const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
      const auto side = planarFaceIndex(box, {1.0, 0.0, 0.0});
      CHECK(side);
      const auto reference = solidar::makeFaceReference(box, 1, 1, *side);
      TopoDS_Shape result;
      solidar::FaceExtrudeGeometry geometry;
      std::string error;
      CHECK(solidar::buildExtrusionFromFace(box, reference, 10.0,
                                            ExtrudeOperation::Join, false,
                                            &result, &geometry, &error));
      CHECK(near(geometry.normal.X(), 1.0, 1e-6));
      CHECK(near(geometry.normal.Z(), 0.0, 1e-6));
      const auto bounds = solidar::test::boundsOf(result);
      CHECK(near(bounds.x(), 50.0));
      CHECK(near(bounds.y(), 30.0));
      CHECK(near(bounds.z(), 20.0));
    }

    // D. Reversed face extrude: the prism is built in the opposite direction.
    {
      const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
      const auto top = solidar::test::topPlanarFace(box, 20.0);
      CHECK(top);
      const auto reference = solidar::makeFaceReference(box, 1, 1, *top);

      // Non-reversed Join grows +Z.
      TopoDS_Shape joined;
      solidar::FaceExtrudeGeometry geometry;
      std::string error;
      CHECK(solidar::buildExtrusionFromFace(box, reference, 10.0,
                                            ExtrudeOperation::Join, false,
                                            &joined, &geometry, &error));
      CHECK(near(solidar::test::boundsOf(joined).maxZ, 30.0));

      // Reversed Join points -Z (into the body) and must not grow it.
      TopoDS_Shape inward;
      CHECK(!solidar::buildExtrusionFromFace(box, reference, 10.0,
                                             ExtrudeOperation::Join, true,
                                             &inward, &geometry, &error));

      // The prism exists before the Join is rejected.  No partially computed
      // output may escape that failure boundary.
      const TopoDS_Shape resultSentinel =
          BRepPrimAPI_MakeBox(1.0, 2.0, 3.0).Shape();
      const TopoDS_Shape toolSentinel =
          BRepPrimAPI_MakeBox(4.0, 5.0, 6.0).Shape();
      TopoDS_Shape atomicResult = resultSentinel;
      TopoDS_Shape atomicTool = toolSentinel;
      const auto sentinelFace = faceAt(box, 0);
      CHECK(!sentinelFace.IsNull());
      solidar::FaceExtrudeGeometry atomicGeometry{
          sentinelFace, gp_Pnt(7.0, 8.0, 9.0), gp_Dir(1.0, 0.0, 0.0)};
      CHECK(!solidar::buildExtrusionFromFace(
          box, reference, 10.0, ExtrudeOperation::Join, true, &atomicResult,
          &atomicGeometry, &error, &atomicTool));
      CHECK(atomicResult.IsSame(resultSentinel));
      CHECK(atomicTool.IsSame(toolSentinel));
      CHECK(atomicGeometry.face.IsSame(sentinelFace));
      CHECK(near(atomicGeometry.centroid.X(), 7.0));
      CHECK(near(atomicGeometry.centroid.Y(), 8.0));
      CHECK(near(atomicGeometry.centroid.Z(), 9.0));
      CHECK(near(atomicGeometry.normal.X(), 1.0));

      // Reversed Cut removes the top slab: the void grows in -Z.
      TopoDS_Shape cutResult;
      CHECK(solidar::buildExtrusionFromFace(box, reference, 10.0,
                                            ExtrudeOperation::Cut, true,
                                            &cutResult, &geometry, &error));
      CHECK(near(solidar::test::boundsOf(cutResult).maxZ, 10.0));
    }

    // E. Cut: inward extrude decreases the volume.
    {
      const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
      const auto top = solidar::test::topPlanarFace(box, 20.0);
      CHECK(top);
      const auto reference = solidar::makeFaceReference(box, 1, 1, *top);
      TopoDS_Shape result;
      solidar::FaceExtrudeGeometry geometry;
      std::string error;
      CHECK(solidar::buildExtrusionFromFace(box, reference, 10.0,
                                            ExtrudeOperation::Cut, true,
                                            &result, &geometry, &error));
      const double baseVolume = 40.0 * 30.0 * 20.0;
      const double removedVolume = 40.0 * 30.0 * 10.0;
      CHECK(near(volumeOf(result), baseVolume - removedVolume, 1e-2));
    }

    // F. Sloped planar face: the resolved normal matches the face plane normal.
    {
      const TopoDS_Shape wedge = BRepPrimAPI_MakeWedge(40.0, 30.0, 20.0, 20.0)
                                     .Solid();
      const auto sloped = slopedFaceIndex(wedge);
      CHECK(sloped);
      const auto expectedNormal =
          solidar::resolveFacePlacement(wedge, *sloped).placement.normal();
      // The face must actually be sloped (not axis aligned).
      const int nonZero = (std::abs(expectedNormal.x) > 0.1 ? 1 : 0) +
                          (std::abs(expectedNormal.y) > 0.1 ? 1 : 0) +
                          (std::abs(expectedNormal.z) > 0.1 ? 1 : 0);
      CHECK(nonZero >= 2);
      const auto reference = solidar::makeFaceReference(wedge, 1, 1, *sloped);
      TopoDS_Shape result;
      solidar::FaceExtrudeGeometry geometry;
      std::string error;
      CHECK(solidar::buildExtrusionFromFace(wedge, reference, 10.0,
                                            ExtrudeOperation::Join, false,
                                            &result, &geometry, &error));
      CHECK(near(geometry.normal.X(), expectedNormal.x, 1e-6));
      CHECK(near(geometry.normal.Y(), expectedNormal.y, 1e-6));
      CHECK(near(geometry.normal.Z(), expectedNormal.z, 1e-6));
      CHECK(volumeOf(result) > volumeOf(wedge));
    }

    // G. Face with an inner wire (hole): the prism preserves the hole, so the
    // fused volume grows by the annular area times the length.
    {
      const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
      const TopoDS_Shape cylinder =
          BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(20.0, 15.0, 0.0),
                                          gp_Dir(0.0, 0.0, 1.0)),
                                   5.0, 20.0)
              .Shape();
      BRepAlgoAPI_Cut cut(box, cylinder);
      cut.Build();
      CHECK(cut.IsDone());
      const TopoDS_Shape holeyBox = cut.Shape();
      const auto top = solidar::test::topPlanarFace(holeyBox, 20.0);
      CHECK(top);
      const auto reference = solidar::makeFaceReference(holeyBox, 1, 1, *top);
      TopoDS_Shape result;
      solidar::FaceExtrudeGeometry geometry;
      std::string error;
      CHECK(solidar::buildExtrusionFromFace(holeyBox, reference, 10.0,
                                            ExtrudeOperation::Join, false,
                                            &result, &geometry, &error));
      const double pi = std::numbers::pi;
      const double baseVolume = 40.0 * 30.0 * 20.0 - pi * 25.0 * 20.0;
      const double annularArea = 40.0 * 30.0 - pi * 25.0;
      CHECK(near(volumeOf(result), baseVolume + annularArea * 10.0, 1e-1));
    }

    // H. Non-planar face (cylinder side) returns false with a planar error and
    // never falls back to a default direction.
    {
      const TopoDS_Shape cylinder =
          BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0.0, 0.0, 0.0),
                                          gp_Dir(0.0, 0.0, 1.0)),
                                   10.0, 20.0)
              .Shape();
      const auto side = nonPlanarFaceIndex(cylinder);
      CHECK(side);
      const auto reference =
          solidar::makeFaceReference(cylinder, 1, 1, *side);
      TopoDS_Shape result;
      solidar::FaceExtrudeGeometry geometry;
      std::string error;
      solidar::OperationFailureCode code{
          solidar::OperationFailureCode::None};
      CHECK(!solidar::buildExtrusionFromFace(cylinder, reference, 10.0,
                                             ExtrudeOperation::Join, false,
                                             &result, &geometry, &error,
                                             nullptr, &code));
      CHECK(error.find("planar") != std::string::npos);
      CHECK(code == solidar::OperationFailureCode::NonPlanarFace);
    }

    // L. Same-domain unification after Join removes the coplanar side seam at
    // the FACE level (a clean box: 6 faces) while keeping the volume and real
    // corners; the holey-box test G exercises hole preservation through the
    // same unification path.
    {
      const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
      const auto top = solidar::test::topPlanarFace(box, 20.0);
      CHECK(top);
      const auto reference = solidar::makeFaceReference(box, 1, 1, *top);
      TopoDS_Shape result;
      solidar::FaceExtrudeGeometry geometry;
      std::string error;
      CHECK(solidar::buildExtrusionFromFace(box, reference, 10.0,
                                            ExtrudeOperation::Join, false,
                                            &result, &geometry, &error));
      const double baseVolume = 40.0 * 30.0 * 20.0;
      const double faceArea = 40.0 * 30.0;
      CHECK(near(volumeOf(result), baseVolume + faceArea * 10.0, 1e-2));
      std::size_t faceCount = 0;
      for (TopExp_Explorer faces(result, TopAbs_FACE); faces.More();
           faces.Next())
        ++faceCount;
      // The coplanar side faces are merged into a single box face each: the
      // seam is gone as a face boundary (faceCount == 6). ShapeUpgrade_
      // UnifySameDomain does not collapse the retained seam ring, so the result
      // carries 24 edges (measured) instead of a clean box's 12; this does not
      // affect the face-level seam removal or the volume.
      CHECK(faceCount == 6);

      // Real corners remain: the 4 vertical box corners survive as vertical
      // lines (each split into two collinear segments by the retained seam
      // ring). Grouping the vertical edges by their (x, y) position recovers
      // exactly the 4 corners.
      std::vector<std::pair<double, double>> verticalCorners;
      for (TopExp_Explorer edges(result, TopAbs_EDGE); edges.More();
           edges.Next()) {
        const TopoDS_Edge edge = TopoDS::Edge(edges.Current());
        BRepAdaptor_Curve curve(edge);
        if (curve.GetType() != GeomAbs_Line) continue;
        const gp_Pnt first = curve.Value(curve.FirstParameter());
        const gp_Pnt last = curve.Value(curve.LastParameter());
        const gp_Vec direction(first, last);
        if (direction.Magnitude() < 1e-6) continue;
        const gp_Dir normalized(direction);
        if (std::abs(normalized.Z()) <= 0.999) continue;
        const double x = first.X();
        const double y = first.Y();
        const bool seen = std::any_of(
            verticalCorners.begin(), verticalCorners.end(), [&](const auto& p) {
              return std::abs(p.first - x) < 1e-4 &&
                     std::abs(p.second - y) < 1e-4;
            });
        if (!seen) verticalCorners.emplace_back(x, y);
      }
      CHECK(verticalCorners.size() == 4);
    }

    // I + J + K. Feature-level face extrude: edit length, upstream dimension
    // change, and serialization roundtrip all preserve the persistent face.
    {
      solidar::Document document;
      auto& sketch = document.addSketch("Base");
      sketch.geometry.addRectangle({0.0, 0.0}, {40.0, 30.0});
      const auto sketchId = sketch.id;
      auto& body = document.addBody();
      const auto bodyId = body.id();
      auto base = std::make_unique<solidar::ExtrudeFeature>(
          sketchId, 20.0, "Box");
      auto* basePtr = base.get();
      body.addFeature(std::move(base));
      CHECK(document.recompute());

      const auto top = solidar::test::topPlanarFace(*body.resultShape(), 20.0);
      CHECK(top);
      const auto reference = solidar::makeFaceReference(*body.resultShape(),
                                                        bodyId, basePtr->id(),
                                                        *top);
      auto faceExtrude = std::make_unique<solidar::ExtrudeFeature>(
          reference, 10.0, "Face Join", ExtrudeOperation::Join, false);
      auto* facePtr = faceExtrude.get();
      body.addFeature(std::move(faceExtrude));
      CHECK(document.recompute());
      CHECK(facePtr->isValid());
      CHECK(facePtr->isFaceSource());
      CHECK(facePtr->profileSketchId() == solidar::kInvalidSketchId);
      CHECK(facePtr->faceReference());
      CHECK(facePtr->faceReference()->bodyId == bodyId);
      CHECK(facePtr->faceReference()->featureId == basePtr->id());
      CHECK(!facePtr->dependsOnSketch(sketchId));
      CHECK(near(solidar::test::boundsOf(*body.resultShape()).z(), 30.0));

      // K. Editing the face extrude length rebuilds validly.
      facePtr->setLengthMm(15.0);
      CHECK(document.recompute());
      CHECK(facePtr->isValid());
      CHECK(near(solidar::test::boundsOf(*body.resultShape()).z(), 35.0));
      const double expectedVolume = 40.0 * 30.0 * (20.0 + 15.0);
      CHECK(near(volumeOf(*body.resultShape()), expectedVolume, 1e-2));
      facePtr->setLengthMm(10.0);
      CHECK(document.recompute());

      // J. Upstream dimension change: the persistent face still resolves.
      basePtr->setLengthMm(30.0);
      CHECK(document.recompute());
      CHECK(facePtr->isValid());
      CHECK(near(solidar::test::boundsOf(*body.resultShape()).z(), 40.0));

      // I. Save/load roundtrip preserves both sketch and face sources.
      QTemporaryDir temporary(QDir::current().filePath(
          QStringLiteral("face-extrude-tests-XXXXXX")));
      CHECK(temporary.isValid());
      const QString path = temporary.filePath("face-extrude.solidar");
      QString saveError;
      CHECK(solidar::project::ProjectFile::saveDocument(path, document,
                                                        &saveError));
      solidar::Document loaded;
      QString loadError;
      CHECK(solidar::project::ProjectFile::loadDocument(path, &loaded,
                                                        &loadError));
      const auto* loadedBody = loaded.findBody(bodyId);
      CHECK(loadedBody && loadedBody->features().size() == 2);
      const auto* loadedBase = dynamic_cast<const solidar::ExtrudeFeature*>(
          loadedBody->features()[0].get());
      const auto* loadedFace = dynamic_cast<const solidar::ExtrudeFeature*>(
          loadedBody->features()[1].get());
      CHECK(loadedBase && !loadedBase->isFaceSource());
      CHECK(loadedBase->profileSketchId() == sketchId);
      CHECK(loadedFace && loadedFace->isFaceSource());
      const auto loadedReference = loadedFace->faceReference();
      CHECK(loadedReference);
      CHECK(loadedReference->bodyId == bodyId);
      CHECK(loadedReference->featureId == basePtr->id());
      CHECK(loadedReference->signature);
      // Saved after J: base length 30 + face length 10.
      CHECK(near(volumeOf(*loadedBody->resultShape()), 40.0 * 30.0 * 40.0,
                 1e-2));
    }

    // A face source belongs to the immediate previous Feature, not merely to
    // the same Body. A zero-offset intermediate Feature makes the geometry
    // identical and proves owner validation cannot be replaced by shape fit.
    {
      solidar::Document ownerDocument;
      auto& profile = ownerDocument.addSketch("Owner base");
      profile.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
      auto& ownerBody = ownerDocument.addBody("Owner body");
      auto base = std::make_unique<solidar::ExtrudeFeature>(
          profile.id, 10.0, "Base", ExtrudeOperation::NewBody);
      auto* basePtr = base.get();
      ownerBody.addFeature(std::move(base));
      CHECK(ownerDocument.recompute());
      const auto top =
          solidar::test::topPlanarFace(*basePtr->shape(), 10.0);
      CHECK(top);
      const auto baseReference = solidar::makeFaceReference(
          *basePtr->shape(), ownerBody.id(), basePtr->id(), *top);
      CHECK(baseReference.signature);

      auto move = std::make_unique<solidar::MoveFeature>(
          basePtr->id(), solidar::Vector3d{}, "Identity move");
      auto* movePtr = move.get();
      ownerBody.addFeature(std::move(move));
      CHECK(ownerDocument.recompute());

      auto foreignFeatureExtrude = std::make_unique<solidar::ExtrudeFeature>(
          baseReference, 2.0, "Wrong owner", ExtrudeOperation::Join, false);
      auto* foreignFeaturePtr = foreignFeatureExtrude.get();
      ownerBody.addFeature(std::move(foreignFeatureExtrude));
      CHECK(!ownerDocument.recompute());
      CHECK(foreignFeaturePtr->error().find("immediate source Feature") !=
            std::string::npos);

      const auto movedTop =
          solidar::test::topPlanarFace(*movePtr->lastValidShape(), 10.0);
      CHECK(movedTop);
      const auto movedReference = solidar::makeFaceReference(
          *movePtr->lastValidShape(), ownerBody.id(), movePtr->id(),
          *movedTop);
      CHECK(movedReference.signature);
      const auto cachedIndex = movePtr->lastValidTopologyIndex();
      CHECK(cachedIndex);
      const auto beforeSessionBuilds =
          solidar::TopologyIndex::buildAttemptCount();
      solidar::ExtrudeToolSession cachedSession;
      cachedSession.begin(ownerBody.id(), movePtr->id(),
                          movePtr->lastValidShape(), movedReference, 2.0,
                          ExtrudeOperation::Join, false, std::nullopt,
                          cachedIndex);
      CHECK(cachedSession.lifecycle() == solidar::ToolLifecycle::PreviewValid);
      CHECK(solidar::TopologyIndex::buildAttemptCount() ==
            beforeSessionBuilds);
      cachedSession.setLengthFromPanel(3.0);
      CHECK(cachedSession.manipulator());
      CHECK(solidar::TopologyIndex::buildAttemptCount() ==
            beforeSessionBuilds);

      solidar::ExtrudeToolSession rejectedSession;
      rejectedSession.begin(ownerBody.id(), movePtr->id(),
                            movePtr->lastValidShape(), baseReference, 2.0,
                            ExtrudeOperation::Join, false, std::nullopt,
                            cachedIndex);
      CHECK(rejectedSession.lifecycle() ==
            solidar::ToolLifecycle::PreviewInvalid);
      CHECK(rejectedSession.error().find("source Feature") !=
            std::string::npos);
      CHECK(rejectedSession.errorCode() ==
            solidar::OperationFailureCode::TopologyReferenceMismatch);
    }
  } catch (const std::exception& error) {
    std::cerr << "face extrude regression failure: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
