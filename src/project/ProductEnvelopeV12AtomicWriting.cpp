#include "project/internal/ProductEnvelopeV12AtomicWriting.h"

#include <QByteArrayView>

#include <utility>

#include "project/ProductEnvelopeV12Decoder.h"
#include "project/ProductEnvelopeV12Encoder.h"
#include "project/ProductEnvelopeV12Materialization.h"

namespace solidar::project::detail {
namespace {

using Point = ProductEnvelopeV12AtomicWritePreparationFailurePoint;

ProjectAtomicWriteResult preparationFailure(
    const ProductEnvelopeV12EncodeResult& encoded) {
  auto failure = ProjectAtomicWriteFailure::Encode;
  if (encoded.status == ProductEnvelopeV12EncodeStatus::PayloadTooLarge)
    failure = ProjectAtomicWriteFailure::PayloadTooLarge;
  else if (encoded.status ==
           ProductEnvelopeV12EncodeStatus::SerializedPayloadValidation)
    failure = ProjectAtomicWriteFailure::SerializedPayloadValidation;
  return {failure, encoded.error};
}

ProjectAtomicWriteResult injected(Point point) {
  switch (point) {
    case Point::EncodeInitial:
    case Point::EncodeFixedPoint:
    case Point::EncodeStable:
      return {ProjectAtomicWriteFailure::Encode,
              QStringLiteral("injected V12 encode failure")};
    case Point::ScanInitial:
    case Point::ScanFixedPoint:
      return {ProjectAtomicWriteFailure::SerializedPayloadValidation,
              QStringLiteral("injected V12 scan failure")};
    case Point::MaterializeInitial:
    case Point::MaterializeFixedPoint:
      return {ProjectAtomicWriteFailure::MaterializationValidation,
              QStringLiteral("injected V12 materialization failure")};
    case Point::FixedPointMismatch:
    case Point::StableMismatch:
      return {ProjectAtomicWriteFailure::SerializedPayloadValidation,
              QStringLiteral("injected V12 fixed-point mismatch")};
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
      encodeProductEnvelopeV12Root(source, outputProjectId, metadata);
  if (!initial.succeeded()) return preparationFailure(initial);
  const QByteArray& bytes0 = initial.payload->bytes;

  if (failurePoint == Point::ScanInitial) return injected(failurePoint);
  auto scan0 = scanProductEnvelopeV12(QByteArrayView(bytes0));
  if (!scan0.hasCandidate())
    return {ProjectAtomicWriteFailure::SerializedPayloadValidation,
            std::move(scan0.error)};

  if (failurePoint == Point::MaterializeInitial) return injected(failurePoint);
  auto model0 = materializeProductEnvelopeV12(
      *scan0.candidate,
      ProductEnvelopeV12MaterializationFailurePoint::None,
      ProductEnvelopeV12MaterializationMode::ValidationOnly);
  if (!model0.succeeded())
    return {ProjectAtomicWriteFailure::MaterializationValidation,
            std::move(model0.error)};

  if (failurePoint == Point::EncodeFixedPoint) return injected(failurePoint);
  const auto fixed = encodeProductEnvelopeV12Root(
      *model0.product, outputProjectId, metadata);
  if (!fixed.succeeded()) return preparationFailure(fixed);
  if (failurePoint == Point::FixedPointMismatch ||
      fixed.payload->bytes != bytes0)
    return {ProjectAtomicWriteFailure::SerializedPayloadValidation,
            QStringLiteral("V12 authored payload is not a fixed point")};

  if (failurePoint == Point::ScanFixedPoint) return injected(failurePoint);
  auto scan1 = scanProductEnvelopeV12(QByteArrayView(fixed.payload->bytes));
  if (!scan1.hasCandidate())
    return {ProjectAtomicWriteFailure::SerializedPayloadValidation,
            std::move(scan1.error)};

  if (failurePoint == Point::MaterializeFixedPoint)
    return injected(failurePoint);
  auto model1 = materializeProductEnvelopeV12(
      *scan1.candidate,
      ProductEnvelopeV12MaterializationFailurePoint::None,
      ProductEnvelopeV12MaterializationMode::ValidationOnly);
  if (!model1.succeeded())
    return {ProjectAtomicWriteFailure::MaterializationValidation,
            std::move(model1.error)};

  if (failurePoint == Point::EncodeStable) return injected(failurePoint);
  const auto stable = encodeProductEnvelopeV12Root(
      *model1.product, outputProjectId, metadata);
  if (!stable.succeeded()) return preparationFailure(stable);
  if (failurePoint == Point::StableMismatch ||
      stable.payload->bytes != fixed.payload->bytes)
    return {ProjectAtomicWriteFailure::SerializedPayloadValidation,
            QStringLiteral("V12 authored payload is not stable")};

  return std::forward<Publish>(publish)(QByteArrayView(bytes0));
}

}  // namespace

ProjectAtomicWriteResult writeProductEnvelopeV12Atomically(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata,
    ProjectAtomicWriteTransaction& transaction, Point failurePoint) {
  return prepareAndPublish(
      source, outputProjectId, metadata, failurePoint,
      [&transaction](QByteArrayView bytes) {
        return publishProjectPayloadAtomically(bytes, transaction);
      });
}

ProjectAtomicWriteResult writeProductEnvelopeV12Atomically(
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
