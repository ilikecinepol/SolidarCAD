#include "presentation/ComponentPresentation.h"
#include "presentation/ComponentPresentationBuilder.h"
#include "presentation/ComponentScenePublicationCoordinator.h"

#include <cstdlib>
#include <memory>
#include <type_traits>
#include <utility>

static_assert(std::is_trivially_copyable_v<
              solidar::ProductPresentationEpoch>);
static_assert(!std::is_default_constructible_v<
              solidar::ComponentPresentationSnapshot>);
static_assert(std::is_same_v<
              decltype(solidar::ComponentPresentationBuilder::build(
                  std::declval<const solidar::ProductDocument&>(),
                  solidar::ProductPresentationEpoch{1},
                  solidar::AssemblyMeshProfile{
                      solidar::AssemblyMeshQuality::Normal})),
              solidar::ComponentPresentationBuildResult>);
static_assert(std::is_same_v<
              decltype(std::declval<const solidar::ComponentInstanceAddress&>()
                           .occurrencePath),
              std::optional<solidar::OccurrencePath>>);
static_assert(std::is_same_v<
              decltype(std::declval<
                           const solidar::ComponentPresentationBuildResult&>()
                           .snapshot),
              std::shared_ptr<
                  const solidar::ComponentPresentationSnapshot>>);
static_assert(noexcept(
    std::declval<const solidar::ComponentPresentationBuildResult&>()
        .diagnostic()));
static_assert(std::is_same_v<
              decltype(std::declval<
                           const solidar::ComponentPresentationSnapshot&>()
                           .graphTraversal()),
              const solidar::ComponentGraphTraversal&>);
static_assert(std::is_same_v<
              decltype(std::declval<
                           const solidar::ComponentPresentationSnapshot&>()
                           .treeProjection()),
              const solidar::ComponentTreeProjection&>);
static_assert(std::is_same_v<
              decltype(std::declval<
                           solidar::ComponentScenePublicationCoordinator&>()
                           .active()),
              solidar::ComponentScenePublicationHandle>);
static_assert(std::is_same_v<
              decltype(std::declval<
                           const solidar::ComponentScenePublicationHandle&>()
                           .snapshotIfCurrent()),
              std::shared_ptr<
                  const solidar::ComponentPresentationSnapshot>>);

int main() { return EXIT_SUCCESS; }
