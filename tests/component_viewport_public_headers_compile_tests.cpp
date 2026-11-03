#include "ui/ComponentViewportPicking.h"
#include "ui/ComponentViewportSession.h"

#include <cstdlib>
#include <memory>
#include <type_traits>
#include <utility>

static_assert(std::is_same_v<
              decltype(std::declval<solidar::ComponentViewportPicking&>()
                           .publish(std::declval<std::shared_ptr<const
                                        solidar::ComponentViewportScene>>())),
              solidar::ComponentViewportPickingPublishResult>);
static_assert(std::is_same_v<
              decltype(solidar::ComponentViewportSession::prepare(
                  std::declval<const solidar::ProductDocument&>(),
                  solidar::ProductPresentationEpoch{1},
                  solidar::AssemblyMeshProfile{
                      solidar::AssemblyMeshQuality::Normal})),
              solidar::ComponentViewportSession::PrepareResult>);
static_assert(!std::is_copy_constructible_v<
              solidar::ComponentViewportSession>);

int main() { return EXIT_SUCCESS; }
