#include "project/internal/ProductEnvelopeV7AtomicWriting.h"

#include <utility>

#include "project/ProductEnvelopeV7Encoder.h"

namespace solidar::project::detail {
namespace {

ProjectAtomicWriteResult preparationFailure(
    const ProductEnvelopeV7EncodeResult& encoded) {
  ProjectAtomicWriteFailure failure = ProjectAtomicWriteFailure::Encode;
  if (encoded.status == ProductEnvelopeV7EncodeStatus::PayloadTooLarge)
    failure = ProjectAtomicWriteFailure::PayloadTooLarge;
  else if (encoded.status ==
           ProductEnvelopeV7EncodeStatus::SerializedPayloadValidation)
    failure = ProjectAtomicWriteFailure::SerializedPayloadValidation;
  return {failure, encoded.error};
}

}  // namespace

ProjectAtomicWriteResult writeProductEnvelopeV7Atomically(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata,
    ProjectAtomicWriteTransaction& transaction) {
  auto encoded = encodeProductEnvelopeV7Root(source, outputProjectId, metadata);
  if (!encoded.succeeded()) return preparationFailure(encoded);
  return publishProjectPayloadAtomically(
      QByteArrayView(encoded.payload->bytes), transaction);
}

ProjectAtomicWriteResult writeProductEnvelopeV7Atomically(
    const QString& path, const ProductDocument& source,
    ProjectId outputProjectId, const ProductEnvelopeV5Metadata& metadata) {
  auto encoded = encodeProductEnvelopeV7Root(source, outputProjectId, metadata);
  if (!encoded.succeeded()) return preparationFailure(encoded);
  return publishProjectPayloadAtomically(
      path, QByteArrayView(encoded.payload->bytes));
}

}  // namespace solidar::project::detail
