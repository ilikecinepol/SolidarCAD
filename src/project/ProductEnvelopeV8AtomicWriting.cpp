#include "project/internal/ProductEnvelopeV8AtomicWriting.h"

#include <utility>

#include <QByteArrayView>

#include "project/ProductEnvelopeV8Decoder.h"
#include "project/ProductEnvelopeV8Encoder.h"
#include "project/ProductEnvelopeV8Materialization.h"

namespace solidar::project::detail {
namespace {

ProjectAtomicWriteResult preparationFailure(
    const ProductEnvelopeV8EncodeResult& encoded) {
  ProjectAtomicWriteFailure failure = ProjectAtomicWriteFailure::Encode;
  if (encoded.status == ProductEnvelopeV8EncodeStatus::PayloadTooLarge)
    failure = ProjectAtomicWriteFailure::PayloadTooLarge;
  else if (encoded.status ==
           ProductEnvelopeV8EncodeStatus::SerializedPayloadValidation)
    failure = ProjectAtomicWriteFailure::SerializedPayloadValidation;
  return {failure, encoded.error};
}

ProjectAtomicWriteResult injectedFailure(
    ProductEnvelopeV8AtomicWritePreparationFailurePoint point) {
  switch (point) {
    case ProductEnvelopeV8AtomicWritePreparationFailurePoint::Encode:
      return {ProjectAtomicWriteFailure::Encode,
              QStringLiteral("injected V8 encode failure")};
    case ProductEnvelopeV8AtomicWritePreparationFailurePoint::Scan:
      return {ProjectAtomicWriteFailure::SerializedPayloadValidation,
              QStringLiteral("injected V8 scan failure")};
    case ProductEnvelopeV8AtomicWritePreparationFailurePoint::Materialize:
      return {ProjectAtomicWriteFailure::MaterializationValidation,
              QStringLiteral("injected V8 materialization failure")};
    case ProductEnvelopeV8AtomicWritePreparationFailurePoint::None:
      break;
  }
  return {ProjectAtomicWriteFailure::None, {}};
}

template <class Publish>
ProjectAtomicWriteResult prepareAndPublish(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata,
    ProductEnvelopeV8AtomicWritePreparationFailurePoint failurePoint,
    Publish&& publish) {
  if (failurePoint ==
      ProductEnvelopeV8AtomicWritePreparationFailurePoint::Encode)
    return injectedFailure(failurePoint);
  auto encoded = encodeProductEnvelopeV8Root(source, outputProjectId, metadata);
  if (!encoded.succeeded()) return preparationFailure(encoded);

  if (failurePoint ==
      ProductEnvelopeV8AtomicWritePreparationFailurePoint::Scan)
    return injectedFailure(failurePoint);
  auto scanned =
      scanProductEnvelopeV8(QByteArrayView(encoded.payload->bytes));
  if (!scanned.hasCandidate())
    return {ProjectAtomicWriteFailure::SerializedPayloadValidation,
            std::move(scanned.error)};

  if (failurePoint ==
      ProductEnvelopeV8AtomicWritePreparationFailurePoint::Materialize)
    return injectedFailure(failurePoint);
  auto materialized = materializeProductEnvelopeV8(*scanned.candidate);
  if (!materialized.succeeded())
    return {ProjectAtomicWriteFailure::MaterializationValidation,
            std::move(materialized.error)};

  return std::forward<Publish>(publish)(
      QByteArrayView(encoded.payload->bytes));
}

}  // namespace

ProjectAtomicWriteResult writeProductEnvelopeV8Atomically(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata,
    ProjectAtomicWriteTransaction& transaction,
    ProductEnvelopeV8AtomicWritePreparationFailurePoint failurePoint) {
  return prepareAndPublish(
      source, outputProjectId, metadata, failurePoint,
      [&transaction](QByteArrayView bytes) {
        return publishProjectPayloadAtomically(bytes, transaction);
      });
}

ProjectAtomicWriteResult writeProductEnvelopeV8Atomically(
    const QString& path, const ProductDocument& source,
    ProjectId outputProjectId, const ProductEnvelopeV5Metadata& metadata,
    ProductEnvelopeV8AtomicWritePreparationFailurePoint failurePoint) {
  return prepareAndPublish(
      source, outputProjectId, metadata, failurePoint,
      [&path](QByteArrayView bytes) {
        return publishProjectPayloadAtomically(path, bytes);
      });
}

}  // namespace solidar::project::detail
