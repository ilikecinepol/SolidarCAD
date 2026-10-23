#include "project/ProductEnvelopeV5Conversion.h"

#include <QByteArray>

#include <cstdint>
#include <exception>
#include <string>
#include <utility>

#include "project/ProjectFile.h"

namespace solidar::project {
namespace {

std::string exactUtf8(const QString& value) {
  const QByteArray bytes = value.toUtf8();
  return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

ProductEnvelopeV5ConversionResult failure(
    ProductEnvelopeV5ConversionStatus status, QString error) {
  ProductEnvelopeV5ConversionResult result;
  result.status = status;
  result.error = std::move(error);
  return result;
}

ProductEnvelopeV5ConversionStatus conversionStatus(
    ProductEnvelopeV5EncodeStatus status) {
  switch (status) {
    case ProductEnvelopeV5EncodeStatus::PayloadTooLarge:
      return ProductEnvelopeV5ConversionStatus::PayloadTooLarge;
    case ProductEnvelopeV5EncodeStatus::SerializedPayloadValidation:
      return ProductEnvelopeV5ConversionStatus::SerializedPayloadValidation;
    case ProductEnvelopeV5EncodeStatus::Valid:
      return ProductEnvelopeV5ConversionStatus::SerializedPayloadValidation;
    case ProductEnvelopeV5EncodeStatus::Invalid:
      return ProductEnvelopeV5ConversionStatus::Invalid;
  }
  return ProductEnvelopeV5ConversionStatus::Invalid;
}

}  // namespace

ProductEnvelopeV5ConversionResult
prepareStandaloneProductEnvelopeV5Conversion(
    const ProductDocument& source, ProjectId outputProjectId,
    DefinitionId outputRootDefinitionId,
    const ProductEnvelopeV5Metadata& metadata) {
  try {
    if (source.identity()) {
      return failure(ProductEnvelopeV5ConversionStatus::Invalid,
                     QStringLiteral(
                         "source is already an identity-bearing product"));
    }
    if (source.definitions().size() != 1) {
      return failure(ProductEnvelopeV5ConversionStatus::Invalid,
                     QStringLiteral(
                         "standalone conversion requires one definition"));
    }
    if (!outputProjectId.isValid()) {
      return failure(ProductEnvelopeV5ConversionStatus::Invalid,
                     QStringLiteral("output project ID is invalid"));
    }
    if (!outputRootDefinitionId.isValid() ||
        outputRootDefinitionId.value() >
            static_cast<std::uint64_t>(ProjectFile::kMaximumPersistedId)) {
      return failure(
          ProductEnvelopeV5ConversionStatus::Invalid,
          QStringLiteral(
              "output root definition ID is outside its persisted range"));
    }

    auto encoded = encodeProductEnvelopeV5Root(
        source, outputProjectId, outputRootDefinitionId, metadata);
    if (!encoded.succeeded()) {
      return failure(conversionStatus(encoded.status), std::move(encoded.error));
    }

    ProductEnvelopeV5ConversionResult result;
    result.status = ProductEnvelopeV5ConversionStatus::Prepared;
    result.prepared.emplace(PreparedProductEnvelopeV5Conversion{
        {outputProjectId,
         {exactUtf8(metadata.name), exactUtf8(metadata.createdAt)}},
        outputRootDefinitionId, std::move(*encoded.payload)});
    return result;
  } catch (const std::exception& exception) {
    return failure(ProductEnvelopeV5ConversionStatus::Invalid,
                   QStringLiteral("failure while preparing v5 conversion: %1")
                       .arg(QString::fromUtf8(exception.what())));
  } catch (...) {
    return failure(
        ProductEnvelopeV5ConversionStatus::Invalid,
        QStringLiteral("unknown failure while preparing v5 conversion"));
  }
}

}  // namespace solidar::project
