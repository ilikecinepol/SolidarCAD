#pragma once

#include <memory>
#include <map>
#include <optional>
#include <utility>

#include <QString>
#include <QByteArray>

#include "model/ProductDocument.h"
#include "project/ProjectPersistenceState.h"
#include "project/ProductEnvelopeV9Materialization.h"

namespace solidar {

// Stable application-layer failures. UI code localizes/presents these values;
// persistence and exchange diagnostics remain available as technical detail.
enum class ProjectApplicationErrorCode {
  None,
  InvalidProject,
  UnsupportedProject,
  CreateFailed,
  SaveFailed,
  StepImportFailed,
  StepExportFailed,
  NoExportableShapes,
  StlExportFailed,
  RelocateNotEligible,
  RelocateSameFile,
  RelocateDestinationExists,
  RelocateAmbiguousFileIdentity,
  RelocateCrossVolume,
  RelocateSourceChanged,
  RelocateDestinationChanged,
  RelocateUnsupportedFilesystem,
  RelocateFailed,
  UnexpectedFailure,
};

enum class LinkedPartRuntimeStatus : std::uint8_t {
  Unchecked,
  Exact,
  Missing,
  Outdated,
  IdentityMismatch,
  SecurityRejected,
  ResourceFailure,
  InvalidSource,
};

struct LinkedPartRuntimeState final {
  LinkedPartRuntimeStatus status{LinkedPartRuntimeStatus::Unchecked};
  QString detail;

  friend bool operator==(const LinkedPartRuntimeState&,
                         const LinkedPartRuntimeState&) = default;
};

using LinkedPartRuntimeStates =
    std::map<DefinitionId, LinkedPartRuntimeState>;

struct ProjectApplicationStatus {
  ProjectApplicationErrorCode code{ProjectApplicationErrorCode::None};
  QString detail;

  [[nodiscard]] bool succeeded() const noexcept {
    return code == ProjectApplicationErrorCode::None;
  }
};

// A completely validated replacement candidate. It owns no UI state and can
// be discarded without changing the active document, tools or presentation.
class StagedApplicationDocument final {
 public:
  StagedApplicationDocument(
      ProductDocument&& productDocument,
      std::optional<SketchId> historicalLegacyExtrusionSourceSketchId,
      std::optional<project::ProjectPersistenceState> persistenceState);
  StagedApplicationDocument(
      Document&& document,
      std::optional<SketchId> historicalLegacyExtrusionSourceSketchId,
      std::optional<project::ProjectPersistenceState> persistenceState);
  ~StagedApplicationDocument() = default;

  StagedApplicationDocument(const StagedApplicationDocument&) = delete;
  StagedApplicationDocument& operator=(const StagedApplicationDocument&) =
      delete;
  StagedApplicationDocument(StagedApplicationDocument&&) noexcept = default;
  StagedApplicationDocument& operator=(StagedApplicationDocument&&) noexcept =
      default;

  [[nodiscard]] const ProductDocument& productDocument() const noexcept {
    return productDocument_;
  }
  [[nodiscard]] DefinitionKind rootDefinitionKind() const noexcept {
    return productDocument_.rootDefinitionKind();
  }
  [[nodiscard]] const Document* rootPartDocument() const noexcept {
    const auto* root = productDocument_.rootPartDefinition();
    return root ? &root->document() : nullptr;
  }
  [[nodiscard]] const Document& rootDocument() const {
    return productDocument_.rootDocument();
  }
  [[nodiscard]] ProductDocument releaseProductDocument() && noexcept {
    return std::move(productDocument_);
  }

  std::optional<SketchId> historicalLegacyExtrusionSourceSketchId;
  std::optional<project::ProjectPersistenceState> persistenceState;
  LinkedPartRuntimeStates linkedPartRuntimeStates;
  std::vector<project::MateRuntimeDiagnostics> mateRuntimeDiagnostics;

 private:
  ProductDocument productDocument_;
};

struct StagedApplicationDocumentResult {
  ProjectApplicationStatus status;
  std::optional<StagedApplicationDocument> staged;

  [[nodiscard]] bool succeeded() const noexcept {
    return status.succeeded() && staged.has_value();
  }
};

// Move-only, file-independent carrier for A5b embedded insertion. The bytes
// are a canonical authored v4 document graph used by Undo/Redo; runtime B-Rep
// remains only in document and is never retained by history.
struct StagedEmbeddedPart final {
  std::unique_ptr<Document> document;
  QByteArray authoredDocument;
};

struct StagedEmbeddedPartResult final {
  ProjectApplicationStatus status;
  std::optional<StagedEmbeddedPart> staged;

  [[nodiscard]] bool succeeded() const noexcept {
    return status.succeeded() && staged.has_value() &&
           staged->document != nullptr && !staged->authoredDocument.isEmpty();
  }
};

enum class ProductV5OpenPolicy {
  Disallow,
  Allow,
};

struct StandaloneProductConversionRequest {
  ProjectId outputProjectId;
  DefinitionId outputRootDefinitionId;
  QString name;
  QString createdAt;
};

// Application compatibility state that is intentionally not serialized in
// the standalone Document graph. Conversion must fail closed while a legacy
// v1 extrusion is still represented only by this source-sketch marker.
struct StandaloneProductConversionPrecondition {
  std::optional<SketchId> historicalLegacyExtrusionSourceSketchId;
};

struct ProductCloneRequest {
  ProjectId outputProjectId;
  QString name;
  QString createdAt;
};

struct AssemblySaveAsRequest final {
  ProductDocumentIdentity replacementIdentity;
  QString sourcePath;
};

struct ComponentSaveAsRequest final {
  ProductDocumentIdentity replacementIdentity;
  QString sourcePath;
};

struct ProductRelocationPrecondition {
  bool sourceIsClean{false};
  std::optional<SketchId> historicalLegacyExtrusionSourceSketchId;
};

// Non-QObject application boundary for project and geometry exchange I/O.
// It deliberately owns neither project path/dirty state nor widgets/dialogs.
class ProjectApplicationService final {
 public:
  [[nodiscard]] ProjectApplicationStatus createProject(
      const QString& path) const noexcept;
  [[nodiscard]] StagedApplicationDocumentResult stageOpen(
      const QString& path,
      ProductV5OpenPolicy productV5Policy =
          ProductV5OpenPolicy::Disallow) const noexcept;
  // Assembly-aware staging boundary. V1-V4 parts, V5 products and V6 flat
  // assemblies are fully validated/materialized without touching editor state.
  [[nodiscard]] StagedApplicationDocumentResult stageOpenProduct(
      const QString& path) const noexcept;
  // A5b-only in-memory new assembly. No file is created and no identity or
  // save policy is invented here.
  [[nodiscard]] StagedApplicationDocumentResult stageNewAssembly() const
      noexcept;
  // Reuses the validated open pipeline and accepts only a single part root.
  // Source project identity/path are intentionally absent from the result.
  [[nodiscard]] StagedEmbeddedPartResult stageEmbeddedPart(
      const QString& path) const noexcept;
  [[nodiscard]] ProjectApplicationStatus save(
      const QString& path, const Document& document) const noexcept;
  [[nodiscard]] ProjectApplicationStatus save(
      const QString& path, const Document& document,
      project::ProjectPersistenceState persistenceState) const noexcept;
  [[nodiscard]] ProjectApplicationStatus save(
      const QString& path,
      const ProductDocument& productDocument) const noexcept;
  [[nodiscard]] ProjectApplicationStatus saveAssemblyV7(
      const QString& path, ProductDocument& productDocument,
      std::optional<ProductDocumentIdentity> firstSaveIdentity =
          std::nullopt) const noexcept;
  [[nodiscard]] ProjectApplicationStatus saveAssembly(
      const QString& path, ProductDocument& productDocument,
      project::ProjectPersistenceState persistenceState,
      std::optional<ProductDocumentIdentity> firstSaveIdentity =
          std::nullopt) const noexcept;
  [[nodiscard]] ProjectApplicationStatus saveAssemblyV7As(
      const QString& targetPath, ProductDocument& productDocument,
      const AssemblySaveAsRequest& request) const noexcept;
  [[nodiscard]] ProjectApplicationStatus saveAssemblyAs(
      const QString& targetPath, ProductDocument& productDocument,
      project::ProjectPersistenceState persistenceState,
      const AssemblySaveAsRequest& request) const noexcept;
  [[nodiscard]] ProjectApplicationStatus saveComponent(
      const QString& path, ProductDocument& productDocument,
      std::optional<ProductDocumentIdentity> firstSaveIdentity =
          std::nullopt,
      project::ProjectPersistenceState currentPersistence =
          {project::ProjectPersistenceFormat::V10}) const noexcept;
  [[nodiscard]] ProjectApplicationStatus saveComponentIdentityTransition(
      const QString& path, ProductDocument& productDocument,
      const ComponentIdentityTransition& transition,
      project::ProjectPersistenceState currentPersistence =
          {project::ProjectPersistenceFormat::V10}) const noexcept;
  [[nodiscard]] ProjectApplicationStatus saveComponentAs(
      const QString& targetPath, ProductDocument& productDocument,
      const ComponentSaveAsRequest& request,
      project::ProjectPersistenceState currentPersistence =
          {project::ProjectPersistenceFormat::V10}) const noexcept;
  [[nodiscard]] ProjectApplicationStatus saveComponentAsIdentityTransition(
      const QString& targetPath, ProductDocument& productDocument,
      const QString& sourcePath,
      const ComponentIdentityTransition& transition,
      project::ProjectPersistenceState currentPersistence =
          {project::ProjectPersistenceFormat::V10}) const noexcept;
  // Explicit application transaction boundary for a future V8 -> V9 UI
  // command. Save never promotes or downgrades the selected format.
  [[nodiscard]] ProjectApplicationStatus validateAssemblyV9Promotion(
      const ProductDocument& productDocument,
      project::ProjectPersistenceState currentState) const noexcept;
  [[nodiscard]] ProjectApplicationStatus promoteAssemblyPersistenceToV9(
      project::ProjectPersistenceState& state,
      const ProductDocument& productDocument) const noexcept;
  [[nodiscard]] ProjectApplicationStatus convertStandaloneToProduct(
      const QString& targetPath, ProductDocument& source,
      const StandaloneProductConversionRequest& request,
      const StandaloneProductConversionPrecondition& precondition)
      const noexcept;
  [[nodiscard]] ProjectApplicationStatus saveProductAsClone(
      const QString& targetPath, ProductDocument& source,
      const ProductCloneRequest& request) const noexcept;
  [[nodiscard]] ProjectApplicationStatus relocateProduct(
      const QString& sourcePath, const QString& targetPath,
      const ProductDocument& source,
      const ProductRelocationPrecondition& precondition) const noexcept;
  [[nodiscard]] ProjectApplicationStatus upgradeProjectFormat(
      const QString& path, const Document& document,
      project::ProjectPersistenceState currentState) const noexcept;

  [[nodiscard]] StagedApplicationDocumentResult stageImportStep(
      const QString& path, const Document& currentDocument,
      const QString& featureName = {}) const noexcept;
  [[nodiscard]] StagedApplicationDocumentResult stageImportStep(
      const QString& path, const ProductDocument& currentProductDocument,
      const QString& featureName = {}) const noexcept;
  [[nodiscard]] ProjectApplicationStatus exportStep(
      const QString& path, const Document& document) const noexcept;

  [[nodiscard]] ProjectApplicationStatus preflightStlExport(
      const Document& document) const noexcept;
  [[nodiscard]] ProjectApplicationStatus exportStl(
      const QString& path, const Document& document) const noexcept;
  [[nodiscard]] ProjectApplicationStatus preflightFullProductExport(
      const ProductDocument& productDocument,
      const QString& projectPath) const noexcept;
};

}  // namespace solidar
