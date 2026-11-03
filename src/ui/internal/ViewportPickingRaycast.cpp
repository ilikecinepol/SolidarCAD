#include "ui/internal/ViewportPickingRaycast.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <tuple>

#include "ui/RigidTransform3dOps.h"

namespace solidar::detail {
namespace {

struct Vec3 final { double x{}, y{}, z{}; };
bool finite(Vec3 v) noexcept { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
Vec3 vector(Point3d p) noexcept { return {p.x,p.y,p.z}; }
Point3d point(Vec3 v) noexcept { return {v.x,v.y,v.z}; }
Vec3 operator+(Vec3 a,Vec3 b) noexcept { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 operator-(Vec3 a,Vec3 b) noexcept { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 operator*(Vec3 a,double s) noexcept { return {a.x*s,a.y*s,a.z*s}; }
double dot(Vec3 a,Vec3 b) noexcept { return std::fma(a.x,b.x,std::fma(a.y,b.y,a.z*b.z)); }
Vec3 cross(Vec3 a,Vec3 b) noexcept { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
double squaredLength(Vec3 v) noexcept { return dot(v,v); }
void increment(std::uint64_t* value) noexcept { if(value)++*value; }

struct Ray final { Vec3 origin,direction,view;double length{},nearDepth{},scale{}; };
std::optional<Ray> ray(const ViewportCameraState& camera,QPointF pixel) noexcept {
  if(!std::isfinite(camera.yawDeg)||!std::isfinite(camera.pitchDeg)||!std::isfinite(camera.zoom)||camera.zoom<=0||
     !std::isfinite(camera.pan.x())||!std::isfinite(camera.pan.y())||camera.logicalSize.width()<=0||camera.logicalSize.height()<=0||
     !std::isfinite(camera.devicePixelRatio)||camera.devicePixelRatio<=0||!finite(vector(camera.center))||
     !std::isfinite(camera.depthExtent)||camera.depthExtent<=0||!std::isfinite(pixel.x())||!std::isfinite(pixel.y()))return std::nullopt;
  const double width=camera.logicalSize.width(),height=camera.logicalSize.height();
  const double scale=std::min(width,height)*0.008*camera.zoom,extent=std::max(1.0,camera.depthExtent);
  if(!std::isfinite(scale)||scale<=0||!std::isfinite(extent*2))return std::nullopt;
  const double yaw=camera.yawDeg*std::numbers::pi/180.0,pitch=camera.pitchDeg*std::numbers::pi/180.0;
  const double cy=std::cos(yaw),sy=std::sin(yaw),cp=std::cos(pitch),sp=std::sin(pitch);
  const Vec3 right{cy,-sy,0},up{-sy*cp,-cy*cp,sp},view{sy*sp,cy*sp,cp};
  const double rc=(pixel.x()-width*.5-camera.pan.x())/scale,uc=(height*.52+camera.pan.y()-pixel.y())/scale;
  const double nearDepth=dot(vector(camera.center),view)+extent;
  Ray result{right*rc+up*uc+view*nearDepth,view*-1,view,extent*2,nearDepth,scale};
  return finite(result.origin)&&finite(result.direction)&&finite(result.view)?std::optional<Ray>{result}:std::nullopt;
}
bool intersects(const Ray& ray,const Bounds3d& bounds,double expansion=0) noexcept {
  if(!std::isfinite(expansion)||expansion<0)return false;
  const auto lo=bounds.minimum(),hi=bounds.maximum();
  const std::array<double,3> o{ray.origin.x,ray.origin.y,ray.origin.z},d{ray.direction.x,ray.direction.y,ray.direction.z};
  const std::array<double,3> low{lo.x-expansion,lo.y-expansion,lo.z-expansion},high{hi.x+expansion,hi.y+expansion,hi.z+expansion};
  double entry=0,exit=ray.length;
  for(std::size_t axis=0;axis<3;++axis){if(!std::isfinite(low[axis])||!std::isfinite(high[axis]))return false;
    if(std::abs(d[axis])<=1e-15){if(o[axis]<low[axis]||o[axis]>high[axis])return false;continue;}
    double a=(low[axis]-o[axis])/d[axis],b=(high[axis]-o[axis])/d[axis];if(a>b)std::swap(a,b);
    entry=std::max(entry,a);exit=std::min(exit,b);if(entry>exit)return false;}
  return exit>=0&&entry<=ray.length;
}
Vec3 inversePoint(const std::array<double,16>& m,Vec3 w) noexcept {const Vec3 s{w.x-m[3],w.y-m[7],w.z-m[11]};return {std::fma(m[0],s.x,std::fma(m[4],s.y,m[8]*s.z)),std::fma(m[1],s.x,std::fma(m[5],s.y,m[9]*s.z)),std::fma(m[2],s.x,std::fma(m[6],s.y,m[10]*s.z))};}
Vec3 inverseDirection(const std::array<double,16>& m,Vec3 w) noexcept {return {std::fma(m[0],w.x,std::fma(m[4],w.y,m[8]*w.z)),std::fma(m[1],w.x,std::fma(m[5],w.y,m[9]*w.z)),std::fma(m[2],w.x,std::fma(m[6],w.y,m[10]*w.z))};}
std::optional<double> triangleAt(Vec3 origin,Vec3 direction,double maximum,const RenderTriangle& triangle,double scale) noexcept {
  const Vec3 a=vector(triangle.a),e1=vector(triangle.b)-a,e2=vector(triangle.c)-a,crossDirection=cross(direction,e2);
  const double determinant=dot(e1,crossDirection),tolerance=std::max(1.0,scale*scale)*1e-14;
  if(!std::isfinite(determinant)||std::abs(determinant)<=tolerance)return std::nullopt;
  const double inverse=1/determinant;const Vec3 from=origin-a;const double u=dot(from,crossDirection)*inverse;constexpr double bt=1e-12;
  if(u < -bt||u > 1+bt)return std::nullopt;const Vec3 q=cross(from,e1);const double v=dot(direction,q)*inverse;
  if(v < -bt||u+v > 1+bt)return std::nullopt;const double parameter=dot(e2,q)*inverse,pt=std::max(1.0,scale)*1e-11;
  if(!std::isfinite(parameter)||parameter < -pt||parameter > maximum+pt)return std::nullopt;return std::clamp(parameter,0.0,maximum);
}
struct Pair final { double first{},second{},squared{}; };
Pair closest(Vec3 p0,Vec3 p1,Vec3 q0,Vec3 q1) noexcept {const Vec3 d1=p1-p0,d2=q1-q0,r=p0-q0;const double a=squaredLength(d1),e=squaredLength(d2),f=dot(d2,r);constexpr double eps=1e-24;double s=0,t=0;
  if(a<=eps&&e<=eps)return {0,0,squaredLength(r)};if(a<=eps)t=std::clamp(f/e,0.0,1.0);else{const double c=dot(d1,r);if(e<=eps)s=std::clamp(-c/a,0.0,1.0);else{const double b=dot(d1,d2),den=a*e-b*b;if(std::abs(den)>eps)s=std::clamp((b*f-c*e)/den,0.0,1.0);t=(b*s+f)/e;if(t<0){t=0;s=std::clamp(-c/a,0.0,1.0);}else if(t>1){t=1;s=std::clamp((b-c)/a,0.0,1.0);}}}const Vec3 x=p0+d1*s,y=q0+d2*t;return {s,t,squaredLength(x-y)};}
bool validInstance(const ViewportPickingRayInstance& instance) noexcept {return instance.source&&instance.source->mesh&&instance.ordinal!=std::numeric_limits<std::size_t>::max();}

}  // namespace

ViewportPickingFaceRayResult viewportFaceAt(
    const std::vector<ViewportPickingRayInstance>& instances,
    const std::optional<Bounds3d>& sceneBounds,const ViewportCameraState& camera,
    QPointF pixel,const std::function<bool()>& current,
    ViewportPickingRayCounters counters) noexcept {
  try{if(!current())return {ViewportPickingRayStatus::StalePublication,{}};const auto pickingRay=ray(camera,pixel);if(!pickingRay)return {ViewportPickingRayStatus::InvalidInput,{}};
    struct Candidate{double parameter{},depth{};Point3d point{};std::size_t instance{},raw{},primitive{};};std::optional<Candidate> best;
    const double scale=sceneBounds?std::max(1.0,sceneBounds->diagonal()):1.0,tie=std::max({scale,camera.depthExtent,1.0})*1e-10;
    for(const auto& instance:instances){if(!current())return {ViewportPickingRayStatus::StalePublication,{}};if(!validInstance(instance))return {ViewportPickingRayStatus::InvalidInput,{}};if(!intersects(*pickingRay,instance.worldBounds))continue;increment(counters.broadPhaseCandidates);
      const auto origin=inversePoint(instance.localToWorld,pickingRay->origin),direction=inverseDirection(instance.localToWorld,pickingRay->direction);
      for(std::size_t ordinal=0;ordinal<instance.source->mesh->triangleCount();++ordinal){increment(counters.triangleTests);const auto triangle=instance.source->mesh->triangle(ordinal);if(triangle.faceIndex>=instance.source->faces.size())return {ViewportPickingRayStatus::InvalidInput,{}};const auto parameter=triangleAt(origin,direction,pickingRay->length,triangle,scale);if(!parameter)continue;const auto world=pickingRay->origin+pickingRay->direction**parameter;const double depth=dot(world,pickingRay->view);Candidate candidate{*parameter,depth,point(world),instance.ordinal,triangle.faceIndex,ordinal};if(!best||candidate.parameter<best->parameter-tie||(std::abs(candidate.parameter-best->parameter)<=tie&&std::tie(candidate.instance,candidate.raw,candidate.primitive)<std::tie(best->instance,best->raw,best->primitive)))best=candidate;}}
    if(!current())return {ViewportPickingRayStatus::StalePublication,{}};if(!best)return {ViewportPickingRayStatus::Miss,{}};const auto found=std::find_if(instances.begin(),instances.end(),[&](const auto& value){return value.ordinal==best->instance;});if(found==instances.end())return {ViewportPickingRayStatus::InvalidInput,{}};
    return {ViewportPickingRayStatus::Hit,ViewportPickingFaceRayHit{best->instance,found->source->faces[best->raw],best->point,best->parameter,best->depth}};
  }catch(...){return {ViewportPickingRayStatus::ResourceFailure,{}};}
}

ViewportPickingEdgeRayResult viewportEdgeAt(
    const std::vector<ViewportPickingRayInstance>& instances,
    const std::optional<Bounds3d>& sceneBounds,const ViewportCameraState& camera,
    QPointF pixel,double radiusPixels,const std::function<bool()>& current,
    ViewportPickingRayCounters counters) noexcept {
  try{if(!current())return {ViewportPickingRayStatus::StalePublication,{}};const auto pickingRay=ray(camera,pixel);if(!pickingRay||!std::isfinite(radiusPixels)||radiusPixels<=0)return {ViewportPickingRayStatus::InvalidInput,{}};const double radius=radiusPixels/pickingRay->scale;if(!std::isfinite(radius)||radius<=0)return {ViewportPickingRayStatus::InvalidInput,{}};
    struct Candidate{double parameter{},depth{},distance{};Point3d point{};std::size_t instance{},raw{},primitive{};};std::optional<Candidate> best;const double scale=sceneBounds?std::max(1.0,sceneBounds->diagonal()):1.0,dt=std::max({scale,camera.depthExtent,1.0})*1e-8,distt=std::max(1.0,radius)*1e-10;
    const auto occluded=[&](Point3d point,double depth)->std::optional<bool>{const double parameter=pickingRay->nearDepth-depth;if(!std::isfinite(parameter)||parameter<0||parameter>pickingRay->length+dt)return true;Ray rayToPoint{vector(point)+pickingRay->view*parameter,pickingRay->direction,pickingRay->view,std::clamp(parameter,0.0,pickingRay->length),pickingRay->nearDepth,pickingRay->scale};for(const auto& instance:instances){if(!current())return std::nullopt;if(!intersects(rayToPoint,instance.worldBounds))continue;const auto origin=inversePoint(instance.localToWorld,rayToPoint.origin),direction=inverseDirection(instance.localToWorld,rayToPoint.direction);for(const auto triangle:instance.source->mesh->triangles()){increment(counters.occluderTests);const auto hit=triangleAt(origin,direction,rayToPoint.length,triangle,scale);if(hit&&*hit<rayToPoint.length-dt)return true;}}return false;};
    for(const auto& instance:instances){if(!current())return {ViewportPickingRayStatus::StalePublication,{}};if(!validInstance(instance))return {ViewportPickingRayStatus::InvalidInput,{}};if(!intersects(*pickingRay,instance.worldBounds,radius))continue;increment(counters.broadPhaseCandidates);const auto origin=inversePoint(instance.localToWorld,pickingRay->origin),direction=inverseDirection(instance.localToWorld,pickingRay->direction),end=origin+direction*pickingRay->length;for(const auto& edge:instance.source->mesh->edges()){if(edge.edgeIndex>=instance.source->edges.size())return {ViewportPickingRayStatus::InvalidInput,{}};for(std::size_t segment=1;segment<edge.points.size();++segment){increment(counters.edgeSegmentTests);const auto a=vector(edge.points[segment-1]),b=vector(edge.points[segment]);const auto pair=closest(origin,end,a,b);if(!std::isfinite(pair.squared)||pair.squared>radius*radius)continue;const auto world=transformPoint(instance.localToWorld,point(a+(b-a)*pair.second));if(!world)continue;const double depth=dot(vector(*world),pickingRay->view),parameter=pickingRay->nearDepth-depth,distance=std::sqrt(std::max(0.0,pair.squared));const auto hidden=occluded(*world,depth);if(!hidden)return {ViewportPickingRayStatus::StalePublication,{}};if(*hidden)continue;Candidate candidate{parameter,depth,distance,*world,instance.ordinal,edge.edgeIndex,segment-1};if(!best||candidate.parameter<best->parameter-dt||(std::abs(candidate.parameter-best->parameter)<=dt&&(candidate.distance<best->distance-distt||(std::abs(candidate.distance-best->distance)<=distt&&std::tie(candidate.instance,candidate.raw,candidate.primitive)<std::tie(best->instance,best->raw,best->primitive)))))best=candidate;}}}
    if(!current())return {ViewportPickingRayStatus::StalePublication,{}};if(!best)return {ViewportPickingRayStatus::Miss,{}};const auto found=std::find_if(instances.begin(),instances.end(),[&](const auto& value){return value.ordinal==best->instance;});if(found==instances.end())return {ViewportPickingRayStatus::InvalidInput,{}};
    return {ViewportPickingRayStatus::Hit,ViewportPickingEdgeRayHit{best->instance,found->source->edges[best->raw],best->point,best->parameter,best->depth,best->distance}};
  }catch(...){return {ViewportPickingRayStatus::ResourceFailure,{}};}
}

}  // namespace solidar::detail
