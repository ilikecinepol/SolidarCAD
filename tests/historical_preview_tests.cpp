#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <QApplication>

#include "model/ChamferBuilder.h"
#include "model/ChamferFeature.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/ShellBuilder.h"
#include "model/ShellFeature.h"
#include "model/TopologyReferenceResolver.h"
#include "ui/PartDesignHistory.h"

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return EXIT_FAILURE; } } while(false)

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  solidar::Document document;
  auto& sketch = document.addSketch("Preview sketch");
  sketch.geometry.addRectangle({0.0, 0.0}, {60.0, 40.0});
  auto& body = document.addBody("Preview body");
  auto& extrude = body.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      sketch.id, 30.0, "Extrude"));
  const auto extrudeId = extrude.id();
  CHECK(document.recompute() && extrude.shape());

  std::size_t edgeIndex = static_cast<std::size_t>(-1);
  std::size_t openingIndex = static_cast<std::size_t>(-1);
  for (std::size_t edge = 0; edge < 64; ++edge) {
    std::string error;
    const auto chamferShape =
        solidar::buildChamferShape(*extrude.shape(), {edge}, 2.0, &error);
    if (!chamferShape) {
      if (error.find("resolved") != std::string::npos) break;
      continue;
    }
    for (std::size_t face = 0; face < 32; ++face) {
      auto shellShape =
          solidar::buildShellShape(*chamferShape, {face}, 1.0, false, &error);
      if (shellShape) { edgeIndex = edge; openingIndex = face; break; }
      if (error.find("resolved") != std::string::npos) break;
    }
    if (edgeIndex != static_cast<std::size_t>(-1)) break;
  }
  CHECK(edgeIndex != static_cast<std::size_t>(-1));
  const auto edgeReference = solidar::makeEdgeReference(
      *extrude.shape(), body.id(), extrudeId, edgeIndex);
  auto& chamfer = body.addFeature(std::make_unique<solidar::ChamferFeature>(
      edgeReference, 2.0, "Chamfer"));
  const auto chamferId = chamfer.id();
  CHECK(document.recompute() && chamfer.shape());
  const auto opening = solidar::makeFaceReference(
      *chamfer.shape(), body.id(), chamferId, openingIndex);
  auto& shell = body.addFeature(std::make_unique<solidar::ShellFeature>(
      chamferId, std::vector{opening}, 1.0, false, "Shell"));
  const auto shellId = shell.id();
  CHECK(document.recompute() && shell.shape());

  const auto steps = solidar::buildPartDesignHistory(document, body);
  CHECK(steps.size() == 4);
  CHECK(steps[0].sketchId == sketch.id && !steps[0].shape);
  CHECK(steps[1].featureId == extrudeId && steps[1].shape);
  CHECK(steps[2].featureId == chamferId && steps[2].shape);
  CHECK(steps[3].featureId == shellId && steps[3].shape);
  CHECK(steps[3].shape->IsSame(*body.resultShape()));
  CHECK(body.features().size() == 3);
  CHECK(body.features()[0]->id() == extrudeId);
  CHECK(body.features()[1]->id() == chamferId);
  CHECK(body.features()[2]->id() == shellId);
  return EXIT_SUCCESS;
}
