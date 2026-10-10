#pragma once

#include <Standard_Failure.hxx>

#include <exception>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

namespace solidar {

enum class GeometryFailureKind {
  None,
  InvalidInput,
  ResourceLimit,
  IncompleteDocument,
  InvalidBRep,
  MeshingFailed,
  InvalidMesh,
  PartialTransfer,
  IoFailure,
  OcctException,
  StandardException,
  UnknownException,
};

struct GeometryFailure {
  GeometryFailureKind kind{GeometryFailureKind::None};
  std::string detail;
};

// Common exception boundary for geometry adapters.  A callable may return
// bool (forwarded to the caller) or void (success when it returns normally).
// The failure kind is deliberately stable; detail is technical context and
// must not be used as a test oracle or shown directly as the public UI error.
template <typename Callable>
bool runGeometryOperation(Callable&& callable,
                          GeometryFailure* failure = nullptr) noexcept {
  if (failure) {
    failure->kind = GeometryFailureKind::None;
    failure->detail.clear();
  }
  try {
    if constexpr (std::is_void_v<std::invoke_result_t<Callable>>) {
      std::invoke(std::forward<Callable>(callable));
      return true;
    } else {
      return static_cast<bool>(std::invoke(std::forward<Callable>(callable)));
    }
  } catch (const Standard_Failure& exception) {
    if (failure) {
      failure->kind = GeometryFailureKind::OcctException;
      try {
        if (const char* detail = exception.what(); detail && *detail)
          failure->detail = detail;
      } catch (...) {
      }
    }
  } catch (const std::exception& exception) {
    if (failure) {
      failure->kind = GeometryFailureKind::StandardException;
      try {
        failure->detail = exception.what();
      } catch (...) {
      }
    }
  } catch (...) {
    if (failure)
      failure->kind = GeometryFailureKind::UnknownException;
  }
  return false;
}

} // namespace solidar
