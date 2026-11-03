#include "presentation/ComponentPresentation.h"

#include <algorithm>

namespace solidar {
namespace {

bool semanticInstanceEqual(const ComponentPresentationInstance& left,
                           const ComponentPresentationInstance& right) noexcept {
  return left.bodyIdentity() == right.bodyIdentity() &&
         left.sourceKey().semanticKey() == right.sourceKey().semanticKey() &&
         left.worldTransform() == right.worldTransform() &&
         left.componentVisible() == right.componentVisible() &&
         left.suppressed() == right.suppressed() &&
         left.grounded() == right.grounded() &&
         left.bodyVisible() == right.bodyVisible() &&
         left.effectivelyVisible() == right.effectivelyVisible();
}

}  // namespace

const ComponentTreeDefinitionProjection*
ComponentTreeProjection::findDefinition(DefinitionId id) const noexcept {
  const auto found = std::lower_bound(
      definitions.begin(), definitions.end(), id,
      [](const ComponentTreeDefinitionProjection& definition,
         DefinitionId candidate) { return definition.id < candidate; });
  return found != definitions.end() && found->id == id ? &*found : nullptr;
}

bool ComponentPresentationSnapshot::semanticallyEquivalent(
    const ComponentPresentationSnapshot& other) const noexcept {
  if (epoch_ != other.epoch_ || meshProfile_ != other.meshProfile_ ||
      graphTraversal_.status != other.graphTraversal_.status ||
      graphTraversal_.graphStatus != other.graphTraversal_.graphStatus ||
      graphTraversal_.visits != other.graphTraversal_.visits ||
      treeProjection_ != other.treeProjection_ ||
      sources_.size() != other.sources_.size() ||
      instances_.size() != other.instances_.size())
    return false;

  for (std::size_t index = 0; index < sources_.size(); ++index) {
    if (sources_[index].key().semanticKey() !=
        other.sources_[index].key().semanticKey())
      return false;
  }
  for (std::size_t index = 0; index < instances_.size(); ++index) {
    if (!semanticInstanceEqual(instances_[index], other.instances_[index]))
      return false;
  }
  return true;
}

std::string_view ComponentPresentationBuildResult::diagnostic() const noexcept {
  using enum ComponentPresentationBuildStatus;
  switch (status) {
    case Success:
      return {};
    case InvalidEpoch:
      return "Product presentation epoch is invalid";
    case InvalidMeshProfile:
      return "Component mesh profile is invalid";
    case InvalidRootDefinitionId:
      return "Product root definition ID is invalid";
    case MissingRootDefinition:
      return "Product root definition is missing";
    case RootIsNotComponent:
      return "Product root definition is not a component";
    case InvalidComponentGraph:
      return "Component graph is invalid";
    case DanglingTargetDefinition:
      return "Component occurrence target definition is missing";
    case InvalidOccurrencePath:
      return "Component occurrence path is invalid";
    case InvalidTransform:
      return "Component occurrence transform is invalid";
    case InvalidComponentDefinition:
      return "Component definition is inconsistent";
    case InvalidBodyId:
      return "Referenced Body ID is invalid";
    case InvalidFeatureId:
      return "Presentation Feature ID is invalid";
    case InvalidShapeRevision:
      return "Presentation Shape revision is invalid";
    case MissingRetainedShape:
      return "Featureful Body has no retained presentation Shape";
    case NullRetainedShape:
      return "Retained presentation Shape is null";
    case DuplicateStableIdentity:
      return "Component presentation contains a duplicate stable identity";
    case ResourceLimitExceeded:
      return "Component presentation resource limit exceeded";
    case CountOverflow:
      return "Component presentation count overflow";
    case OpenCascadeException:
      return "OpenCASCADE failed while reading component presentation state";
    case StandardException:
      return "Component presentation build failed with a standard exception";
    case UnknownException:
      return "Component presentation build failed with an unknown exception";
  }
  return "Component presentation build failed with an unknown status";
}

}  // namespace solidar
