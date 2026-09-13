#include "model/SketchExtrudeBuilder.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "model/Document.h"
#include "model/SketchProfileBuilder.h"

namespace solidar {
namespace {

bool samePoint(sketch::Point a, sketch::Point b) {
  return std::abs(a.xMm - b.xMm) <= 1e-7 &&
         std::abs(a.yMm - b.yMm) <= 1e-7;
}

bool exactlyOneSolid(const TopoDS_Shape& shape, TopoDS_Shape* solid,
                     std::string* error) {
  TopExp_Explorer solids(shape, TopAbs_SOLID);
  if (!solids.More()) {
    if (error) *error = "Extrude result does not contain a solid";
    return false;
  }
  TopoDS_Shape candidate = solids.Current();
  solids.Next();
  if (solids.More()) {
    if (error) *error = "Extrude result contains multiple solids";
    return false;
  }
  *solid = candidate;
  return true;
}

}  // namespace

bool isSupportedSingleSketchProfile(const DocumentSketch& profile,
                                    std::string* error) {
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  if (profile.id == kInvalidSketchId) return fail("Profile SketchId is invalid");
  if (!profile.supportResolved) return fail("Profile support is unresolved");

  const auto normal = profile.placement.normal();
  const double normalLength = std::sqrt(normal.x * normal.x + normal.y * normal.y +
                                        normal.z * normal.z);
  if (!std::isfinite(normal.x) || !std::isfinite(normal.y) ||
      !std::isfinite(normal.z) || normalLength < 1e-12)
    return fail("Profile placement normal is degenerate");

  std::vector<const sketch::Line*> lines;
  for (const auto& line : profile.geometry.lines()) {
    if (line.dashed) continue;
    if (!std::isfinite(line.start.xMm) || !std::isfinite(line.start.yMm) ||
        !std::isfinite(line.end.xMm) || !std::isfinite(line.end.yMm))
      return fail("Profile line coordinates must be finite");
    lines.push_back(&line);
  }
  std::vector<const sketch::Circle*> circles;
  for (const auto& circle : profile.geometry.circles()) {
    if (circle.dashed) continue;
    if (!std::isfinite(circle.center.xMm) ||
        !std::isfinite(circle.center.yMm) ||
        !std::isfinite(circle.radiusMm) || circle.radiusMm <= 0.0)
      return fail("Profile circle must be finite and positive");
    circles.push_back(&circle);
  }
  if (!lines.empty() && !circles.empty())
    return fail("Extrude 2.0 supports one profile at a time");
  if (circles.size() == 1 && lines.empty()) return true;
  if (!circles.empty()) return fail("Profile contains multiple circles");
  if (lines.size() < 3 || !profile.geometry.isClosed())
    return fail("Profile is not one closed line wire");

  // isClosed permits several independent loops. Walk endpoint-connected lines
  // to prove that every solid line belongs to one wire.
  std::vector<bool> reached(lines.size(), false);
  reached[0] = true;
  std::size_t reachedCount = 1;
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (reached[i]) continue;
      for (std::size_t j = 0; j < lines.size(); ++j) {
        if (!reached[j]) continue;
        if (samePoint(lines[i]->start, lines[j]->start) ||
            samePoint(lines[i]->start, lines[j]->end) ||
            samePoint(lines[i]->end, lines[j]->start) ||
            samePoint(lines[i]->end, lines[j]->end)) {
          reached[i] = true;
          ++reachedCount;
          changed = true;
          break;
        }
      }
    }
  }
  return reachedCount == lines.size()
             ? true
             : fail("Profile contains multiple closed regions or holes");
}

bool buildExtrusionFromSketch(const DocumentSketch& profile,
                              const TopoDS_Shape* baseShape,
                              double lengthMm, ExtrudeOperation operation,
                              bool reversed, TopoDS_Shape* result,
                              SketchExtrudeGeometry* geometry,
                              std::string* error) {
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  if (!result) return fail("Sketch extrude result output is missing");
  if (!std::isfinite(lengthMm) || lengthMm <= 0.0)
    return fail("Extrude length must be a finite positive value");
  if (!isSupportedSingleSketchProfile(profile, error)) return false;
  if (operation == ExtrudeOperation::NewBody) {
    if (baseShape && !baseShape->IsNull())
      return fail("Extrude New Body must be the first feature of a Body");
  } else if (!baseShape || baseShape->IsNull()) {
    return fail(operation == ExtrudeOperation::Join
                    ? "Extrude Join base shape is missing"
                    : "Extrude Cut base shape is missing");
  }

  try {
    TopoDS_Face profileFace;
    if (!buildPlanarFaceFromSketch(profile, &profileFace, error)) return false;
    GProp_GProps surfaceProperties;
    BRepGProp::SurfaceProperties(profileFace, surfaceProperties);

    TopoDS_Shape prism;
    if (!buildExtrusionPrismFromSketch(profile, lengthMm, reversed, &prism,
                                       error))
      return false;
    TopoDS_Shape prismSolid;
    if (!exactlyOneSolid(prism, &prismSolid, error)) return false;

    TopoDS_Shape singleSolid;
    if (operation == ExtrudeOperation::NewBody) {
      singleSolid = prismSolid;
    } else {
      GProp_GProps beforeProperties;
      BRepGProp::VolumeProperties(*baseShape, beforeProperties);
      TopoDS_Shape booleanResult;
      if (operation == ExtrudeOperation::Join) {
        BRepAlgoAPI_Fuse fuse(*baseShape, prismSolid);
        fuse.Build();
        if (!fuse.IsDone() || fuse.Shape().IsNull())
          return fail("Extrude Join boolean fuse failed");
        booleanResult = fuse.Shape();
      } else {
        BRepAlgoAPI_Cut cut(*baseShape, prismSolid);
        cut.Build();
        if (!cut.IsDone() || cut.Shape().IsNull())
          return fail("Extrude Cut boolean cut failed");
        booleanResult = cut.Shape();
      }
      if (!exactlyOneSolid(booleanResult, &singleSolid, error)) {
        if (operation == ExtrudeOperation::Join && error &&
            *error == "Extrude result contains multiple solids")
          *error = "Extrude Join does not intersect the body";
        return false;
      }
      GProp_GProps afterProperties;
      BRepGProp::VolumeProperties(singleSolid, afterProperties);
      const double before = beforeProperties.Mass();
      const double after = afterProperties.Mass();
      const double tolerance = std::max(1e-7, std::abs(before) * 1e-10);
      if (operation == ExtrudeOperation::Join && after <= before + tolerance)
        return fail("Extrude Join does not intersect the body");
      if (operation == ExtrudeOperation::Cut && after >= before - tolerance)
        return fail("Extrude Cut does not intersect the body");
      if (operation == ExtrudeOperation::Join) {
        ShapeUpgrade_UnifySameDomain unify(singleSolid, true, true, false);
        unify.Build();
        if (unify.Shape().IsNull())
          return fail("Extrude same-domain unification failed");
        singleSolid = unify.Shape();
      }
    }

    BRepCheck_Analyzer analyzer(singleSolid);
    if (!analyzer.IsValid()) return fail("Extrude result is invalid");
    *result = singleSolid;
    if (geometry) {
      geometry->centroid = surfaceProperties.CentreOfMass();
      const auto normal = profile.placement.normal();
      geometry->normal = gp_Dir(normal.x, normal.y, normal.z);
    }
    return true;
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    return fail(message && *message
                    ? std::string("OCCT sketch extrude error: ") + message
                    : "OCCT sketch extrude operation failed");
  } catch (const std::exception& failure) {
    return fail(std::string("Sketch extrude error: ") + failure.what());
  } catch (...) {
    return fail("Unexpected sketch extrude geometry error");
  }
}

}  // namespace solidar
