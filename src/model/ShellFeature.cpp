#include "model/ShellFeature.h"

#include <TopoDS_Shape.hxx>

#include <cmath>
#include <utility>

#include "model/Body.h"
#include "model/ShellBuilder.h"
#include "model/TopologyReferenceResolver.h"

namespace solidar {

ShellFeature::ShellFeature(FeatureId source, std::vector<FaceReference> faces,
                           double thickness, bool outside, std::string name)
    : ShapeFeature(name.empty() ? "Shell" : std::move(name)),
      sourceFeatureId_(source), removedFaces_(std::move(faces)),
      thicknessMm_(thickness), outside_(outside) {}
ShellFeature::ShellFeature(FeatureId id, FeatureId source,
                           std::vector<FaceReference> faces, double thickness,
                           bool outside, std::string name)
    : ShapeFeature(id, std::move(name)), sourceFeatureId_(source),
      removedFaces_(std::move(faces)), thicknessMm_(thickness),
      outside_(outside) {}
FeatureId ShellFeature::sourceFeatureId() const noexcept { return sourceFeatureId_; }
const std::vector<FaceReference>& ShellFeature::removedFaces() const noexcept { return removedFaces_; }
double ShellFeature::thicknessMm() const noexcept { return thicknessMm_; }
bool ShellFeature::outside() const noexcept { return outside_; }
void ShellFeature::setRemovedFaces(std::vector<FaceReference> value) { if (removedFaces_ != value) { removedFaces_ = std::move(value); setDirty(); } }
void ShellFeature::setThicknessMm(double value) noexcept { if (thicknessMm_ != value) { thicknessMm_ = value; setDirty(); } }
void ShellFeature::setOutside(bool value) noexcept { if (outside_ != value) { outside_ = value; setDirty(); } }
std::string ShellFeature::typeName() const { return "Shell"; }

bool ShellFeature::rebuild(const RebuildContext& context) {
  clearShape();
  if (!std::isfinite(thicknessMm_) || thicknessMm_ <= 0.0) {
    markError("Shell thickness must be a finite positive value"); return false;
  }
  if (!context.previousShape || context.previousShape->IsNull()) {
    markError("Shell base shape is missing"); return false;
  }
  if (!context.body || removedFaces_.empty()) {
    markError(removedFaces_.empty() ? "Shell requires at least one face"
                                    : "Shell face belongs to a different Body");
    return false;
  }
  const auto& features = context.body->features();
  std::size_t ownIndex = features.size();
  for (std::size_t i = 0; i < features.size(); ++i)
    if (features[i].get() == this) { ownIndex = i; break; }
  if (ownIndex == 0 || ownIndex == features.size() ||
      features[ownIndex - 1]->id() != sourceFeatureId_) {
    markError("Shell source Feature could not be resolved"); return false;
  }
  std::vector<std::size_t> indices;
  for (const auto& face : removedFaces_) {
    if (face.bodyId != context.body->id() || face.featureId != sourceFeatureId_) {
      markError("Shell faces must belong to the active source Feature"); return false;
    }
    const auto resolved = resolveFaceReference(*context.previousShape, face.topology());
    if (!resolved) { markError("Shell face could not be resolved: " + resolved.error); return false; }
    indices.push_back(resolved.index);
  }
  std::string error;
  auto result = buildShellShape(*context.previousShape, indices, thicknessMm_, outside_, &error);
  if (!result) { markError(std::move(error)); return false; }
  setShape(std::move(result)); markValid(); return true;
}

std::unique_ptr<Feature> ShellFeature::clone() const { return std::make_unique<ShellFeature>(*this); }

}  // namespace solidar
