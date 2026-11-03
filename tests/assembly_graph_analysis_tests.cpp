#include "TestAssertions.h"

#include <cmath>
#include <memory>
#include <limits>
#include <vector>

#include "model/AssemblyGraphAnalysis.h"

namespace {
using namespace solidar;

AssemblyGraphOccurrence edge(std::uint64_t occurrence,
                             std::uint64_t target,
                             RigidTransform3d transform =
                                 RigidTransform3d::identity(),
                             OccurrenceFlags flags = {},
                             std::string displayNameUtf8 = {}) {
  return {OccurrenceId{occurrence}, DefinitionId{target}, transform, flags,
          std::move(displayNameUtf8)};
}

AssemblyGraphDefinition assembly(
    std::initializer_list<AssemblyGraphOccurrence> occurrences) {
  return {DefinitionKind::Assembly, occurrences};
}

AssemblyGraphDefinition part() { return {DefinitionKind::Part, {}}; }

AssemblyGraphAnalysis inspect(AssemblyGraphView graph) {
  return analyzeAssemblyGraph(graph);
}

void expectCycle(AssemblyGraphView graph) {
  CHECK(inspect(std::move(graph)).status == AssemblyGraphStatus::Cycle);
}

bool near(double left, double right) {
  return std::abs(left - right) < 1.0e-10;
}

}  // namespace

int main() {
  CHECK(fitsAssemblyGraphBudget(0, kAssemblyGraphMaximumExpandedVisits,
                                kAssemblyGraphMaximumExpandedVisits));
  CHECK(!fitsAssemblyGraphBudget(kAssemblyGraphMaximumExpandedVisits, 1,
                                 kAssemblyGraphMaximumExpandedVisits));
  CHECK(!fitsAssemblyGraphBudget(
      std::numeric_limits<std::size_t>::max(), 1,
      kAssemblyGraphMaximumExpandedVisits));
  {
    AssemblyGraphView self{DefinitionId{1},
                           {{DefinitionId{1}, assembly({edge(1, 1)})}}};
    expectCycle(std::move(self));
    AssemblyGraphView pair{
        DefinitionId{1},
        {{DefinitionId{1}, assembly({edge(1, 2)})},
         {DefinitionId{2}, assembly({edge(1, 1)})}}};
    expectCycle(std::move(pair));
    AssemblyGraphView triple{
        DefinitionId{1},
        {{DefinitionId{1}, assembly({edge(1, 2)})},
         {DefinitionId{2}, assembly({edge(1, 3)})},
         {DefinitionId{3}, assembly({edge(1, 1)})}}};
    expectCycle(std::move(triple));
    AssemblyGraphView orphan{
        DefinitionId{1},
        {{DefinitionId{1}, assembly({edge(1, 9)})},
         {DefinitionId{2}, assembly({edge(1, 3)})},
         {DefinitionId{3}, assembly({edge(1, 2)})},
         {DefinitionId{9}, part()}}};
    expectCycle(std::move(orphan));
  }

  {
    AssemblyGraphView diamond{
        DefinitionId{1},
        {{DefinitionId{1}, assembly({edge(5, 2), edge(6, 3)})},
         {DefinitionId{2}, assembly({edge(7, 4)})},
         {DefinitionId{3}, assembly({edge(7, 4)})},
         {DefinitionId{4}, part()}}};
    const auto analysis = inspect(std::move(diamond));
    CHECK(analysis);
    CHECK(analysis.leaves.size() == 2);
    CHECK(analysis.visits.size() == 4);
    CHECK(analysis.visits[0].path.entries() ==
          std::vector<OccurrenceId>({OccurrenceId{5}}));
    CHECK(analysis.visits[0].owningAssemblyDefinitionId == DefinitionId{1});
    CHECK(analysis.visits[0].targetDefinitionId == DefinitionId{2});
    CHECK(analysis.visits[0].targetKind == DefinitionKind::Assembly);
    CHECK(!analysis.visits[0].terminalPart());
    CHECK(analysis.visits[1].path.entries() ==
          std::vector<OccurrenceId>({OccurrenceId{5}, OccurrenceId{7}}));
    CHECK(analysis.visits[1].owningAssemblyDefinitionId == DefinitionId{2});
    CHECK(analysis.visits[1].targetKind == DefinitionKind::Part);
    CHECK(analysis.visits[1].terminalPart());
    CHECK(analysis.visits[2].path.entries() ==
          std::vector<OccurrenceId>({OccurrenceId{6}}));
    CHECK(analysis.visits[3].path.entries() ==
          std::vector<OccurrenceId>({OccurrenceId{6}, OccurrenceId{7}}));
    CHECK(analysis.leaves[0].path.entries() ==
          std::vector<OccurrenceId>({OccurrenceId{5}, OccurrenceId{7}}));
    CHECK(analysis.leaves[1].path.entries() ==
          std::vector<OccurrenceId>({OccurrenceId{6}, OccurrenceId{7}}));
    CHECK(analysis.uniqueSources == 1);
    CHECK(analysis.reachableSources == 1);
    const auto resourceFailure = analyzeAssemblyGraph(
        AssemblyGraphView{
            DefinitionId{1},
            {{DefinitionId{1}, assembly({edge(1, 4)})},
             {DefinitionId{4}, part()}}},
        AssemblyGraphFailurePoint::BeforeLeafPathAllocation);
    CHECK(resourceFailure.status == AssemblyGraphStatus::ResourceFailure);
    CHECK(resourceFailure.leaves.empty());
    CHECK(resourceFailure.childFirstAssemblyOrder.empty());
    AssemblyGraphView permuted;
    permuted.rootDefinitionId = DefinitionId{1};
    permuted.definitions.emplace(DefinitionId{4}, part());
    permuted.definitions.emplace(DefinitionId{3}, assembly({edge(7, 4)}));
    permuted.definitions.emplace(DefinitionId{1},
                                 assembly({edge(5, 2), edge(6, 3)}));
    permuted.definitions.emplace(DefinitionId{2}, assembly({edge(7, 4)}));
    const auto again = inspect(std::move(permuted));
    CHECK(again);
    CHECK(again.childFirstAssemblyOrder ==
          analysis.childFirstAssemblyOrder);
    CHECK(again.leaves.size() == analysis.leaves.size());
    CHECK(again.visits.size() == analysis.visits.size());
    for (std::size_t i = 0; i < analysis.leaves.size(); ++i)
      CHECK(again.leaves[i].path == analysis.leaves[i].path);
    for (std::size_t i = 0; i < analysis.visits.size(); ++i)
      CHECK(again.visits[i].path == analysis.visits[i].path);
  }

  {
    const auto rz = RigidTransform3d::create(
        {10.0, 0.0, 0.0},
        {0.7071067811865476, 0.0, 0.0, 0.7071067811865476});
    const auto tx = RigidTransform3d::create({2.0, 0.0, 0.0}, {1, 0, 0, 0});
    const auto ty = RigidTransform3d::create({0.0, 3.0, 0.0}, {1, 0, 0, 0});
    CHECK(rz && tx && ty);
    const auto parentLocal = RigidTransform3d::compose(*rz.transform,
                                                       *tx.transform);
    const auto localParent = RigidTransform3d::compose(*tx.transform,
                                                       *rz.transform);
    CHECK(parentLocal && localParent);
    CHECK(parentLocal.transform->translationMm() !=
          localParent.transform->translationMm());
    AssemblyGraphView chain{
        DefinitionId{1},
        {{DefinitionId{1},
          assembly({edge(5, 2, *rz.transform, {false, false, true},
                         "верхняя сборка")})},
         {DefinitionId{2},
          assembly({edge(7, 3, *tx.transform, {true, true, false})})},
         {DefinitionId{3},
          assembly({edge(7, 4, *ty.transform, {true, false, true})})},
         {DefinitionId{4}, part()}}};
    const auto analysis = inspect(std::move(chain));
    CHECK(analysis && analysis.leaves.size() == 1);
    CHECK(analysis.visits.size() == 3);
    CHECK(analysis.visits[0].displayNameUtf8 == "верхняя сборка");
    CHECK(!analysis.visits[0].effectiveVisible);
    CHECK(!analysis.visits[0].effectiveSuppressed);
    CHECK(!analysis.visits[1].effectiveVisible);
    CHECK(analysis.visits[1].effectiveSuppressed);
    CHECK(!analysis.visits[2].effectiveVisible);
    CHECK(analysis.visits[2].effectiveSuppressed);
    const auto& leaf = analysis.leaves.front();
    CHECK(leaf.path.entries() ==
          std::vector<OccurrenceId>({OccurrenceId{5}, OccurrenceId{7},
                                     OccurrenceId{7}}));
    CHECK(!leaf.effectiveVisible);
    CHECK(leaf.effectiveSuppressed);
    CHECK(leaf.authoredLeafFlags.visible);
    CHECK(!leaf.authoredLeafFlags.suppressed);
    CHECK(leaf.authoredLeafFlags.grounded);
    CHECK(near(leaf.worldTransform.translationMm()[0], 7.0));
    CHECK(near(leaf.worldTransform.translationMm()[1], 2.0));
  }

  {
    AssemblyGraphView dangling{DefinitionId{1},
                               {{DefinitionId{1},
                                 assembly({edge(1, 99)})}}};
    CHECK(inspect(std::move(dangling)).status ==
          AssemblyGraphStatus::DanglingTarget);
    AssemblyGraphView badRoot{DefinitionId{2},
                              {{DefinitionId{1}, part()}}};
    CHECK(inspect(std::move(badRoot)).status ==
          AssemblyGraphStatus::InvalidRoot);
    AssemblyGraphView partRoot{DefinitionId{1},
                               {{DefinitionId{1}, part()}}};
    CHECK(inspect(std::move(partRoot)).status ==
          AssemblyGraphStatus::RootIsNotAssembly);
  }

  {
    AssemblyGraphView exact;
    exact.rootDefinitionId = DefinitionId{1};
    for (std::size_t depth = 1;
         depth <= kAssemblyGraphMaximumPathDepth; ++depth) {
      exact.definitions.emplace(
          DefinitionId{depth},
          assembly({edge(1, depth == kAssemblyGraphMaximumPathDepth
                                ? 100
                                : depth + 1)}));
    }
    exact.definitions.emplace(DefinitionId{100}, part());
    CHECK(inspect(exact));
    auto tooDeep = exact;
    tooDeep.definitions[DefinitionId{kAssemblyGraphMaximumPathDepth}]
        .occurrences.front().targetDefinitionId = DefinitionId{99};
    tooDeep.definitions.emplace(DefinitionId{99},
                                assembly({edge(1, 100)}));
    CHECK(inspect(std::move(tooDeep)).status ==
          AssemblyGraphStatus::UnsupportedDepth);
    AssemblyGraphView orphanTooDeep;
    orphanTooDeep.rootDefinitionId = DefinitionId{200};
    orphanTooDeep.definitions.emplace(DefinitionId{200}, assembly({}));
    for (std::size_t depth = 1;
         depth <= kAssemblyGraphMaximumPathDepth + 1; ++depth) {
      orphanTooDeep.definitions.emplace(
          DefinitionId{depth},
          assembly({edge(1, depth == kAssemblyGraphMaximumPathDepth + 1
                                ? 100
                                : depth + 1)}));
    }
    orphanTooDeep.definitions.emplace(DefinitionId{100}, part());
    CHECK(inspect(std::move(orphanTooDeep)).status ==
          AssemblyGraphStatus::UnsupportedDepth);
    auto pathEntries = std::vector<OccurrenceId>(
        kAssemblyGraphMaximumPathDepth, OccurrenceId{1});
    CHECK(OccurrencePath::create(pathEntries));
    pathEntries.push_back(OccurrenceId{1});
    CHECK(OccurrencePath::create(std::move(pathEntries)).status ==
          OccurrencePathStatus::UnsupportedDepth);
  }

  {
    AssemblyGraphView exactOccurrences;
    exactOccurrences.rootDefinitionId = DefinitionId{1};
    auto& root = exactOccurrences.definitions[DefinitionId{1}];
    root.kind = DefinitionKind::Assembly;
    root.occurrences.reserve(kAssemblyGraphMaximumAuthoredOccurrences);
    for (std::size_t i = 1; i <= kAssemblyGraphMaximumAuthoredOccurrences; ++i)
      root.occurrences.push_back(edge(i, 2));
    exactOccurrences.definitions.emplace(DefinitionId{2}, part());
    CHECK(inspect(exactOccurrences));
    root.occurrences.push_back(
        edge(kAssemblyGraphMaximumAuthoredOccurrences + 1, 2));
    CHECK(inspect(std::move(exactOccurrences)).status ==
          AssemblyGraphStatus::OccurrenceBudgetExceeded);
  }

  {
    AssemblyGraphView exactDefinitions;
    exactDefinitions.rootDefinitionId = DefinitionId{1};
    exactDefinitions.definitions.emplace(DefinitionId{1}, assembly({}));
    for (std::size_t id = 2; id <= kAssemblyGraphMaximumDefinitions; ++id)
      exactDefinitions.definitions.emplace(DefinitionId{id}, assembly({}));
    CHECK(inspect(exactDefinitions));
    exactDefinitions.definitions.emplace(
        DefinitionId{kAssemblyGraphMaximumDefinitions + 1}, part());
    CHECK(inspect(std::move(exactDefinitions)).status ==
          AssemblyGraphStatus::DefinitionBudgetExceeded);
  }

  {
    AssemblyGraphView sources;
    sources.rootDefinitionId = DefinitionId{1};
    sources.definitions.emplace(DefinitionId{1}, assembly({}));
    for (std::size_t id = 2; id <= kAssemblyGraphMaximumUniqueSources + 1;
         ++id)
      sources.definitions.emplace(DefinitionId{id}, part());
    const auto exactSources = inspect(sources);
    CHECK(exactSources);
    CHECK(exactSources.uniqueSources == kAssemblyGraphMaximumUniqueSources);
    CHECK(exactSources.reachableSources == 0);
    sources.definitions.emplace(
        DefinitionId{kAssemblyGraphMaximumUniqueSources + 2}, part());
    CHECK(inspect(std::move(sources)).status ==
          AssemblyGraphStatus::UniqueSourceBudgetExceeded);
  }

  {
    // A shared binary DAG expands to 32768 lexicographic leaves while using
    // only 30 authored edges. One additional direct leaf crosses only the
    // leaf-instance budget.
    AssemblyGraphView leaves;
    leaves.rootDefinitionId = DefinitionId{1};
    for (std::uint64_t id = 1; id <= 15; ++id)
      leaves.definitions.emplace(
          DefinitionId{id},
          assembly({edge(1, id == 15 ? 100 : id + 1),
                    edge(2, id == 15 ? 100 : id + 1)}));
    leaves.definitions.emplace(DefinitionId{100}, part());
    CHECK(inspect(leaves));
    leaves.definitions.at(DefinitionId{1}).occurrences.push_back(edge(3, 100));
    CHECK(inspect(std::move(leaves)).status ==
          AssemblyGraphStatus::LeafBudgetExceeded);
  }

  {
    // The same shared DAG may terminate in an empty assembly, producing no
    // leaves. This isolates the expanded-work exact/+1 boundary.
    AssemblyGraphView work;
    work.rootDefinitionId = DefinitionId{1};
    for (std::uint64_t id = 1; id <= 15; ++id)
      work.definitions.emplace(
          DefinitionId{id},
          assembly({edge(1, id == 15 ? 99 : id + 1),
                    edge(2, id == 15 ? 99 : id + 1)}));
    work.definitions.emplace(DefinitionId{99}, assembly({}));
    work.definitions.emplace(DefinitionId{100}, part());
    work.definitions.at(DefinitionId{1}).occurrences.push_back(edge(3, 100));
    work.definitions.at(DefinitionId{1}).occurrences.push_back(edge(4, 100));
    const auto exactWork = inspect(work);
    CHECK(exactWork);
    CHECK(exactWork.expandedVisits == kAssemblyGraphMaximumExpandedVisits);
    work.definitions.at(DefinitionId{1}).occurrences.push_back(edge(5, 100));
    CHECK(inspect(std::move(work)).status ==
          AssemblyGraphStatus::ExpandedWorkBudgetExceeded);
  }

  {
    std::vector<PartDefinition> parts;
    parts.emplace_back(DefinitionId{4}, std::make_unique<Document>());
    std::vector<AssemblyDefinition> assemblies;
    AssemblyDefinition child(DefinitionId{2});
    CHECK(child.insert(ComponentOccurrence(OccurrenceId{7}, DefinitionId{4},
                                           RigidTransform3d::identity())) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    AssemblyDefinition root(DefinitionId{1});
    CHECK(root.insert(ComponentOccurrence(OccurrenceId{5}, DefinitionId{2},
                                          RigidTransform3d::identity())) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    // Intentionally child before root: construction accepts forward refs and
    // publishes only after whole-registry validation.
    assemblies.push_back(std::move(child));
    assemblies.push_back(std::move(root));
    auto product = ProductDocument::createNested(
        std::move(parts), std::move(assemblies), DefinitionId{1});
    CHECK(product);
    CHECK(product.product->definitions().findAssembly(DefinitionId{2}));
    const auto parentPath = OccurrencePath::create({OccurrenceId{5}});
    const auto leafPath = OccurrencePath::create(
        {OccurrenceId{5}, OccurrenceId{7}});
    const auto stalePath = OccurrencePath::create(
        {OccurrenceId{5}, OccurrenceId{8}});
    const auto nonterminalPath = OccurrencePath::create(
        {OccurrenceId{5}, OccurrenceId{7}, OccurrenceId{1}});
    CHECK(parentPath && leafPath && stalePath && nonterminalPath);
    const auto parent = product.product->resolveOccurrence(*parentPath.path);
    CHECK(parent);
    CHECK(parent.ownerAssembly ==
          product.product->definitions().findAssembly(DefinitionId{1}));
    CHECK(parent.targetAssembly ==
          product.product->definitions().findAssembly(DefinitionId{2}));
    CHECK(!parent.targetPart && !parent.terminalPart());
    const auto leaf = product.product->resolveOccurrence(*leafPath.path);
    CHECK(leaf && leaf.terminalPart());
    CHECK(leaf.ownerAssembly ==
          product.product->definitions().findAssembly(DefinitionId{2}));
    CHECK(leaf.occurrence->id() == OccurrenceId{7});
    CHECK(leaf.targetPart ==
          product.product->definitions().findPart(DefinitionId{4}));
    CHECK(product.product->resolveOccurrence(*stalePath.path).status ==
          RecursiveOccurrenceResolveStatus::OccurrenceMissing);
    CHECK(product.product->resolveOccurrence(*nonterminalPath.path).status ==
          RecursiveOccurrenceResolveStatus::IntermediateTargetIsPart);
    CHECK(product.product->remove(DefinitionId{2}) ==
          ProductDocumentRemoveStatus::DefinitionRemovalUnsupported);
  }

  {
    AssemblyDefinition cyclic(DefinitionId{1});
    CHECK(cyclic.insert(ComponentOccurrence(
              OccurrenceId{1}, DefinitionId{1},
              RigidTransform3d::identity())) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    std::vector<AssemblyDefinition> assemblies;
    assemblies.push_back(std::move(cyclic));
    auto rejected = ProductDocument::createNested(
        {}, std::move(assemblies), DefinitionId{1});
    CHECK(!rejected);
    CHECK(rejected.status == ProductDocumentStatus::AssemblyGraphCycle);
  }

  return EXIT_SUCCESS;
}
