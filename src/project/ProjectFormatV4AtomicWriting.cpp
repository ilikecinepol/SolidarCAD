#include "project/internal/ProjectFormatV4AtomicWriting.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>

#include <exception>
#include <utility>

#include "project/ProjectFile.h"
#include "project/ProjectFormatV4EnvelopeDecoder.h"
#include "project/ProjectFormatV4ModelValidator.h"
#include "project/ProjectFormatV4PayloadScanner.h"

namespace solidar::project::detail {
namespace {

QString fallbackError(QString error, const char *fallback) {
  return error.isEmpty() ? QString::fromLatin1(fallback) : std::move(error);
}

QString exceptionError(const std::exception &exception) {
  return QString::fromUtf8(exception.what());
}

} // namespace

ProjectFormatV4PayloadPreparationResult prepareProjectFormatV4DocumentPayload(
    const Document &document, const ProjectFormatV4DocumentMetadata &metadata) {
  ProjectFormatV4PayloadPreparationResult result;
  try {
    auto encoded = encodeProjectFormatV4DocumentRoot(document, metadata);
    if (!encoded.succeeded()) {
      result.failure = ProjectFormatV4AtomicWriteFailure::Encode;
      result.error = fallbackError(std::move(encoded.error),
                                   "v4 document encoding failed");
      return result;
    }

    QJsonObject root = std::move(*encoded.root);
    QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (bytes.size() > ProjectFile::kMaximumFileBytes) {
      result.failure = ProjectFormatV4AtomicWriteFailure::PayloadTooLarge;
      result.error = QStringLiteral("serialized v4 payload exceeds %1 bytes")
                         .arg(ProjectFile::kMaximumFileBytes);
      return result;
    }

    auto envelope = scanProjectFormatV4Envelope(QByteArrayView(bytes));
    if (!envelope.hasCandidate() || envelope.candidate->authoredRoot != root) {
      result.failure =
          ProjectFormatV4AtomicWriteFailure::SerializedPayloadValidation;
      result.error = fallbackError(
          std::move(envelope.error),
          "serialized v4 envelope does not match the encoded root");
      return result;
    }
    auto payload = scanProjectFormatV4Payload(std::move(*envelope.candidate));
    if (!payload.hasCandidate() ||
        payload.candidate->envelope.authoredRoot != root) {
      result.failure =
          ProjectFormatV4AtomicWriteFailure::SerializedPayloadValidation;
      result.error = fallbackError(
          std::move(payload.error),
          "serialized v4 payload does not match the encoded root");
      return result;
    }
    auto model = validateProjectFormatV4Model(std::move(*payload.candidate));
    if (model.status != ProjectFormatV4ModelValidationStatus::Candidate ||
        !model.candidate ||
        model.candidate->legacySketch.topology.payload.envelope.authoredRoot !=
            root) {
      result.failure =
          ProjectFormatV4AtomicWriteFailure::SerializedPayloadValidation;
      result.error =
          fallbackError(std::move(model.error),
                        "serialized v4 model does not match the encoded root");
      return result;
    }

    result.failure = ProjectFormatV4AtomicWriteFailure::None;
    result.payload.emplace(
        ProjectFormatV4PreparedPayload{std::move(root), std::move(bytes)});
    return result;
  } catch (const std::exception &exception) {
    result.failure = ProjectFormatV4AtomicWriteFailure::Encode;
    result.error = exceptionError(exception);
    return result;
  } catch (...) {
    result.failure = ProjectFormatV4AtomicWriteFailure::Encode;
    result.error =
        QStringLiteral("unknown exception while preparing v4 payload");
    return result;
  }
}

ProjectFormatV4AtomicWriteResult publishProjectFormatV4PayloadAtomically(
    QByteArrayView bytes, ProjectFormatV4AtomicWriteTransaction &transaction) {
  if (bytes.size() > ProjectFile::kMaximumFileBytes) {
    return {ProjectFormatV4AtomicWriteFailure::PayloadTooLarge,
            QStringLiteral("serialized v4 payload exceeds %1 bytes")
                .arg(ProjectFile::kMaximumFileBytes)};
  }
  return publishProjectPayloadAtomically(bytes, transaction);
}

ProjectFormatV4AtomicWriteResult writeProjectFormatV4DocumentAtomically(
    const Document &document, const ProjectFormatV4DocumentMetadata &metadata,
    ProjectFormatV4AtomicWriteTransaction &transaction) {
  auto prepared = prepareProjectFormatV4DocumentPayload(document, metadata);
  if (!prepared.succeeded())
    return {prepared.failure, std::move(prepared.error)};
  return publishProjectFormatV4PayloadAtomically(
      QByteArrayView(prepared.payload->bytes), transaction);
}

ProjectFormatV4AtomicWriteResult
writeProjectFormatV4DocumentAtomically(const QString &path,
                                       const Document &document) {
  const ProjectFormatV4DocumentMetadata metadata{
      QFileInfo(path).completeBaseName(),
      QDateTime::currentDateTimeUtc().toString(Qt::ISODate)};
  auto prepared = prepareProjectFormatV4DocumentPayload(document, metadata);
  if (!prepared.succeeded())
    return {prepared.failure, std::move(prepared.error)};

  return publishProjectPayloadAtomically(
      path, QByteArrayView(prepared.payload->bytes));
}

} // namespace solidar::project::detail
