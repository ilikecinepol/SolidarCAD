#include "presentation/AssemblyPresentation.h"

#include <algorithm>

namespace solidar {
namespace {

bool semanticInstanceEqual(const AssemblyPresentationInstance& left,
                           const AssemblyPresentationInstance& right) noexcept {
  return left.occurrenceIdentity() == right.occurrenceIdentity() &&
         left.sourceKey().semanticKey() ==
             right.sourceKey().semanticKey() &&
         left.worldTransform() == right.worldTransform() &&
         left.occurrenceVisible() == right.occurrenceVisible() &&
         left.suppressed() == right.suppressed() &&
         left.grounded() == right.grounded() &&
         left.bodyVisible() == right.bodyVisible() &&
         left.effectivelyVisible() == right.effectivelyVisible();
}

}  // namespace

bool AssemblyPresentationSnapshot::semanticallyEquivalent(
    const AssemblyPresentationSnapshot& other) const noexcept {
  if (epoch_ != other.epoch_ || meshProfile_ != other.meshProfile_ ||
      graphAnalysis_ != other.graphAnalysis_ ||
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

std::string_view AssemblyPresentationBuildResult::diagnostic() const noexcept {
  using enum AssemblyPresentationBuildStatus;
  switch (status) {
    case Success:
      return {};
    case InvalidEpoch:
      return "Product presentation epoch is invalid";
    case InvalidMeshProfile:
      return "Assembly mesh profile is invalid";
    case InvalidRootDefinitionId:
      return "Product root definition ID is invalid";
    case MissingRootDefinition:
      return "Product root definition is missing";
    case RootIsNotAssembly:
      return "Product root definition is not an assembly";
    case InvalidAssemblyDefinition:
      return "Root assembly definition is inconsistent";
    case InvalidOccurrenceId:
      return "Assembly occurrence ID is invalid or inconsistent";
    case InvalidOccurrencePath:
      return "Assembly occurrence path is invalid";
    case InvalidTargetDefinitionId:
      return "Assembly occurrence target definition ID is invalid";
    case DanglingTargetDefinition:
      return "Assembly occurrence target definition is missing";
    case UnsupportedTargetDefinitionType:
      return "Assembly occurrence target is not a part definition";
    case InvalidPartDefinition:
      return "Referenced part definition is inconsistent";
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
      return "Presentation contains a duplicate stable identity";
    case ResourceLimitExceeded:
      return "Assembly presentation resource limit exceeded";
    case CountOverflow:
      return "Assembly presentation count overflow";
    case OpenCascadeException:
      return "OpenCASCADE failed while reading presentation state";
    case StandardException:
      return "Assembly presentation build failed with a standard exception";
    case UnknownException:
      return "Assembly presentation build failed with an unknown exception";
  }
  return "Assembly presentation build failed with an unknown status";
}

}  // namespace solidar
