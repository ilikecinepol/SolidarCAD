#include "AssemblyGpuSmokeClassification.h"
#include "TestAssertions.h"

#include <array>
#include <cstdlib>

int main() {
  using solidar::test::classifyGpuSmokeRuntime;
  using solidar::test::SpatialEvidenceStatus;
  using solidar::test::validA4c3HiddenSuppressedEvidence;
  using solidar::test::A5bGpuSmokeEvidence;
  using solidar::test::validA5bGpuSmoke;
  using solidar::test::A5c1GpuSmokeEvidence;
  using solidar::test::validA5c1GpuSmoke;
  using solidar::test::A5c2GpuSmokeEvidence;
  using solidar::test::validA5c2GpuSmoke;
  using solidar::test::A5c2SpatialConsumerEvidence;
  using solidar::test::validA5c2SpatialConsumerEvidence;
  using solidar::test::A5c3GpuSmokeEvidence;
  using solidar::test::validA5c3GpuSmoke;
  using solidar::test::A5c4GpuSmokeEvidence;
  using solidar::test::validA5c4GpuSmoke;
  using solidar::test::A5c5GpuSmokeEvidence;
  using solidar::test::validA5c5GpuSmoke;
  using solidar::test::A5c6GpuSmokeEvidence;
  using solidar::test::validA5c6GpuSmoke;
  using solidar::test::A5dGpuSmokeEvidence;
  using solidar::test::validA5dGpuSmoke;
  using solidar::test::A6b2GpuSmokeEvidence;
  using solidar::test::A6c1GpuSmokeEvidence;
  using solidar::test::validA6b2GpuSmoke;
  using solidar::test::A8aGpuSmokeEvidence;
  using solidar::test::validA8aGpuSmoke;
  CHECK(classifyGpuSmokeRuntime("NVIDIA Corporation", "RTX 4090",
                                "4.6 NVIDIA", "windows")
            .realGpu);
  CHECK(classifyGpuSmokeRuntime("AMD", "Radeon", "4.6 Mesa", "xcb")
            .realGpu);
  CHECK(classifyGpuSmokeRuntime("Intel", "Arc", "4.6", "wayland")
            .realGpu);
  CHECK(classifyGpuSmokeRuntime("Apple", "Apple M4", "4.1 Metal", "cocoa")
            .realGpu);

  struct Evidence final {
    const char* vendor;
    const char* renderer;
    const char* version;
    const char* platform;
  };
  constexpr std::array invalidEvidence{
      Evidence{"", "RTX 4090", "4.6", "windows"},
      Evidence{"NVIDIA", "", "4.6", "windows"},
      Evidence{"NVIDIA", "RTX 4090", "", "windows"},
      Evidence{"NVIDIA", "RTX 4090", "4.6", ""},
      Evidence{"   ", "RTX 4090", "4.6", "windows"},
      Evidence{"NVIDIA", "<unavailable>", "4.6", "windows"},
      Evidence{"NVIDIA", "RTX 4090", "<unavailable>", "windows"},
      Evidence{"<unavailable>", "RTX 4090", "4.6", "windows"},
      Evidence{"NVIDIA", "RTX 4090", "4.6", "<unavailable>"},
      Evidence{"Unknown Vendor", "RTX 4090", "4.6", "windows"},
      Evidence{"NVIDIA", "unknown renderer", "4.6", "windows"},
      Evidence{"NVIDIA", "RTX 4090", "unknown", "windows"},
      Evidence{"NVIDIA", "RTX 4090", "4.6", "unknown"},
      Evidence{"(null)", "RTX 4090", "4.6", "windows"},
      Evidence{"NVIDIA", "nullptr", "4.6", "windows"},
      Evidence{"NVIDIA", "RTX 4090", "N/A", "windows"},
      Evidence{"NVIDIA", "RTX 4090", "4.6", "none"},
      Evidence{"Microsoft", "GDI Generic", "1.1", "windows"},
      Evidence{"Mesa", "SOFTPIPE", "4.5", "xcb"},
      Evidence{"Mesa", "llVMPipe", "4.5", "xcb"},
      Evidence{"Mesa", "LavaPipe", "4.5", "wayland"},
      Evidence{"Google", "SwiftShader Device", "4.5", "windows"},
      Evidence{"Vendor", "Software Rasterizer", "4.5", "windows"},
      Evidence{"Vendor", "Hardware Name", "4.5 llvmpipe", "windows"},
      Evidence{"NVIDIA software", "RTX 4090", "4.6", "windows"},
      Evidence{"NVIDIA", "RTX 4090", "4.6", "offscreen"},
      Evidence{"NVIDIA", "RTX 4090", "4.6", "minimal"},
      Evidence{"NVIDIA", "RTX 4090", "4.6", "minimalegl"},
      Evidence{"NVIDIA", "RTX 4090", "4.6", "headless-egl"},
      Evidence{"NVIDIA", "RTX 4090", "4.6", "linuxfb"},
      Evidence{"NVIDIA", "RTX 4090", "4.6", "windows-offscreen"},
      Evidence{"NVIDIA", "RTX 4090", "4.6", "eglfs"}};
  for (const auto& evidence : invalidEvidence) {
    const auto result = classifyGpuSmokeRuntime(
        evidence.vendor, evidence.renderer, evidence.version,
        evidence.platform);
    CHECK(!result.realGpu);
    CHECK(!result.reason.empty());
  }
  CHECK(validA4c3HiddenSuppressedEvidence(
      true, SpatialEvidenceStatus::Miss, true, true,
      SpatialEvidenceStatus::Miss, true, true));
  CHECK(validA4c3HiddenSuppressedEvidence(
      true, SpatialEvidenceStatus::Hit, true, true,
      SpatialEvidenceStatus::Hit, true, true));
  CHECK(!validA4c3HiddenSuppressedEvidence(
      true, SpatialEvidenceStatus::Other, true, true,
      SpatialEvidenceStatus::Miss, true, true));
  CHECK(!validA4c3HiddenSuppressedEvidence(
      true, SpatialEvidenceStatus::Miss, true, true,
      SpatialEvidenceStatus::Other, true, true));
  CHECK(!validA4c3HiddenSuppressedEvidence(
      true, SpatialEvidenceStatus::Hit, false, true,
      SpatialEvidenceStatus::Miss, true, true));
  CHECK(!validA4c3HiddenSuppressedEvidence(
      true, SpatialEvidenceStatus::Miss, true, true,
      SpatialEvidenceStatus::Hit, false, true));
  CHECK(!validA4c3HiddenSuppressedEvidence(
      true, SpatialEvidenceStatus::Hit, true, false,
      SpatialEvidenceStatus::Miss, true, true));
  CHECK(!validA4c3HiddenSuppressedEvidence(
      false, SpatialEvidenceStatus::Miss, true, true,
      SpatialEvidenceStatus::Miss, true, true));
  A5bGpuSmokeEvidence a5b{true, true, true, true, true, true, true,
                          true, true, true, true, true, true, true};
  CHECK(validA5bGpuSmoke(a5b));
  bool* fields[]{&a5b.realGpu,
                 &a5b.newAssembly,
                 &a5b.insertGhost,
                 &a5b.moveRotateGhost,
                 &a5b.applyOneUndo,
                 &a5b.cancelExactRestore,
                 &a5b.escapeNestedThenTool,
                 &a5b.failedStagePreservesPreview,
                 &a5b.failedOpenPreservesPreview,
                 &a5b.replacementTeardownOrder,
                 &a5b.undoRedoStableIds,
                 &a5b.hiddenSuppressed,
                 &a5b.selectionPublicationRefresh,
                 &a5b.layoutMatrix};
  for (bool* field : fields) {
    *field = false;
    CHECK(!validA5bGpuSmoke(a5b));
    *field = true;
  }
  A5c1GpuSmokeEvidence a5c1{true, true, true, true, true,
                            true, true, true, true, true};
  CHECK(validA5c1GpuSmoke(a5c1));
  bool* renameFields[]{&a5c1.realGpu,
                       &a5c1.entrypoints,
                       &a5c1.applyCancel,
                       &a5c1.undoRedo,
                       &a5c1.onlyName,
                       &a5c1.disabled,
                       &a5c1.selectionPublicationStable,
                       &a5c1.noUpload,
                       &a5c1.replacementTeardown,
                       &a5c1.layoutMatrix};
  for (bool* field : renameFields) {
    *field = false;
    CHECK(!validA5c1GpuSmoke(a5c1));
    *field = true;
  }
  A5c2GpuSmokeEvidence a5c2{true, true, true, true, true, true,
                            true, true, true, true, true, true};
  CHECK(validA5c2GpuSmoke(a5c2));
  bool* visibilityFields[]{&a5c2.realGpu,
                           &a5c2.entrypoints,
                           &a5c2.visibilityUndoRedo,
                           &a5c2.onlyVisible,
                           &a5c2.hiddenSpatialExcluded,
                           &a5c2.sharedSourceNoRemeshUpload,
                           &a5c2.lastUserPruneReacquire,
                           &a5c2.isolateShowAll,
                           &a5c2.isolatePresentationOnly,
                           &a5c2.isolateNoUploadPrune,
                           &a5c2.replacementTeardown,
                           &a5c2.layoutMatrix};
  for (bool* field : visibilityFields) {
    *field = false;
    CHECK(!validA5c2GpuSmoke(a5c2));
    *field = true;
  }
  A5c2SpatialConsumerEvidence spatial{true, true, true, true, true, true};
  CHECK(validA5c2SpatialConsumerEvidence(spatial));
  bool* spatialFields[]{&spatial.publicationCurrent,
                        &spatial.faceCurrentAndFiltered,
                        &spatial.rectangleCurrentAndFiltered,
                        &spatial.snapCurrentAndFiltered,
                        &spatial.rulerCurrentAndFiltered,
                        &spatial.fitCurrentAndFiltered};
  for (bool* field : spatialFields) {
    *field = false;
    CHECK(!validA5c2SpatialConsumerEvidence(spatial));
    *field = true;
  }
  A5c3GpuSmokeEvidence a5c3{true, true, true, true, true, true, true,
                            true, true, true, true, true, true};
  CHECK(validA5c3GpuSmoke(a5c3));
  bool* groundingFields[]{&a5c3.realGpu,
                          &a5c3.entrypoints,
                          &a5c3.groundUnground,
                          &a5c3.undoRedo,
                          &a5c3.onlyGrounded,
                          &a5c3.placementGuard,
                          &a5c3.visibilitySuppressionIndependent,
                          &a5c3.selectionPublicationRefresh,
                          &a5c3.noRemeshUpload,
                          &a5c3.insertChain,
                          &a5c3.isolateDisabled,
                          &a5c3.replacementTeardown,
                          &a5c3.layoutMatrix};
  for (bool* field : groundingFields) {
    *field = false;
    CHECK(!validA5c3GpuSmoke(a5c3));
    *field = true;
  }
  A5c4GpuSmokeEvidence a5c4{true, true, true, true, true, true, true,
                            true, true, true, true, true, true, true};
  CHECK(validA5c4GpuSmoke(a5c4));
  bool* suppressionFields[]{&a5c4.realGpu,
                            &a5c4.entrypoints,
                            &a5c4.suppressUnsuppress,
                            &a5c4.undoRedo,
                            &a5c4.onlySuppressed,
                            &a5c4.visibilityGroundedIndependent,
                            &a5c4.suppressedSpatialExcluded,
                            &a5c4.sharedSourceNoRemeshUpload,
                            &a5c4.lastUserPruneReacquire,
                            &a5c4.selectionPublicationRefresh,
                            &a5c4.insertChain,
                            &a5c4.isolateDisabled,
                            &a5c4.replacementTeardown,
                            &a5c4.layoutMatrix};
  for (bool* field : suppressionFields) {
    *field = false;
    CHECK(!validA5c4GpuSmoke(a5c4));
    *field = true;
  }
  A5c5GpuSmokeEvidence a5c5{true, true, true, true, true, true, true, true,
                            true, true, true, true, true, true, true, true};
  CHECK(validA5c5GpuSmoke(a5c5));
  bool* duplicateFields[]{&a5c5.realGpu,
                          &a5c5.entrypoints,
                          &a5c5.newStableOccurrenceId,
                          &a5c5.sameDefinitionDocumentBrep,
                          &a5c5.exactPlacementNameFlags,
                          &a5c5.selectionApplyUndoRedo,
                          &a5c5.hiddenSuppressedGrounded,
                          &a5c5.sameEpochPublicationRefresh,
                          &a5c5.sharedSourceNoRemeshUpload,
                          &a5c5.instanceDraw,
                          &a5c5.rectanglePickingBoth,
                          &a5c5.topologyReuse,
                          &a5c5.insertRenameIsolateDisabled,
                          &a5c5.failureAtomicity,
                          &a5c5.replacementTeardown,
                          &a5c5.layoutMatrix};
  for (bool* field : duplicateFields) {
    *field = false;
    CHECK(!validA5c5GpuSmoke(a5c5));
    *field = true;
  }
  A5c6GpuSmokeEvidence a5c6{true, true, true, true, true, true, true,
                            true, true, true, true, true, true, true,
                            true, true, true, true, true, true, true};
  CHECK(validA5c6GpuSmoke(a5c6));
  bool* removeFields[]{&a5c6.realGpu,
                       &a5c6.entrypoints,
                       &a5c6.removeUndoRedo,
                       &a5c6.onlyOccurrenceNode,
                       &a5c6.orphanDefinitionRetained,
                       &a5c6.orphanTreeFeedback,
                       &a5c6.lastOccurrenceEmptyRoot,
                       &a5c6.hiddenSuppressedGrounded,
                       &a5c6.selectionClearRestore,
                       &a5c6.sameEpochPublicationRefresh,
                       &a5c6.sharedSourceNoRemeshUpload,
                       &a5c6.lastUserPruneReacquire,
                       &a5c6.instanceDrawPicking,
                       &a5c6.undoCollisionRollback,
                       &a5c6.mixedChain,
                       &a5c6.emptyCamera,
                       &a5c6.topologyPruneReacquire,
                       &a5c6.isolateDisabled,
                       &a5c6.failureAtomicity,
                       &a5c6.replacementTeardown,
                       &a5c6.layoutMatrix};
  for (bool* field : removeFields) {
    *field = false;
    CHECK(!validA5c6GpuSmoke(a5c6));
    *field = true;
  }
  A5dGpuSmokeEvidence a5d{true, true, true, true, true, true, true, true,
                          true, true, true, true, true, true, true};
  CHECK(validA5dGpuSmoke(a5d));
  bool* definitionEditFields[]{&a5d.realGpu,
                               &a5d.entryReturnBreadcrumb,
                               &a5d.oneOwnerOneHistory,
                               &a5d.sharedEditAllOccurrences,
                               &a5d.idsPlacementsFlagsStable,
                               &a5d.failedRecomputeRollback,
                               &a5d.failedProjectionRollback,
                               &a5d.undoRedoShared,
                               &a5d.toolReplacementTeardown,
                               &a5d.editedSourceOneUpload,
                               &a5d.otherSourcesReused,
                               &a5d.hiddenSuppressedStable,
                               &a5d.selectionCameraRestore,
                               &a5d.orphanDefinitionEdit,
                               &a5d.layoutMatrix};
  for (bool* field : definitionEditFields) {
    *field = false;
    CHECK(!validA5dGpuSmoke(a5d));
    *field = true;
  }
  A6b2GpuSmokeEvidence a6b2{true, true, true, true, true, true,
                            true, true, true, true, true};
  CHECK(validA6b2GpuSmoke(a6b2));
  bool* linkedLifecycleFields[]{
      &a6b2.realGpu,
      &a6b2.initialActive,
      &a6b2.reload,
      &a6b2.coldUndo,
      &a6b2.coldRedo,
      &a6b2.stableOccurrenceFields,
      &a6b2.selectionHoverOccurrenceScoped,
      &a6b2.oneSharedSource,
      &a6b2.oldSourcesRetired,
      &a6b2.oneUploadPerRevision,
      &a6b2.dpiIndependent};
  for (bool* field : linkedLifecycleFields) {
    *field = false;
    CHECK(!validA6b2GpuSmoke(a6b2));
    *field = true;
  }
  A6c1GpuSmokeEvidence a6c1{true, true, true, true, true, true, true,
                            true, true, true, true, true, true};
  CHECK(validA6c1GpuSmoke(a6c1));
  bool* recursiveLifecycleFields[]{
      &a6c1.realGpu,
      &a6c1.productionV8Open,
      &a6c1.sharedRecursivePrefixes,
      &a6c1.noncommutativeWorldTransforms,
      &a6c1.oneSourceTwoInstances,
      &a6c1.independentPickSelectionHoverHighlight,
      &a6c1.terminalIsolation,
      &a6c1.intermediateIsolationDisabled,
      &a6c1.ancestorHideSuppressNoUpload,
      &a6c1.linkedReloadColdUndoRedo,
      &a6c1.oneUploadPerRevision,
      &a6c1.oldResourcesRetired,
      &a6c1.dpiIndependent};
  for (bool* field : recursiveLifecycleFields) {
    *field = false;
    CHECK(!validA6c1GpuSmoke(a6c1));
    *field = true;
  }
  A8aGpuSmokeEvidence a8a{true, true, true, true, true,
                          true, true, true, true, true};
  CHECK(validA8aGpuSmoke(a8a));
  bool* mateFields[]{&a8a.realGpu,
                     &a8a.typedTwoStage,
                     &a8a.occurrenceScope,
                     &a8a.strictFilter,
                     &a8a.alignedOpposedPreview,
                     &a8a.failedPreviewRetentionAndDisabledApply,
                     &a8a.applyCancelEscape,
                     &a8a.oneUndoRedo,
                     &a8a.teardownReentrancy,
                     &a8a.layoutDpr};
  for (bool* field : mateFields) {
    *field = false;
    CHECK(!validA8aGpuSmoke(a8a));
    *field = true;
  }
  return EXIT_SUCCESS;
}
