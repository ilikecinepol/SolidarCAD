#include "ui/application/ProjectApplicationService.h"

#include <exception>
#include <utility>

#include "io/DocumentExportShapes.h"
#include "io/StepExchange.h"
#include "io/StlExporter.h"
#include "model/IdGeneration.h"
#include "project/ProjectFile.h"

namespace solidar {
namespace {

ProjectApplicationStatus failure(ProjectApplicationErrorCode code,
                                 QString detail) {
  return {code, std::move(detail)};
}

ProjectApplicationStatus unexpectedFailure(const char* operation,
                                            const std::exception& exception) {
  return failure(
      ProjectApplicationErrorCode::UnexpectedFailure,
      QString::fromUtf8("%1: %2")
          .arg(QString::fromUtf8(operation),
               QString::fromUtf8(exception.what())));
}

ProjectApplicationStatus unknownFailure(const char* operation) {
  return failure(ProjectApplicationErrorCode::UnexpectedFailure,
                 QString::fromUtf8("%1: неизвестная ошибка.")
                     .arg(QString::fromUtf8(operation)));
}

SketchPlacement legacyV1SketchPlacement(const QString& support,
                                         const BoxParameters& box) {
  if (support.contains(QStringLiteral("XZ")) ||
      support.contains(QString::fromUtf8("Передняя")) ||
      support.contains(QString::fromUtf8("Задняя"))) {
    auto placement = SketchPlacement::xz();
    if (support.contains(QString::fromUtf8("Передняя")))
      placement.origin.y = -box.depthMm * 0.5;
    else if (support.contains(QString::fromUtf8("Задняя")))
      placement.origin.y = box.depthMm * 0.5;
    return placement;
  }
  if (support.contains(QStringLiteral("YZ")) ||
      support.contains(QString::fromUtf8("Правая")) ||
      support.contains(QString::fromUtf8("Левая"))) {
    auto placement = SketchPlacement::yz();
    if (support.contains(QString::fromUtf8("Правая")))
      placement.origin.x = box.widthMm * 0.5;
    else if (support.contains(QString::fromUtf8("Левая")))
      placement.origin.x = -box.widthMm * 0.5;
    return placement;
  }
  auto placement = SketchPlacement::xy();
  if (support.contains(QString::fromUtf8("Верхняя")))
    placement.origin.z = box.heightMm;
  return placement;
}

}  // namespace

ProjectApplicationStatus ProjectApplicationService::createProject(
    const QString& path) const noexcept {
  try {
    QString detail;
    if (!project::ProjectFile::create(path, &detail))
      return failure(ProjectApplicationErrorCode::CreateFailed,
                     std::move(detail));
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка создания проекта", exception);
  } catch (...) {
    return unknownFailure("Ошибка создания проекта");
  }
}

StagedApplicationDocumentResult ProjectApplicationService::stageOpen(
    const QString& path) const noexcept {
  try {
    auto loaded = project::ProjectFile::stageLoad(path);
    if (!loaded.succeeded()) {
      const auto code = loaded.kind == project::ProjectLoadKind::Unsupported
                            ? ProjectApplicationErrorCode::UnsupportedProject
                            : ProjectApplicationErrorCode::InvalidProject;
      return {{code, std::move(loaded.error)}, std::nullopt};
    }

    StagedApplicationDocument candidate;
    if (loaded.kind == project::ProjectLoadKind::ValidV2) {
      if (!loaded.document) {
        return {{ProjectApplicationErrorCode::InvalidProject,
                 QString::fromUtf8(
                     "Проект v2 не содержит восстановленного документа.")},
                std::nullopt};
      }
      candidate.document = std::move(*loaded.document);
    } else {
      candidate.document.setBox(loaded.legacy.box);
      // v1 does not persist IDs. Use deterministic local IDs while explicit
      // reservation is paused so speculative staging cannot consume process-
      // global edit IDs. The commit boundary reserves them atomically later.
      detail::ScopedExplicitIdReservationPause reservationPause;
      for (std::size_t index = 0; index < loaded.legacy.sketches.size();
           ++index) {
        const auto& saved = loaded.legacy.sketches[index];
        auto& modelSketch = candidate.document.addSketch(
            static_cast<SketchId>(index + 1), "Loaded sketch", {});
        modelSketch.geometry = saved.geometry;
        modelSketch.placement =
            legacyV1SketchPlacement(saved.support, loaded.legacy.box);
      }
      if (loaded.legacy.hasExtrusion &&
          loaded.legacy.extrusionSourceSketch &&
          *loaded.legacy.extrusionSourceSketch <
              candidate.document.sketches().size()) {
        candidate.historicalLegacyExtrusionSourceSketchId =
            candidate.document
                .sketches()[*loaded.legacy.extrusionSourceSketch]
                .id;
      }
    }
    return {{}, std::move(candidate)};
  } catch (const std::exception& exception) {
    return {unexpectedFailure("Ошибка подготовки проекта", exception),
            std::nullopt};
  } catch (...) {
    return {unknownFailure("Ошибка подготовки проекта"), std::nullopt};
  }
}

ProjectApplicationStatus ProjectApplicationService::save(
    const QString& path, const Document& document) const noexcept {
  try {
    QString detail;
    if (!project::ProjectFile::saveDocument(path, document, &detail))
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     std::move(detail));
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка сохранения проекта", exception);
  } catch (...) {
    return unknownFailure("Ошибка сохранения проекта");
  }
}

StagedApplicationDocumentResult ProjectApplicationService::stageImportStep(
    const QString& path, const Document& currentDocument,
    const QString& featureName) const noexcept {
  try {
    StagedApplicationDocument candidate{currentDocument, std::nullopt};
    QString detail;
    if (!io::importDocumentStep(path, &candidate.document, featureName,
                                &detail)) {
      return {{ProjectApplicationErrorCode::StepImportFailed,
               std::move(detail)},
              std::nullopt};
    }
    return {{}, std::move(candidate)};
  } catch (const std::exception& exception) {
    return {unexpectedFailure("Ошибка импорта STEP", exception),
            std::nullopt};
  } catch (...) {
    return {unknownFailure("Ошибка импорта STEP"), std::nullopt};
  }
}

ProjectApplicationStatus ProjectApplicationService::exportStep(
    const QString& path, const Document& document) const noexcept {
  try {
    QString detail;
    if (!io::exportDocumentStep(path, document, &detail))
      return failure(ProjectApplicationErrorCode::StepExportFailed,
                     std::move(detail));
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка экспорта STEP", exception);
  } catch (...) {
    return unknownFailure("Ошибка экспорта STEP");
  }
}

ProjectApplicationStatus ProjectApplicationService::preflightStlExport(
    const Document& document) const noexcept {
  try {
    QString detail;
    if (!io::hasExportableDocumentShapes(document, &detail))
      return failure(ProjectApplicationErrorCode::NoExportableShapes,
                     std::move(detail));
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка проверки экспорта STL", exception);
  } catch (...) {
    return unknownFailure("Ошибка проверки экспорта STL");
  }
}

ProjectApplicationStatus ProjectApplicationService::exportStl(
    const QString& path, const Document& document) const noexcept {
  try {
    QString detail;
    if (!io::exportDocumentAsciiStl(path, document, &detail))
      return failure(ProjectApplicationErrorCode::StlExportFailed,
                     std::move(detail));
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка экспорта STL", exception);
  } catch (...) {
    return unknownFailure("Ошибка экспорта STL");
  }
}

}  // namespace solidar
