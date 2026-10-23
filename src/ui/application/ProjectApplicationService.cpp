#include "ui/application/ProjectApplicationService.h"

#include <QFile>
#include <QJsonDocument>

#include <cmath>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "io/DocumentExportShapes.h"
#include "io/StepExchange.h"
#include "io/StlExporter.h"
#include "model/IdGeneration.h"
#include "project/ProductEnvelopeV5Conversion.h"
#include "project/ProductEnvelopeV5Decoder.h"
#include "project/ProductEnvelopeV5Materialization.h"
#include "project/ProjectFile.h"
#include "project/internal/ProjectAtomicWriting.h"
#include "project/internal/ProductEnvelopeV5AtomicWriting.h"
#include "ui/application/internal/ProductConversionApplicationService.h"

namespace solidar {
namespace {

constexpr DefinitionId kTransientApplicationRootDefinitionId{1};
constexpr double kMaximumExactJsonInteger = 9007199254740991.0;

ProductDocument makeSingleDefinitionProduct(
    Document&& document, DefinitionId rootDefinitionId,
    std::optional<ProductDocumentIdentity> identity = std::nullopt) {
  DefinitionRegistry definitions;
  PartDefinition root(rootDefinitionId,
                      std::make_unique<Document>(std::move(document)));
  const auto inserted = definitions.insert(std::move(root));
  if (inserted != DefinitionInsertStatus::Inserted)
    throw std::logic_error("failed to stage root definition");

  auto created = ProductDocument::create(std::move(definitions),
                                         rootDefinitionId,
                                         std::move(identity));
  if (!created)
    throw std::logic_error("failed to create staged product document");
  return std::move(*created.product);
}

ProductDocument makeTransientProductDocument(Document&& document) {
  return makeSingleDefinitionProduct(
      std::move(document), kTransientApplicationRootDefinitionId);
}

std::optional<QByteArray> shallowProductV5Payload(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
  const qint64 size = file.size();
  if (size < 0 || size > project::ProjectFile::kMaximumFileBytes)
    return std::nullopt;
  const QByteArray payload = file.readAll();
  if (file.error() != QFileDevice::NoError || payload.size() != size)
    return std::nullopt;

  QJsonParseError parseError;
  const QJsonDocument parsed = QJsonDocument::fromJson(payload, &parseError);
  if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
    return std::nullopt;
  const QJsonObject root = parsed.object();
  if (!root.value(QStringLiteral("format")).isString() ||
      root.value(QStringLiteral("format")).toString() !=
          QStringLiteral("solidar-project")) {
    return std::nullopt;
  }
  const QJsonValue versionValue = root.value(QStringLiteral("version"));
  if (!versionValue.isDouble()) return std::nullopt;
  const double version = versionValue.toDouble();
  if (!std::isfinite(version) || std::trunc(version) != version ||
      version < 5.0 || version > kMaximumExactJsonInteger) {
    return std::nullopt;
  }
  return payload;
}

std::optional<QString> exactQStringFromUtf8(const std::string& bytes) {
  const QByteArray original(bytes.data(),
                            static_cast<qsizetype>(bytes.size()));
  const QString decoded = QString::fromUtf8(original);
  if (decoded.toUtf8() != original) return std::nullopt;
  return decoded;
}

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

template <typename Publisher>
ProjectApplicationStatus convertStandaloneToProductImpl(
    ProductDocument& source,
    const StandaloneProductConversionRequest& request,
    Publisher&& publisher) noexcept {
  try {
    const project::ProductEnvelopeV5Metadata metadata{request.name,
                                                       request.createdAt};
    auto prepared = project::prepareStandaloneProductEnvelopeV5Conversion(
        source, request.outputProjectId, request.outputRootDefinitionId,
        metadata);
    if (!prepared.succeeded()) {
      const auto code =
          prepared.status ==
                  project::ProductEnvelopeV5ConversionStatus::PayloadTooLarge
              ? ProjectApplicationErrorCode::SaveFailed
              : ProjectApplicationErrorCode::InvalidProject;
      return failure(code, std::move(prepared.error));
    }

    auto scanned = project::scanProductEnvelopeV5(
        QByteArrayView(prepared.prepared->payload.bytes));
    if (scanned.status != project::ProductEnvelopeV5ScanStatus::Candidate ||
        !scanned.candidate) {
      const auto code =
          scanned.status == project::ProductEnvelopeV5ScanStatus::Unsupported
              ? ProjectApplicationErrorCode::UnsupportedProject
              : ProjectApplicationErrorCode::InvalidProject;
      return failure(code, std::move(scanned.error));
    }

    auto materialized =
        project::materializeProductEnvelopeV5(*scanned.candidate);
    if (!materialized.succeeded()) {
      const auto code = materialized.status ==
                                project::ProductEnvelopeV5MaterializationStatus::
                                    Unsupported
                            ? ProjectApplicationErrorCode::UnsupportedProject
                            : ProjectApplicationErrorCode::InvalidProject;
      return failure(code, std::move(materialized.error));
    }

    const auto published = publisher(
        QByteArrayView(prepared.prepared->payload.bytes));
    if (!published.succeeded()) {
      return failure(
          ProjectApplicationErrorCode::SaveFailed,
          published.error.isEmpty()
              ? QStringLiteral("Failed to publish prepared product payload.")
              : std::move(published.error));
    }

    const auto committed = source.commitPreparedStandaloneConversion(
        std::move(*materialized.product));
    if (committed != ProductDocumentStandaloneConversionStatus::Committed) {
      return failure(
          ProjectApplicationErrorCode::UnexpectedFailure,
          QStringLiteral(
              "Prepared product was published but its model commit failed."));
    }
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка преобразования продукта", exception);
  } catch (...) {
    return unknownFailure("Ошибка преобразования продукта");
  }
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

StagedApplicationDocument::StagedApplicationDocument(
    ProductDocument&& productDocument,
    std::optional<SketchId> historicalLegacyExtrusionSourceSketchId,
    std::optional<project::ProjectPersistenceState> persistenceState)
    : historicalLegacyExtrusionSourceSketchId(
          historicalLegacyExtrusionSourceSketchId),
      persistenceState(persistenceState),
      productDocument_(std::move(productDocument)) {}

StagedApplicationDocument::StagedApplicationDocument(
    Document&& document,
    std::optional<SketchId> historicalLegacyExtrusionSourceSketchId,
    std::optional<project::ProjectPersistenceState> persistenceState)
    : StagedApplicationDocument(
          makeTransientProductDocument(std::move(document)),
          historicalLegacyExtrusionSourceSketchId, persistenceState) {}

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
    const QString& path, ProductV5OpenPolicy productV5Policy) const noexcept {
  try {
    if (productV5Policy == ProductV5OpenPolicy::Allow) {
      auto payload = shallowProductV5Payload(path);
      if (payload) {
        auto scanned =
            project::scanProductEnvelopeV5(QByteArrayView(*payload));
        if (scanned.status == project::ProductEnvelopeV5ScanStatus::Invalid ||
            scanned.status ==
                project::ProductEnvelopeV5ScanStatus::NotApplicable) {
          return {{ProjectApplicationErrorCode::InvalidProject,
                   std::move(scanned.error)},
                  std::nullopt};
        }
        if (!scanned.candidate) {
          return {{ProjectApplicationErrorCode::UnsupportedProject,
                   std::move(scanned.error)},
                  std::nullopt};
        }

        auto materialized =
            project::materializeProductEnvelopeV5(*scanned.candidate);
        if (!materialized.succeeded()) {
          const bool unsupported =
              materialized.status ==
              project::ProductEnvelopeV5MaterializationStatus::Unsupported;
          const auto code =
              unsupported ? ProjectApplicationErrorCode::UnsupportedProject
                          : ProjectApplicationErrorCode::InvalidProject;
          return {{code, std::move(materialized.error)}, std::nullopt};
        }
        return {{}, StagedApplicationDocument(
                        std::move(*materialized.product), std::nullopt,
                        project::ProjectPersistenceState{
                            project::ProjectPersistenceFormat::V5})};
      }
    }

    auto loaded = project::ProjectFile::stageLoad(path);
    if (!loaded.succeeded()) {
      const auto code = loaded.status == project::ProjectLoadStatus::Unsupported
                            ? ProjectApplicationErrorCode::UnsupportedProject
                            : ProjectApplicationErrorCode::InvalidProject;
      return {{code, std::move(loaded.error)}, std::nullopt};
    }

    Document candidate;
    std::optional<SketchId> historicalLegacyExtrusionSourceSketchId;
    switch (project::classifyPartPayload(loaded.payloadKind)) {
      case project::PartPayloadDisposition::Parametric:
        if (!loaded.document) {
          return {{ProjectApplicationErrorCode::InvalidProject,
                   QString::fromUtf8(
                       "Проект v2/v3/v4 не содержит восстановленного документа.")},
                  std::nullopt};
        }
        candidate = std::move(*loaded.document);
        break;
      case project::PartPayloadDisposition::Legacy: {
        candidate.setBox(loaded.legacy.box);
        // v1 does not persist IDs. Use deterministic local IDs while explicit
        // reservation is paused so speculative staging cannot consume process-
        // global edit IDs. The commit boundary reserves them atomically later.
        detail::ScopedExplicitIdReservationPause reservationPause;
        for (std::size_t index = 0; index < loaded.legacy.sketches.size();
             ++index) {
          const auto& saved = loaded.legacy.sketches[index];
          auto& modelSketch = candidate.addSketch(
              static_cast<SketchId>(index + 1), "Loaded sketch", {});
          modelSketch.geometry = saved.geometry;
          modelSketch.placement =
              legacyV1SketchPlacement(saved.support, loaded.legacy.box);
        }
        if (loaded.legacy.hasExtrusion &&
            loaded.legacy.extrusionSourceSketch &&
            *loaded.legacy.extrusionSourceSketch < candidate.sketches().size()) {
          historicalLegacyExtrusionSourceSketchId =
              candidate.sketches()[*loaded.legacy.extrusionSourceSketch]
                  .id;
        }
        break;
      }
      case project::PartPayloadDisposition::Unsupported:
      default:
        return {{ProjectApplicationErrorCode::UnsupportedProject,
                 QString::fromUtf8(
                     "Проект не содержит поддерживаемых данных детали.")},
                std::nullopt};
    }
    return {{}, StagedApplicationDocument(
                    std::move(candidate),
                    historicalLegacyExtrusionSourceSketchId,
                    loaded.persistenceState)};
  } catch (const std::exception& exception) {
    return {unexpectedFailure("Ошибка подготовки проекта", exception),
            std::nullopt};
  } catch (...) {
    return {unknownFailure("Ошибка подготовки проекта"), std::nullopt};
  }
}

ProjectApplicationStatus ProjectApplicationService::save(
    const QString& path, const Document& document) const noexcept {
  return save(path, document, {project::ProjectPersistenceFormat::V3});
}

ProjectApplicationStatus ProjectApplicationService::convertStandaloneToProduct(
    const QString& targetPath, ProductDocument& source,
    const StandaloneProductConversionRequest& request) const noexcept {
  return convertStandaloneToProductImpl(
      source, request, [&targetPath](QByteArrayView bytes) {
        return project::detail::publishProjectPayloadAtomically(targetPath,
                                                                 bytes);
      });
}

ProjectApplicationStatus ProjectApplicationService::save(
    const QString& path, const Document& document,
    project::ProjectPersistenceState persistenceState) const noexcept {
  try {
    QString detail;
    if (!project::ProjectFile::saveDocument(path, document, persistenceState,
                                            &detail))
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     std::move(detail));
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка сохранения проекта", exception);
  } catch (...) {
    return unknownFailure("Ошибка сохранения проекта");
  }
}

ProjectApplicationStatus ProjectApplicationService::save(
    const QString& path,
    const ProductDocument& productDocument) const noexcept {
  try {
    if (!productDocument.identity()) {
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral(
                         "ProductDocument v5 requires an identity bundle."));
    }
    if (productDocument.definitions().size() != 1) {
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral(
                         "ProductDocument v5 requires one definition."));
    }

    const ProductDocumentIdentity& identity = *productDocument.identity();
    const auto name = exactQStringFromUtf8(identity.metadata.nameUtf8);
    const auto createdAt =
        exactQStringFromUtf8(identity.metadata.createdAtUtf8);
    if (!name || !createdAt) {
      return failure(ProjectApplicationErrorCode::SaveFailed,
                     QStringLiteral(
                         "ProductDocument metadata is not exact UTF-8."));
    }
    const project::ProductEnvelopeV5Metadata metadata{*name, *createdAt};
    const auto written = project::detail::writeProductEnvelopeV5Atomically(
        path, productDocument, identity.projectId, metadata);
    if (!written.succeeded()) {
      return failure(
          ProjectApplicationErrorCode::SaveFailed,
          written.error.isEmpty()
              ? QStringLiteral("Failed to atomically save product v5.")
              : std::move(written.error));
    }
    return {};
  } catch (const std::exception& exception) {
    return unexpectedFailure("Ошибка сохранения продукта", exception);
  } catch (...) {
    return unknownFailure("Ошибка сохранения продукта");
  }
}

ProjectApplicationStatus ProjectApplicationService::upgradeProjectFormat(
    const QString& path, const Document& document,
    project::ProjectPersistenceState currentState) const noexcept {
  switch (currentState.format) {
    case project::ProjectPersistenceFormat::V3:
      return save(path, document, {project::ProjectPersistenceFormat::V4});
    case project::ProjectPersistenceFormat::V4:
    case project::ProjectPersistenceFormat::V5:
      return {};
  }
  return failure(ProjectApplicationErrorCode::SaveFailed,
                 QStringLiteral("Unknown project persistence format."));
}

StagedApplicationDocumentResult ProjectApplicationService::stageImportStep(
    const QString& path, const Document& currentDocument,
    const QString& featureName) const noexcept {
  try {
    Document candidate = currentDocument;
    QString detail;
    if (!io::importDocumentStep(path, &candidate, featureName, &detail)) {
      return {{ProjectApplicationErrorCode::StepImportFailed,
               std::move(detail)},
              std::nullopt};
    }
    return {{}, StagedApplicationDocument(
                    std::move(candidate), std::nullopt, std::nullopt)};
  } catch (const std::exception& exception) {
    return {unexpectedFailure("Ошибка импорта STEP", exception),
            std::nullopt};
  } catch (...) {
    return {unknownFailure("Ошибка импорта STEP"), std::nullopt};
  }
}

StagedApplicationDocumentResult ProjectApplicationService::stageImportStep(
    const QString& path, const ProductDocument& currentProductDocument,
    const QString& featureName) const noexcept {
  try {
    if (currentProductDocument.definitions().size() != 1) {
      return {{ProjectApplicationErrorCode::InvalidProject,
               QStringLiteral(
                   "STEP product staging requires one definition.")},
              std::nullopt};
    }
    detail::ScopedExplicitIdReservationPause reservationPause;
    Document candidate = currentProductDocument.rootDocument();
    QString detail;
    if (!io::importDocumentStep(path, &candidate, featureName, &detail)) {
      return {{ProjectApplicationErrorCode::StepImportFailed,
               std::move(detail)},
              std::nullopt};
    }
    ProductDocument stagedProduct = makeSingleDefinitionProduct(
        std::move(candidate), currentProductDocument.rootDefinitionId(),
        currentProductDocument.identity());
    return {{}, StagedApplicationDocument(
                    std::move(stagedProduct), std::nullopt, std::nullopt)};
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

namespace solidar::application::detail {

ProjectApplicationStatus convertStandaloneToProduct(
    ProductDocument& source,
    const StandaloneProductConversionRequest& request,
    project::detail::ProjectAtomicWriteTransaction& transaction) noexcept {
  return convertStandaloneToProductImpl(
      source, request, [&transaction](QByteArrayView bytes) {
        return project::detail::publishProjectPayloadAtomically(bytes,
                                                                 transaction);
      });
}

}  // namespace solidar::application::detail
