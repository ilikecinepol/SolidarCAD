#pragma once

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <string_view>

namespace solidar::test {

struct GpuSmokeClassification final {
  bool realGpu{};
  std::string reason;
};

struct A5bGpuSmokeEvidence final {
  bool realGpu{};
  bool newAssembly{};
  bool insertGhost{};
  bool moveRotateGhost{};
  bool applyOneUndo{};
  bool cancelExactRestore{};
  bool escapeNestedThenTool{};
  bool failedStagePreservesPreview{};
  bool failedOpenPreservesPreview{};
  bool replacementTeardownOrder{};
  bool undoRedoStableIds{};
  bool hiddenSuppressed{};
  bool selectionPublicationRefresh{};
  bool layoutMatrix{};
};

inline bool validA5bGpuSmoke(const A5bGpuSmokeEvidence& evidence) noexcept {
  return evidence.realGpu && evidence.newAssembly && evidence.insertGhost &&
         evidence.moveRotateGhost && evidence.applyOneUndo &&
         evidence.cancelExactRestore && evidence.escapeNestedThenTool &&
         evidence.failedStagePreservesPreview &&
         evidence.failedOpenPreservesPreview &&
         evidence.replacementTeardownOrder && evidence.undoRedoStableIds &&
         evidence.hiddenSuppressed && evidence.selectionPublicationRefresh &&
         evidence.layoutMatrix;
}

struct A5c1GpuSmokeEvidence final {
  bool realGpu{};
  bool entrypoints{};
  bool applyCancel{};
  bool undoRedo{};
  bool onlyName{};
  bool disabled{};
  bool selectionPublicationStable{};
  bool noUpload{};
  bool replacementTeardown{};
  bool layoutMatrix{};
};

inline bool validA5c1GpuSmoke(const A5c1GpuSmokeEvidence& evidence) noexcept {
  return evidence.realGpu && evidence.entrypoints && evidence.applyCancel &&
         evidence.undoRedo && evidence.onlyName && evidence.disabled &&
         evidence.selectionPublicationStable && evidence.noUpload &&
         evidence.replacementTeardown && evidence.layoutMatrix;
}

struct A5c2GpuSmokeEvidence final {
  bool realGpu{};
  bool entrypoints{};
  bool visibilityUndoRedo{};
  bool onlyVisible{};
  bool hiddenSpatialExcluded{};
  bool sharedSourceNoRemeshUpload{};
  bool lastUserPruneReacquire{};
  bool isolateShowAll{};
  bool isolatePresentationOnly{};
  bool isolateNoUploadPrune{};
  bool replacementTeardown{};
  bool layoutMatrix{};
};

inline bool validA5c2GpuSmoke(const A5c2GpuSmokeEvidence& evidence) noexcept {
  return evidence.realGpu && evidence.entrypoints &&
         evidence.visibilityUndoRedo && evidence.onlyVisible &&
         evidence.hiddenSpatialExcluded &&
         evidence.sharedSourceNoRemeshUpload &&
         evidence.lastUserPruneReacquire && evidence.isolateShowAll &&
         evidence.isolatePresentationOnly &&
         evidence.isolateNoUploadPrune && evidence.replacementTeardown &&
         evidence.layoutMatrix;
}

struct A5c3GpuSmokeEvidence final {
  bool realGpu{};
  bool entrypoints{};
  bool groundUnground{};
  bool undoRedo{};
  bool onlyGrounded{};
  bool placementGuard{};
  bool visibilitySuppressionIndependent{};
  bool selectionPublicationRefresh{};
  bool noRemeshUpload{};
  bool insertChain{};
  bool isolateDisabled{};
  bool replacementTeardown{};
  bool layoutMatrix{};
};

inline bool validA5c3GpuSmoke(const A5c3GpuSmokeEvidence& evidence) noexcept {
  return evidence.realGpu && evidence.entrypoints &&
         evidence.groundUnground && evidence.undoRedo &&
         evidence.onlyGrounded && evidence.placementGuard &&
         evidence.visibilitySuppressionIndependent &&
         evidence.selectionPublicationRefresh && evidence.noRemeshUpload &&
         evidence.insertChain && evidence.isolateDisabled &&
         evidence.replacementTeardown && evidence.layoutMatrix;
}

struct A5c4GpuSmokeEvidence final {
  bool realGpu{};
  bool entrypoints{};
  bool suppressUnsuppress{};
  bool undoRedo{};
  bool onlySuppressed{};
  bool visibilityGroundedIndependent{};
  bool suppressedSpatialExcluded{};
  bool sharedSourceNoRemeshUpload{};
  bool lastUserPruneReacquire{};
  bool selectionPublicationRefresh{};
  bool insertChain{};
  bool isolateDisabled{};
  bool replacementTeardown{};
  bool layoutMatrix{};
};

inline bool validA5c4GpuSmoke(const A5c4GpuSmokeEvidence& evidence) noexcept {
  return evidence.realGpu && evidence.entrypoints &&
         evidence.suppressUnsuppress && evidence.undoRedo &&
         evidence.onlySuppressed && evidence.visibilityGroundedIndependent &&
         evidence.suppressedSpatialExcluded &&
         evidence.sharedSourceNoRemeshUpload &&
         evidence.lastUserPruneReacquire &&
         evidence.selectionPublicationRefresh && evidence.insertChain &&
         evidence.isolateDisabled && evidence.replacementTeardown &&
         evidence.layoutMatrix;
}

struct A5c5GpuSmokeEvidence final {
  bool realGpu{};
  bool entrypoints{};
  bool newStableOccurrenceId{};
  bool sameDefinitionDocumentBrep{};
  bool exactPlacementNameFlags{};
  bool selectionApplyUndoRedo{};
  bool hiddenSuppressedGrounded{};
  bool sameEpochPublicationRefresh{};
  bool sharedSourceNoRemeshUpload{};
  bool instanceDraw{};
  bool rectanglePickingBoth{};
  bool topologyReuse{};
  bool insertRenameIsolateDisabled{};
  bool failureAtomicity{};
  bool replacementTeardown{};
  bool layoutMatrix{};
};

inline bool validA5c5GpuSmoke(const A5c5GpuSmokeEvidence& evidence) noexcept {
  return evidence.realGpu && evidence.entrypoints &&
         evidence.newStableOccurrenceId &&
         evidence.sameDefinitionDocumentBrep &&
         evidence.exactPlacementNameFlags &&
         evidence.selectionApplyUndoRedo &&
         evidence.hiddenSuppressedGrounded &&
         evidence.sameEpochPublicationRefresh &&
         evidence.sharedSourceNoRemeshUpload &&
         evidence.instanceDraw && evidence.rectanglePickingBoth &&
         evidence.topologyReuse &&
         evidence.insertRenameIsolateDisabled && evidence.failureAtomicity &&
         evidence.replacementTeardown && evidence.layoutMatrix;
}

struct A5c6GpuSmokeEvidence final {
  bool realGpu{};
  bool entrypoints{};
  bool removeUndoRedo{};
  bool onlyOccurrenceNode{};
  bool orphanDefinitionRetained{};
  bool orphanTreeFeedback{};
  bool lastOccurrenceEmptyRoot{};
  bool hiddenSuppressedGrounded{};
  bool selectionClearRestore{};
  bool sameEpochPublicationRefresh{};
  bool sharedSourceNoRemeshUpload{};
  bool lastUserPruneReacquire{};
  bool instanceDrawPicking{};
  bool undoCollisionRollback{};
  bool mixedChain{};
  bool emptyCamera{};
  bool topologyPruneReacquire{};
  bool isolateDisabled{};
  bool failureAtomicity{};
  bool replacementTeardown{};
  bool layoutMatrix{};
};

inline bool validA5c6GpuSmoke(const A5c6GpuSmokeEvidence& evidence) noexcept {
  return evidence.realGpu && evidence.entrypoints &&
         evidence.removeUndoRedo && evidence.onlyOccurrenceNode &&
         evidence.orphanDefinitionRetained && evidence.orphanTreeFeedback &&
         evidence.lastOccurrenceEmptyRoot &&
         evidence.hiddenSuppressedGrounded &&
         evidence.selectionClearRestore &&
         evidence.sameEpochPublicationRefresh &&
         evidence.sharedSourceNoRemeshUpload &&
         evidence.lastUserPruneReacquire && evidence.instanceDrawPicking &&
         evidence.undoCollisionRollback &&
         evidence.mixedChain && evidence.emptyCamera &&
         evidence.topologyPruneReacquire && evidence.isolateDisabled &&
         evidence.failureAtomicity && evidence.replacementTeardown &&
         evidence.layoutMatrix;
}

struct A5dGpuSmokeEvidence final {
  bool realGpu{};
  bool entryReturnBreadcrumb{};
  bool oneOwnerOneHistory{};
  bool sharedEditAllOccurrences{};
  bool idsPlacementsFlagsStable{};
  bool failedRecomputeRollback{};
  bool failedProjectionRollback{};
  bool undoRedoShared{};
  bool toolReplacementTeardown{};
  bool editedSourceOneUpload{};
  bool otherSourcesReused{};
  bool hiddenSuppressedStable{};
  bool selectionCameraRestore{};
  bool orphanDefinitionEdit{};
  bool layoutMatrix{};
};

inline bool validA5dGpuSmoke(const A5dGpuSmokeEvidence& evidence) noexcept {
  return evidence.realGpu && evidence.entryReturnBreadcrumb &&
         evidence.oneOwnerOneHistory && evidence.sharedEditAllOccurrences &&
         evidence.idsPlacementsFlagsStable && evidence.failedRecomputeRollback &&
         evidence.failedProjectionRollback && evidence.undoRedoShared &&
         evidence.toolReplacementTeardown && evidence.editedSourceOneUpload &&
         evidence.otherSourcesReused && evidence.hiddenSuppressedStable &&
         evidence.selectionCameraRestore && evidence.orphanDefinitionEdit &&
         evidence.layoutMatrix;
}

struct A6b2GpuSmokeEvidence final {
  bool realGpu{};
  bool initialActive{};
  bool reload{};
  bool coldUndo{};
  bool coldRedo{};
  bool stableOccurrenceFields{};
  bool selectionHoverOccurrenceScoped{};
  bool oneSharedSource{};
  bool oldSourcesRetired{};
  bool oneUploadPerRevision{};
  bool dpiIndependent{};
};

inline bool validA6b2GpuSmoke(const A6b2GpuSmokeEvidence& evidence) noexcept {
  return evidence.realGpu && evidence.initialActive && evidence.reload &&
         evidence.coldUndo && evidence.coldRedo &&
         evidence.stableOccurrenceFields &&
         evidence.selectionHoverOccurrenceScoped && evidence.oneSharedSource &&
         evidence.oldSourcesRetired && evidence.oneUploadPerRevision &&
         evidence.dpiIndependent;
}

// A6c1 is deliberately a conjunctive end-to-end marker.  In particular,
// successful V8 parsing or a non-empty scene cannot stand in for recursive
// identity, linked lifecycle, or renderer-retirement evidence.
struct A6c1GpuSmokeEvidence final {
  bool realGpu{};
  bool productionV8Open{};
  bool sharedRecursivePrefixes{};
  bool noncommutativeWorldTransforms{};
  bool oneSourceTwoInstances{};
  bool independentPickSelectionHoverHighlight{};
  bool terminalIsolation{};
  bool intermediateIsolationDisabled{};
  bool ancestorHideSuppressNoUpload{};
  bool linkedReloadColdUndoRedo{};
  bool oneUploadPerRevision{};
  bool oldResourcesRetired{};
  bool dpiIndependent{};
};

inline bool validA6c1GpuSmoke(const A6c1GpuSmokeEvidence& evidence) noexcept {
  return evidence.realGpu && evidence.productionV8Open &&
         evidence.sharedRecursivePrefixes &&
         evidence.noncommutativeWorldTransforms &&
         evidence.oneSourceTwoInstances &&
         evidence.independentPickSelectionHoverHighlight &&
         evidence.terminalIsolation &&
         evidence.intermediateIsolationDisabled &&
         evidence.ancestorHideSuppressNoUpload &&
         evidence.linkedReloadColdUndoRedo &&
         evidence.oneUploadPerRevision && evidence.oldResourcesRetired &&
         evidence.dpiIndependent;
}

// A8a remains a conjunctive native-evidence marker. Headless runs may prove
// every interaction/layout bit but must leave realGpu false.
struct A8aGpuSmokeEvidence final {
  bool realGpu{};
  bool typedTwoStage{};
  bool occurrenceScope{};
  bool strictFilter{};
  bool alignedOpposedPreview{};
  bool failedPreviewRetentionAndDisabledApply{};
  bool applyCancelEscape{};
  bool oneUndoRedo{};
  bool teardownReentrancy{};
  bool layoutDpr{};
};

inline bool validA8aGpuSmoke(const A8aGpuSmokeEvidence& evidence) noexcept {
  return evidence.realGpu && evidence.typedTwoStage &&
         evidence.occurrenceScope && evidence.strictFilter &&
         evidence.alignedOpposedPreview &&
         evidence.failedPreviewRetentionAndDisabledApply &&
         evidence.applyCancelEscape && evidence.oneUndoRedo &&
         evidence.teardownReentrancy && evidence.layoutDpr;
}

struct A5c2SpatialConsumerEvidence final {
  bool publicationCurrent{};
  bool faceCurrentAndFiltered{};
  bool rectangleCurrentAndFiltered{};
  bool snapCurrentAndFiltered{};
  bool rulerCurrentAndFiltered{};
  bool fitCurrentAndFiltered{};
};

// A5c2 markers are consumer-boundary evidence, not scene-count evidence. A
// stale cache in any individual consumer invalidates the whole marker.
inline bool validA5c2SpatialConsumerEvidence(
    const A5c2SpatialConsumerEvidence& evidence) noexcept {
  return evidence.publicationCurrent && evidence.faceCurrentAndFiltered &&
         evidence.rectangleCurrentAndFiltered &&
         evidence.snapCurrentAndFiltered &&
         evidence.rulerCurrentAndFiltered &&
         evidence.fitCurrentAndFiltered;
}

enum class SpatialEvidenceStatus { Hit, Miss, Other };

// A native marker must never turn an invalid/stale/uncertain query into
// exclusion evidence merely because its optional hit collection is empty.
inline bool validA4c3HiddenSuppressedEvidence(
    bool publicationCurrent, SpatialEvidenceStatus rectangleStatus,
    bool rectangleHitsAreCurrent, bool rectangleExcludesHidden,
    SpatialEvidenceStatus snapStatus, bool snapHitIsCurrent,
    bool snapExcludesHidden) noexcept {
  const bool rectangleValid =
      rectangleStatus == SpatialEvidenceStatus::Miss ||
      (rectangleStatus == SpatialEvidenceStatus::Hit &&
       rectangleHitsAreCurrent && rectangleExcludesHidden);
  const bool snapValid =
      snapStatus == SpatialEvidenceStatus::Miss ||
      (snapStatus == SpatialEvidenceStatus::Hit && snapHitIsCurrent &&
       snapExcludesHidden);
  return publicationCurrent && rectangleValid && snapValid;
}

inline std::string asciiLower(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return result;
}

inline std::string_view trimAsciiWhitespace(std::string_view value) noexcept {
  constexpr std::string_view whitespace{" \t\r\n\f\v"};
  const auto first = value.find_first_not_of(whitespace);
  if (first == std::string_view::npos) return {};
  const auto last = value.find_last_not_of(whitespace);
  return value.substr(first, last - first + 1);
}

inline bool missingGpuEvidence(std::string_view value) {
  const std::string normalized = asciiLower(trimAsciiWhitespace(value));
  if (normalized.empty()) return true;
  constexpr std::string_view missingTokens[]{"unavailable", "unknown"};
  for (const auto token : missingTokens) {
    if (normalized.find(token) != std::string::npos) return true;
  }
  constexpr std::string_view nullSubstitutes[]{
      "null", "(null)", "<null>", "nullptr", "none", "n/a", "na"};
  return std::find(std::begin(nullSubstitutes), std::end(nullSubstitutes),
                   normalized) != std::end(nullSubstitutes);
}

inline GpuSmokeClassification classifyGpuSmokeRuntime(
    std::string_view vendor, std::string_view renderer,
    std::string_view version, std::string_view qtPlatform) {
  if (missingGpuEvidence(vendor))
    return {false, "missing GL vendor evidence"};
  if (missingGpuEvidence(renderer))
    return {false, "missing GL renderer evidence"};
  if (missingGpuEvidence(version))
    return {false, "missing GL version evidence"};
  if (missingGpuEvidence(qtPlatform))
    return {false, "missing Qt platform evidence"};

  const std::string platform = asciiLower(trimAsciiWhitespace(qtPlatform));
  constexpr std::string_view nativePlatforms[]{
      "windows", "xcb", "wayland", "wayland-egl", "cocoa"};
  if (std::find(std::begin(nativePlatforms), std::end(nativePlatforms),
                platform) == std::end(nativePlatforms))
    return {false, "unsupported or non-native Qt platform: " + platform};

  const std::string implementation = asciiLower(
      std::string(vendor) + " " + std::string(renderer) + " " +
      std::string(version));
  constexpr std::string_view softwareTokens[]{
      "gdi generic", "software", "softpipe", "llvmpipe", "lavapipe",
      "swiftshader", "basic render", "swrast", "osmesa", "warp"};
  for (const std::string_view token : softwareTokens) {
    if (implementation.find(token) != std::string::npos)
      return {false, "software renderer token: " + std::string(token)};
  }
  return {true, {}};
}

}  // namespace solidar::test
