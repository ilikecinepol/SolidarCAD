#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <QApplication>

#include <cstdlib>
#include <memory>
#include <optional>
#include <utility>

#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "ui/ComponentWorkspace.h"
#include "ui/Viewport.h"
#include "ui/application/ModelWorkspaceProjectionSuccessor.h"
#include "ui/application/PartWorkspaceProjection.h"

namespace {

using namespace solidar;

constexpr AssemblyMeshProfile kProfile{AssemblyMeshQuality::Normal};

std::unique_ptr<Document> twoBodyDocument() {
  auto document = std::make_unique<Document>();
  auto& first = document->addBody(BodyId{201}, "first");
  first.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{301},
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(8.0, 6.0, 4.0).Shape()),
      "first feature"));
  auto& second = document->addBody(BodyId{202}, "second");
  second.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{302},
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(5.0, 4.0, 3.0).Shape()),
      "second feature"));
  CHECK(document->recompute());
  return document;
}

ProductDocument partProduct() {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(
            PartDefinition(DefinitionId{1}, twoBodyDocument())) ==
        DefinitionInsertStatus::Inserted);
  auto created =
      ProductDocument::create(std::move(definitions), DefinitionId{1});
  CHECK(created);
  return std::move(*created.product);
}

ProductDocument componentProduct() {
  auto rootDocument = std::make_unique<Document>();
  auto& rootBody = rootDocument->addBody(BodyId{10}, "root body");
  rootBody.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{11},
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(10.0, 8.0, 6.0).Shape()),
      "root feature"));
  CHECK(rootDocument->recompute());
  auto childDocument = std::make_unique<Document>();
  auto& childBody = childDocument->addBody(BodyId{10}, "child body");
  childBody.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{11},
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(4.0, 3.0, 2.0).Shape()),
      "child feature"));
  CHECK(childDocument->recompute());

  ComponentDefinition root(DefinitionId{100}, std::move(rootDocument));
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{7}, DefinitionId{200},
            RigidTransform3d::identity(), {}, "child")) ==
        ComponentOccurrenceInsertStatus::Inserted);
  std::vector<ComponentDefinition> components;
  components.emplace_back(DefinitionId{200}, std::move(childDocument));
  components.push_back(std::move(root));
  auto created = ProductDocument::createComponentGraph(
      std::move(components), DefinitionId{100});
  CHECK(created);
  return std::move(*created.product);
}

ComponentWorkspaceInstanceKey rootKey(const ProductDocument& product) {
  return {product.rootDefinitionId(), std::nullopt};
}

ComponentWorkspaceSemanticState partSemantic(
    const ProductDocument& product, std::optional<BodyId> body = std::nullopt) {
  const auto root = rootKey(product);
  return {root, root,
          body ? std::optional<ComponentWorkspaceBodyKey>{{root, *body}}
               : std::nullopt,
          "part semantic"};
}

ComponentWorkspaceSemanticState componentChildSemantic(
    const ProductDocument& product, DefinitionId definition,
    OccurrenceId occurrence, BodyId body) {
  auto path = OccurrencePath::create({occurrence});
  CHECK(path);
  ComponentWorkspaceInstanceKey child{definition, std::move(*path.path)};
  return {child, child, ComponentWorkspaceBodyKey{child, body},
          "component semantic"};
}

ComponentExtractionAddress addressFor(const ProductDocument& product,
                                      BodyId body, DefinitionId child,
                                      OccurrenceId occurrence) {
  return {product.incarnation(), product.rootDefinitionId(),
          product.rootDefinitionId(),
          product.definitionStateId(product.rootDefinitionId()), body, child,
          occurrence};
}

ComponentExtractionTransaction transactionFor(
    const ProductDocument& product, BodyId body, DefinitionId child,
    OccurrenceId occurrence) {
  auto prepared = product.prepareComponentExtraction(
      addressFor(product, body, child, occurrence));
  CHECK(prepared);
  return std::move(*prepared.transaction);
}

ProductIncarnationId incarnationProbe() {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            DefinitionId{9000}, std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  auto created =
      ProductDocument::create(std::move(definitions), DefinitionId{9000});
  CHECK(created);
  return created.product->incarnation();
}

}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);

  // Part preparation owns all topology, tessellation and bounds work. A
  // failed candidate cannot disturb an already installed ordinary Part view.
  {
    auto product = partProduct();
    auto prepared = PartWorkspaceProjectionPreparer::prepare(
        product, ProductPresentationEpoch{10}, kProfile,
        partSemantic(product, BodyId{201}));
    CHECK(prepared);
    CHECK(prepared.projection->bodyDisplay());
    CHECK(prepared.projection->bodyDisplay()->bodyCount() == 2);
    CHECK(prepared.projection->bodyDisplay()->diagonal() > 0.0);

    Viewport viewport;
    auto display = prepared.projection->releaseBodyDisplay();
    CHECK(display);
    CHECK(viewport.installPreparedBodyDisplay(std::move(*display)));
    CHECK(viewport.displayedBodyCount() == 2);

    for (const auto point : {PartWorkspaceProjectionFailurePoint::Topology,
                             PartWorkspaceProjectionFailurePoint::Meshing,
                             PartWorkspaceProjectionFailurePoint::Bounds,
                             PartWorkspaceProjectionFailurePoint::Resource}) {
      const auto rejected = PartWorkspaceProjectionPreparer::prepare(
          product, ProductPresentationEpoch{11}, kProfile,
          partSemantic(product), point);
      CHECK(!rejected);
      CHECK(viewport.displayedBodyCount() == 2);
    }
    auto forged = partSemantic(product);
    forged.activeInstance.definitionId = DefinitionId{999};
    CHECK(!PartWorkspaceProjectionPreparer::prepare(
        product, ProductPresentationEpoch{12}, kProfile, forged));
    CHECK(viewport.displayedBodyCount() == 2);
  }

  // Component projection keeps the candidate product borrowed, prepares a
  // parentless tree and restores typed semantic component selection exactly.
  {
    auto product = componentProduct();
    const auto semantic = componentChildSemantic(
        product, DefinitionId{200}, OccurrenceId{7}, BodyId{10});
    auto prepared = ComponentProjectionPreparer::prepare(
        product, ProductPresentationEpoch{20}, kProfile, semantic);
    CHECK(prepared);
    CHECK(prepared.projection->session());
    CHECK(prepared.projection->treeModel());
    CHECK(prepared.projection->treeModel()->QObject::parent() == nullptr);
    CHECK(prepared.projection->session()->selectedComponent());
    CHECK(prepared.projection->semanticState() == semantic);

    auto badSession = ComponentProjectionPreparer::prepare(
        product, ProductPresentationEpoch{21}, kProfile, semantic,
        ComponentViewportSessionFailurePoint::Resource);
    CHECK(!badSession);
    CHECK(badSession.status == ComponentWorkspacePrepareStatus::ResourceFailed);
    auto badTree = ComponentProjectionPreparer::prepare(
        product, ProductPresentationEpoch{22}, kProfile, semantic,
        ComponentViewportSessionFailurePoint::None,
        ComponentTreeModelFailurePoint::ResourceFailure);
    CHECK(!badTree);
    CHECK(badTree.status == ComponentWorkspacePrepareStatus::ResourceFailed);
    auto forged = semantic;
    forged.selectedInstance->definitionId = DefinitionId{999};
    CHECK(!ComponentProjectionPreparer::prepare(
        product, ProductPresentationEpoch{23}, kProfile, forged));

    // Tree-only component mutations (such as adopting the current placement
    // as home) validate the shared successor preconditions without rebuilding
    // or retaining viewport geometry.
    ModelWorkspaceProjectionSuccessor treeOnly(
        ProductPresentationEpoch{24}, kProfile, semantic);
    CHECK(treeOnly.prepare(product, ProductDocumentSuccessorImpact::TreeOnly) ==
          ProductDocumentSuccessorStatus::Prepared);
    CHECK(!treeOnly.part());
    CHECK(!treeOnly.component());
    ModelWorkspaceProjectionSuccessor treeResource(
        ProductPresentationEpoch{25}, kProfile, semantic,
        ModelWorkspaceProjectionFailurePoint::Resource);
    CHECK(treeResource.prepare(product,
                               ProductDocumentSuccessorImpact::TreeOnly) ==
          ProductDocumentSuccessorStatus::ResourceFailure);
    ModelWorkspaceProjectionSuccessor treeInvalidEpoch(
        ProductPresentationEpoch{}, kProfile, semantic);
    CHECK(treeInvalidEpoch.prepare(product,
                                   ProductDocumentSuccessorImpact::TreeOnly) ==
          ProductDocumentSuccessorStatus::Rejected);
  }

  // The successor retains exactly one typed projection and consumption is
  // one-shot. It never takes ownership of the supplied product.
  {
    auto product = partProduct();
    ModelWorkspaceProjectionSuccessor successor(
        ProductPresentationEpoch{30}, kProfile, partSemantic(product));
    CHECK(successor.prepare(product,
                            ProductDocumentSuccessorImpact::FullProjection) ==
          ProductDocumentSuccessorStatus::Prepared);
    CHECK(successor.part());
    CHECK(!successor.component());
    CHECK(successor.takePart());
    CHECK(!successor.takePart());
    CHECK(successor.status() == ModelWorkspaceProjectionStatus::Consumed);
    CHECK(successor.prepare(product,
                            ProductDocumentSuccessorImpact::FullProjection) ==
          ProductDocumentSuccessorStatus::Rejected);
  }

  // Extraction successor sees the fully staged after-candidate. Revert sees a
  // Part candidate, and a second extraction remains Component->Component.
  {
    auto product = partProduct();
    const auto first = transactionFor(product, BodyId{201}, DefinitionId{2},
                                      OccurrenceId{41});
    const auto beforeIds = incarnationProbe();
    ModelWorkspaceProjectionSuccessor firstAfter(
        ProductPresentationEpoch{40}, kProfile,
        componentChildSemantic(product, DefinitionId{2}, OccurrenceId{41},
                               BodyId{201}));
    CHECK(product.applyComponentExtraction(first, firstAfter) ==
          ComponentExtractionStatus::Committed);
    CHECK(firstAfter.component());
    CHECK(firstAfter.takeComponent());
    CHECK(product.rootDefinitionKind() == DefinitionKind::Component);

    ModelWorkspaceProjectionSuccessor undoToPart(
        ProductPresentationEpoch{41}, kProfile,
        partSemantic(product, BodyId{201}));
    // The root ID is stable across Component->Part, so the semantic root key
    // can be formed from the current product before revert.
    CHECK(product.revertComponentExtraction(first, undoToPart) ==
          ComponentExtractionStatus::Reverted);
    CHECK(undoToPart.part());
    CHECK(product.rootDefinitionKind() == DefinitionKind::Part);

    ModelWorkspaceProjectionSuccessor redoFirst(
        ProductPresentationEpoch{42}, kProfile,
        componentChildSemantic(product, DefinitionId{2}, OccurrenceId{41},
                               BodyId{201}));
    CHECK(product.applyComponentExtraction(first, redoFirst) ==
          ComponentExtractionStatus::Committed);
    const auto second = transactionFor(product, BodyId{202}, DefinitionId{3},
                                       OccurrenceId{42});
    ModelWorkspaceProjectionSuccessor secondAfter(
        ProductPresentationEpoch{43}, kProfile,
        componentChildSemantic(product, DefinitionId{3}, OccurrenceId{42},
                               BodyId{202}));
    CHECK(product.applyComponentExtraction(second, secondAfter) ==
          ComponentExtractionStatus::Committed);
    CHECK(secondAfter.component());
    CHECK(product.rootComponentDefinition()->size() == 2);
    const auto afterIds = incarnationProbe();
    CHECK(afterIds.value() == beforeIds.value() + 1);
  }

  // Successor rejection/resource failure occurs before ProductDocument's
  // atomic swap and preserves the original Part candidate exactly.
  {
    auto product = partProduct();
    const auto transaction = transactionFor(
        product, BodyId{201}, DefinitionId{4}, OccurrenceId{51});
    ModelWorkspaceProjectionSuccessor rejected(
        ProductPresentationEpoch{50}, kProfile,
        componentChildSemantic(product, DefinitionId{4}, OccurrenceId{51},
                               BodyId{201}),
        ModelWorkspaceProjectionFailurePoint::ComponentSession);
    CHECK(product.applyComponentExtraction(transaction, rejected) ==
          ComponentExtractionStatus::SuccessorResourceFailure);
    CHECK(product.rootDefinitionKind() == DefinitionKind::Part);
    CHECK(product.rootDocument().bodies().size() == 2);
    CHECK(!rejected.part());
    CHECK(!rejected.component());

    ModelWorkspaceProjectionSuccessor allocationFailure(
        ProductPresentationEpoch{51}, kProfile,
        componentChildSemantic(product, DefinitionId{4}, OccurrenceId{51},
                               BodyId{201}),
        ModelWorkspaceProjectionFailurePoint::Resource);
    CHECK(product.applyComponentExtraction(transaction, allocationFailure) ==
          ComponentExtractionStatus::SuccessorResourceFailure);
    CHECK(product.rootDefinitionKind() == DefinitionKind::Part);
  }

  return EXIT_SUCCESS;
}
