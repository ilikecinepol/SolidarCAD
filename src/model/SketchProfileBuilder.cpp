#include "model/SketchProfileBuilder.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <Geom_BezierCurve.hxx>
#include <NCollection_Array1.hxx>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "model/Document.h"

namespace solidar {

namespace {

bool reconcileFaceEdges(
    const TopoDS_Face& face,
    const std::vector<std::pair<sketch::GeometryId, TopoDS_Edge>>& authored,
    std::vector<SketchProfileEdgeBuild>* edges, std::string* error) {
  const auto fail = [&](const char* message) {
    if (error) *error = message;
    return false;
  };
  if (!edges || authored.empty())
    return fail("Profile edge identity mapping is empty");
  std::vector<TopoDS_Edge> faceEdges;
  for (TopExp_Explorer explorer(face, TopAbs_EDGE); explorer.More();
       explorer.Next()) {
    const auto candidate = TopoDS::Edge(explorer.Current());
    if (std::none_of(faceEdges.begin(), faceEdges.end(),
                     [&](const auto& existing) {
                       return existing.IsSame(candidate);
                     }))
      faceEdges.push_back(candidate);
  }
  if (faceEdges.size() != authored.size())
    return fail("Profile face edge identity coverage is incomplete");

  std::vector<SketchProfileEdgeBuild> mapped;
  mapped.reserve(authored.size());
  for (const auto& [geometryId, authoredEdge] : authored) {
    if (geometryId == sketch::kInvalidGeometryId || authoredEdge.IsNull())
      return fail("Profile edge identity is invalid");
    if (std::any_of(mapped.begin(), mapped.end(), [&](const auto& existing) {
          return existing.geometryId == geometryId;
        }))
      return fail("Profile geometry identity is duplicated");
    const TopoDS_Edge* match = nullptr;
    for (const auto& faceEdge : faceEdges) {
      if (!faceEdge.IsSame(authoredEdge)) continue;
      if (match != nullptr)
        return fail("Profile edge identity is ambiguous");
      match = &faceEdge;
    }
    if (match == nullptr)
      return fail("Profile edge identity is missing from the face");
    if (std::any_of(mapped.begin(), mapped.end(), [&](const auto& existing) {
          return existing.edge->IsSame(*match);
        }))
      return fail("Profile face edge is mapped more than once");
    mapped.push_back(
        {geometryId, std::make_shared<TopoDS_Edge>(*match)});
  }
  for (const auto& faceEdge : faceEdges) {
    if (std::none_of(mapped.begin(), mapped.end(), [&](const auto& mapping) {
          return mapping.edge->IsSame(faceEdge);
        }))
      return fail("Profile face contains an uncovered edge");
  }
  *edges = std::move(mapped);
  return true;
}

}  // namespace

static bool buildPlanarFaceFromSketchImpl(const DocumentSketch& profile,
                                          bool requireEdgeMapping,
                                          SketchProfileFaceBuild* result,
                                          std::string* error) {
  if (error) error->clear();
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  if (!result) return fail("Profile face build output is missing");
  if (!profile.supportResolved) return fail("Profile support is unresolved");
  const auto& geometry = profile.geometry;
  std::size_t solidLines = 0;
  for (const auto& line : geometry.lines())
    if (!line.dashed) ++solidLines;
  std::size_t solidCircles = 0;
  const sketch::Circle* circle = nullptr;
  sketch::GeometryId circleId = sketch::kInvalidGeometryId;
  for (std::size_t index = 0; index < geometry.circles().size(); ++index) {
    const auto& candidate = geometry.circles()[index];
    if (!candidate.dashed) {
      ++solidCircles;
      circle = &candidate;
      circleId = geometry.circleId(index);
    }
  }
  std::size_t solidArcs = 0;
  for (const auto& arc : geometry.arcs())
    if (!arc.dashed) ++solidArcs;
  std::size_t solidBeziers = 0;
  for (const auto& bezier : geometry.beziers())
    if (!bezier.dashed) ++solidBeziers;
  if ((solidLines + solidArcs + solidBeziers > 0 && solidCircles > 0) ||
      solidCircles > 1)
    return fail("Extrude 2.0 supports one profile at a time");
  // Two edges are sufficient for a valid closed wire: a semicircle plus its
  // diameter, or two arcs sharing both endpoints (a lens/full split circle).
  if (solidCircles == 0 && solidLines + solidArcs + solidBeziers < 2)
    return fail("Profile has insufficient geometry");
  if (solidCircles == 0 && !geometry.isClosed())
    return fail("Profile is not closed");

  try {
    if (circle) {
      if (!std::isfinite(circle->radiusMm) || circle->radiusMm <= 0.0 ||
          !std::isfinite(circle->center.xMm) ||
          !std::isfinite(circle->center.yMm))
        return fail("Circle radius and center must be finite and positive");
      const auto center = profile.placement.toWorld(circle->center.xMm,
                                                    circle->center.yMm);
      const auto normal = profile.placement.normal();
      const auto x = profile.placement.xDirection;
      gp_Ax2 axes(gp_Pnt(center.x, center.y, center.z),
                  gp_Dir(normal.x, normal.y, normal.z),
                  gp_Dir(x.x, x.y, x.z));
      BRepBuilderAPI_MakeEdge edgeBuilder(gp_Circ(axes, circle->radiusMm));
      if (!edgeBuilder.IsDone()) return fail("Could not build a circle edge");
      BRepBuilderAPI_MakeWire circleWire(edgeBuilder.Edge());
      if (!circleWire.IsDone()) return fail("Could not build a circle wire");
      BRepBuilderAPI_MakeFace circleFace(circleWire.Wire(), true);
      if (!circleFace.IsDone()) return fail("Could not build a circle face");
      const TopoDS_Face builtFace = circleFace.Face();
      BRepCheck_Analyzer analyzer(builtFace);
      if (!analyzer.IsValid()) return fail("Profile face is invalid");
      SketchProfileFaceBuild built;
      built.face = std::make_shared<TopoDS_Face>(builtFace);
      if (requireEdgeMapping &&
          !reconcileFaceEdges(builtFace, {{circleId, edgeBuilder.Edge()}},
                              &built.edges, error))
        return false;
      *result = std::move(built);
      return true;
    }

    const auto toWorldPoint = [&profile](const sketch::Point& point) {
      const auto world = profile.placement.toWorld(point.xMm, point.yMm);
      return gp_Pnt(world.x, world.y, world.z);
    };

    struct PendingEdge {
      sketch::GeometryId geometryId{sketch::kInvalidGeometryId};
      gp_Pnt start;
      gp_Pnt end;
      TopoDS_Edge edge;
    };
    std::vector<PendingEdge> pending;

    // Build one canonical endpoint set for the wire. Sketch snapping and
    // decimal persistence can leave sub-micron noise between endpoints that
    // belongs to the same topological vertex. Prefer exact analytic arc ends,
    // then snap line endpoints to the first matching canonical point.
    constexpr double kEndpointTolerance = 1e-6;
    std::vector<sketch::Point> canonicalPoints;
    for (const auto& arc : geometry.arcs()) {
      if (arc.dashed) continue;
      canonicalPoints.push_back(sketch::arcStartPoint(arc));
      canonicalPoints.push_back(sketch::arcEndPoint(arc));
    }
    for (const auto& bezier : geometry.beziers()) {
      if (bezier.dashed) continue;
      canonicalPoints.push_back(bezier.points[0]);
      canonicalPoints.push_back(bezier.points[3]);
    }
    const auto canonicalPoint = [&canonicalPoints](sketch::Point point) {
      for (const auto& candidate : canonicalPoints)
        if (std::abs(candidate.xMm - point.xMm) <= kEndpointTolerance &&
            std::abs(candidate.yMm - point.yMm) <= kEndpointTolerance)
          return candidate;
      canonicalPoints.push_back(point);
      return point;
    };

    for (std::size_t index = 0; index < geometry.lines().size(); ++index) {
      const auto& line = geometry.lines()[index];
      if (line.dashed) continue;
      const gp_Pnt start = toWorldPoint(canonicalPoint(line.start));
      const gp_Pnt end = toWorldPoint(canonicalPoint(line.end));
      BRepBuilderAPI_MakeEdge edgeBuilder(start, end);
      if (!edgeBuilder.IsDone()) return fail("Could not build a profile edge");
      pending.push_back(
          {geometry.lineId(index), start, end, edgeBuilder.Edge()});
    }

    const auto normal = profile.placement.normal();
    const auto xDirection = profile.placement.xDirection;
    const gp_Dir normalDir(normal.x, normal.y, normal.z);
    const gp_Dir xDir(xDirection.x, xDirection.y, xDirection.z);

    for (std::size_t index = 0; index < geometry.arcs().size(); ++index) {
      const auto& arc = geometry.arcs()[index];
      if (arc.dashed) continue;
      if (!std::isfinite(arc.radiusMm) || arc.radiusMm <= 0.0 ||
          !std::isfinite(arc.center.xMm) || !std::isfinite(arc.center.yMm))
        return fail("Arc radius and center must be finite and positive");
      const auto center = profile.placement.toWorld(arc.center.xMm,
                                                    arc.center.yMm);
      gp_Ax2 axes(gp_Pnt(center.x, center.y, center.z), normalDir, xDir);
      BRepBuilderAPI_MakeEdge edgeBuilder(
          gp_Circ(axes, arc.radiusMm), arc.startAngleRad,
          arc.startAngleRad + arc.sweepAngleRad);
      if (!edgeBuilder.IsDone()) return fail("Could not build an arc edge");
      pending.push_back({geometry.arcId(index),
                         toWorldPoint(sketch::arcStartPoint(arc)),
                         toWorldPoint(sketch::arcEndPoint(arc)),
                         edgeBuilder.Edge()});
    }

    for (std::size_t index = 0; index < geometry.beziers().size(); ++index) {
      const auto& bezier = geometry.beziers()[index];
      if (bezier.dashed) continue;
      NCollection_Array1<gp_Pnt> poles(1, 4);
      for (int pole = 1; pole <= 4; ++pole)
        poles.SetValue(pole, toWorldPoint(bezier.points[pole - 1]));
      Handle(Geom_BezierCurve) curve = new Geom_BezierCurve(poles);
      BRepBuilderAPI_MakeEdge edgeBuilder(curve);
      if (!edgeBuilder.IsDone())
        return fail("Could not build a Bezier profile edge");
      pending.push_back({geometry.bezierId(index),
                         toWorldPoint(canonicalPoint(bezier.points[0])),
                         toWorldPoint(canonicalPoint(bezier.points[3])),
                         edgeBuilder.Edge()});
    }

    if (pending.size() < 2)
      return fail("Could not build a profile wire");

    // Chain the mixed line/arc edges into one connected wire. Lines and arcs
    // are stored in separate arrays, so connectivity must be resolved by
    // matching world-space endpoints.
    BRepBuilderAPI_MakeWire wireBuilder;
    std::vector<bool> used(pending.size(), false);
    const auto closeEnough = [](const gp_Pnt& first, const gp_Pnt& second) {
      return first.Distance(second) <= kEndpointTolerance;
    };

    for (std::size_t seed = 0; seed < pending.size(); ++seed) {
      if (used[seed]) continue;
      used[seed] = true;
      wireBuilder.Add(pending[seed].edge);
      pending[seed].edge = wireBuilder.Edge();
      gp_Pnt cursor = pending[seed].end;

      bool progressed = true;
      while (progressed) {
        progressed = false;
        for (std::size_t index = 0; index < pending.size(); ++index) {
          if (used[index]) continue;
          if (closeEnough(pending[index].start, cursor)) {
            wireBuilder.Add(pending[index].edge);
            pending[index].edge = wireBuilder.Edge();
            cursor = pending[index].end;
            used[index] = true;
            progressed = true;
          } else if (closeEnough(pending[index].end, cursor)) {
            wireBuilder.Add(TopoDS::Edge(pending[index].edge.Reversed()));
            pending[index].edge = wireBuilder.Edge();
            cursor = pending[index].start;
            used[index] = true;
            progressed = true;
          }
        }
      }
    }

    if (!wireBuilder.IsDone())
      return fail("Could not build a profile wire");
    BRepBuilderAPI_MakeFace faceBuilder(wireBuilder.Wire(), true);
    if (!faceBuilder.IsDone()) return fail("Could not build a profile face");
    const TopoDS_Face builtFace = faceBuilder.Face();
    BRepCheck_Analyzer analyzer(builtFace);
    if (!analyzer.IsValid()) return fail("Profile face is invalid");
    std::vector<std::pair<sketch::GeometryId, TopoDS_Edge>> authored;
    authored.reserve(pending.size());
    for (const auto& edge : pending)
      authored.emplace_back(edge.geometryId, edge.edge);
    SketchProfileFaceBuild built;
    built.face = std::make_shared<TopoDS_Face>(builtFace);
    if (requireEdgeMapping &&
        !reconcileFaceEdges(builtFace, authored, &built.edges, error))
      return false;
    *result = std::move(built);
    return true;
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    return fail(message && *message ? std::string("OCCT profile error: ") + message
                                    : "OCCT profile operation failed");
  } catch (...) {
    return fail("Unexpected profile geometry error");
  }
}

bool buildPlanarFaceFromSketchWithEdges(const DocumentSketch& profile,
                                        SketchProfileFaceBuild* result,
                                        std::string* error) {
  return buildPlanarFaceFromSketchImpl(profile, true, result, error);
}

bool buildPlanarFaceFromSketch(const DocumentSketch& profile,
                               TopoDS_Face* face, std::string* error) {
  if (!face) {
    if (error) *error = "Profile output face is missing";
    return false;
  }
  SketchProfileFaceBuild build;
  if (!buildPlanarFaceFromSketchImpl(profile, false, &build, error))
    return false;
  *face = *build.face;
  return true;
}

bool buildExtrusionPrismFromSketch(const DocumentSketch& profile,
                                   double distanceMm, bool reversed,
                                   TopoDS_Shape* prism, std::string* error) {
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  if (!prism) return fail("Prism output shape is missing");
  if (!std::isfinite(distanceMm) || distanceMm <= 0.0)
    return fail("distance must be a finite positive value");
  TopoDS_Face profileFace;
  if (!buildPlanarFaceFromSketch(profile, &profileFace, error)) return false;
  try {
    const auto normal = profile.placement.normal();
    const double sign = reversed ? -1.0 : 1.0;
    BRepPrimAPI_MakePrism builder(
        profileFace, gp_Vec(sign * normal.x * distanceMm,
                            sign * normal.y * distanceMm,
                            sign * normal.z * distanceMm));
    builder.Build();
    if (!builder.IsDone() || builder.Shape().IsNull())
      return fail("could not build a solid prism");
    const TopoDS_Shape builtPrism = builder.Shape();
    BRepCheck_Analyzer analyzer(builtPrism);
    if (!analyzer.IsValid()) return fail("extrusion prism is invalid");
    *prism = builtPrism;
    return true;
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    return fail(message && *message ? std::string("OCCT prism error: ") + message
                                    : "OCCT prism operation failed");
  } catch (...) {
    return fail("Unexpected prism geometry error");
  }
}

}  // namespace solidar
