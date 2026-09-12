#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

#include "TestGeometryUtils.h"
#include "model/ExtrudeToolSession.h"
#include "model/TopologyReferenceResolver.h"

namespace {

class TestFailure final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition))                                                        \
      throw TestFailure(std::string(__FILE__) + ":" +                        \
                        std::to_string(__LINE__) + ": " #condition);         \
  } while (false)

using solidar::ExtrudeOperation;
using solidar::test::near;
using solidar::test::volumeOf;

std::optional<std::size_t> nonPlanarFaceIndex(const TopoDS_Shape& shape) {
  std::size_t index = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next(), ++index) {
    const auto placement = solidar::resolveFacePlacement(shape, index);
    if (!placement.planar) return index;
  }
  return std::nullopt;
}

solidar::FaceReference boxTopFace(const TopoDS_Shape& box, double expectedZ) {
  const auto top = solidar::test::topPlanarFace(box, expectedZ);
  if (!top) throw TestFailure("top planar face not found");
  return solidar::makeFaceReference(box, 1, 1, *top);
}

}  // namespace

int main() {
  try {
    // 1. begin(face) -> PreviewValid, non-null preview, manipulator origin at
    //    the face centroid, direction along the face normal, value == length.
    {
      const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
      const auto reference = boxTopFace(box, 20.0);
      solidar::ExtrudeToolSession session;
      session.begin(1, 1, std::make_shared<TopoDS_Shape>(box), reference, 10.0,
                    ExtrudeOperation::Join, false);
      CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
      CHECK(session.previewShape() != nullptr);
      CHECK(session.error().empty());
      CHECK(near(session.lengthMm(), 10.0));
      CHECK(session.operation() == ExtrudeOperation::Join);
      CHECK(!session.reversed());

      const auto manip = session.manipulator();
      CHECK(manip.has_value());
      CHECK(near(manip->origin.x, 20.0, 1e-4));
      CHECK(near(manip->origin.y, 15.0, 1e-4));
      CHECK(near(manip->origin.z, 20.0, 1e-4));
      CHECK(near(manip->direction.x, 0.0, 1e-6));
      CHECK(near(manip->direction.y, 0.0, 1e-6));
      CHECK(near(manip->direction.z, 1.0, 1e-6));
      CHECK(near(manip->valueMm, 10.0));

      const auto params = session.parameters();
      CHECK(params.size() == 1);
      CHECK(params[0].id == "distance");
      CHECK(params[0].type == solidar::ToolParameterType::Distance);
      CHECK(params[0].editableInHud);
      CHECK(near(std::get<double>(params[0].value), 10.0));
      CHECK(near(params[0].minimum, 0.01));
      CHECK(near(params[0].maximum, manip->maximumMm));
    }

    // 2. setLengthFromManipulator(valid) updates the preview and clamps to
    //    [minimum, maximum].
    {
      const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
      const auto reference = boxTopFace(box, 20.0);
      solidar::ExtrudeToolSession session;
      session.begin(1, 1, std::make_shared<TopoDS_Shape>(box), reference, 10.0,
                    ExtrudeOperation::Join, false);

      session.setLengthFromManipulator(-5.0);
      CHECK(near(session.lengthMm(), 0.01));
      CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);

      session.setLengthFromManipulator(1e12);
      const double cap = session.manipulator()->maximumMm;
      CHECK(near(session.lengthMm(), cap));
      CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
      CHECK(cap >= 100.0 && cap <= 100000.0);
    }

    // 3. setLengthFromPanel(oversized) that makes the preview fail restores
    //    the previous accepted length/preview/lifecycle/error.
    {
      const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
      const auto reference = boxTopFace(box, 20.0);
      solidar::ExtrudeToolSession session;
      session.begin(1, 1, std::make_shared<TopoDS_Shape>(box), reference, 10.0,
                    ExtrudeOperation::Cut, true);
      CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
      const double acceptedVolume = volumeOf(*session.previewShape());
      const double expected = 40.0 * 30.0 * 20.0 - 40.0 * 30.0 * 10.0;
      CHECK(near(acceptedVolume, expected, 1e-2));

      const double previousLength = session.lengthMm();
      const auto previousLifecycle = session.lifecycle();
      const auto previousError = session.error();

      // 30mm inward cut on a 20mm box removes the whole solid -> preview fails.
      session.setLengthFromPanel(30.0);
      CHECK(near(session.lengthMm(), previousLength));
      CHECK(session.lifecycle() == previousLifecycle);
      CHECK(session.error() == previousError);
      CHECK(session.previewShape() != nullptr);
      CHECK(near(volumeOf(*session.previewShape()), acceptedVolume, 1e-2));
    }

    // 4. setReversed(true) flips the manipulator direction inward; Cut with
    //    reversed inward decreases volume vs. the outward Join preview.
    {
      const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
      const auto reference = boxTopFace(box, 20.0);
      solidar::ExtrudeToolSession session;
      session.begin(1, 1, std::make_shared<TopoDS_Shape>(box), reference, 10.0,
                    ExtrudeOperation::Join, false);
      CHECK(near(session.manipulator()->direction.z, 1.0, 1e-6));
      const double joinVolume = volumeOf(*session.previewShape());

      session.setReversed(true);
      // Reversed Join goes inward and does not add volume -> preview invalid.
      CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
      CHECK(near(session.manipulator()->direction.z, -1.0, 1e-6));

      session.setOperation(ExtrudeOperation::Cut);
      CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
      CHECK(volumeOf(*session.previewShape()) < joinVolume);
      CHECK(near(volumeOf(*session.previewShape()),
                 40.0 * 30.0 * 20.0 - 40.0 * 30.0 * 10.0, 1e-2));
    }

    // 5. Join default; setOperation(Cut) re-previews; setOperation(NewBody)
    //    -> PreviewInvalid + error, no crash.
    {
      const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
      const auto reference = boxTopFace(box, 20.0);
      solidar::ExtrudeToolSession session;
      session.begin(1, 1, std::make_shared<TopoDS_Shape>(box), reference, 10.0,
                    ExtrudeOperation::Join, false);
      CHECK(session.operation() == ExtrudeOperation::Join);

      // Outward Cut on the top face does not intersect the body, so the
      // preview is recomputed and becomes invalid.
      session.setOperation(ExtrudeOperation::Cut);
      CHECK(session.operation() == ExtrudeOperation::Cut);
      CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);

      session.setOperation(ExtrudeOperation::NewBody);
      CHECK(session.operation() == ExtrudeOperation::NewBody);
      CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
      CHECK(!session.error().empty());
      CHECK(session.previewShape() == nullptr);
    }

    // 6. Non-planar face -> PreviewInvalid + planar-required error, no crash.
    {
      const TopoDS_Shape cylinder =
          BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0.0, 0.0, 0.0),
                                          gp_Dir(0.0, 0.0, 1.0)),
                                   10.0, 20.0)
              .Shape();
      const auto side = nonPlanarFaceIndex(cylinder);
      CHECK(side);
      const auto reference = solidar::makeFaceReference(cylinder, 1, 1, *side);
      solidar::ExtrudeToolSession session;
      session.begin(1, 1, std::make_shared<TopoDS_Shape>(cylinder), reference,
                    10.0, ExtrudeOperation::Join, false);
      CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
      CHECK(!session.error().empty());
      CHECK(session.error().find("planar") != std::string::npos);
      CHECK(session.previewShape() == nullptr);
    }

    // 7. cancel() -> Inactive, preview null.
    {
      const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
      const auto reference = boxTopFace(box, 20.0);
      solidar::ExtrudeToolSession session;
      session.begin(1, 1, std::make_shared<TopoDS_Shape>(box), reference, 10.0,
                    ExtrudeOperation::Join, false);
      CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
      session.cancel();
      CHECK(session.lifecycle() == solidar::ToolLifecycle::Inactive);
      CHECK(session.previewShape() == nullptr);
    }

    // 8. manipulator() minimum == 0.01 and a bounded maximum.
    {
      const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
      const auto reference = boxTopFace(box, 20.0);
      solidar::ExtrudeToolSession session;
      session.begin(1, 1, std::make_shared<TopoDS_Shape>(box), reference, 10.0,
                    ExtrudeOperation::Join, false);
      const auto manip = session.manipulator();
      CHECK(manip.has_value());
      CHECK(near(manip->minimumMm, 0.01));
      CHECK(manip->maximumMm >= manip->valueMm);
      CHECK(manip->maximumMm <= 100000.0);
    }
  } catch (const std::exception& error) {
    std::cerr << "extrude tool session regression failure: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
