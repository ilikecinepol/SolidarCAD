#include "model/SketchPlacement.h"

#include <BRepAdaptor_Surface.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>

namespace solidar {
namespace {
Vector3d normalized(Vector3d value) noexcept {
  const double length = std::sqrt(value.x * value.x + value.y * value.y +
                                  value.z * value.z);
  return length > 0.0 ? Vector3d{value.x / length, value.y / length,
                                value.z / length}
                      : Vector3d{};
}
}  // namespace

SketchPlacement SketchPlacement::xy() noexcept { return {}; }
SketchPlacement SketchPlacement::xz() noexcept {
  return {{}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}};
}
SketchPlacement SketchPlacement::yz() noexcept {
  return {{}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
}

Point3d SketchPlacement::toWorld(double xMm, double yMm) const noexcept {
  return {origin.x + xDirection.x * xMm + yDirection.x * yMm,
          origin.y + xDirection.y * xMm + yDirection.y * yMm,
          origin.z + xDirection.z * xMm + yDirection.z * yMm};
}

Vector3d SketchPlacement::normal() const noexcept {
  return normalized({xDirection.y * yDirection.z - xDirection.z * yDirection.y,
                     xDirection.z * yDirection.x - xDirection.x * yDirection.z,
                     xDirection.x * yDirection.y - xDirection.y * yDirection.x});
}

ResolvedFacePlacement resolveFacePlacement(const TopoDS_Shape& shape,
                                           std::size_t faceIndex) {
  std::size_t index = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next(), ++index) {
    if (index != faceIndex) continue;
    const TopoDS_Face face = TopoDS::Face(explorer.Current());
    BRepAdaptor_Surface surface(face, true);
    if (surface.GetType() != GeomAbs_Plane) return {};

    const gp_Ax3 axes = surface.Plane().Position();
    const gp_Pnt location = axes.Location();
    gp_Dir x = axes.XDirection();
    gp_Dir y = axes.YDirection();
    if (face.Orientation() == TopAbs_REVERSED) y.Reverse();
    return {{{location.X(), location.Y(), location.Z()},
             {x.X(), x.Y(), x.Z()},
             {y.X(), y.Y(), y.Z()}},
            true};
  }
  return {};
}

}  // namespace solidar
