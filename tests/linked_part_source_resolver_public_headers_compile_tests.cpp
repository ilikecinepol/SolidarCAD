#include "project/LinkedPartSourceResolver.h"

#include <type_traits>

static_assert(std::is_trivially_copyable_v<solidar::ExternalSourceKey>);
static_assert(!std::is_copy_constructible_v<
              solidar::project::ResolvedLinkedPartSource>);

int main() { return 0; }
