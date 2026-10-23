#include "model/ProductDocument.h"

#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <cstdint>
#include <cstdlib>
#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "model/ImportedShapeFeature.h"

namespace {

constexpr solidar::DefinitionId kRootId{1};
constexpr solidar::DefinitionId kSecondId{2};
constexpr solidar::DefinitionId kPreparedRootId{4242};
constexpr solidar::DefinitionId kMissingId{999};

constexpr solidar::ProjectId projectId(std::uint8_t lastByte) {
  auto bytes = solidar::ProjectId::Bytes{};
  bytes.back() = lastByte;
  return solidar::ProjectId{bytes};
}

solidar::ProductDocumentIdentity productIdentity(
    solidar::ProjectId id, std::string name = "product",
    std::string createdAt = "2026-10-23T00:00:00Z") {
  return {id, {std::move(name), std::move(createdAt)}};
}

solidar::PartDefinition makePart(solidar::DefinitionId id) {
  return solidar::PartDefinition(id, std::make_unique<solidar::Document>());
}

std::unique_ptr<solidar::ProductDocument> makeShapedProduct(
    solidar::DefinitionId rootId,
    std::optional<solidar::ProductDocumentIdentity> identity,
    const char* label) {
  auto document = std::make_unique<solidar::Document>();
  auto& body = document->addBody(std::string(label) + " body");
  body.addFeature(std::make_unique<solidar::ImportedShapeFeature>(
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(20.0, 10.0, 5.0).Shape()),
      std::string(label) + " feature"));
  CHECK(document->recompute());
  solidar::DefinitionRegistry definitions;
  CHECK(definitions.insert(
            solidar::PartDefinition(rootId, std::move(document))) ==
        solidar::DefinitionInsertStatus::Inserted);
  auto created = solidar::ProductDocument::create(
      std::move(definitions), rootId, std::move(identity));
  CHECK(created);
  return std::move(created.product);
}

struct CarrierSnapshot {
  solidar::DefinitionId rootId;
  std::optional<solidar::ProductDocumentIdentity> identity;
  std::size_t definitionCount{};
  const solidar::Document* rootDocument{};
  const solidar::Body* firstBody{};
  const solidar::Feature* firstFeature{};
  const TopoDS_Shape* firstShape{};
  solidar::ShapeRevision firstShapeRevision{solidar::kInvalidShapeRevision};
};

CarrierSnapshot snapshot(const solidar::ProductDocument& product) {
  CarrierSnapshot result{product.rootDefinitionId(), product.identity(),
                         product.definitions().size()};
  const auto* root = product.definitions().find(product.rootDefinitionId());
  if (!root) return result;
  result.rootDocument = &root->document();
  if (root->document().bodies().empty()) return result;
  result.firstBody = &root->document().bodies().front();
  if (result.firstBody->features().empty()) return result;
  result.firstFeature = result.firstBody->features().front().get();
  const auto* shapeFeature =
      dynamic_cast<const solidar::ShapeFeature*>(result.firstFeature);
  if (shapeFeature) {
    result.firstShape = shapeFeature->lastValidShape().get();
    result.firstShapeRevision = shapeFeature->shapeRevision();
  }
  return result;
}

bool matchesSnapshot(const solidar::ProductDocument& product,
                     const CarrierSnapshot& expected) {
  const auto actual = snapshot(product);
  return actual.rootId == expected.rootId &&
         actual.identity == expected.identity &&
         actual.definitionCount == expected.definitionCount &&
         actual.rootDocument == expected.rootDocument &&
         actual.firstBody == expected.firstBody &&
         actual.firstFeature == expected.firstFeature &&
         actual.firstShape == expected.firstShape &&
         actual.firstShapeRevision == expected.firstShapeRevision;
}

template <typename T>
concept HasLvalueRootRelease = requires(T& value) {
  value.releaseRootDocument();
};

template <typename T>
concept HasRvalueRootRelease = requires(T&& value) {
  std::move(value).releaseRootDocument();
};

}  // namespace

int main() {
  static_assert(!std::is_same_v<solidar::DefinitionId, std::uint64_t>);
  static_assert(!std::is_same_v<solidar::ProjectId,
                                solidar::ProjectId::Bytes>);
  static_assert(!std::is_convertible_v<solidar::ProjectId::Bytes,
                                       solidar::ProjectId>);
  static_assert(!std::is_convertible_v<std::uint64_t,
                                       solidar::DefinitionId>);
  static_assert(!std::is_convertible_v<solidar::DefinitionId,
                                       std::uint64_t>);
  static_assert(!std::is_copy_constructible_v<solidar::PartDefinition>);
  static_assert(!std::is_copy_constructible_v<solidar::DefinitionRegistry>);
  static_assert(!std::is_copy_constructible_v<solidar::ProductDocument>);
  static_assert(std::is_same_v<
                decltype(std::declval<solidar::ProductDocument&>()
                             .rootDocument()),
                solidar::Document&>);
  static_assert(std::is_same_v<
                decltype(std::declval<const solidar::ProductDocument&>()
                             .rootDocument()),
                const solidar::Document&>);
  static_assert(std::is_same_v<
                decltype(std::declval<solidar::ProductDocument&>()
                             .definitions()),
                const solidar::DefinitionRegistry&>);
  static_assert(std::is_same_v<
                decltype(std::declval<solidar::ProductDocument&>()
                             .rootDefinition()),
                const solidar::PartDefinition&>);
  static_assert(!HasLvalueRootRelease<solidar::ProductDocument>);
  static_assert(HasRvalueRootRelease<solidar::ProductDocument>);
  static_assert(noexcept(std::declval<solidar::ProductDocument&&>()
                             .releaseRootDocument()));
  static_assert(noexcept(std::declval<solidar::ProductDocument&>()
                             .commitProductIdentity(std::declval<
                                 solidar::ProductDocumentIdentity>())));
  static_assert(std::is_same_v<
                decltype(std::declval<solidar::ProductDocument&>()
                             .commitPreparedStandaloneConversion(
                                 std::declval<solidar::ProductDocument&&>())),
                solidar::ProductDocumentStandaloneConversionStatus>);
  static_assert(noexcept(
      std::declval<solidar::ProductDocument&>()
          .commitPreparedStandaloneConversion(
              std::declval<solidar::ProductDocument&&>())));
  static_assert(std::is_same_v<
                decltype(std::declval<solidar::ProductDocument&>()
                             .commitForkIdentity(
                                 std::declval<const solidar::
                                                  ProductDocumentIdentity&>(),
                                 std::declval<solidar::
                                                  ProductDocumentIdentity>())),
                solidar::ProductDocumentForkIdentityStatus>);
  static_assert(noexcept(
      std::declval<solidar::ProductDocument&>().commitForkIdentity(
          std::declval<const solidar::ProductDocumentIdentity&>(),
          std::declval<solidar::ProductDocumentIdentity>())));
  static_assert(std::is_nothrow_move_constructible_v<
                solidar::ProductDocumentIdentity>);
  static_assert(std::is_same_v<
                decltype(std::declval<const solidar::ProductDocument&>()
                             .identity()),
                const std::optional<solidar::ProductDocumentIdentity>&>);
  static_assert(std::is_same_v<
                decltype(std::declval<const solidar::ProductDocument&>()
                             .projectId()),
                std::optional<solidar::ProjectId>>);

  CHECK(!solidar::ProjectId{}.isValid());
  CHECK(solidar::kInvalidProjectId == solidar::ProjectId{});
  CHECK(projectId(1).isValid());
  CHECK(projectId(1) != projectId(2));
  CHECK(projectId(1) < projectId(2));

  CHECK(!solidar::DefinitionId{}.isValid());
  CHECK(solidar::kInvalidDefinitionId == solidar::DefinitionId{});
  CHECK(kRootId.isValid());
  CHECK(kRootId.value() == 1);
  CHECK(kRootId != kSecondId);
  CHECK(kRootId < kSecondId);

  {
    auto retainedDocument = std::make_unique<solidar::Document>();
    auto* const retainedAddress = retainedDocument.get();
    bool rejected = false;
    try {
      solidar::PartDefinition invalid(solidar::kInvalidDefinitionId,
                                      std::move(retainedDocument));
      (void)invalid;
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    CHECK(rejected);
    CHECK(retainedDocument);
    CHECK(retainedDocument.get() == retainedAddress);

    rejected = false;
    std::unique_ptr<solidar::Document> nullDocument;
    try {
      solidar::PartDefinition invalid(kRootId, std::move(nullDocument));
      (void)invalid;
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    CHECK(rejected);
    CHECK(!nullDocument);
  }

  // Registry failures are atomic. In particular, a duplicate insert checks
  // the ID before moving from the caller's named candidate.
  {
    solidar::DefinitionRegistry registry;
    auto first = makePart(kRootId);
    auto* const firstDocument = &first.document();
    CHECK(registry.insert(std::move(first)) ==
          solidar::DefinitionInsertStatus::Inserted);
    CHECK(registry.size() == 1);
    CHECK(registry.find(kRootId));
    CHECK(&registry.find(kRootId)->document() == firstDocument);

    auto duplicate = makePart(kRootId);
    auto* const duplicateDocument = &duplicate.document();
    CHECK(registry.insert(std::move(duplicate)) ==
          solidar::DefinitionInsertStatus::DuplicateDefinitionId);
    CHECK(&duplicate.document() == duplicateDocument);
    CHECK(registry.size() == 1);
    CHECK(&registry.find(kRootId)->document() == firstDocument);
    CHECK(registry.find(kMissingId) == nullptr);
    CHECK(!registry.contains(kMissingId));

    CHECK(registry.erase(kMissingId) ==
          solidar::DefinitionEraseStatus::DefinitionMissing);
    CHECK(registry.size() == 1);
    CHECK(registry.erase(kRootId) ==
          solidar::DefinitionEraseStatus::Erased);
    CHECK(registry.size() == 0);
  }

  // A rejected factory call validates the root before moving the registry,
  // leaving the caller's ownership graph intact.
  {
    solidar::DefinitionRegistry registry;
    auto root = makePart(kRootId);
    auto* const document = &root.document();
    CHECK(registry.insert(std::move(root)) ==
          solidar::DefinitionInsertStatus::Inserted);

    auto invalid = solidar::ProductDocument::create(
        std::move(registry), solidar::kInvalidDefinitionId);
    CHECK(invalid.status == solidar::ProductDocumentStatus::InvalidRootId);
    CHECK(!invalid.product);
    CHECK(registry.size() == 1);
    CHECK(&registry.find(kRootId)->document() == document);

    auto missing =
        solidar::ProductDocument::create(std::move(registry), kMissingId);
    CHECK(missing.status ==
          solidar::ProductDocumentStatus::MissingRootDefinition);
    CHECK(!missing.product);
    CHECK(registry.size() == 1);
    CHECK(&registry.find(kRootId)->document() == document);
  }

  // The optional identity bundle is authoritative in ProductDocument. An
  // invalid ID rejects the whole bundle before moving the registry.
  {
    solidar::DefinitionRegistry registry;
    auto root = makePart(kRootId);
    auto* const document = &root.document();
    CHECK(registry.insert(std::move(root)) ==
          solidar::DefinitionInsertStatus::Inserted);
    auto invalidIdentity = solidar::ProductDocument::create(
        std::move(registry), kRootId,
        productIdentity(solidar::ProjectId{}, "must stay outside",
                        "invalid"));
    CHECK(invalidIdentity.status ==
          solidar::ProductDocumentStatus::InvalidProjectId);
    CHECK(!invalidIdentity.product);
    CHECK(registry.size() == 1);
    CHECK(&registry.find(kRootId)->document() == document);

    auto standalone =
        solidar::ProductDocument::create(std::move(registry), kRootId);
    CHECK(standalone);
    CHECK(!standalone.product->identity().has_value());
    CHECK(!standalone.product->projectId().has_value());
    CHECK(!standalone.product->commitProductIdentity(
        productIdentity(solidar::ProjectId{}, "invalid", "invalid")));
    CHECK(!standalone.product->identity().has_value());
    CHECK(!standalone.product->projectId().has_value());

    const std::string unicodeName =
        "\xD0\x98\xD0\xB7\xD0\xB4\xD0\xB5\xD0\xBB\xD0\xB8\xD0\xB5"
        " \xCE\xA9";
    const std::string createdAt = "2026-10-23T12:34:56Z";
    CHECK(standalone.product->commitProductIdentity(
        productIdentity(projectId(1), unicodeName, createdAt)));
    CHECK(standalone.product->identity().has_value());
    CHECK(standalone.product->projectId() == projectId(1));
    CHECK(standalone.product->identity()->metadata.nameUtf8 == unicodeName);
    CHECK(standalone.product->identity()->metadata.createdAtUtf8 == createdAt);

    const auto committedIdentity = *standalone.product->identity();
    CHECK(!standalone.product->commitProductIdentity(
        productIdentity(solidar::ProjectId{}, "invalid overwrite",
                        "invalid overwrite")));
    CHECK(*standalone.product->identity() == committedIdentity);
    CHECK(standalone.product->projectId() == projectId(1));
    CHECK(!standalone.product->commitProductIdentity(
        productIdentity(projectId(2), "valid overwrite",
                        "2027-01-01T00:00:00Z")));
    CHECK(*standalone.product->identity() == committedIdentity);
    CHECK(standalone.product->projectId() == projectId(1));
  }

  // Factory-created product identity and metadata move as one bundle.
  {
    solidar::DefinitionRegistry registry;
    auto root = makePart(kRootId);
    CHECK(registry.insert(std::move(root)) ==
          solidar::DefinitionInsertStatus::Inserted);
    const auto expectedIdentity =
        productIdentity(projectId(7), "assembly", "2026-10-23T01:02:03Z");
    auto identified = solidar::ProductDocument::create(
        std::move(registry), kRootId, expectedIdentity);
    CHECK(identified);
    CHECK(identified.product->identity() == expectedIdentity);
    CHECK(identified.product->projectId() == projectId(7));

    auto moved = std::move(*identified.product);
    CHECK(moved.identity() == expectedIdentity);
    CHECK(moved.projectId() == projectId(7));
  }

  // The successful ownership path moves the same Document allocation through
  // PartDefinition, registry and product root without copying its B-Rep owner.
  {
    auto document = std::make_unique<solidar::Document>();
    auto* const documentAddress = document.get();
    auto& body = document->addBody("root body");
    auto* const bodyAddress = &body;
    body.addFeature(std::make_unique<solidar::ImportedShapeFeature>(
        std::make_shared<const TopoDS_Shape>(
            BRepPrimAPI_MakeBox(20.0, 10.0, 5.0).Shape()),
        "root feature"));
    CHECK(document->recompute());
    auto* const featureAddress = body.features().front().get();
    const auto* const shapeFeature =
        dynamic_cast<const solidar::ShapeFeature*>(featureAddress);
    CHECK(shapeFeature);
    const auto shapeAddress = shapeFeature->lastValidShape().get();
    const auto shapeRevision = shapeFeature->shapeRevision();

    solidar::PartDefinition root(kRootId, std::move(document));
    CHECK(&root.document() == documentAddress);
    solidar::DefinitionRegistry registry;
    CHECK(registry.insert(std::move(root)) ==
          solidar::DefinitionInsertStatus::Inserted);
    CHECK(&registry.find(kRootId)->document() == documentAddress);
    CHECK(registry.find(kRootId)->document().findBody(body.id()) ==
          bodyAddress);

    auto created =
        solidar::ProductDocument::create(std::move(registry), kRootId);
    CHECK(created.status == solidar::ProductDocumentStatus::Valid);
    CHECK(created.product);
    CHECK(created.product->rootDefinitionId() == kRootId);
    CHECK(created.product->definitions().size() == 1);
    CHECK(&created.product->rootDefinition().document() == documentAddress);
    CHECK(&created.product->rootDocument() == documentAddress);
    CHECK(&std::as_const(*created.product).rootDocument() == documentAddress);
    CHECK(created.product->rootDefinition().document().findBody(body.id()) ==
          bodyAddress);

    auto second = makePart(kSecondId);
    CHECK(created.product->insert(std::move(second)) ==
          solidar::DefinitionInsertStatus::Inserted);
    CHECK(created.product->definitions().size() == 2);
    CHECK(created.product->remove(kMissingId) ==
          solidar::ProductDocumentRemoveStatus::Missing);
    CHECK(created.product->definitions().size() == 2);
    CHECK(created.product->remove(kRootId) ==
          solidar::ProductDocumentRemoveStatus::RootProtected);
    CHECK(created.product->definitions().size() == 2);
    CHECK(created.product->remove(kSecondId) ==
          solidar::ProductDocumentRemoveStatus::Removed);
    CHECK(created.product->definitions().size() == 1);
    CHECK(&created.product->rootDefinition().document() == documentAddress);

    auto moved = std::move(created.product);
    CHECK(moved);
    CHECK(&moved->rootDefinition().document() == documentAddress);

    auto released = std::move(*moved).releaseRootDocument();
    CHECK(released);
    CHECK(released.get() == documentAddress);
    CHECK(&released->bodies().front() == bodyAddress);
    CHECK(released->bodies().front().features().front().get() ==
          featureAddress);
    const auto* const releasedShapeFeature =
        dynamic_cast<const solidar::ShapeFeature*>(featureAddress);
    CHECK(releasedShapeFeature);
    CHECK(releasedShapeFeature->lastValidShape().get() == shapeAddress);
    CHECK(releasedShapeFeature->shapeRevision() == shapeRevision);
  }

  // A prepared standalone conversion adopts the persistent carrier while the
  // live source Document graph remains at exactly the same addresses. The
  // reconstructed candidate graph is destroyed during the atomic commit.
  {
    const std::string unicodeName =
        "\xD0\x98\xD0\xB7\xD0\xB4\xD0\xB5\xD0\xBB\xD0\xB8\xD0\xB5"
        " \xCE\xA9/\xE9\x9B\xAA";
    const std::string unicodeCreatedAt =
        "2026-10-23T20:15:00+03:00/"
        "\xD1\x82\xD0\xBE\xD1\x87\xD0\xBD\xD0\xBE";
    const auto expectedIdentity =
        productIdentity(projectId(31), unicodeName, unicodeCreatedAt);
    auto source = makeShapedProduct(kRootId, std::nullopt, "live");
    auto prepared =
        makeShapedProduct(kPreparedRootId, expectedIdentity, "candidate");
    const auto liveBefore = snapshot(*source);
    const auto candidateBefore = snapshot(*prepared);
    std::weak_ptr<const TopoDS_Shape> candidateShape;
    {
      const auto* feature = dynamic_cast<const solidar::ShapeFeature*>(
          prepared->rootDocument().bodies().front().features().front().get());
      CHECK(feature);
      candidateShape = feature->lastValidShape();
    }
    CHECK(!candidateShape.expired());

    CHECK(source->commitPreparedStandaloneConversion(std::move(*prepared)) ==
          solidar::ProductDocumentStandaloneConversionStatus::Committed);
    CHECK(source->rootDefinitionId() == kPreparedRootId);
    CHECK(source->identity() == expectedIdentity);
    CHECK(source->identity()->metadata.nameUtf8 == unicodeName);
    CHECK(source->identity()->metadata.createdAtUtf8 == unicodeCreatedAt);
    CHECK(source->definitions().size() == 1);
    CHECK(&source->rootDocument() == liveBefore.rootDocument);
    CHECK(&source->rootDocument().bodies().front() == liveBefore.firstBody);
    CHECK(source->rootDocument().bodies().front().features().front().get() ==
          liveBefore.firstFeature);
    const auto* committedFeature =
        dynamic_cast<const solidar::ShapeFeature*>(liveBefore.firstFeature);
    CHECK(committedFeature);
    CHECK(committedFeature->lastValidShape().get() == liveBefore.firstShape);
    CHECK(candidateBefore.rootDocument != liveBefore.rootDocument);
    CHECK(candidateShape.expired());

    // Conversion is one-shot. A repeat leaves the active and newly prepared
    // carrier graphs byte/value/address-equivalent to their snapshots.
    auto repeatPrepared = makeShapedProduct(
        solidar::DefinitionId{5000},
        productIdentity(projectId(32), "repeat", "repeat"), "repeat");
    const auto committedSnapshot = snapshot(*source);
    const auto repeatSnapshot = snapshot(*repeatPrepared);
    CHECK(source->commitPreparedStandaloneConversion(
              std::move(*repeatPrepared)) ==
          solidar::ProductDocumentStandaloneConversionStatus::
              SourceAlreadyIdentified);
    CHECK(matchesSnapshot(*source, committedSnapshot));
    CHECK(matchesSnapshot(*repeatPrepared, repeatSnapshot));
  }

  // Equal transient and persistent root IDs are valid: identity changes, but
  // the live Document allocation is still retained and the reconstruction is
  // still destroyed.
  {
    auto source = makeShapedProduct(kRootId, std::nullopt, "same-live");
    const auto expectedIdentity =
        productIdentity(projectId(40), "same root", "same root time");
    auto prepared = makeShapedProduct(kRootId, expectedIdentity,
                                      "same-candidate");
    const auto liveBefore = snapshot(*source);
    std::weak_ptr<const TopoDS_Shape> candidateShape;
    const auto* candidateFeature =
        dynamic_cast<const solidar::ShapeFeature*>(
            prepared->rootDocument().bodies().front().features().front().get());
    CHECK(candidateFeature);
    candidateShape = candidateFeature->lastValidShape();
    CHECK(source->commitPreparedStandaloneConversion(std::move(*prepared)) ==
          solidar::ProductDocumentStandaloneConversionStatus::Committed);
    CHECK(source->rootDefinitionId() == kRootId);
    CHECK(source->identity() == expectedIdentity);
    CHECK(&source->rootDocument() == liveBefore.rootDocument);
    CHECK(candidateShape.expired());
  }

  // Every invalid transition is a strict no-op for both operands.
  {
    auto self = makeShapedProduct(kRootId, std::nullopt, "self");
    const auto selfBefore = snapshot(*self);
    CHECK(self->commitPreparedStandaloneConversion(std::move(*self)) ==
          solidar::ProductDocumentStandaloneConversionStatus::SelfTransition);
    CHECK(matchesSnapshot(*self, selfBefore));

    auto identifiedSource = makeShapedProduct(
        kRootId, productIdentity(projectId(50)), "identified-source");
    auto prepared = makeShapedProduct(
        kPreparedRootId, productIdentity(projectId(51)), "prepared-a");
    const auto identifiedBefore = snapshot(*identifiedSource);
    const auto preparedBefore = snapshot(*prepared);
    CHECK(identifiedSource->commitPreparedStandaloneConversion(
              std::move(*prepared)) ==
          solidar::ProductDocumentStandaloneConversionStatus::
              SourceAlreadyIdentified);
    CHECK(matchesSnapshot(*identifiedSource, identifiedBefore));
    CHECK(matchesSnapshot(*prepared, preparedBefore));

    auto multiSource = makeShapedProduct(kRootId, std::nullopt, "multi-source");
    CHECK(multiSource->insert(makePart(kSecondId)) ==
          solidar::DefinitionInsertStatus::Inserted);
    auto preparedForMultiSource = makeShapedProduct(
        kPreparedRootId, productIdentity(projectId(52)), "prepared-b");
    const auto multiSourceBefore = snapshot(*multiSource);
    const auto preparedForMultiSourceBefore =
        snapshot(*preparedForMultiSource);
    CHECK(multiSource->commitPreparedStandaloneConversion(
              std::move(*preparedForMultiSource)) ==
          solidar::ProductDocumentStandaloneConversionStatus::
              SourceNotSingleRoot);
    CHECK(matchesSnapshot(*multiSource, multiSourceBefore));
    CHECK(matchesSnapshot(*preparedForMultiSource,
                          preparedForMultiSourceBefore));

    auto sourceForIdentityless =
        makeShapedProduct(kRootId, std::nullopt, "identityless-source");
    auto identitylessPrepared = makeShapedProduct(
        kPreparedRootId, std::nullopt, "identityless-prepared");
    const auto sourceForIdentitylessBefore = snapshot(*sourceForIdentityless);
    const auto identitylessPreparedBefore = snapshot(*identitylessPrepared);
    CHECK(sourceForIdentityless->commitPreparedStandaloneConversion(
              std::move(*identitylessPrepared)) ==
          solidar::ProductDocumentStandaloneConversionStatus::
              PreparedMissingIdentity);
    CHECK(matchesSnapshot(*sourceForIdentityless,
                          sourceForIdentitylessBefore));
    CHECK(matchesSnapshot(*identitylessPrepared, identitylessPreparedBefore));

    auto sourceForMultiPrepared =
        makeShapedProduct(kRootId, std::nullopt, "multi-prepared-source");
    auto multiPrepared = makeShapedProduct(
        kPreparedRootId, productIdentity(projectId(53)), "multi-prepared");
    CHECK(multiPrepared->insert(makePart(kSecondId)) ==
          solidar::DefinitionInsertStatus::Inserted);
    const auto sourceForMultiPreparedBefore = snapshot(*sourceForMultiPrepared);
    const auto multiPreparedBefore = snapshot(*multiPrepared);
    CHECK(sourceForMultiPrepared->commitPreparedStandaloneConversion(
              std::move(*multiPrepared)) ==
          solidar::ProductDocumentStandaloneConversionStatus::
              PreparedNotSingleRoot);
    CHECK(matchesSnapshot(*sourceForMultiPrepared,
                          sourceForMultiPreparedBefore));
    CHECK(matchesSnapshot(*multiPrepared, multiPreparedBefore));

    auto sourceForMovedPrepared =
        makeShapedProduct(kRootId, std::nullopt, "moved-prepared-source");
    auto movedPrepared = makeShapedProduct(
        kPreparedRootId, productIdentity(projectId(54)), "moved-prepared");
    solidar::ProductDocument movedDestination(std::move(*movedPrepared));
    const auto sourceForMovedPreparedBefore = snapshot(*sourceForMovedPrepared);
    const auto movedPreparedBefore = snapshot(*movedPrepared);
    CHECK(sourceForMovedPrepared->commitPreparedStandaloneConversion(
              std::move(*movedPrepared)) ==
          solidar::ProductDocumentStandaloneConversionStatus::
              PreparedNotSingleRoot);
    CHECK(matchesSnapshot(*sourceForMovedPrepared,
                          sourceForMovedPreparedBefore));
    CHECK(matchesSnapshot(*movedPrepared, movedPreparedBefore));
    CHECK(movedDestination.identity() == productIdentity(projectId(54)));
  }

  // Save-As cloning is an explicit identified-to-identified fork. It swaps
  // only the complete identity bundle and uses the expected old bundle as a
  // strong stale-source guard.
  {
    const std::string nameA =
        "\xD0\x98\xD1\x81\xD1\x85\xD0\xBE\xD0\xB4\xD0\xBD\xD0\xBE\xD0\xB5"
        " \xCE\xA9/\xE9\x9B\xAA";
    const std::string createdA =
        "2026-10-23T22:00:00+03:00/"
        "\xD1\x82\xD0\xBE\xD1\x87\xD0\xBD\xD0\xBE";
    const auto identityA = productIdentity(projectId(61), nameA, createdA);
    const auto identityB = productIdentity(
        projectId(62),
        "\xD0\x9A\xD0\xBE\xD0\xBF\xD0\xB8\xD1\x8F \xCE\xB2/\xE6\x9D\xB1",
        "2026-10-24T00:01:02Z/\xD0\xBD\xD0\xBE\xD0\xB2\xD1\x8B\xD0\xB9");
    const auto identityC = productIdentity(
        projectId(63), "fork C", "2026-10-25T00:00:00Z");
    auto source = makeShapedProduct(kRootId, identityA, "fork-live");
    const auto graphBefore = snapshot(*source);

    CHECK(source->commitForkIdentity(identityA, identityB) ==
          solidar::ProductDocumentForkIdentityStatus::Committed);
    CHECK(source->identity() == identityB);
    CHECK(source->identity()->metadata.nameUtf8 == identityB.metadata.nameUtf8);
    CHECK(source->identity()->metadata.createdAtUtf8 ==
          identityB.metadata.createdAtUtf8);
    auto afterB = graphBefore;
    afterB.identity = identityB;
    CHECK(matchesSnapshot(*source, afterB));

    const auto beforeStale = snapshot(*source);
    CHECK(source->commitForkIdentity(identityA, identityC) ==
          solidar::ProductDocumentForkIdentityStatus::StaleExpectedIdentity);
    CHECK(matchesSnapshot(*source, beforeStale));
    CHECK(source->commitForkIdentity(
              identityB,
              productIdentity(projectId(62), "same id different metadata",
                              "same id")) ==
          solidar::ProductDocumentForkIdentityStatus::SameProjectId);
    CHECK(matchesSnapshot(*source, beforeStale));
    CHECK(source->commitForkIdentity(
              identityB,
              productIdentity(solidar::ProjectId{}, "nil", "nil")) ==
          solidar::ProductDocumentForkIdentityStatus::
              InvalidReplacementProjectId);
    CHECK(matchesSnapshot(*source, beforeStale));

    CHECK(source->commitForkIdentity(identityB, identityC) ==
          solidar::ProductDocumentForkIdentityStatus::Committed);
    auto afterC = graphBefore;
    afterC.identity = identityC;
    CHECK(matchesSnapshot(*source, afterC));
    CHECK(source->commitForkIdentity(identityB, identityA) ==
          solidar::ProductDocumentForkIdentityStatus::StaleExpectedIdentity);
    CHECK(matchesSnapshot(*source, afterC));

    auto identityless =
        makeShapedProduct(kRootId, std::nullopt, "identityless-fork");
    const auto identitylessBefore = snapshot(*identityless);
    CHECK(identityless->commitForkIdentity(identityA, identityB) ==
          solidar::ProductDocumentForkIdentityStatus::SourceMissingIdentity);
    CHECK(matchesSnapshot(*identityless, identitylessBefore));
  }

  return EXIT_SUCCESS;
}
