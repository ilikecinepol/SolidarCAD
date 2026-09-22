#include <BRepCheck_Analyzer.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#include "TestGeometryUtils.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/SketchExtrudeBuilder.h"

#define CHECK(x)                                                    \
  do {                                                              \
    if (!(x)) {                                                     \
      std::cerr << __LINE__ << ": " #x "\n";                        \
      return EXIT_FAILURE;                                          \
    }                                                               \
  } while (false)

namespace {

solidar::DocumentSketch profileWith(solidar::SketchId id) {
  solidar::DocumentSketch profile;
  profile.id = id;
  return profile;
}

}  // namespace

int main() {
  using namespace solidar;

  // One closed line wire is supported.
  {
    auto profile = profileWith(1);
    profile.geometry.addRectangle({0.0, 0.0}, {20.0, 10.0});
    CHECK(isSupportedSingleSketchProfile(profile));
  }

  // One circle is supported.
  {
    auto profile = profileWith(2);
    profile.geometry.addCircle({0.0, 0.0}, 5.0);
    CHECK(isSupportedSingleSketchProfile(profile));
  }

  // Multiple independent loops are rejected.
  {
    auto profile = profileWith(3);
    profile.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    profile.geometry.addRectangle({20.0, 0.0}, {30.0, 10.0});
    std::string error;
    CHECK(!isSupportedSingleSketchProfile(profile, &error));
    CHECK(!error.empty());
    CHECK(isSupportedSketchProfile(profile, &error));
  }

  // The legacy Sketch::isClosed convenience predicate rejects mixed primitive
  // families, but multi-region extrusion accepts disjoint closed regions. The
  // Apply path must therefore use isSupportedSketchProfile instead.
  {
    auto profile = profileWith(24);
    profile.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    profile.geometry.addCircle({30.0, 5.0}, 5.0);
    CHECK(!profile.geometry.isClosed());
    CHECK(isSupportedSketchProfile(profile));

    TopoDS_Shape result;
    std::string error;
    CHECK(buildExtrusionFromSketch(profile, nullptr, 6.0,
                                   ExtrudeOperation::NewBody, false, &result,
                                   nullptr, &error));
    CHECK(test::solidCount(result) == 2);
  }

  // A hole (nested loop) is rejected.
  {
    auto profile = profileWith(4);
    profile.geometry.addRectangle({0.0, 0.0}, {30.0, 30.0});
    profile.geometry.addRectangle({10.0, 10.0}, {20.0, 20.0});
    std::string error;
    CHECK(!isSupportedSingleSketchProfile(profile, &error));
    CHECK(!error.empty());
    CHECK(!isSupportedSketchProfile(profile, &error));
  }

  // Mixed line + circle profiles are rejected with the committed message.
  {
    auto profile = profileWith(5);
    profile.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
    profile.geometry.addCircle({10.0, 10.0}, 3.0);
    std::string error;
    CHECK(!isSupportedSingleSketchProfile(profile, &error));
    CHECK(error.find("one profile") != std::string::npos);
  }

  // Unresolved support is rejected.
  {
    auto profile = profileWith(6);
    profile.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
    profile.supportResolved = false;
    std::string error;
    CHECK(!isSupportedSingleSketchProfile(profile, &error));
    CHECK(!error.empty());
  }

  // Non-finite coordinates are rejected.
  {
    auto profile = profileWith(7);
    profile.geometry.addLine({0.0, 0.0}, {10.0, 0.0});
    profile.geometry.addLine({10.0, 0.0}, {10.0, 10.0});
    profile.geometry.addLine(
        {10.0, 10.0}, {std::numeric_limits<double>::quiet_NaN(), 0.0});
    std::string error;
    CHECK(!isSupportedSingleSketchProfile(profile, &error));
    CHECK(!error.empty());
  }

  // NewBody yields one valid solid with the expected volume and bounds.
  {
    auto profile = profileWith(8);
    profile.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
    TopoDS_Shape result;
    std::string error;
    CHECK(buildExtrusionFromSketch(profile, nullptr, 50.0,
                                   ExtrudeOperation::NewBody, false, &result,
                                   nullptr, &error));
    CHECK(!result.IsNull());
    CHECK(test::solidCount(result) == 1);
    BRepCheck_Analyzer analyzer(result);
    CHECK(analyzer.IsValid());
    CHECK(test::near(test::volumeOf(result), 80.0 * 35.0 * 50.0, 1e-3));
    const auto bounds = test::boundsOf(result);
    CHECK(test::near(bounds.x(), 80.0, 1e-4));
    CHECK(test::near(bounds.y(), 35.0, 1e-4));
    CHECK(test::near(bounds.z(), 50.0, 1e-4));
  }

  // Join increases volume versus the base shape.
  {
    auto baseProfile = profileWith(9);
    baseProfile.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
    TopoDS_Shape base;
    CHECK(buildExtrusionFromSketch(baseProfile, nullptr, 50.0,
                                   ExtrudeOperation::NewBody, false, &base,
                                   nullptr, nullptr));
    const double before = test::volumeOf(base);

    auto joinProfile = profileWith(10);
    joinProfile.placement.origin.z = 40.0;
    joinProfile.geometry.addRectangle({10.0, 10.0}, {20.0, 20.0});
    TopoDS_Shape joined;
    std::string error;
    CHECK(buildExtrusionFromSketch(joinProfile, &base, 20.0,
                                   ExtrudeOperation::Join, false, &joined,
                                   nullptr, &error));
    CHECK(test::volumeOf(joined) > before + 1e-6);
  }

  // Cut decreases volume versus the base shape.
  {
    auto baseProfile = profileWith(11);
    baseProfile.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
    TopoDS_Shape base;
    CHECK(buildExtrusionFromSketch(baseProfile, nullptr, 50.0,
                                   ExtrudeOperation::NewBody, false, &base,
                                   nullptr, nullptr));
    const double before = test::volumeOf(base);

    auto cutProfile = profileWith(12);
    cutProfile.geometry.addRectangle({10.0, 10.0}, {20.0, 20.0});
    TopoDS_Shape cut;
    std::string error;
    CHECK(buildExtrusionFromSketch(cutProfile, &base, 20.0,
                                   ExtrudeOperation::Cut, false, &cut, nullptr,
                                   &error));
    CHECK(test::volumeOf(cut) < before - 1e-6);
  }

  // Join on a non-intersecting prism fails with a non-empty error.
  {
    auto baseProfile = profileWith(13);
    baseProfile.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    TopoDS_Shape base;
    CHECK(buildExtrusionFromSketch(baseProfile, nullptr, 10.0,
                                   ExtrudeOperation::NewBody, false, &base,
                                   nullptr, nullptr));

    auto remoteProfile = profileWith(14);
    remoteProfile.placement.origin.z = 100.0;
    remoteProfile.geometry.addCircle({0.0, 0.0}, 5.0);
    TopoDS_Shape joined;
    std::string error;
    CHECK(!buildExtrusionFromSketch(remoteProfile, &base, 10.0,
                                    ExtrudeOperation::Join, false, &joined,
                                    nullptr, &error));
    CHECK(!error.empty());
  }

  // Cut on a non-intersecting prism fails with a non-empty error.
  {
    auto baseProfile = profileWith(15);
    baseProfile.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    TopoDS_Shape base;
    CHECK(buildExtrusionFromSketch(baseProfile, nullptr, 10.0,
                                   ExtrudeOperation::NewBody, false, &base,
                                   nullptr, nullptr));

    auto remoteProfile = profileWith(16);
    remoteProfile.placement.origin.z = 100.0;
    remoteProfile.geometry.addCircle({0.0, 0.0}, 5.0);
    TopoDS_Shape cut;
    std::string error;
    CHECK(!buildExtrusionFromSketch(remoteProfile, &base, 10.0,
                                    ExtrudeOperation::Cut, false, &cut,
                                    nullptr, &error));
    CHECK(!error.empty());
  }

  // NewBody with a non-null base shape fails with the committed message.
  {
    auto baseProfile = profileWith(17);
    baseProfile.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    TopoDS_Shape base;
    CHECK(buildExtrusionFromSketch(baseProfile, nullptr, 10.0,
                                   ExtrudeOperation::NewBody, false, &base,
                                   nullptr, nullptr));

    auto profile = profileWith(18);
    profile.geometry.addCircle({0.0, 0.0}, 5.0);
    TopoDS_Shape result;
    std::string error;
    CHECK(!buildExtrusionFromSketch(profile, &base, 10.0,
                                     ExtrudeOperation::NewBody, false, &result,
                                     nullptr, &error));
    CHECK(error.find("first feature") != std::string::npos);
  }

  // A closed contour with arcs (a stadium) is supported and extrudes into a
  // valid solid, proving that arcs survive the sketch -> profile -> 3D path.
  {
    constexpr double kPi = 3.14159265358979323846;
    auto profile = profileWith(19);
    profile.geometry.addLine({0.0, 0.0}, {100.0, 0.0});
    profile.geometry.addArc({100.0, 10.0}, 10.0, -kPi * 0.5, kPi);
    profile.geometry.addLine({100.0, 20.0}, {0.0, 20.0});
    profile.geometry.addArc({0.0, 10.0}, 10.0, kPi * 0.5, kPi);
    CHECK(isSupportedSingleSketchProfile(profile));

    TopoDS_Shape result;
    std::string error;
    CHECK(buildExtrusionFromSketch(profile, nullptr, 50.0,
                                   ExtrudeOperation::NewBody, false, &result,
                                   nullptr, &error));
    CHECK(!result.IsNull());
    CHECK(test::solidCount(result) == 1);
    BRepCheck_Analyzer analyzer(result);
    CHECK(analyzer.IsValid());
    // Stadium area = rectangle 100x20 + full circle radius 10.
    const double expected = (2000.0 + kPi * 100.0) * 50.0;
    CHECK(test::near(test::volumeOf(result), expected, 1e-2));
  }

  // Multiple disjoint selected contours are extruded by one feature and
  // retained as a valid multi-solid result.
  {
    auto profile = profileWith(23);
    profile.geometry.addRectangle({0.0, 0.0}, {20.0, 10.0});
    profile.geometry.addRectangle({40.0, 0.0}, {50.0, 10.0});
    CHECK(isSupportedSketchProfile(profile));
    TopoDS_Shape result;
    std::string error;
    CHECK(buildExtrusionFromSketch(profile, nullptr, 8.0,
                                   ExtrudeOperation::NewBody, false, &result,
                                   nullptr, &error));
    CHECK(test::solidCount(result) == 2);
    BRepCheck_Analyzer analyzer(result);
    CHECK(analyzer.IsValid());
    CHECK(test::near(test::volumeOf(result), (20.0 * 10.0 + 10.0 * 10.0) * 8.0,
                     1e-3));
  }

  // A two-edge D profile (semicircle plus diameter) is a valid closed wire.
  {
    constexpr double kPi = 3.14159265358979323846;
    auto profile = profileWith(20);
    profile.geometry.addLine({-10.0, 0.0}, {10.0, 0.0});
    profile.geometry.addArc({0.0, 0.0}, 10.0, 0.0, kPi);
    CHECK(isSupportedSingleSketchProfile(profile));

    TopoDS_Shape result;
    std::string error;
    CHECK(buildExtrusionFromSketch(profile, nullptr, 25.0,
                                   ExtrudeOperation::NewBody, false, &result,
                                   nullptr, &error));
    CHECK(test::solidCount(result) == 1);
    BRepCheck_Analyzer analyzer(result);
    CHECK(analyzer.IsValid());
    CHECK(test::near(test::volumeOf(result), 0.5 * kPi * 100.0 * 25.0,
                     1e-2));
  }

  // A circle split into two arcs must extrude exactly like a native circle.
  {
    constexpr double kPi = 3.14159265358979323846;
    auto profile = profileWith(21);
    profile.geometry.addArc({0.0, 0.0}, 10.0, 0.0, kPi);
    profile.geometry.addArc({0.0, 0.0}, 10.0, kPi, kPi);
    CHECK(isSupportedSingleSketchProfile(profile));

    TopoDS_Shape result;
    std::string error;
    CHECK(buildExtrusionFromSketch(profile, nullptr, 25.0,
                                   ExtrudeOperation::NewBody, false, &result,
                                   nullptr, &error));
    CHECK(test::solidCount(result) == 1);
    BRepCheck_Analyzer analyzer(result);
    CHECK(analyzer.IsValid());
    CHECK(test::near(test::volumeOf(result), kPi * 100.0 * 25.0, 1e-2));
  }

  // A partial rectangle side replaced by an outward semicircle remains a
  // supported explicit outer-wire profile.
  {
    constexpr double kPi = 3.14159265358979323846;
    auto profile = profileWith(22);
    profile.geometry.addLine({-20.0, -20.0}, {20.0, -20.0});
    profile.geometry.addLine({20.0, -20.0}, {20.0, 20.0});
    profile.geometry.addLine({20.0, 20.0}, {-20.0, 20.0});
    profile.geometry.addLine({-20.0, 20.0}, {-20.0, 0.0});
    profile.geometry.addArc({-20.0, -10.0}, 10.0, kPi * 0.5, kPi);
    CHECK(isSupportedSingleSketchProfile(profile));

    TopoDS_Shape result;
    std::string error;
    CHECK(buildExtrusionFromSketch(profile, nullptr, 10.0,
                                   ExtrudeOperation::NewBody, false, &result,
                                   nullptr, &error));
    CHECK(test::solidCount(result) == 1);
    BRepCheck_Analyzer analyzer(result);
    CHECK(analyzer.IsValid());
    const double expectedArea = 40.0 * 40.0 + 0.5 * kPi * 100.0;
    CHECK(test::near(test::volumeOf(result), expectedArea * 10.0, 1e-2));
  }

  return EXIT_SUCCESS;
}
