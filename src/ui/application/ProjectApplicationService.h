#pragma once

#include <optional>

#include <QString>

#include "model/Document.h"

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
struct StagedApplicationDocument {
  Document document;
  std::optional<SketchId> historicalLegacyExtrusionSourceSketchId;
};

struct StagedApplicationDocumentResult {
  ProjectApplicationStatus status;
  std::optional<StagedApplicationDocument> staged;

  [[nodiscard]] bool succeeded() const noexcept {
    return status.succeeded() && staged.has_value();
  }
};

// Non-QObject application boundary for project and geometry exchange I/O.
// It deliberately owns neither project path/dirty state nor widgets/dialogs.
class ProjectApplicationService final {
 public:
  [[nodiscard]] ProjectApplicationStatus createProject(
      const QString& path) const noexcept;
  [[nodiscard]] StagedApplicationDocumentResult stageOpen(
      const QString& path) const noexcept;
  [[nodiscard]] ProjectApplicationStatus save(
      const QString& path, const Document& document) const noexcept;

  [[nodiscard]] StagedApplicationDocumentResult stageImportStep(
      const QString& path, const Document& currentDocument,
      const QString& featureName = {}) const noexcept;
  [[nodiscard]] ProjectApplicationStatus exportStep(
      const QString& path, const Document& document) const noexcept;

  [[nodiscard]] ProjectApplicationStatus preflightStlExport(
      const Document& document) const noexcept;
  [[nodiscard]] ProjectApplicationStatus exportStl(
      const QString& path, const Document& document) const noexcept;
};

}  // namespace solidar
