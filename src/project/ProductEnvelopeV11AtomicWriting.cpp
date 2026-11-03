#include "project/internal/ProductEnvelopeV11AtomicWriting.h"

#include <QByteArrayView>

#include <utility>

#include "project/ProductEnvelopeV11Decoder.h"
#include "project/ProductEnvelopeV11Encoder.h"
#include "project/ProductEnvelopeV11Materialization.h"

namespace solidar::project::detail {
namespace {

using Point = ProductEnvelopeV11AtomicWritePreparationFailurePoint;

ProjectAtomicWriteResult preparationFailure(
    const ProductEnvelopeV11EncodeResult& encoded) {
  auto failure = ProjectAtomicWriteFailure::Encode;
  if (encoded.status == ProductEnvelopeV11EncodeStatus::PayloadTooLarge)
    failure = ProjectAtomicWriteFailure::PayloadTooLarge;
  else if (encoded.status ==
           ProductEnvelopeV11EncodeStatus::SerializedPayloadValidation)
    failure = ProjectAtomicWriteFailure::SerializedPayloadValidation;
  return {failure, encoded.error};
}

ProjectAtomicWriteResult injected(Point point) {
  switch (point) {
    case Point::EncodeInitial:
    case Point::EncodeFixedPoint:
    case Point::EncodeStable:
      return {ProjectAtomicWriteFailure::Encode,
              QStringLiteral("injected V11 encode failure")};
    case Point::ScanInitial:
    case Point::ScanFixedPoint:
      return {ProjectAtomicWriteFailure::SerializedPayloadValidation,
              QStringLiteral("injected V11 scan failure")};
    case Point::MaterializeInitial:
    case Point::MaterializeFixedPoint:
      return {ProjectAtomicWriteFailure::MaterializationValidation,
              QStringLiteral("injected V11 materialization failure")};
    case Point::FixedPointMismatch:
    case Point::StableMismatch:
      return {ProjectAtomicWriteFailure::SerializedPayloadValidation,
              QStringLiteral("injected V11 fixed-point mismatch")};
    case Point::None:
      break;
  }
  return {};
}

template <class Publish>
ProjectAtomicWriteResult prepareAndPublish(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata, Point failurePoint,
    Publish&& publish) {
  if (failurePoint == Point::EncodeInitial) return injected(failurePoint);
  const auto initial =
      encodeProductEnvelopeV11Root(source, outputProjectId, metadata);
  if (!initial.succeeded()) return preparationFailure(initial);
  const QByteArray& bytes0 = initial.payload->bytes;

  if (failurePoint == Point::ScanInitial) return injected(failurePoint);
  auto scan0 = scanProductEnvelopeV11(QByteArrayView(bytes0));
  if (!scan0.hasCandidate())
    return {ProjectAtomicWriteFailure::SerializedPayloadValidation,
            std::move(scan0.error)};

  if (failurePoint == Point::MaterializeInitial) return injected(failurePoint);
  auto model0 = materializeProductEnvelopeV11(
      *scan0.candidate,
      ProductEnvelopeV11MaterializationFailurePoint::None,
      ProductEnvelopeV11MaterializationMode::ValidationOnly);
  if (!model0.succeeded())
    return {ProjectAtomicWriteFailure::MaterializationValidation,
            std::move(model0.error)};

  if (failurePoint == Point::EncodeFixedPoint) return injected(failurePoint);
  const auto fixed = encodeProductEnvelopeV11Root(
      *model0.product, outputProjectId, metadata);
  if (!fixed.succeeded()) return preparationFailure(fixed);
  if (failurePoint == Point::FixedPointMismatch ||
      fixed.payload->bytes != bytes0)
    return {ProjectAtomicWriteFailure::SerializedPayloadValidation,
            QStringLiteral("V11 authored payload is not a fixed point")};

  if (failurePoint == Point::ScanFixedPoint) return injected(failurePoint);
  auto scan1 = scanProductEnvelopeV11(QByteArrayView(fixed.payload->bytes));
  if (!scan1.hasCandidate())
    return {ProjectAtomicWriteFailure::SerializedPayloadValidation,
            std::move(scan1.error)};

  if (failurePoint == Point::MaterializeFixedPoint)
    return injected(failurePoint);
  auto model1 = materializeProductEnvelopeV11(
      *scan1.candidate,
      ProductEnvelopeV11MaterializationFailurePoint::None,
      ProductEnvelopeV11MaterializationMode::ValidationOnly);
  if (!model1.succeeded())
    return {ProjectAtomicWriteFailure::MaterializationValidation,
            std::move(model1.error)};

  if (failurePoint == Point::EncodeStable) return injected(failurePoint);
  const auto stable = encodeProductEnvelopeV11Root(
      *model1.product, outputProjectId, metadata);
  if (!stable.succeeded()) return preparationFailure(stable);
  if (failurePoint == Point::StableMismatch ||
      stable.payload->bytes != fixed.payload->bytes)
    return {ProjectAtomicWriteFailure::SerializedPayloadValidation,
            QStringLiteral("V11 authored payload is not stable")};

  return std::forward<Publish>(publish)(QByteArrayView(bytes0));
}

}  // namespace

ProjectAtomicWriteResult writeProductEnvelopeV11Atomically(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata,
    ProjectAtomicWriteTransaction& transaction, Point failurePoint) {
  return prepareAndPublish(
      source, outputProjectId, metadata, failurePoint,
      [&transaction](QByteArrayView bytes) {
        return publishProjectPayloadAtomically(bytes, transaction);
      });
}

ProjectAtomicWriteResult writeProductEnvelopeV11Atomically(
    const QString& path, const ProductDocument& source,
    ProjectId outputProjectId, const ProductEnvelopeV5Metadata& metadata,
    Point failurePoint) {
  return prepareAndPublish(source, outputProjectId, metadata, failurePoint,
                           [&path](QByteArrayView bytes) {
                             return publishProjectPayloadAtomically(path,
                                                                     bytes);
                           });
}

}  // namespace solidar::project::detail
