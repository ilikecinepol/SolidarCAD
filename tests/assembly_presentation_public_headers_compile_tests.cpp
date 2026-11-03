#include "presentation/AssemblyPresentation.h"
#include "presentation/AssemblyPresentationBuilder.h"
#include "presentation/AssemblyScenePublicationCoordinator.h"

#include <cstdlib>
#include <memory>
#include <type_traits>
#include <utility>

static_assert(!std::is_convertible_v<std::uint64_t,
                                     solidar::ProductPresentationEpoch>);
static_assert(!std::is_convertible_v<solidar::AssemblyMeshQuality,
                                     solidar::AssemblyMeshProfile>);
static_assert(!std::is_default_constructible_v<
              solidar::AssemblyPresentationSnapshot>);
static_assert(std::is_same_v<
              decltype(solidar::AssemblyPresentationBuilder::build(
                  std::declval<const solidar::ProductDocument&>(),
                  solidar::ProductPresentationEpoch{1},
                  solidar::AssemblyMeshProfile{
                      solidar::AssemblyMeshQuality::Normal})),
              solidar::AssemblyPresentationBuildResult>);
static_assert(std::is_same_v<
              decltype(std::declval<
                           const solidar::AssemblyPresentationBuildResult&>()
                           .snapshot),
              std::shared_ptr<
                  const solidar::AssemblyPresentationSnapshot>>);
static_assert(noexcept(
    std::declval<const solidar::AssemblyPresentationBuildResult&>()
        .diagnostic()));
static_assert(std::is_same_v<
              decltype(std::declval<
                           const solidar::AssemblyPresentationSnapshot&>()
                           .graphAnalysis()),
              const solidar::AssemblyGraphAnalysis&>);
static_assert(!std::is_same_v<solidar::AssemblyScenePublicationGeneration,
                              solidar::ProductPresentationEpoch>);
static_assert(!std::is_convertible_v<
              std::uint64_t, solidar::AssemblyScenePublicationGeneration>);
static_assert(std::is_copy_constructible_v<
              solidar::AssemblyScenePublicationHandle>);
static_assert(!std::is_copy_constructible_v<
              solidar::AssemblyScenePublicationCoordinator>);
static_assert(!std::is_move_constructible_v<
              solidar::AssemblyScenePublicationCoordinator>);
static_assert(noexcept(
    std::declval<solidar::AssemblyScenePublicationHandle>().isCurrent()));
static_assert(noexcept(std::declval<
                       solidar::AssemblyScenePublicationHandle>()
                           .snapshotIfCurrent()));
static_assert(noexcept(
    std::declval<solidar::AssemblyScenePublicationCoordinator&>().clear()));
static_assert(noexcept(
    std::declval<const solidar::AssemblyScenePublicationCoordinator&>()
        .active()));

int main() { return EXIT_SUCCESS; }
