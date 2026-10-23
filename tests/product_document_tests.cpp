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

  return EXIT_SUCCESS;
}
