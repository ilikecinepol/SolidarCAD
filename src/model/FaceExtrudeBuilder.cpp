#include "model/FaceExtrudeBuilder.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <utility>

#include "model/ExtrudeFeature.h"
#include "model/TopologyReferenceResolver.h"

namespace solidar {

bool resolveFacePlaneAndNormal(const TopoDS_Face& face, gp_Pln* plane,
                               gp_Dir* normal, std::string* error) {
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  if (!plane || !normal) return fail("Face plane and normal outputs are missing");
  if (face.IsNull()) return fail("Face for extrusion is null");
  try {
    BRepAdaptor_Surface surface(face, true);
    if (surface.GetType() != GeomAbs_Plane)
      return fail("Face for extrusion must be planar");
    *plane = surface.Plane();
    gp_Dir direction = plane->Axis().Direction();
    if (face.Orientation() == TopAbs_REVERSED) direction.Reverse();
    *normal = direction;
    return true;
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    return fail(message && *message
                    ? std::string("OCCT face plane error: ") + message
                    : "OCCT face plane operation failed");
  } catch (...) {
    return fail("Unexpected face plane geometry error");
  }
}

bool buildExtrusionFromFace(const TopoDS_Shape& baseShape,
                            const FaceReference& source, double lengthMm,
                            ExtrudeOperation operation, bool reversed,
                            TopoDS_Shape* result,
                            FaceExtrudeGeometry* geometry, std::string* error) {
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  if (!result) return fail("Face extrude result output is missing");
  if (!std::isfinite(lengthMm) || lengthMm <= 0.0)
    return fail("Extrude length must be a finite positive value");
  if (operation == ExtrudeOperation::NewBody)
    return fail("Face extrusion cannot create a new body");
  if (baseShape.IsNull()) return fail("Face extrude base shape is missing");

  try {
    const auto resolved = resolveFaceReference(baseShape, source.topology());
    if (!resolved)
      return fail("Face extrude face could not be resolved: " + resolved.error);

    const TopoDS_Face face = *resolved.subshape;
    gp_Pln plane;
    gp_Dir normal;
    if (!resolveFacePlaneAndNormal(face, &plane, &normal, error)) return false;

    GProp_GProps surfaceProperties;
    BRepGProp::SurfaceProperties(face, surfaceProperties);
    const gp_Pnt centroid = surfaceProperties.CentreOfMass();

    const gp_Vec offset = gp_Vec(normal) * (reversed ? -lengthMm : lengthMm);
    BRepPrimAPI_MakePrism prism(face, offset);
    prism.Build();
    if (!prism.IsDone() || prism.Shape().IsNull())
      return fail("Face extrude could not build a solid prism");

    GProp_GProps beforeProperties;
    BRepGProp::VolumeProperties(baseShape, beforeProperties);

    TopoDS_Shape booleanResult;
    if (operation == ExtrudeOperation::Join) {
      BRepAlgoAPI_Fuse fuse(baseShape, prism.Shape());
      fuse.Build();
      if (!fuse.IsDone() || fuse.Shape().IsNull())
        return fail("Face extrude Join boolean fuse failed");
      booleanResult = fuse.Shape();
    } else {
      BRepAlgoAPI_Cut cut(baseShape, prism.Shape());
      cut.Build();
      if (!cut.IsDone() || cut.Shape().IsNull())
        return fail("Face extrude Cut boolean cut failed");
      booleanResult = cut.Shape();
    }

    TopExp_Explorer solids(booleanResult, TopAbs_SOLID);
    if (!solids.More())
      return fail("Face extrude result does not contain a solid");
    TopoDS_Shape singleSolid = solids.Current();
    solids.Next();
    if (solids.More())
      return fail(operation == ExtrudeOperation::Join
                      ? "Face extrude Join does not intersect the body"
                      : "Face extrude result contains multiple solids");

    GProp_GProps afterProperties;
    BRepGProp::VolumeProperties(singleSolid, afterProperties);
    const double before = beforeProperties.Mass();
    const double after = afterProperties.Mass();
    const double tolerance = std::max(1e-7, std::abs(before) * 1e-10);
    if (operation == ExtrudeOperation::Join && after <= before + tolerance)
      return fail("Face extrude Join does not intersect the body");
    if (operation == ExtrudeOperation::Cut && after >= before - tolerance)
      return fail("Face extrude Cut does not intersect the body");

    // Join leaves coplanar seams between the base and the fused prism. Unify
    // only genuine same-domain faces/edges (never real geometric boundaries),
    // then re-validate the healed solid.
    if (operation == ExtrudeOperation::Join) {
      ShapeUpgrade_UnifySameDomain unify(singleSolid, true, true, false);
      unify.Build();
      if (unify.Shape().IsNull())
        return fail("Face extrude same-domain unification failed");
      singleSolid = unify.Shape();
    }

    BRepCheck_Analyzer analyzer(singleSolid);
    if (!analyzer.IsValid())
      return fail("Face extrude result is invalid");

    *result = singleSolid;
    if (geometry) {
      geometry->face = face;
      geometry->centroid = centroid;
      geometry->normal = normal;
    }
    return true;
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    return fail(message && *message
                    ? std::string("OCCT face extrude error: ") + message
                    : "OCCT face extrude operation failed");
  } catch (const std::exception& failure) {
    return fail(std::string("Face extrude error: ") + failure.what());
  } catch (...) {
    return fail("Unexpected face extrude geometry error");
  }
}

}  // namespace solidar
