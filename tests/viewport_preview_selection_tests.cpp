#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <QApplication>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

#include "model/TopologyReferenceResolver.h"
#include "ui/Viewport.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  constexpr solidar::BodyId bodyId = 41;
  constexpr solidar::FeatureId sourceFeatureId = 73;
  constexpr solidar::FeatureId previewFeatureId = 99;
  const auto source = std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape());
  const auto previewA = std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeCylinder(14.0, 25.0).Shape());
  const auto previewB = std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeCylinder(12.0, 35.0).Shape());

  const auto edgeA = solidar::makeEdgeReference(
      *source, bodyId, sourceFeatureId, 0);
  const auto edgeB = solidar::makeEdgeReference(
      *source, bodyId, sourceFeatureId, 1);
  const auto edgeC = solidar::makeEdgeReference(
      *source, bodyId, sourceFeatureId, 2);
  CHECK(edgeA.signature && edgeB.signature && edgeC.signature);

  solidar::Viewport viewport;
  viewport.setBodyShape(source, bodyId, sourceFeatureId);
  viewport.setSelectedBodyEdges({edgeA});
  viewport.setToolPreviewShape(bodyId, previewFeatureId, previewA);
  CHECK(viewport.selectedBodyEdges() ==
        std::vector<solidar::EdgeReference>{edgeA});

  viewport.setSelectedBodyEdges({edgeA, edgeB});
  viewport.setToolPreviewShape(bodyId, previewFeatureId, previewB);
  const auto selected = viewport.selectedBodyEdges();
  CHECK(selected == std::vector<solidar::EdgeReference>({edgeA, edgeB}));
  CHECK(selected[0].featureId == sourceFeatureId);
  CHECK(selected[1].featureId == sourceFeatureId);

  viewport.setSelectedBodyEdges({edgeA, edgeB, edgeC});
  viewport.setToolPreviewShape(bodyId, previewFeatureId, previewA);
  CHECK(viewport.selectedBodyEdges() ==
        std::vector<solidar::EdgeReference>({edgeA, edgeB, edgeC}));

  // Parameter changes rebuild preview repeatedly; toggling B leaves the same
  // source references A+C and never adopts previewFeatureId.
  viewport.setToolPreviewShape(bodyId, previewFeatureId, previewB);
  viewport.setToolPreviewShape(bodyId, previewFeatureId, previewA);
  viewport.setSelectedBodyEdges({edgeA, edgeC});
  const auto afterDeselect = viewport.selectedBodyEdges();
  CHECK(afterDeselect ==
        std::vector<solidar::EdgeReference>({edgeA, edgeC}));
  for (const auto& edge : afterDeselect) {
    CHECK(edge.bodyId == bodyId);
    CHECK(edge.featureId == sourceFeatureId);
    CHECK(edge.topology().kind == solidar::TopologyKind::Edge);
    CHECK(edge.signature);
  }

  viewport.clearToolPreviewShape();
  CHECK(viewport.selectedBodyEdges() ==
        std::vector<solidar::EdgeReference>({edgeA, edgeC}));
  return EXIT_SUCCESS;
}
