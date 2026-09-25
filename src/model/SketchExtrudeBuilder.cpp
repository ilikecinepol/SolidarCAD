#include "model/SketchExtrudeBuilder.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Common.hxx>
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

bool hasAnyTopology(const TopoDS_Shape& shape) {
  if (shape.IsNull()) return false;
  if (shape.ShapeType() != TopAbs_COMPOUND &&
      shape.ShapeType() != TopAbs_COMPSOLID)
    return true;
  for (const auto type : {TopAbs_SOLID, TopAbs_FACE, TopAbs_EDGE,
                          TopAbs_VERTEX}) {
    TopExp_Explorer explorer(shape, type);
    if (explorer.More()) return true;
  }
  return false;
}

std::vector<DocumentSketch> closedProfileComponents(
    const DocumentSketch& profile) {
  struct Primitive {
    bool arc{};
    std::size_t index{};
    sketch::Point start;
    sketch::Point end;
  };
  std::vector<Primitive> primitives;
  for (std::size_t index = 0; index < profile.geometry.lines().size(); ++index) {
    const auto& line = profile.geometry.lines()[index];
    if (!line.dashed)
      primitives.push_back({false, index, line.start, line.end});
  }
  for (std::size_t index = 0; index < profile.geometry.arcs().size(); ++index) {
    const auto& arc = profile.geometry.arcs()[index];
    if (!arc.dashed)
      primitives.push_back(
          {true, index, sketch::arcStartPoint(arc), sketch::arcEndPoint(arc)});
  }

  std::vector<DocumentSketch> result;
  std::vector<bool> assigned(primitives.size(), false);
  for (std::size_t seed = 0; seed < primitives.size(); ++seed) {
    if (assigned[seed]) continue;
    std::vector<std::size_t> component{seed};
    assigned[seed] = true;
    for (std::size_t position = 0; position < component.size(); ++position) {
      const auto& reached = primitives[component[position]];
      for (std::size_t candidate = 0; candidate < primitives.size();
           ++candidate) {
        if (assigned[candidate]) continue;
        const auto& edge = primitives[candidate];
        if (samePoint(reached.start, edge.start) ||
            samePoint(reached.start, edge.end) ||
            samePoint(reached.end, edge.start) ||
            samePoint(reached.end, edge.end)) {
          assigned[candidate] = true;
          component.push_back(candidate);
        }
      }
    }

    DocumentSketch part = profile;
    part.geometry.clear();
    for (const auto primitiveIndex : component) {
      const auto& primitive = primitives[primitiveIndex];
      if (primitive.arc) {
        const auto& arc = profile.geometry.arcs()[primitive.index];
        part.geometry.addArc(arc.center, arc.radiusMm, arc.startAngleRad,
                             arc.sweepAngleRad);
      } else {
        const auto& line = profile.geometry.lines()[primitive.index];
        part.geometry.addLine(line.start, line.end);
      }
    }
    result.push_back(std::move(part));
  }

  for (const auto& circle : profile.geometry.circles()) {
    if (circle.dashed) continue;
    DocumentSketch part = profile;
    part.geometry.clear();
    part.geometry.addCircle(circle.center, circle.radiusMm);
    result.push_back(std::move(part));
  }
  return result;
}

std::size_t shapeSolidCount(const TopoDS_Shape& shape) {
  std::size_t count = 0;
  for (TopExp_Explorer solids(shape, TopAbs_SOLID); solids.More(); solids.Next())
    ++count;
  return count;
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
  std::vector<const sketch::Arc*> arcs;
  for (const auto& arc : profile.geometry.arcs()) {
    if (arc.dashed) continue;
    if (!std::isfinite(arc.center.xMm) ||
        !std::isfinite(arc.center.yMm) ||
        !std::isfinite(arc.radiusMm) || arc.radiusMm <= 0.0)
      return fail("Profile arc must be finite and positive");
    arcs.push_back(&arc);
  }
  if ((!lines.empty() || !arcs.empty()) && !circles.empty())
    return fail("Extrude 2.0 supports one profile at a time");
  if (circles.size() == 1 && lines.empty() && arcs.empty()) return true;
  if (!circles.empty()) return fail("Profile contains multiple circles");
  // A closed wire may legitimately consist of only two edges, for example a
  // semicircular arc and its diameter or two arcs with common endpoints.
  if (lines.size() + arcs.size() < 2 || !profile.geometry.isClosed())
    return fail("Profile is not one closed wire");

  // isClosed permits several independent loops. Walk endpoint-connected lines
  // and arcs to prove that every solid edge belongs to one wire.
  struct Edge {
    sketch::Point start;
    sketch::Point end;
  };
  std::vector<Edge> edges;
  edges.reserve(lines.size() + arcs.size());
  for (const auto* line : lines)
    edges.push_back({line->start, line->end});
  for (const auto* arc : arcs)
    edges.push_back({sketch::arcStartPoint(*arc), sketch::arcEndPoint(*arc)});

  std::vector<bool> reached(edges.size(), false);
  reached[0] = true;
  std::size_t reachedCount = 1;
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t i = 0; i < edges.size(); ++i) {
      if (reached[i]) continue;
      for (std::size_t j = 0; j < edges.size(); ++j) {
        if (!reached[j]) continue;
        if (samePoint(edges[i].start, edges[j].start) ||
            samePoint(edges[i].start, edges[j].end) ||
            samePoint(edges[i].end, edges[j].start) ||
            samePoint(edges[i].end, edges[j].end)) {
          reached[i] = true;
          ++reachedCount;
          changed = true;
          break;
        }
      }
    }
  }
  return reachedCount == edges.size()
             ? true
             : fail("Profile contains multiple closed regions or holes");
}

bool isSupportedSketchProfile(const DocumentSketch& profile,
                              std::string* error) {
  // Endpoint-connected contours are not independent regions.  Detect branch
  // vertices before component extraction so touching line/arc loops produce a
  // stable diagnostic on every OCCT platform rather than falling through to
  // wire construction with version-dependent errors.
  std::vector<sketch::Point> endpoints;
  for (const auto& line : profile.geometry.lines()) {
    if (line.dashed) continue;
    endpoints.push_back(line.start);
    endpoints.push_back(line.end);
  }
  for (const auto& arc : profile.geometry.arcs()) {
    if (arc.dashed) continue;
    endpoints.push_back(sketch::arcStartPoint(arc));
    endpoints.push_back(sketch::arcEndPoint(arc));
  }
  for (std::size_t index = 0; index < endpoints.size(); ++index) {
    std::size_t degree = 0;
    for (const auto& endpoint : endpoints)
      if (samePoint(endpoints[index], endpoint)) ++degree;
    if (degree > 2) {
      if (error) *error = "Profile regions touch each other";
      return false;
    }
  }
  const auto components = closedProfileComponents(profile);
  if (components.empty()) {
    if (error) *error = "Profile has insufficient geometry";
    return false;
  }
  for (const auto& component : components)
    if (!isSupportedSingleSketchProfile(component, error)) return false;

  // Independent selected regions must be strictly disjoint.  Nested,
  // overlapping and touching contours require topology semantics that the
  // current profile snapshot does not represent unambiguously.
  try {
    std::vector<TopoDS_Face> faces;
    faces.reserve(components.size());
    for (const auto& component : components) {
      TopoDS_Face face;
      if (!buildPlanarFaceFromSketch(component, &face, error)) return false;
      BRepCheck_Analyzer analyzer(face);
      if (!analyzer.IsValid()) {
        if (error) *error = "Profile region is self-intersecting or invalid";
        return false;
      }
      GProp_GProps properties;
      BRepGProp::SurfaceProperties(face, properties);
      if (!std::isfinite(properties.Mass()) ||
          std::abs(properties.Mass()) <= 1e-8) {
        if (error) *error = "Profile region has zero area";
        return false;
      }
      faces.push_back(face);
    }
    for (std::size_t first = 0; first < faces.size(); ++first) {
      for (std::size_t second = first + 1; second < faces.size(); ++second) {
        BRepAlgoAPI_Common common(faces[first], faces[second]);
        common.Build();
        if (!common.IsDone() || common.Shape().IsNull()) {
          if (error) *error = "Could not classify multiple profile regions";
          return false;
        }
        GProp_GProps overlap;
        BRepGProp::SurfaceProperties(common.Shape(), overlap);
        if (std::abs(overlap.Mass()) > 1e-8) {
          if (error) *error = "Profile regions overlap or form a hole";
          return false;
        }
        if (hasAnyTopology(common.Shape())) {
          if (error) *error = "Profile regions touch each other";
          return false;
        }
      }
    }
  } catch (const Standard_Failure&) {
    if (error) *error = "Could not classify multiple profile regions";
    return false;
  }
  return true;
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
  if (!isSupportedSketchProfile(profile, error)) return false;
  if (operation == ExtrudeOperation::NewBody) {
    if (baseShape && !baseShape->IsNull())
      return fail("Extrude New Body must be the first feature of a Body");
  } else if (!baseShape || baseShape->IsNull()) {
    return fail(operation == ExtrudeOperation::Join
                    ? "Extrude Join base shape is missing"
                    : "Extrude Cut base shape is missing");
  }

  try {
    const auto components = closedProfileComponents(profile);
    std::vector<TopoDS_Shape> prismSolids;
    prismSolids.reserve(components.size());
    double totalArea = 0.0;
    double centroidX = 0.0;
    double centroidY = 0.0;
    double centroidZ = 0.0;
    for (const auto& component : components) {
      TopoDS_Face profileFace;
      if (!buildPlanarFaceFromSketch(component, &profileFace, error))
        return false;
      GProp_GProps surfaceProperties;
      BRepGProp::SurfaceProperties(profileFace, surfaceProperties);
      const double area = std::abs(surfaceProperties.Mass());
      const gp_Pnt center = surfaceProperties.CentreOfMass();
      totalArea += area;
      centroidX += center.X() * area;
      centroidY += center.Y() * area;
      centroidZ += center.Z() * area;

      TopoDS_Shape prism;
      if (!buildExtrusionPrismFromSketch(component, lengthMm, reversed, &prism,
                                         error))
        return false;
      TopoDS_Shape prismSolid;
      if (!exactlyOneSolid(prism, &prismSolid, error)) return false;
      prismSolids.push_back(std::move(prismSolid));
    }

    TopoDS_Shape extrusionTool = prismSolids.front();
    for (std::size_t index = 1; index < prismSolids.size(); ++index) {
      BRepAlgoAPI_Fuse fuse(extrusionTool, prismSolids[index]);
      fuse.Build();
      if (!fuse.IsDone() || fuse.Shape().IsNull())
        return fail("Could not combine selected profile regions");
      extrusionTool = fuse.Shape();
    }
    if (shapeSolidCount(extrusionTool) == 0)
      return fail("Extrude result does not contain a solid");

    TopoDS_Shape outputShape;
    if (operation == ExtrudeOperation::NewBody) {
      outputShape = extrusionTool;
    } else {
      GProp_GProps beforeProperties;
      BRepGProp::VolumeProperties(*baseShape, beforeProperties);
      const std::size_t beforeSolidCount = shapeSolidCount(*baseShape);
      const double before = beforeProperties.Mass();
      const double tolerance = std::max(1e-7, std::abs(before) * 1e-10);

      // A multi-region boolean is accepted only when every selected region
      // contributes on its own.  This prevents one valid region from masking
      // another disconnected/remote region in the aggregate operation.
      for (const auto& prismSolid : prismSolids) {
        TopoDS_Shape individualResult;
        if (operation == ExtrudeOperation::Join) {
          BRepAlgoAPI_Fuse fuse(*baseShape, prismSolid);
          fuse.Build();
          if (!fuse.IsDone() || fuse.Shape().IsNull())
            return fail("Extrude Join region boolean fuse failed");
          individualResult = fuse.Shape();
          if (shapeSolidCount(individualResult) > beforeSolidCount)
            return fail("Extrude Join region does not intersect the body");
        } else {
          BRepAlgoAPI_Cut cut(*baseShape, prismSolid);
          cut.Build();
          if (!cut.IsDone() || cut.Shape().IsNull())
            return fail("Extrude Cut region boolean cut failed");
          individualResult = cut.Shape();
        }
        GProp_GProps individualProperties;
        BRepGProp::VolumeProperties(individualResult, individualProperties);
        const double individualVolume = individualProperties.Mass();
        if (operation == ExtrudeOperation::Join &&
            individualVolume <= before + tolerance)
          return fail("Extrude Join region does not intersect the body");
        if (operation == ExtrudeOperation::Cut &&
            individualVolume >= before - tolerance)
          return fail("Extrude Cut region does not intersect the body");
      }

      TopoDS_Shape booleanResult;
      if (operation == ExtrudeOperation::Join) {
        BRepAlgoAPI_Fuse fuse(*baseShape, extrusionTool);
        fuse.Build();
        if (!fuse.IsDone() || fuse.Shape().IsNull())
          return fail("Extrude Join boolean fuse failed");
        booleanResult = fuse.Shape();
      } else {
        BRepAlgoAPI_Cut cut(*baseShape, extrusionTool);
        cut.Build();
        if (!cut.IsDone() || cut.Shape().IsNull())
          return fail("Extrude Cut boolean cut failed");
        booleanResult = cut.Shape();
      }
      if (shapeSolidCount(booleanResult) == 0)
        return fail("Extrude result does not contain a solid");
      if (operation == ExtrudeOperation::Join &&
          shapeSolidCount(booleanResult) > beforeSolidCount)
        return fail("Extrude Join does not intersect the body");
      if (operation == ExtrudeOperation::Cut &&
          shapeSolidCount(booleanResult) > beforeSolidCount)
        return fail("Extrude Cut would split the body");
      outputShape = booleanResult;
      GProp_GProps afterProperties;
      BRepGProp::VolumeProperties(outputShape, afterProperties);
      const double after = afterProperties.Mass();
      if (operation == ExtrudeOperation::Join && after <= before + tolerance)
        return fail("Extrude Join does not intersect the body");
      if (operation == ExtrudeOperation::Cut && after >= before - tolerance)
        return fail("Extrude Cut does not intersect the body");
      if (operation == ExtrudeOperation::Join) {
        ShapeUpgrade_UnifySameDomain unify(outputShape, true, true, false);
        unify.Build();
        if (unify.Shape().IsNull())
          return fail("Extrude same-domain unification failed");
        outputShape = unify.Shape();
      }
    }

    BRepCheck_Analyzer analyzer(outputShape);
    if (!analyzer.IsValid()) return fail("Extrude result is invalid");
    SketchExtrudeGeometry outputGeometry;
    if (geometry) {
      if (totalArea <= 1e-12)
        return fail("Extrude profile area is zero");
      outputGeometry.centroid =
          gp_Pnt(centroidX / totalArea, centroidY / totalArea,
                 centroidZ / totalArea);
      const auto normal = profile.placement.normal();
      outputGeometry.normal = gp_Dir(normal.x, normal.y, normal.z);
    }
    *result = outputShape;
    if (geometry) *geometry = outputGeometry;
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
