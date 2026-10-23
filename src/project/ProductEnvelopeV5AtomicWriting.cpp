#include "project/internal/ProductEnvelopeV5AtomicWriting.h"

#include <utility>

#include "project/ProductEnvelopeV5Encoder.h"

namespace solidar::project::detail {
namespace {

ProjectAtomicWriteResult preparationFailure(
    const ProductEnvelopeV5EncodeResult& encoded) {
  ProjectAtomicWriteFailure failure = ProjectAtomicWriteFailure::Encode;
  if (encoded.status == ProductEnvelopeV5EncodeStatus::PayloadTooLarge)
    failure = ProjectAtomicWriteFailure::PayloadTooLarge;
  else if (encoded.status ==
           ProductEnvelopeV5EncodeStatus::SerializedPayloadValidation)
    failure = ProjectAtomicWriteFailure::SerializedPayloadValidation;
  return {failure, encoded.error};
}

}  // namespace

ProjectAtomicWriteResult writeProductEnvelopeV5Atomically(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata,
    ProjectAtomicWriteTransaction& transaction) {
  auto encoded = encodeProductEnvelopeV5Root(source, outputProjectId, metadata);
  if (!encoded.succeeded()) return preparationFailure(encoded);
  return publishProjectPayloadAtomically(
      QByteArrayView(encoded.payload->bytes), transaction);
}

ProjectAtomicWriteResult writeProductEnvelopeV5Atomically(
    const QString& path, const ProductDocument& source,
    ProjectId outputProjectId, const ProductEnvelopeV5Metadata& metadata) {
  auto encoded = encodeProductEnvelopeV5Root(source, outputProjectId, metadata);
  if (!encoded.succeeded()) return preparationFailure(encoded);
  return publishProjectPayloadAtomically(
      path, QByteArrayView(encoded.payload->bytes));
}

}  // namespace solidar::project::detail
