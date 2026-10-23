#pragma once

#include <memory>
#include <optional>
#include <utility>

#include <QString>

#include "model/ProductDocument.h"
#include "project/ProjectPersistenceState.h"

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
  UnexpectedFailure,
};

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
  [[nodiscard]] const Document& rootDocument() const noexcept {
    return productDocument_.rootDocument();
  }
  [[nodiscard]] ProductDocument releaseProductDocument() && noexcept {
    return std::move(productDocument_);
  }

  std::optional<SketchId> historicalLegacyExtrusionSourceSketchId;
  std::optional<project::ProjectPersistenceState> persistenceState;

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
  [[nodiscard]] ProjectApplicationStatus save(
      const QString& path, const Document& document) const noexcept;
  [[nodiscard]] ProjectApplicationStatus save(
      const QString& path, const Document& document,
      project::ProjectPersistenceState persistenceState) const noexcept;
  [[nodiscard]] ProjectApplicationStatus save(
      const QString& path,
      const ProductDocument& productDocument) const noexcept;
  [[nodiscard]] ProjectApplicationStatus convertStandaloneToProduct(
      const QString& targetPath, ProductDocument& source,
      const StandaloneProductConversionRequest& request,
      const StandaloneProductConversionPrecondition& precondition)
      const noexcept;
  [[nodiscard]] ProjectApplicationStatus saveProductAsClone(
      const QString& targetPath, ProductDocument& source,
      const ProductCloneRequest& request) const noexcept;
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
};

}  // namespace solidar
