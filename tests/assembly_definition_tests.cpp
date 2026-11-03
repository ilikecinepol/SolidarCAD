#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "model/RigidTransform3d.h"

namespace {

using namespace solidar;

constexpr DefinitionId kSharedPart{1};
constexpr DefinitionId kOrphanPart{2};
constexpr DefinitionId kAssemblyRoot{100};
constexpr DefinitionId kOtherAssembly{101};
constexpr DefinitionId kMissingDefinition{999};
constexpr OccurrenceId kFirstOccurrence{11};
constexpr OccurrenceId kSecondOccurrence{12};
constexpr OccurrenceId kMissingOccurrence{999};

std::unique_ptr<Document> makeShapedDocument(const char* label) {
  auto document = std::make_unique<Document>();
  auto& body = document->addBody(std::string(label) + " body");
  body.addFeature(std::make_unique<ImportedShapeFeature>(
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape()),
      std::string(label) + " feature"));
  CHECK(document->recompute());
  return document;
}

struct DocumentProbe {
  const Document* document{};
  const Body* body{};
  const Feature* feature{};
  const TopoDS_Shape* shape{};
  ShapeRevision shapeRevision{kInvalidShapeRevision};
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  BoxParameters box;
};

DocumentProbe probe(const PartDefinition& part) {
  DocumentProbe result;
  result.document = &part.document();
  result.box = part.document().box();
  CHECK(!part.document().bodies().empty());
  result.body = &part.document().bodies().front();
  result.bodyId = result.body->id();
  CHECK(!result.body->features().empty());
  result.feature = result.body->features().front().get();
  result.featureId = result.feature->id();
  const auto* shaped = dynamic_cast<const ShapeFeature*>(result.feature);
  CHECK(shaped);
  result.shape = shaped->lastValidShape().get();
  result.shapeRevision = shaped->shapeRevision();
  return result;
}

bool matchesProbe(const PartDefinition& part, const DocumentProbe& expected) {
  if (&part.document() != expected.document ||
      part.document().bodies().empty() ||
      &part.document().bodies().front() != expected.body ||
      part.document().bodies().front().id() != expected.bodyId ||
      part.document().bodies().front().features().empty() ||
      part.document().bodies().front().features().front().get() !=
          expected.feature ||
      part.document().bodies().front().features().front()->id() !=
          expected.featureId ||
      part.document().box().widthMm != expected.box.widthMm ||
      part.document().box().depthMm != expected.box.depthMm ||
      part.document().box().heightMm != expected.box.heightMm)
    return false;
  const auto* shaped = dynamic_cast<const ShapeFeature*>(expected.feature);
  return shaped != nullptr && shaped->lastValidShape().get() == expected.shape &&
         shaped->shapeRevision() == expected.shapeRevision;
}

RigidTransform3d rotationZ90WithTranslation() {
  const double halfSqrt = std::sqrt(0.5);
  auto created = RigidTransform3d::create(
      {10.0, -20.0, 30.0}, {halfSqrt, 0.0, 0.0, halfSqrt});
  CHECK(created);
  return *created.transform;
}

OccurrencePath path(OccurrenceId id) {
  auto created = OccurrencePath::create({id});
  CHECK(created);
  return std::move(*created.path);
}

}  // namespace

int main() {
  static_assert(!std::is_convertible_v<std::uint64_t, OccurrenceId>);
  static_assert(!std::is_convertible_v<OccurrenceId, std::uint64_t>);
  static_assert(!std::is_convertible_v<OccurrenceId, DefinitionId>);
  static_assert(!std::is_convertible_v<DefinitionId, OccurrenceId>);
  static_assert(!std::is_constructible_v<
                RigidTransform3d, std::array<double, 3>,
                std::array<double, 4>>);
  static_assert(!std::is_copy_constructible_v<AssemblyDefinition>);
  static_assert(std::is_nothrow_move_constructible_v<AssemblyDefinition>);
  static_assert(noexcept(isSafeLinkedPartRelativePathHintUtf8(
      std::declval<std::string_view>())));

  {
    const ProjectId project{{0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                             0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00}};
    SourceContentHash hash;
    auto valid = [&](std::string locator) {
      return LinkedPartBinding{{project, DefinitionId{77}},
                               std::move(locator), hash};
    };
    CHECK(valid("library/part.solidar").isValid());
    CHECK(valid("каталог/деталь.solidar").isValid());
    CHECK(valid("a/../part.solidar").isValid());
    CHECK(!valid(std::string("bad\xc3\x28", 5)).isValid());
    CHECK(!valid("//server/share.solidar").isValid());
    CHECK(!valid("\\\\server\\share.solidar").isValid());
    CHECK(!valid("\\\\?\\C:\\part.solidar").isValid());
    CHECK(!valid("C:/part.solidar").isValid());
    CHECK(!valid("part.solidar:stream").isValid());
    CHECK(!valid(std::string("part\0evil", 9)).isValid());
    CHECK(!valid("../part.solidar").isValid());
    CHECK(!valid("a/../../part.solidar").isValid());
    CHECK(!valid("CON.solidar").isValid());
    CHECK(!valid("parts/lpt1.txt").isValid());
    bool threw = false;
    try {
      PartDefinition invalid(DefinitionId{1}, std::make_unique<Document>(),
                             valid("../part.solidar"));
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }

  // Canonical quaternion representation is finite, normalized, sign-stable,
  // and contains no negative zero. Scale/shear/reflection have no public
  // representation in this closed domain type.
  {
    const auto identity = RigidTransform3d::identity();
    const std::array expectedIdentityTranslation{0.0, 0.0, 0.0};
    const std::array expectedIdentityRotation{1.0, 0.0, 0.0, 0.0};
    CHECK(identity.translationMm() == expectedIdentityTranslation);
    CHECK(identity.quaternionWxyz() == expectedIdentityRotation);

    const double negativeZero = -0.0;
    auto positive = RigidTransform3d::create(
        {negativeZero, 2.0, negativeZero}, {0.0, 0.0, 0.0, 2.0});
    auto negative = RigidTransform3d::create(
        {0.0, 2.0, 0.0}, {negativeZero, negativeZero, negativeZero, -4.0});
    CHECK(positive);
    CHECK(negative);
    CHECK(*positive.transform == *negative.transform);
    for (const double value : positive.transform->translationMm())
      CHECK(value != 0.0 || !std::signbit(value));
    for (const double value : positive.transform->quaternionWxyz())
      CHECK(value != 0.0 || !std::signbit(value));

    const std::array<double, 4> reviewerQuaternion{
        0.6000170285909403, 0.6841382767015547, 0.7253981659060906,
        0.35607804413316146};
    const auto firstCanonical = RigidTransform3d::create(
        {1.0, 2.0, 3.0}, reviewerQuaternion);
    CHECK(firstCanonical);
    double canonicalNormSquared = 0.0;
    for (const double value : firstCanonical.transform->quaternionWxyz())
      canonicalNormSquared =
          std::fma(value, value, canonicalNormSquared);
    CHECK(std::abs(std::sqrt(canonicalNormSquared) - 1.0) <=
          std::numeric_limits<double>::epsilon());
    const auto secondCanonical = RigidTransform3d::create(
        firstCanonical.transform->translationMm(),
        firstCanonical.transform->quaternionWxyz());
    CHECK(secondCanonical);
    CHECK(secondCanonical.transform->quaternionWxyz() ==
          firstCanonical.transform->quaternionWxyz());

    // These near-ray examples are retained as numerical regressions, not as a
    // promise for independently rounded inputs that are no longer exactly
    // collinear in binary64.
    auto scaledReviewer = reviewerQuaternion;
    for (double& value : scaledReviewer)
      value *= 1.0 + std::numeric_limits<double>::epsilon();
    const auto scaledCanonical = RigidTransform3d::create(
        {1.0, 2.0, 3.0}, scaledReviewer);
    CHECK(scaledCanonical);
    CHECK(scaledCanonical.transform->quaternionWxyz() ==
          firstCanonical.transform->quaternionWxyz());
    auto negativeReviewer = reviewerQuaternion;
    for (double& value : negativeReviewer) value = -value;
    const auto negativeCanonical = RigidTransform3d::create(
        {1.0, 2.0, 3.0}, negativeReviewer);
    CHECK(negativeCanonical);
    CHECK(negativeCanonical.transform->quaternionWxyz() ==
          firstCanonical.transform->quaternionWxyz());
    auto broadlyScaledReviewer = reviewerQuaternion;
    for (double& value : broadlyScaledReviewer) value *= 7.0;
    const auto broadlyScaledCanonical = RigidTransform3d::create(
        {1.0, 2.0, 3.0}, broadlyScaledReviewer);
    CHECK(broadlyScaledCanonical);
    CHECK(broadlyScaledCanonical.transform->quaternionWxyz() ==
          firstCanonical.transform->quaternionWxyz());

    const auto checkCanonicalContract =
        [](const std::array<double, 4>& source) {
          const auto canonical =
              RigidTransform3d::create({4.0, 5.0, 6.0}, source);
          CHECK(canonical);
          const auto repeated = RigidTransform3d::create(
              canonical.transform->translationMm(),
              canonical.transform->quaternionWxyz());
          CHECK(repeated);
          CHECK(repeated.transform->quaternionWxyz() ==
                canonical.transform->quaternionWxyz());

          auto negativeSource = source;
          for (std::size_t index = 0; index < source.size(); ++index) {
            negativeSource[index] = -source[index];
          }
          const auto negative = RigidTransform3d::create(
              {4.0, 5.0, 6.0}, negativeSource);
          CHECK(negative);
          CHECK(negative.transform->quaternionWxyz() ==
                canonical.transform->quaternionWxyz());

          for (const int exponent : {-400, -17, 19, 400}) {
            auto powerOfTwoScaledSource = source;
            for (double& value : powerOfTwoScaledSource) {
              value = std::ldexp(value, exponent);
              CHECK(std::isfinite(value));
            }
            const auto powerOfTwoScaled = RigidTransform3d::create(
                {4.0, 5.0, 6.0}, powerOfTwoScaledSource);
            CHECK(powerOfTwoScaled);
            CHECK(powerOfTwoScaled.transform->quaternionWxyz() ==
                  canonical.transform->quaternionWxyz());
          }
        };

    checkCanonicalContract({1.0, 1.0, 0.25, 0.0});
    checkCanonicalContract({1.0, -1.0, 0.25, -0.125});
    checkCanonicalContract({1.0, 1.0, 1.0, 0.25});
    checkCanonicalContract({1.0, 1.0, 1.0, 0.0});
    checkCanonicalContract({1.0, 1.0, 1.0, 1.0});
    const double quantizationBoundary = 1.0 - 0x1p-49;
    const double justBelowBoundary =
        std::nextafter(quantizationBoundary, 0.0);
    const double justAboveBoundary =
        std::nextafter(quantizationBoundary, 1.0);
    CHECK(justBelowBoundary < quantizationBoundary);
    CHECK(quantizationBoundary < justAboveBoundary);
    CHECK(justBelowBoundary != justAboveBoundary);
    checkCanonicalContract(
        {1.0, justBelowBoundary, 0.25, 0.125});
    checkCanonicalContract(
        {1.0, justAboveBoundary, 0.25, 0.125});

    const std::array<double, 4> auditorCounterexample{
        0.8716010645330563, 0.8716010645330545,
        -0.034945101736085116, 0.045766538112406385};
    checkCanonicalContract(auditorCounterexample);

    const auto scaledIdentity = RigidTransform3d::create(
        {0.0, 0.0, 0.0},
        {1.0 + std::numeric_limits<double>::epsilon(), 0.0, 0.0, 0.0});
    const auto negativeIdentity = RigidTransform3d::create(
        {0.0, 0.0, 0.0}, {-1.0, 0.0, 0.0, 0.0});
    CHECK(scaledIdentity);
    CHECK(negativeIdentity);
    CHECK(*scaledIdentity.transform == identity);
    CHECK(*negativeIdentity.transform == identity);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    CHECK(RigidTransform3d::create({nan, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0})
              .status == RigidTransform3dStatus::NonFiniteTranslation);
    CHECK(RigidTransform3d::create({0.0, infinity, 0.0},
                                   {1.0, 0.0, 0.0, 0.0})
              .status == RigidTransform3dStatus::NonFiniteTranslation);
    CHECK(RigidTransform3d::create({0.0, 0.0, -infinity},
                                   {1.0, 0.0, 0.0, 0.0})
              .status == RigidTransform3dStatus::NonFiniteTranslation);
    CHECK(RigidTransform3d::create({0.0, 0.0, 0.0},
                                   {nan, 0.0, 0.0, 0.0})
              .status == RigidTransform3dStatus::NonFiniteRotation);
    CHECK(RigidTransform3d::create({0.0, 0.0, 0.0},
                                   {0.0, infinity, 0.0, 0.0})
              .status == RigidTransform3dStatus::NonFiniteRotation);
    const auto zero = RigidTransform3d::create(
        {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0});
    CHECK(zero.status == RigidTransform3dStatus::DegenerateRotation);
    CHECK(!zero.transform.has_value());
  }

  // OccurrencePath carries the full stable path. The current flat assembly
  // resolver, rather than the address type, rejects nested depth.
  {
    const auto empty = OccurrencePath::create({});
    CHECK(empty.status == OccurrencePathStatus::Empty);
    CHECK(!empty.path.has_value());
    const auto invalid = OccurrencePath::create({kInvalidOccurrenceId});
    CHECK(invalid.status == OccurrencePathStatus::InvalidOccurrenceId);
    CHECK(!invalid.path.has_value());
    const auto flat = OccurrencePath::create({kFirstOccurrence});
    CHECK(flat);
    CHECK(flat.path->depth() == 1);
    CHECK(flat.path->entries().front() == kFirstOccurrence);
    const auto nested =
        OccurrencePath::create({kFirstOccurrence, kSecondOccurrence});
    CHECK(nested);
    AssemblyDefinition flatAssembly(DefinitionId{1});
    CHECK(flatAssembly.resolve(*nested.path).status ==
          FlatOccurrenceResolveStatus::UnsupportedDepth);
  }

  // IDs are validated before a ComponentOccurrence can enter an assembly.
  {
    bool rejected = false;
    try {
      ComponentOccurrence invalid(kInvalidOccurrenceId, kSharedPart,
                                  RigidTransform3d::identity());
      static_cast<void>(invalid);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    CHECK(rejected);
    rejected = false;
    try {
      ComponentOccurrence invalid(kFirstOccurrence, kInvalidDefinitionId,
                                  RigidTransform3d::identity());
      static_cast<void>(invalid);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    CHECK(rejected);
  }

  // Registry validation is cross-kind and moves candidates only after every
  // duplicate/dangling/nesting check has succeeded.
  {
    DefinitionRegistry duplicateRegistry;
    CHECK(duplicateRegistry.insert(
              PartDefinition(kSharedPart, makeShapedDocument("duplicate"))) ==
          DefinitionInsertStatus::Inserted);
    AssemblyDefinition duplicate(kSharedPart);
    CHECK(duplicateRegistry.insert(std::move(duplicate)) ==
          DefinitionInsertStatus::DuplicateDefinitionId);
    CHECK(duplicate.id() == kSharedPart);

    AssemblyDefinition dangling(kAssemblyRoot);
    CHECK(dangling.insert(ComponentOccurrence(
              kFirstOccurrence, kMissingDefinition,
              RigidTransform3d::identity())) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    CHECK(duplicateRegistry.insert(std::move(dangling)) ==
          DefinitionInsertStatus::DanglingOccurrenceTarget);
    CHECK(dangling.find(kFirstOccurrence));

    DefinitionRegistry nestedRegistry;
    CHECK(nestedRegistry.insert(AssemblyDefinition(kAssemblyRoot)) ==
          DefinitionInsertStatus::Inserted);
    AssemblyDefinition nested(kOtherAssembly);
    CHECK(nested.insert(ComponentOccurrence(
              kFirstOccurrence, kAssemblyRoot,
              RigidTransform3d::identity())) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    CHECK(nestedRegistry.insert(std::move(nested)) ==
          DefinitionInsertStatus::UnsupportedAssemblyTarget);
    CHECK(nested.find(kFirstOccurrence));
  }

  // Build a valid flat assembly with two independently authored occurrences
  // that both refer to the same single-owned PartDefinition/Document.
  DefinitionRegistry definitions;
  auto sharedDocument = makeShapedDocument("shared");
  const Document* const sharedDocumentAddress = sharedDocument.get();
  CHECK(definitions.insert(
            PartDefinition(kSharedPart, std::move(sharedDocument))) ==
        DefinitionInsertStatus::Inserted);
  auto orphanDocument = makeShapedDocument("orphan");
  const Document* const orphanDocumentAddress = orphanDocument.get();
  CHECK(definitions.insert(
            PartDefinition(kOrphanPart, std::move(orphanDocument))) ==
        DefinitionInsertStatus::Inserted);
  const auto sharedProbe = probe(*definitions.findPart(kSharedPart));
  const auto orphanProbe = probe(*definitions.findPart(kOrphanPart));

  AssemblyDefinition assembly(kAssemblyRoot);
  CHECK(assembly.insert(ComponentOccurrence(
            kFirstOccurrence, kSharedPart, RigidTransform3d::identity(),
            {.visible = true, .suppressed = false, .grounded = true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(assembly.insert(ComponentOccurrence(
            kSecondOccurrence, kSharedPart, rotationZ90WithTranslation(),
            {.visible = false, .suppressed = true, .grounded = false})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  ComponentOccurrence duplicateOccurrence(
      kFirstOccurrence, kSharedPart, rotationZ90WithTranslation());
  CHECK(assembly.insert(std::move(duplicateOccurrence)) ==
        AssemblyOccurrenceInsertStatus::DuplicateOccurrenceId);
  CHECK(duplicateOccurrence.id() == kFirstOccurrence);
  CHECK(duplicateOccurrence.targetDefinitionId() == kSharedPart);
  CHECK(definitions.insert(std::move(assembly)) ==
        DefinitionInsertStatus::Inserted);

  auto created =
      ProductDocument::create(std::move(definitions), kAssemblyRoot);
  CHECK(created);
  CHECK(created.product->rootDefinitionKind() == DefinitionKind::Assembly);
  CHECK(created.product->rootPartDefinition() == nullptr);
  CHECK(created.product->rootAssemblyDefinition());
  CHECK(created.product->rootAssemblyDefinition()->size() == 2);
  CHECK(created.product->definitions().size() == 3);
  CHECK(&created.product->definitions().findPart(kSharedPart)->document() ==
        sharedDocumentAddress);
  CHECK(&created.product->definitions().findPart(kOrphanPart)->document() ==
        orphanDocumentAddress);
  CHECK(matchesProbe(*created.product->definitions().findPart(kSharedPart),
                     sharedProbe));
  CHECK(matchesProbe(*created.product->definitions().findPart(kOrphanPart),
                     orphanProbe));

  const auto firstPath = path(kFirstOccurrence);
  const auto secondPath = path(kSecondOccurrence);
  auto first = created.product->resolveRootOccurrence(firstPath);
  auto second = created.product->resolveRootOccurrence(secondPath);
  CHECK(first);
  CHECK(second);
  CHECK(first.occurrence->targetDefinitionId() == kSharedPart);
  CHECK(second.occurrence->targetDefinitionId() == kSharedPart);
  CHECK(first.occurrence->id() != second.occurrence->id());
  CHECK(first.occurrence->placement() != second.occurrence->placement());
  CHECK(first.occurrence->flags() != second.occurrence->flags());

  CHECK(created.product->resolveRootOccurrence(path(kMissingOccurrence)).status ==
        FlatOccurrenceResolveStatus::OccurrenceMissing);

  // Rejected mutations are exact no-ops and successful placement/flag edits
  // do not touch either shared or orphan Document semantics.
  {
    const auto firstBefore = *first.occurrence;
    const auto secondBefore = *second.occurrence;
    ComponentOccurrence duplicate(
        kFirstOccurrence, kOrphanPart, rotationZ90WithTranslation());
    CHECK(created.product->insertRootOccurrence(std::move(duplicate)) ==
          ProductOccurrenceMutationStatus::DuplicateOccurrenceId);
    CHECK(duplicate.targetDefinitionId() == kOrphanPart);
    ComponentOccurrence dangling(
        OccurrenceId{13}, kMissingDefinition, RigidTransform3d::identity());
    CHECK(created.product->insertRootOccurrence(std::move(dangling)) ==
          ProductOccurrenceMutationStatus::DanglingTargetDefinition);
    CHECK(dangling.targetDefinitionId() == kMissingDefinition);
    ComponentOccurrence nested(
        OccurrenceId{14}, kAssemblyRoot, RigidTransform3d::identity());
    CHECK(created.product->insertRootOccurrence(std::move(nested)) ==
          ProductOccurrenceMutationStatus::UnsupportedAssemblyTarget);
    CHECK(nested.targetDefinitionId() == kAssemblyRoot);

    CHECK(created.product->setRootOccurrencePlacement(
              firstPath, rotationZ90WithTranslation()) ==
          ProductOccurrenceMutationStatus::OccurrenceGrounded);
    CHECK(created.product->resolveRootOccurrence(firstPath)
              .occurrence->placement() == firstBefore.placement());
    CHECK(created.product->setRootOccurrenceFlags(
              firstPath,
              {.visible = false, .suppressed = false, .grounded = false}) ==
          ProductOccurrenceMutationStatus::Committed);
    CHECK(created.product->setRootOccurrencePlacement(
              firstPath, rotationZ90WithTranslation()) ==
          ProductOccurrenceMutationStatus::Committed);
    const auto changed = created.product->resolveRootOccurrence(firstPath);
    const auto unchanged = created.product->resolveRootOccurrence(secondPath);
    CHECK(changed);
    CHECK(unchanged);
    CHECK(changed.occurrence->targetDefinitionId() ==
          firstBefore.targetDefinitionId());
    CHECK(changed.occurrence->placement() == rotationZ90WithTranslation());
    const OccurrenceFlags expectedChangedFlags{
        .visible = false, .suppressed = false, .grounded = false};
    CHECK(changed.occurrence->flags() == expectedChangedFlags);
    CHECK(unchanged.occurrence->id() == secondBefore.id());
    CHECK(unchanged.occurrence->targetDefinitionId() ==
          secondBefore.targetDefinitionId());
    CHECK(unchanged.occurrence->placement() == secondBefore.placement());
    CHECK(unchanged.occurrence->flags() == secondBefore.flags());
    CHECK(matchesProbe(*created.product->definitions().findPart(kSharedPart),
                       sharedProbe));
    CHECK(matchesProbe(*created.product->definitions().findPart(kOrphanPart),
                       orphanProbe));
  }

  // Removing occurrences never cascades to definitions. Even after the last
  // occurrence is gone, both the formerly referenced part and pre-existing
  // orphan retain their exact owner allocations.
  CHECK(created.product->removeRootOccurrence(firstPath) ==
        ProductOccurrenceMutationStatus::Committed);
  CHECK(created.product->removeRootOccurrence(firstPath) ==
        ProductOccurrenceMutationStatus::OccurrenceMissing);
  CHECK(created.product->removeRootOccurrence(secondPath) ==
        ProductOccurrenceMutationStatus::Committed);
  CHECK(created.product->rootAssemblyDefinition()->size() == 0);
  CHECK(created.product->definitions().size() == 3);
  CHECK(matchesProbe(*created.product->definitions().findPart(kSharedPart),
                     sharedProbe));
  CHECK(matchesProbe(*created.product->definitions().findPart(kOrphanPart),
                     orphanProbe));
  CHECK(created.product->remove(kSharedPart) ==
        ProductDocumentRemoveStatus::DefinitionRemovalUnsupported);
  CHECK(created.product->remove(kOrphanPart) ==
        ProductDocumentRemoveStatus::DefinitionRemovalUnsupported);
  CHECK(created.product->definitions().size() == 3);

  // Product creation rejects any assembly outside the root and preserves the
  // caller registry and all owned Document allocations.
  {
    DefinitionRegistry rejectedRegistry;
    auto document = makeShapedDocument("rejected");
    const auto* address = document.get();
    CHECK(rejectedRegistry.insert(
              PartDefinition(kSharedPart, std::move(document))) ==
          DefinitionInsertStatus::Inserted);
    CHECK(rejectedRegistry.insert(AssemblyDefinition(kAssemblyRoot)) ==
          DefinitionInsertStatus::Inserted);
    const auto rejected = ProductDocument::create(
        std::move(rejectedRegistry), kSharedPart);
    CHECK(rejected.status ==
          ProductDocumentStatus::UnsupportedNonRootAssemblyDefinition);
    CHECK(!rejected.product);
    CHECK(rejectedRegistry.size() == 2);
    CHECK(&rejectedRegistry.findPart(kSharedPart)->document() == address);
    CHECK(rejectedRegistry.findAssembly(kAssemblyRoot));
  }

  return EXIT_SUCCESS;
}
