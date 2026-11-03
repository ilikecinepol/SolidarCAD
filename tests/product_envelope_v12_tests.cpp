#include "TestAssertions.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "model/ProductDocument.h"
#include "project/ProductEnvelopeV11Encoder.h"
#include "project/ProductEnvelopeV12Decoder.h"
#include "project/ProductEnvelopeV12Encoder.h"
#include "project/ProductEnvelopeV12Materialization.h"
#include "project/internal/ProductEnvelopeV12AtomicWriting.h"

namespace {
using namespace solidar;
using namespace solidar::project;

constexpr ProjectId kProject{{0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                              0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x10}};

RigidTransform3d transform(double x, double y, double z,
                           std::array<double, 4> quaternion) {
  auto created = RigidTransform3d::create({x, y, z}, quaternion);
  CHECK(created);
  return *created.transform;
}

std::unique_ptr<ProductDocument> product() {
  const double half = std::sqrt(0.5);
  ComponentDefinition child(DefinitionId{2}, std::make_unique<Document>());
  ComponentDefinition root(DefinitionId{1}, std::make_unique<Document>());
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{7}, DefinitionId{2},
            transform(9, 8, 7, {half, 0, half, 0}),
            transform(1, 2, 3, {half, half, 0, 0}),
            OccurrenceFlags{}, "child")) ==
        ComponentOccurrenceInsertStatus::Inserted);
  std::vector<ComponentDefinition> components;
  components.push_back(std::move(root));
  components.push_back(std::move(child));
  auto created = ProductDocument::createComponentGraph(
      std::move(components), DefinitionId{1});
  CHECK(created);
  return std::move(created.product);
}

class Transaction final
    : public solidar::project::detail::ProjectAtomicWriteTransaction {
 public:
  bool open(QString*) override {
    calls << QStringLiteral("open");
    return fail != detail::ProjectAtomicWriteFailure::OpenTemporary;
  }
  qint64 write(QByteArrayView bytes, QString*) override {
    calls << QStringLiteral("write");
    staged = QByteArray(bytes.data(), bytes.size());
    return fail == detail::ProjectAtomicWriteFailure::WriteTemporary
               ? bytes.size() - 1
               : bytes.size();
  }
  bool flush(QString*) override {
    calls << QStringLiteral("flush");
    return fail != detail::ProjectAtomicWriteFailure::FlushTemporary;
  }
  bool sync(QString*) override {
    calls << QStringLiteral("sync");
    return fail != detail::ProjectAtomicWriteFailure::SyncTemporary;
  }
  bool commit(QString*) override {
    calls << QStringLiteral("commit");
    if (fail == detail::ProjectAtomicWriteFailure::ReplaceDestination)
      return false;
    destination = staged;
    return true;
  }
  void cancel() noexcept override { calls << QStringLiteral("cancel"); }

  detail::ProjectAtomicWriteFailure fail{
      detail::ProjectAtomicWriteFailure::None};
  QStringList calls;
  QByteArray staged;
  QByteArray destination{"sentinel"};
};

}  // namespace

int main() {
  const ProductEnvelopeV5Metadata metadata{
      QStringLiteral("v12"), QStringLiteral("2026-11-02T00:00:00Z")};
  auto source = product();

  const auto encoded = encodeProductEnvelopeV12Root(*source, kProject, metadata);
  CHECK(encoded.succeeded());
  CHECK(encoded.payload->root.value(QStringLiteral("version")).toInt() == 12);
  CHECK(encoded.payload->root.value(QStringLiteral("capabilities"))
            .toArray()
            .contains(QStringLiteral("component-home-placement-v1")));
  const auto scanned =
      scanProductEnvelopeV12(QByteArrayView(encoded.payload->bytes));
  CHECK(scanned.hasCandidate());
  auto materialized = materializeProductEnvelopeV12(
      *scanned.candidate, ProductEnvelopeV12MaterializationFailurePoint::None,
      ProductEnvelopeV12MaterializationMode::ValidationOnly);
  CHECK(materialized.succeeded());
  const auto* occurrence =
      materialized.product->rootComponentDefinition()->find(OccurrenceId{7});
  CHECK(occurrence);
  CHECK(occurrence->placement() != occurrence->homePlacement());
  CHECK(occurrence->homePlacement() ==
        source->rootComponentDefinition()->find(OccurrenceId{7})
            ->homePlacement());
  const auto fixed =
      encodeProductEnvelopeV12Root(*materialized.product, kProject, metadata);
  CHECK(fixed.succeeded());
  CHECK(fixed.payload->bytes == encoded.payload->bytes);

  // The old exact schema fails closed rather than discarding homePlacement.
  CHECK(!encodeProductEnvelopeV11Root(*source, kProject, metadata).succeeded());

  // Required field, exact capability set and canonical quaternion validation.
  QJsonObject malformed = encoded.payload->root;
  auto definitions = malformed.value(QStringLiteral("definitions")).toArray();
  auto definition = definitions.at(0).toObject();
  auto occurrences = definition.value(QStringLiteral("occurrences")).toArray();
  auto occurrenceObject = occurrences.at(0).toObject();
  occurrenceObject.remove(QStringLiteral("homePlacement"));
  occurrences[0] = occurrenceObject;
  definition[QStringLiteral("occurrences")] = occurrences;
  definitions[0] = definition;
  malformed[QStringLiteral("definitions")] = definitions;
  CHECK(scanProductEnvelopeV12(QByteArrayView(
            QJsonDocument(malformed).toJson(QJsonDocument::Compact))).status ==
        ProductEnvelopeV12ScanStatus::Invalid);

  malformed = encoded.payload->root;
  malformed[QStringLiteral("version")] = 13;
  CHECK(scanProductEnvelopeV12(QByteArrayView(
            QJsonDocument(malformed).toJson(QJsonDocument::Compact))).status ==
        ProductEnvelopeV12ScanStatus::Unsupported);

  malformed = encoded.payload->root;
  auto capabilities = malformed.value(QStringLiteral("capabilities")).toArray();
  capabilities.push_back(QStringLiteral("unknown-future-capability"));
  malformed[QStringLiteral("capabilities")] = capabilities;
  CHECK(scanProductEnvelopeV12(QByteArrayView(
            QJsonDocument(malformed).toJson(QJsonDocument::Compact))).status ==
        ProductEnvelopeV12ScanStatus::Invalid);

  QByteArray negativeZeroHome =
      QJsonDocument(encoded.payload->root).toJson(QJsonDocument::Compact);
  const qsizetype homeOffset =
      negativeZeroHome.indexOf(QByteArrayLiteral("\"homePlacement\""));
  CHECK(homeOffset >= 0);
  const QByteArray canonicalHomeTranslation(
      "\"translationMm\":[1,2,3]");
  const qsizetype translationOffset =
      negativeZeroHome.indexOf(canonicalHomeTranslation, homeOffset);
  CHECK(translationOffset >= 0);
  negativeZeroHome.replace(
      translationOffset, canonicalHomeTranslation.size(),
      QByteArrayLiteral("\"translationMm\":[-0,2,3]"));
  CHECK(scanProductEnvelopeV12(QByteArrayView(negativeZeroHome)).status ==
        ProductEnvelopeV12ScanStatus::Invalid);

  malformed = encoded.payload->root;
  definitions = malformed.value(QStringLiteral("definitions")).toArray();
  definition = definitions.at(0).toObject();
  occurrences = definition.value(QStringLiteral("occurrences")).toArray();
  occurrenceObject = occurrences.at(0).toObject();
  auto home = occurrenceObject.value(QStringLiteral("homePlacement")).toObject();
  home[QStringLiteral("quaternionWxyz")] = QJsonArray{2, 0, 0, 0};
  occurrenceObject[QStringLiteral("homePlacement")] = home;
  occurrences[0] = occurrenceObject;
  definition[QStringLiteral("occurrences")] = occurrences;
  definitions[0] = definition;
  malformed[QStringLiteral("definitions")] = definitions;
  CHECK(scanProductEnvelopeV12(QByteArrayView(
            QJsonDocument(malformed).toJson(QJsonDocument::Compact))).status ==
        ProductEnvelopeV12ScanStatus::Invalid);

  malformed = encoded.payload->root;
  definitions = malformed.value(QStringLiteral("definitions")).toArray();
  definition = definitions.at(0).toObject();
  occurrences = definition.value(QStringLiteral("occurrences")).toArray();
  occurrenceObject = occurrences.at(0).toObject();
  home = occurrenceObject.value(QStringLiteral("homePlacement")).toObject();
  home[QStringLiteral("translationMm")] = QJsonArray{
      QStringLiteral("not-finite"), 2, 3};
  occurrenceObject[QStringLiteral("homePlacement")] = home;
  occurrences[0] = occurrenceObject;
  definition[QStringLiteral("occurrences")] = occurrences;
  definitions[0] = definition;
  malformed[QStringLiteral("definitions")] = definitions;
  CHECK(scanProductEnvelopeV12(QByteArrayView(
            QJsonDocument(malformed).toJson(QJsonDocument::Compact))).status ==
        ProductEnvelopeV12ScanStatus::Invalid);

  Transaction transaction;
  const auto written =
      solidar::project::detail::writeProductEnvelopeV12Atomically(
          *source, kProject, metadata, transaction);
  CHECK(written.succeeded());
  CHECK(transaction.destination == encoded.payload->bytes);
  CHECK((transaction.calls ==
         QStringList{"open", "write", "flush", "sync", "commit"}));

  constexpr std::array preparationPoints{
      detail::ProductEnvelopeV12AtomicWritePreparationFailurePoint::EncodeInitial,
      detail::ProductEnvelopeV12AtomicWritePreparationFailurePoint::ScanInitial,
      detail::ProductEnvelopeV12AtomicWritePreparationFailurePoint::MaterializeInitial,
      detail::ProductEnvelopeV12AtomicWritePreparationFailurePoint::EncodeFixedPoint,
      detail::ProductEnvelopeV12AtomicWritePreparationFailurePoint::FixedPointMismatch,
      detail::ProductEnvelopeV12AtomicWritePreparationFailurePoint::ScanFixedPoint,
      detail::ProductEnvelopeV12AtomicWritePreparationFailurePoint::MaterializeFixedPoint,
      detail::ProductEnvelopeV12AtomicWritePreparationFailurePoint::EncodeStable,
      detail::ProductEnvelopeV12AtomicWritePreparationFailurePoint::StableMismatch};
  for (const auto point : preparationPoints) {
    Transaction rejectedTransaction;
    const auto rejected = detail::writeProductEnvelopeV12Atomically(
        *source, kProject, metadata, rejectedTransaction, point);
    CHECK(!rejected.succeeded());
    CHECK(rejectedTransaction.calls.isEmpty());
    CHECK(rejectedTransaction.destination == QByteArray("sentinel"));
  }
  for (const auto failure :
       {detail::ProjectAtomicWriteFailure::OpenTemporary,
        detail::ProjectAtomicWriteFailure::WriteTemporary,
        detail::ProjectAtomicWriteFailure::FlushTemporary,
        detail::ProjectAtomicWriteFailure::SyncTemporary,
        detail::ProjectAtomicWriteFailure::ReplaceDestination}) {
    Transaction rejectedTransaction;
    rejectedTransaction.fail = failure;
    const auto rejected = detail::writeProductEnvelopeV12Atomically(
        *source, kProject, metadata, rejectedTransaction);
    CHECK(rejected.failure == failure);
    CHECK(rejectedTransaction.destination == QByteArray("sentinel"));
    CHECK(rejectedTransaction.calls.contains(QStringLiteral("cancel")));
  }
  return 0;
}
