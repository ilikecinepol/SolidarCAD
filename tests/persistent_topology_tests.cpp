#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "model/SketchPlacement.h"
#include "model/TopologyReferenceResolver.h"

namespace {

class TestFailure final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition))                                                        \
      throw TestFailure(std::string(__FILE__) + ":" +                       \
                        std::to_string(__LINE__) + ": " #condition);         \
  } while (false)

double dot(solidar::TopologyPoint3d a, solidar::TopologyPoint3d b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

}  // namespace

int main() {
  try {
    constexpr solidar::BodyId bodyId = 101;
    constexpr solidar::FeatureId featureId = 202;
    const TopoDS_Shape original = BRepPrimAPI_MakeBox(80.0, 35.0, 50.0).Shape();
    const TopoDS_Shape resized = BRepPrimAPI_MakeBox(95.0, 35.0, 60.0).Shape();

    // A semantic planar extremum identifies the logical top face even when
    // dimensions and the legacy enumeration are no longer the identity.
    std::optional<solidar::FaceReference> top;
    for (std::size_t index = 0; index < 16; ++index) {
      auto reference = solidar::makeFaceReference(
          original, bodyId, featureId, index);
      if (reference.persistentTag == "planar:max-z") {
        top = std::move(reference);
        break;
      }
    }
    CHECK(top && top->signature);
    const auto resolvedTop =
        solidar::resolveFaceReference(resized, top->topology());
    CHECK(resolvedTop);
    CHECK(resolvedTop.method == solidar::TopologyMatchMethod::SemanticTag);
    const auto placement =
        solidar::resolveFacePlacement(resized, resolvedTop.index);
    CHECK(placement.planar);
    CHECK(placement.placement.normal().z > 0.99);
    CHECK(std::abs(placement.placement.origin.z - 60.0) < 1e-6);

    // A geometric edge signature follows a distinctive vertical corner edge
    // through an ordinary upstream dimension edit.
    std::optional<solidar::EdgeReference> vertical;
    for (std::size_t index = 0; index < 32; ++index) {
      auto reference = solidar::makeEdgeReference(
          original, bodyId, featureId, index);
      if (!reference.signature) break;
      const auto& signature = *reference.signature;
      if (std::abs(signature.tangent.z) > 0.99 &&
          signature.midpoint.x < 1e-6 && signature.midpoint.y < 1e-6) {
        vertical = std::move(reference);
        break;
      }
    }
    CHECK(vertical && vertical->signature);
    const auto resolvedVertical =
        solidar::resolveEdgeReference(resized, vertical->topology());
    CHECK(resolvedVertical);
    CHECK(resolvedVertical.method ==
          solidar::TopologyMatchMethod::GeometricSignature);

    // Old v2 references contain only an index and must retain their fallback.
    solidar::EdgeReference legacy{bodyId, featureId, 0};
    const auto resolvedLegacy =
        solidar::resolveEdgeReference(original, legacy.topology());
    CHECK(resolvedLegacy);
    CHECK(resolvedLegacy.method == solidar::TopologyMatchMethod::LegacyIndex);

    // Put a synthetic signature exactly between two equivalent parallel box
    // edges. The resolver must reject the tie instead of selecting iteration
    // order. This exercises safety independently of a particular edge index.
    std::vector<solidar::EdgeReference> edges;
    for (std::size_t index = 0; index < 32; ++index) {
      auto reference = solidar::makeEdgeReference(
          original, bodyId, featureId, index);
      if (!reference.signature) break;
      edges.push_back(std::move(reference));
    }
    std::optional<std::pair<std::size_t, std::size_t>> equivalent;
    for (std::size_t first = 0; first < edges.size() && !equivalent; ++first)
      for (std::size_t second = first + 1; second < edges.size(); ++second) {
        const auto& a = *edges[first].signature;
        const auto& b = *edges[second].signature;
        if (a.curve == b.curve &&
            std::abs(a.length - b.length) < 1e-7 &&
            std::abs(dot(a.tangent, b.tangent)) > 0.999) {
          equivalent = {{first, second}};
          break;
        }
      }
    CHECK(equivalent);
    auto ambiguous = edges[equivalent->first];
    const auto& other = *edges[equivalent->second].signature;
    ambiguous.signature->midpoint = {
        (ambiguous.signature->midpoint.x + other.midpoint.x) * 0.5,
        (ambiguous.signature->midpoint.y + other.midpoint.y) * 0.5,
        (ambiguous.signature->midpoint.z + other.midpoint.z) * 0.5};
    const auto ambiguity =
        solidar::resolveEdgeReference(original, ambiguous.topology());
    CHECK(!ambiguity);
    CHECK(ambiguity.error.find("ambiguous") != std::string::npos);

    auto impossible = vertical->topology();
    impossible.edgeSignature->midpoint = {10000.0, 10000.0, 10000.0};
    const auto missing = solidar::resolveEdgeReference(original, impossible);
    CHECK(!missing);
    CHECK(missing.error.find("no longer") != std::string::npos);
  } catch (const std::exception& error) {
    std::cerr << "persistent topology regression failure: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
