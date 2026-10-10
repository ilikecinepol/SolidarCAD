#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "model/Document.h"
#include "model/SketchPlacement.h"
#include "model/TopologyReferenceResolver.h"

namespace {

using Clock = std::chrono::steady_clock;

class BenchmarkFeature final : public solidar::ShapeFeature {
 public:
  explicit BenchmarkFeature(std::string name)
      : ShapeFeature(std::move(name)) {}

  [[nodiscard]] std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<BenchmarkFeature>(*this);
  }

  std::uint64_t rebuildCount{};

 protected:
  bool rebuildImpl(const solidar::RebuildContext&) override {
    ++rebuildCount;
    setShape(std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape()));
    markValid();
    return true;
  }
};

template <typename Operation>
long long timedMicroseconds(Operation&& operation) {
  const auto started = Clock::now();
  if (!operation()) throw std::runtime_error("benchmark operation failed");
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                               started)
      .count();
}

struct TimingSummary {
  long long p50{};
  long long p95{};
  long long p99{};
};

long long percentile(const std::vector<long long>& sorted, double fraction) {
  const auto ordinal = static_cast<std::size_t>(
      std::ceil(fraction * static_cast<double>(sorted.size())));
  return sorted[std::min(sorted.size() - 1, std::max<std::size_t>(1, ordinal) -
                                                    1)];
}

template <typename Operation>
TimingSummary sampleTimings(Operation&& operation) {
  constexpr std::size_t kWarmupCount = 5;
  constexpr std::size_t kSampleCount = 101;
  for (std::size_t warmup = 0; warmup < kWarmupCount; ++warmup)
    if (!operation()) throw std::runtime_error("benchmark warm-up failed");
  std::vector<long long> samples;
  samples.reserve(kSampleCount);
  for (std::size_t sample = 0; sample < kSampleCount; ++sample)
    samples.push_back(timedMicroseconds(operation));
  std::sort(samples.begin(), samples.end());
  return {percentile(samples, 0.50), percentile(samples, 0.95),
          percentile(samples, 0.99)};
}

void printSummary(const char* suite, std::size_t count, const char* scenario,
                  const TimingSummary& summary, std::uint64_t workUnits,
                  std::uint64_t topologyIndexBuilds) {
  std::cout << suite << ',' << count << ',' << scenario << ',' << summary.p50
            << ',' << summary.p95 << ',' << summary.p99 << ',' << workUnits
            << ',' << topologyIndexBuilds << '\n';
}

void benchmarkHistory(std::size_t featureCount) {
  solidar::Document document;
  auto& body = document.addBody("Benchmark body");
  BenchmarkFeature* first{};
  BenchmarkFeature* last{};
  std::vector<BenchmarkFeature*> features;
  features.reserve(featureCount);
  for (std::size_t index = 0; index < featureCount; ++index) {
    auto feature =
        std::make_unique<BenchmarkFeature>("Feature " + std::to_string(index));
    last = feature.get();
    if (!first) first = last;
    features.push_back(last);
    body.addFeature(std::move(feature));
  }
  if (!document.recompute())
    throw std::runtime_error("initial history build failed");

  const auto rebuildWork = [&] {
    std::uint64_t total = 0;
    for (const auto* feature : features) total += feature->rebuildCount;
    return total;
  };
  auto before = rebuildWork();
  const auto clean = sampleTimings([&] { return document.recompute(); });
  const auto cleanWork = rebuildWork() - before;
  before = rebuildWork();
  const auto leaf = sampleTimings([&] {
    last->setDirty();
    return document.recompute();
  });
  const auto leafWork = rebuildWork() - before;
  before = rebuildWork();
  const auto root = sampleTimings([&] {
    first->setDirty();
    return document.recompute();
  });
  const auto rootWork = rebuildWork() - before;

  printSummary("history", featureCount, "clean", clean, cleanWork, 0);
  printSummary("history", featureCount, "leaf", leaf, leafWork, 0);
  printSummary("history", featureCount, "root", root, rootWork, 0);
}

void benchmarkTopology(std::size_t referenceCount) {
  constexpr solidar::BodyId bodyId = 1;
  constexpr solidar::FeatureId featureId = 1;
  const TopoDS_Shape shape = BRepPrimAPI_MakeBox(80.0, 35.0, 50.0).Shape();
  const auto topologyIndex = solidar::TopologyIndex::build(shape);
  if (!topologyIndex)
    throw std::runtime_error("topology benchmark index is invalid");
  const auto face = topologyIndex->createFaceReference(bodyId, featureId, 0);
  const auto edge = topologyIndex->createEdgeReference(bodyId, featureId, 0);
  if (!face || !edge)
    throw std::runtime_error("topology benchmark fixture is invalid");

  const auto faceReference = face.reference.topology();
  const auto edgeReference = edge.reference.topology();
  auto beforeBuilds = solidar::TopologyIndex::buildAttemptCount();
  const auto faces = sampleTimings([&] {
    for (std::size_t sample = 0; sample < referenceCount; ++sample)
      if (!solidar::resolveFaceReference(*topologyIndex, faceReference))
        return false;
    return true;
  });
  const auto faceBuilds =
      solidar::TopologyIndex::buildAttemptCount() - beforeBuilds;
  beforeBuilds = solidar::TopologyIndex::buildAttemptCount();
  const auto edges = sampleTimings([&] {
    for (std::size_t sample = 0; sample < referenceCount; ++sample)
      if (!solidar::resolveEdgeReference(*topologyIndex, edgeReference))
        return false;
    return true;
  });
  const auto edgeBuilds =
      solidar::TopologyIndex::buildAttemptCount() - beforeBuilds;

  constexpr std::uint64_t measuredRuns = 5 + 101;
  printSummary("topology", referenceCount, "face", faces,
               measuredRuns * referenceCount, faceBuilds);
  printSummary("topology", referenceCount, "edge", edges,
               measuredRuns * referenceCount, edgeBuilds);
}

}  // namespace

int main() {
  try {
    std::cout << "suite,count,scenario,p50_us,p95_us,p99_us,work_units,"
                 "topology_index_builds\n";
    for (const std::size_t count : std::array<std::size_t, 3>{10, 100, 1000})
      benchmarkHistory(count);
    for (const std::size_t count : std::array<std::size_t, 3>{10, 100, 1000})
      benchmarkTopology(count);
  } catch (const std::exception& error) {
    std::cerr << "stage4 benchmark setup failure: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
