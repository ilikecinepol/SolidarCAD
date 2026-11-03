#include "TestAssertions.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "model/AssemblyGraphAnalysis.h"
#include "model/ComponentGraphAnalysis.h"
#include "model/ProductDocument.h"

namespace solidar {

class ComponentDrawingValidationTestAccess final {
 public:
  static void appendUnchecked(ComponentDefinition& definition,
                              ComponentDrawing drawing) {
    definition.drawings_.push_back(std::move(drawing));
  }
};

}  // namespace solidar

namespace {

using namespace solidar;

ComponentDefinition makeComponent(DefinitionId id) {
  return ComponentDefinition(id, std::make_unique<Document>());
}

ComponentDrawing drawing(std::uint64_t id, std::string name,
                         std::vector<BodyId> bodies = {},
                         double scale = 1.0) {
  return ComponentDrawing(DrawingId{id}, std::move(name),
                          ComponentDrawingPaper::A4,
                          ComponentDrawingOrientation::Portrait, scale,
                          std::move(bodies));
}

ComponentOccurrence makeOccurrence(std::uint64_t occurrenceId,
                                   DefinitionId target) {
  return ComponentOccurrence(OccurrenceId{occurrenceId}, target,
                             RigidTransform3d::identity());
}

void addChild(ComponentDefinition& owner, std::uint64_t occurrenceId,
              DefinitionId target) {
  CHECK(owner.insert(makeOccurrence(occurrenceId, target)) ==
        ComponentOccurrenceInsertStatus::Inserted);
}

}  // namespace

int main() {
  static_assert(!std::is_copy_constructible_v<ComponentDefinition>);
  static_assert(std::is_nothrow_move_constructible_v<ComponentDefinition>);
  static_assert(!std::is_same_v<DrawingId, BodyId>);
  static_assert(!std::is_same_v<DrawingId, DefinitionId>);

  // Drawing values validate bounded scalar metadata and canonicalize their
  // definition-local body reference set.
  {
    auto canonical = drawing(7, "A4 assembly", {BodyId{9}, BodyId{3},
                                                 BodyId{9}});
    CHECK(canonical.id() == DrawingId{7});
    CHECK(canonical.paper() == ComponentDrawingPaper::A4);
    CHECK(canonical.orientation() == ComponentDrawingOrientation::Portrait);
    CHECK(canonical.scale() == 1.0);
    CHECK(canonical.sourceBodyIds() ==
          std::vector<BodyId>({BodyId{3}, BodyId{9}}));

    for (const double invalidScale :
         {0.0, -1.0, std::numeric_limits<double>::infinity(),
          kComponentDrawingMinimumScale * 0.5,
          kComponentDrawingMaximumScale * 2.0}) {
      bool rejected = false;
      try {
        auto invalid = drawing(8, "invalid scale", {}, invalidScale);
        static_cast<void>(invalid);
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      CHECK(rejected);
    }
    bool rejected = false;
    try {
      ComponentDrawing invalid(
          DrawingId{9}, " ", ComponentDrawingPaper::A4,
          ComponentDrawingOrientation::Landscape, 1.0, {});
      static_cast<void>(invalid);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    CHECK(rejected);
  }

  // Construction validates before taking ownership from a named candidate.
  {
    auto retained = std::make_unique<Document>();
    auto* const address = retained.get();
    bool rejected = false;
    try {
      ComponentDefinition invalid(kInvalidDefinitionId, std::move(retained));
      static_cast<void>(invalid);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    CHECK(rejected);
    CHECK(retained.get() == address);

    std::unique_ptr<Document> missing;
    rejected = false;
    try {
      ComponentDefinition invalid(DefinitionId{1}, std::move(missing));
      static_cast<void>(invalid);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    CHECK(rejected);
  }

  // A component owns ordinary document content and a stable child collection.
  {
    auto document = std::make_unique<Document>();
    auto* const address = document.get();
    const BodyId bodyId = document->addBody("component body").id();
    ComponentDefinition component(DefinitionId{10}, std::move(document));
    CHECK(&component.document() == address);
    CHECK(component.document().findBody(bodyId) != nullptr);
    CHECK(component.stateId().isValid());

    addChild(component, 1, DefinitionId{11});
    CHECK(component.size() == 1);
    CHECK(component.find(OccurrenceId{1}) != nullptr);
    CHECK(component.find(OccurrenceId{1})->targetDefinitionId() ==
          DefinitionId{11});

    auto duplicate = makeOccurrence(1, DefinitionId{12});
    CHECK(component.insert(std::move(duplicate)) ==
          ComponentOccurrenceInsertStatus::DuplicateOccurrenceId);
    CHECK(duplicate.id() == OccurrenceId{1});
    CHECK(duplicate.targetDefinitionId() == DefinitionId{12});
    CHECK(component.find(OccurrenceId{1})->targetDefinitionId() ==
          DefinitionId{11});
    CHECK(component.erase(OccurrenceId{99}) ==
          ComponentOccurrenceEraseStatus::OccurrenceMissing);
    CHECK(component.erase(OccurrenceId{1}) ==
          ComponentOccurrenceEraseStatus::Erased);
    CHECK(component.size() == 0);
  }

  // Drawings are ordered definition-owned values. Constructor validation is
  // local and exact; numeric equality with other strong-ID domains is valid.
  {
    auto document = std::make_unique<Document>();
    document->addBody(BodyId{1}, "body");
    std::vector<ComponentDrawing> drawings;
    drawings.push_back(drawing(1, "first", {BodyId{1}}));
    drawings.push_back(drawing(2, "second"));
    ComponentDefinition component(DefinitionId{1}, std::move(document),
                                  std::move(drawings));
    CHECK(component.drawings().size() == 2);
    CHECK(component.drawings()[0].id() == DrawingId{1});
    CHECK(component.findDrawing(DrawingId{2}) == &component.drawings()[1]);
    CHECK(component.findDrawing(DrawingId{99}) == nullptr);
    CHECK(component.stateId() == DefinitionStateId{1});

    auto missingDocument = std::make_unique<Document>();
    missingDocument->addBody(BodyId{10}, "present");
    bool rejected = false;
    try {
      ComponentDefinition invalid(
          DefinitionId{10}, std::move(missingDocument),
          std::vector<ComponentDrawing>{
              drawing(1, "missing", {BodyId{11}})});
      static_cast<void>(invalid);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    CHECK(rejected);
    CHECK(missingDocument != nullptr);

    auto duplicateDocument = std::make_unique<Document>();
    duplicateDocument->addBody(BodyId{12}, "body");
    rejected = false;
    try {
      ComponentDefinition invalid(
          DefinitionId{12}, std::move(duplicateDocument),
          std::vector<ComponentDrawing>{drawing(3, "a"),
                                        drawing(3, "b")});
      static_cast<void>(invalid);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    CHECK(rejected);
    CHECK(duplicateDocument != nullptr);
  }

  // Registry insertion is child-first, typed, and rejection is an exact
  // ownership no-op.
  {
    DefinitionRegistry registry;
    auto child = makeComponent(DefinitionId{2});
    auto* const childDocument = &child.document();
    CHECK(registry.insert(std::move(child)) ==
          DefinitionInsertStatus::Inserted);
    CHECK(registry.findComponent(DefinitionId{2}) != nullptr);
    CHECK(&registry.findComponent(DefinitionId{2})->document() ==
          childDocument);
    CHECK(registry.kind(DefinitionId{2}) == DefinitionKind::Component);
    CHECK(registry.findPart(DefinitionId{2}) == nullptr);
    CHECK(registry.findAssembly(DefinitionId{2}) == nullptr);

    auto parent = makeComponent(DefinitionId{1});
    addChild(parent, 1, DefinitionId{2});
    CHECK(registry.insert(std::move(parent)) ==
          DefinitionInsertStatus::Inserted);
    CHECK(registry.erase(DefinitionId{2}) ==
          DefinitionEraseStatus::DefinitionReferenced);

    auto duplicate = makeComponent(DefinitionId{2});
    auto* const duplicateDocument = &duplicate.document();
    CHECK(registry.insert(std::move(duplicate)) ==
          DefinitionInsertStatus::DuplicateDefinitionId);
    CHECK(&duplicate.document() == duplicateDocument);

    auto dangling = makeComponent(DefinitionId{3});
    addChild(dangling, 1, DefinitionId{999});
    auto* const danglingDocument = &dangling.document();
    CHECK(registry.insert(std::move(dangling)) ==
          DefinitionInsertStatus::DanglingOccurrenceTarget);
    CHECK(&dangling.document() == danglingDocument);

    auto selfCycle = makeComponent(DefinitionId{4});
    addChild(selfCycle, 1, DefinitionId{4});
    CHECK(registry.insert(std::move(selfCycle)) ==
          DefinitionInsertStatus::ComponentGraphCycle);

    CHECK(registry.insert(PartDefinition(
              DefinitionId{5}, std::make_unique<Document>())) ==
          DefinitionInsertStatus::Inserted);
    auto wrongKind = makeComponent(DefinitionId{6});
    addChild(wrongKind, 1, DefinitionId{5});
    auto* const wrongKindDocument = &wrongKind.document();
    CHECK(registry.insert(std::move(wrongKind)) ==
          DefinitionInsertStatus::UnsupportedComponentTarget);
    CHECK(&wrongKind.document() == wrongKindDocument);

    CHECK(registry.erase(DefinitionId{1}) ==
          DefinitionEraseStatus::Erased);
    CHECK(registry.erase(DefinitionId{2}) ==
          DefinitionEraseStatus::Erased);
    CHECK(registry.erase(DefinitionId{5}) ==
          DefinitionEraseStatus::Erased);
  }

  // A complete graph accepts forward references and retains each component's
  // own Document allocation.
  {
    auto root = makeComponent(DefinitionId{100});
    auto child = makeComponent(DefinitionId{200});
    auto leaf = makeComponent(DefinitionId{300});
    auto* const rootDocument = &root.document();
    auto* const childDocument = &child.document();
    auto* const leafDocument = &leaf.document();
    addChild(root, 10, DefinitionId{200});
    addChild(child, 20, DefinitionId{300});
    std::vector<ComponentDefinition> components;
    components.push_back(std::move(root));
    components.push_back(std::move(leaf));
    components.push_back(std::move(child));

    auto created = ProductDocument::createComponentGraph(
        std::move(components), DefinitionId{100});
    CHECK(created);
    CHECK(created.product->rootDefinitionKind() == DefinitionKind::Component);
    CHECK(created.product->rootComponentDefinition() != nullptr);
    CHECK(&created.product->rootComponentDefinition()->document() ==
          rootDocument);
    CHECK(created.product->componentDocument(DefinitionId{100}) ==
          rootDocument);
    CHECK(created.product->componentDocument(DefinitionId{200}) ==
          childDocument);
    CHECK(created.product->componentDocument(DefinitionId{300}) ==
          leafDocument);
    CHECK(created.product->definitionStateId(DefinitionId{200}).isValid());
    CHECK(created.product->rootPartDefinition() == nullptr);
    CHECK(created.product->rootAssemblyDefinition() == nullptr);
  }

  // Dangling targets and cycles fail before a ProductDocument is published.
  {
    std::vector<ComponentDefinition> dangling;
    dangling.push_back(makeComponent(DefinitionId{1}));
    addChild(dangling.front(), 1, DefinitionId{999});
    auto rejected = ProductDocument::createComponentGraph(
        std::move(dangling), DefinitionId{1});
    CHECK(!rejected);
    CHECK(rejected.status == ProductDocumentStatus::DanglingOccurrenceTarget);

    std::vector<ComponentDefinition> cycle;
    cycle.push_back(makeComponent(DefinitionId{1}));
    cycle.push_back(makeComponent(DefinitionId{2}));
    addChild(cycle[0], 1, DefinitionId{2});
    addChild(cycle[1], 1, DefinitionId{1});
    rejected = ProductDocument::createComponentGraph(
        std::move(cycle), DefinitionId{1});
    CHECK(!rejected);
    CHECK(rejected.status == ProductDocumentStatus::ComponentGraphCycle);
  }

  // Root and shared nested definitions retain one ordered drawing collection
  // regardless of how many occurrence paths instantiate the definition.
  {
    auto rootDocument = std::make_unique<Document>();
    rootDocument->addBody(BodyId{1}, "root body");
    ComponentDefinition root(
        DefinitionId{1}, std::move(rootDocument),
        std::vector<ComponentDrawing>{
            drawing(1, "root drawing", {BodyId{1}})});
    auto childDocument = std::make_unique<Document>();
    childDocument->addBody(BodyId{2}, "child body");
    ComponentDefinition child(
        DefinitionId{2}, std::move(childDocument),
        std::vector<ComponentDrawing>{
            drawing(2, "child drawing", {BodyId{2}})});
    addChild(root, 1, DefinitionId{2});
    addChild(root, 2, DefinitionId{2});
    std::vector<ComponentDefinition> graph;
    graph.push_back(std::move(root));
    graph.push_back(std::move(child));
    auto created = ProductDocument::createComponentGraph(
        std::move(graph), DefinitionId{1});
    CHECK(created);
    CHECK(created.product->rootComponentDefinition()->drawings().size() == 1);
    const auto* shared =
        created.product->definitions().findComponent(DefinitionId{2});
    CHECK(shared);
    CHECK(shared->drawings().size() == 1);
    CHECK(shared->findDrawing(DrawingId{2}) != nullptr);
    CHECK(created.product->rootComponentDefinition()->size() == 2);
  }

  // Drawing accounting is graph-wide. Each definition is locally within its
  // constructor limit, but the complete graph still fails closed.
  {
    auto missingDocument = std::make_unique<Document>();
    missingDocument->addBody(BodyId{1}, "present");
    ComponentDefinition missing(DefinitionId{40},
                                std::move(missingDocument));
    ComponentDrawingValidationTestAccess::appendUnchecked(
        missing, drawing(1, "missing source", {BodyId{2}}));
    std::vector<ComponentDefinition> missingGraph;
    missingGraph.push_back(std::move(missing));
    auto rejected = ProductDocument::createComponentGraph(
        std::move(missingGraph), DefinitionId{40});
    CHECK(!rejected);
    CHECK(rejected.status ==
          ProductDocumentStatus::DanglingComponentDrawingBody);

    ComponentDefinition duplicate(DefinitionId{41},
                                  std::make_unique<Document>());
    ComponentDrawingValidationTestAccess::appendUnchecked(
        duplicate, drawing(1, "first"));
    ComponentDrawingValidationTestAccess::appendUnchecked(
        duplicate, drawing(1, "duplicate"));
    std::vector<ComponentDefinition> duplicateGraph;
    duplicateGraph.push_back(std::move(duplicate));
    rejected = ProductDocument::createComponentGraph(
        std::move(duplicateGraph), DefinitionId{41});
    CHECK(!rejected);
    CHECK(rejected.status ==
          ProductDocumentStatus::DuplicateComponentDrawingId);

    const std::size_t firstCount = kComponentGraphMaximumDrawings / 2 + 1;
    const std::size_t secondCount =
        kComponentGraphMaximumDrawings - firstCount + 1;
    const auto many = [](DefinitionId definitionId, std::size_t count,
                         std::uint64_t idBase) {
      std::vector<ComponentDrawing> drawings;
      drawings.reserve(count);
      for (std::size_t index = 0; index < count; ++index)
        drawings.push_back(drawing(idBase + index, "bounded drawing"));
      return ComponentDefinition(definitionId, std::make_unique<Document>(),
                                 std::move(drawings));
    };
    std::vector<ComponentDefinition> graph;
    graph.push_back(many(DefinitionId{50}, firstCount, 1));
    graph.push_back(many(DefinitionId{51}, secondCount, 10000));
    rejected = ProductDocument::createComponentGraph(
        std::move(graph), DefinitionId{50});
    CHECK(!rejected);
    CHECK(rejected.status ==
          ProductDocumentStatus::ComponentDrawingBudgetExceeded);

    ComponentGraphView invalid;
    invalid.rootDefinitionId = DefinitionId{60};
    invalid.definitions[DefinitionId{60}].bodyIds = {BodyId{1}};
    invalid.definitions[DefinitionId{60}].drawings = {
        {DrawingId{1}, {BodyId{2}}}};
    CHECK(analyzeComponentGraph(invalid) ==
          ComponentGraphStatus::DanglingDrawingBody);
    invalid.definitions[DefinitionId{60}].drawings = {
        {DrawingId{1}, {}}, {DrawingId{1}, {}}};
    CHECK(analyzeComponentGraph(invalid) ==
          ComponentGraphStatus::DuplicateDrawingId);
  }

  // The detached analyzer preserves the exact typed graph-policy boundary,
  // including failures in authored definitions unreachable from the root.
  {
    std::vector<ComponentDefinition> valid;
    valid.push_back(makeComponent(DefinitionId{1}));
    valid.push_back(makeComponent(DefinitionId{2}));
    addChild(valid[0], 1, DefinitionId{2});
    CHECK(analyzeComponentGraph(valid, DefinitionId{1}) ==
          ComponentGraphStatus::Valid);
    CHECK(analyzeComponentGraph(valid, DefinitionId{999}) ==
          ComponentGraphStatus::MissingRootDefinition);
    CHECK(analyzeComponentGraph(
              valid, DefinitionId{1},
              ComponentGraphFailurePoint::BeforeValidationAllocation) ==
          ComponentGraphStatus::ResourceFailure);

    std::vector<ComponentDefinition> unreachableCycle;
    unreachableCycle.push_back(makeComponent(DefinitionId{10}));
    unreachableCycle.push_back(makeComponent(DefinitionId{20}));
    unreachableCycle.push_back(makeComponent(DefinitionId{30}));
    addChild(unreachableCycle[1], 1, DefinitionId{30});
    addChild(unreachableCycle[2], 1, DefinitionId{20});
    CHECK(analyzeComponentGraph(unreachableCycle, DefinitionId{10}) ==
          ComponentGraphStatus::Cycle);

    std::vector<ComponentDefinition> dangling;
    dangling.push_back(makeComponent(DefinitionId{40}));
    addChild(dangling[0], 1, DefinitionId{41});
    CHECK(analyzeComponentGraph(dangling, DefinitionId{40}) ==
          ComponentGraphStatus::DanglingTarget);
  }

  // Authored-occurrence accounting is graph-wide and exact at the limit.
  {
    std::vector<ComponentDefinition> authored;
    authored.push_back(makeComponent(DefinitionId{1}));
    authored.push_back(makeComponent(DefinitionId{2}));
    authored.push_back(makeComponent(DefinitionId{3}));
    for (std::size_t index = 0;
         index < kAssemblyGraphMaximumAuthoredOccurrences; ++index)
      addChild(authored[1], index + 1, DefinitionId{3});
    CHECK(analyzeComponentGraph(authored, DefinitionId{1}) ==
          ComponentGraphStatus::Valid);
    addChild(authored[1], kAssemblyGraphMaximumAuthoredOccurrences + 1,
             DefinitionId{3});
    CHECK(analyzeComponentGraph(authored, DefinitionId{1}) ==
          ComponentGraphStatus::OccurrenceBudgetExceeded);
  }

  // Graph-wide depth and definition budgets are bounded even for unreachable
  // authored definitions.
  {
    std::vector<ComponentDefinition> tooDeep;
    tooDeep.push_back(makeComponent(DefinitionId{999}));
    for (std::uint64_t index = 0;
         index < kAssemblyGraphMaximumPathDepth + 2; ++index)
      tooDeep.push_back(makeComponent(DefinitionId{1000 + index}));
    for (std::size_t index = 1; index + 1 < tooDeep.size(); ++index)
      addChild(tooDeep[index], 1, tooDeep[index + 1].id());
    CHECK(analyzeComponentGraph(tooDeep, DefinitionId{999}) ==
          ComponentGraphStatus::UnsupportedDepth);
    auto rejected = ProductDocument::createComponentGraph(
        std::move(tooDeep), DefinitionId{999});
    CHECK(!rejected);
    CHECK(rejected.status ==
          ProductDocumentStatus::ComponentGraphBudgetExceeded);

    std::vector<ComponentDefinition> tooMany;
    for (std::size_t index = 0;
         index < kAssemblyGraphMaximumDefinitions + 1; ++index)
      tooMany.push_back(
          makeComponent(DefinitionId{2000 + index}));
    CHECK(analyzeComponentGraph(tooMany, DefinitionId{2000}) ==
          ComponentGraphStatus::DefinitionBudgetExceeded);
    rejected = ProductDocument::createComponentGraph(
        std::move(tooMany), DefinitionId{2000});
    CHECK(!rejected);
    CHECK(rejected.status ==
          ProductDocumentStatus::ComponentGraphBudgetExceeded);
  }

  // A small authored DAG may still expand beyond the work budget through
  // repeated occurrences of shared child definitions.
  {
    std::vector<ComponentDefinition> exponential;
    constexpr std::size_t kLevels = 18;
    for (std::size_t index = 0; index < kLevels; ++index)
      exponential.push_back(
          makeComponent(DefinitionId{3000 + index}));
    for (std::size_t index = 0; index + 1 < exponential.size(); ++index) {
      addChild(exponential[index], 1, exponential[index + 1].id());
      addChild(exponential[index], 2, exponential[index + 1].id());
    }
    CHECK(analyzeComponentGraph(exponential, DefinitionId{3000}) ==
          ComponentGraphStatus::ExpandedWorkBudgetExceeded);
    auto rejected = ProductDocument::createComponentGraph(
        std::move(exponential), DefinitionId{3000});
    CHECK(!rejected);
    CHECK(rejected.status ==
          ProductDocumentStatus::ComponentGraphBudgetExceeded);
  }

  // Legacy constructors retain their exact Part/Assembly domain. A component
  // cannot be silently interpreted as a Part by assembly analysis.
  {
    DefinitionRegistry partRegistry;
    CHECK(partRegistry.insert(PartDefinition(
              DefinitionId{1}, std::make_unique<Document>())) ==
          DefinitionInsertStatus::Inserted);
    auto partProduct = ProductDocument::create(
        std::move(partRegistry), DefinitionId{1});
    CHECK(partProduct);
    CHECK(partProduct.product->rootDefinitionKind() == DefinitionKind::Part);

    DefinitionRegistry componentRegistry;
    CHECK(componentRegistry.insert(makeComponent(DefinitionId{10})) ==
          DefinitionInsertStatus::Inserted);
    const auto legacyAnalysis =
        analyzeAssemblyGraph(componentRegistry, DefinitionId{10});
    CHECK(legacyAnalysis.status ==
          AssemblyGraphStatus::UnsupportedDefinitionKind);
    auto legacyCreate = ProductDocument::create(
        std::move(componentRegistry), DefinitionId{10});
    CHECK(!legacyCreate);
    CHECK(legacyCreate.status ==
          ProductDocumentStatus::UnsupportedComponentDefinition);
  }

  return EXIT_SUCCESS;
}
