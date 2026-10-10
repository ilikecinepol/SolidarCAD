#include "sketch/Sketch.h"
#include "sketch/SketchConstraintDiagnostics.h"
#include "sketch/SketchSolver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <vector>

namespace allocation_probe {

struct Counter {
  std::uint64_t id{};
  std::size_t events{};
  std::size_t bytes{};
  std::size_t currentBytes{};
  std::size_t peakBytes{};
};

struct Header {
  void* allocation{};
  std::size_t size{};
  std::uint64_t scopeId{};
};

thread_local Counter* activeCounter = nullptr;
thread_local std::uint64_t nextScopeId = 1;

class Scope final {
 public:
  explicit Scope(Counter& counter) noexcept
      : previous_(activeCounter) {
    counter.id = nextScopeId++;
    activeCounter = &counter;
  }
  ~Scope() { activeCounter = previous_; }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Counter* previous_{};
};

void* allocate(std::size_t size, std::size_t alignment) {
  size = std::max<std::size_t>(size, 1);
  alignment = std::max(alignment, alignof(std::max_align_t));
  if (size > std::numeric_limits<std::size_t>::max() - alignment -
                 sizeof(Header))
    throw std::bad_alloc();
  void* allocation = std::malloc(size + alignment + sizeof(Header));
  if (!allocation) throw std::bad_alloc();
  const auto unaligned = reinterpret_cast<std::uintptr_t>(allocation) +
                         sizeof(Header);
  const auto aligned = (unaligned + alignment - 1) & ~(alignment - 1);
  auto* header = reinterpret_cast<Header*>(aligned - sizeof(Header));
  header->allocation = allocation;
  header->size = size;
  header->scopeId = activeCounter ? activeCounter->id : 0;
  if (activeCounter) {
    ++activeCounter->events;
    activeCounter->bytes += size;
    activeCounter->currentBytes += size;
    activeCounter->peakBytes =
        std::max(activeCounter->peakBytes, activeCounter->currentBytes);
  }
  return reinterpret_cast<void*>(aligned);
}

void deallocate(void* pointer) noexcept {
  if (!pointer) return;
  auto* header = reinterpret_cast<Header*>(
      reinterpret_cast<std::uintptr_t>(pointer) - sizeof(Header));
  if (activeCounter && header->scopeId == activeCounter->id) {
    activeCounter->currentBytes -=
        std::min(activeCounter->currentBytes, header->size);
  }
  std::free(header->allocation);
}

}  // namespace allocation_probe

void* operator new(std::size_t size) {
  return allocation_probe::allocate(size, alignof(std::max_align_t));
}
void* operator new[](std::size_t size) {
  return allocation_probe::allocate(size, alignof(std::max_align_t));
}
void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocation_probe::allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return allocation_probe::allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try { return ::operator new[](size); } catch (...) { return nullptr; }
}
void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  try { return ::operator new(size, alignment); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  try { return ::operator new[](size, alignment); } catch (...) { return nullptr; }
}
void operator delete(void* pointer) noexcept {
  allocation_probe::deallocate(pointer);
}
void operator delete[](void* pointer) noexcept {
  allocation_probe::deallocate(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
  allocation_probe::deallocate(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
  allocation_probe::deallocate(pointer);
}
void operator delete(void* pointer, std::align_val_t) noexcept {
  allocation_probe::deallocate(pointer);
}
void operator delete[](void* pointer, std::align_val_t) noexcept {
  allocation_probe::deallocate(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
  allocation_probe::deallocate(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
  allocation_probe::deallocate(pointer);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
  allocation_probe::deallocate(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
  allocation_probe::deallocate(pointer);
}
void operator delete(void* pointer, std::align_val_t,
                     const std::nothrow_t&) noexcept {
  allocation_probe::deallocate(pointer);
}
void operator delete[](void* pointer, std::align_val_t,
                       const std::nothrow_t&) noexcept {
  allocation_probe::deallocate(pointer);
}

namespace {

using Clock = std::chrono::steady_clock;
using solidar::sketch::BasicSketchSolver;
using solidar::sketch::Constraint;
using solidar::sketch::ConstraintType;
using solidar::sketch::GeometryId;
using solidar::sketch::PointReference;
using solidar::sketch::Sketch;

std::uint64_t semanticHash(const Sketch& sketch) {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&hash](std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  for (const auto& line : sketch.lines()) {
    for (const double value : {line.start.xMm, line.start.yMm,
                               line.end.xMm, line.end.yMm}) {
      std::uint64_t bits{};
      static_assert(sizeof(bits) == sizeof(value));
      std::memcpy(&bits, &value, sizeof(bits));
      mix(bits);
    }
  }
  for (const auto& constraint : sketch.constraints()) mix(constraint.id);
  return hash;
}

Sketch makeSketch(std::size_t count, bool connected) {
  Sketch sketch;
  std::vector<Constraint> constraints;
  constraints.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const double x = static_cast<double>(index) * 2.0;
    sketch.addLine({x, 0.0}, {x + 1.0, 0.25});
    Constraint horizontal;
    horizontal.id = static_cast<solidar::sketch::ConstraintId>(index + 1);
    horizontal.type = ConstraintType::Horizontal;
    horizontal.firstGeometry = sketch.lineId(index);
    constraints.push_back(horizontal);
    if (connected && index > 0) {
      Constraint equal;
      equal.id = static_cast<solidar::sketch::ConstraintId>(count + index);
      equal.type = ConstraintType::Equal;
      equal.firstGeometry = sketch.lineId(index - 1);
      equal.secondGeometry = sketch.lineId(index);
      constraints.push_back(equal);
    }
  }
  static_cast<void>(sketch.restoreConstraints(std::move(constraints)));
  return sketch;
}

double percentile(std::vector<double> values, double p) {
  std::sort(values.begin(), values.end());
  const auto index = static_cast<std::size_t>(
      std::ceil(p * static_cast<double>(values.size())) - 1.0);
  return values[std::min(index, values.size() - 1)];
}

void run(std::size_t count, const std::string& mode, int samples,
         const std::string& implementation) {
  Sketch source = makeSketch(count, mode == "connected_drag");
  const bool historical = implementation == "historical";
  std::vector<double> elapsed;
  elapsed.reserve(static_cast<std::size_t>(samples));
  solidar::sketch::SolveResult last;
  std::uint64_t hash{};
  std::size_t retainedBytes{};
  for (int sample = 0; sample < samples; ++sample) {
    Sketch sketch = source;
    if (mode == "unchanged_solve")
      static_cast<void>(BasicSketchSolver::solveStable(sketch));
    allocation_probe::Counter allocations;
    const auto begin = Clock::now();
    {
      allocation_probe::Scope allocationScope(allocations);
      if (mode == "localized_drag") {
        last = BasicSketchSolver::translateThenSolveForBenchmark(
            sketch, {sketch.lineId(0)}, 0.1, 0.0, historical);
      } else if (mode == "connected_drag") {
        last = BasicSketchSolver::translateThenSolveForBenchmark(
            sketch, {sketch.lineId(0)}, 0.1, 0.0, historical);
      } else if (mode == "unchanged_solve") {
        last = historical
            ? BasicSketchSolver::solveStableHistoricalForBenchmark(sketch)
            : BasicSketchSolver::solveStable(sketch);
      } else {
        last = {};
        const auto diagnostics =
            solidar::sketch::analyzeConstraintSystem(sketch, true);
        last.violatedConstraints = diagnostics.violations.size();
        last.fullDiagnosticsCount = 1;
      }
    }
    const auto end = Clock::now();
    last.allocationCount = allocations.events;
    last.allocatedBytes = allocations.bytes;
    last.peakOwnedBytes = allocations.peakBytes;
    elapsed.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
    hash = semanticHash(sketch);
    retainedBytes = sketch.ownedBytes();
  }
  std::cout << implementation << ',' << count << ',' << mode << ',' << samples << ','
            << std::fixed << std::setprecision(6)
            << percentile(elapsed, 0.50) << ',' << percentile(elapsed, 0.95)
            << ',' << percentile(elapsed, 0.99) << ',' << last.passes << ','
            << last.componentsVisited << ',' << last.geometriesVisited << ','
            << last.constraintsVisited << ',' << last.lockedSnapshotSize << ','
            << last.fullDiagnosticsCount << ',' << last.allocationCount << ','
            << last.allocatedBytes << ',' << last.peakOwnedBytes << ','
            << retainedBytes << ',' << hash
            << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t count = argc > 1 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 100;
  const std::string mode = argc > 2 ? argv[2] : "localized_drag";
  const int samples = argc > 3 ? std::max(1, std::atoi(argv[3])) : 21;
  const std::string implementation = argc > 4 ? argv[4] : "optimized";
  if (implementation != "optimized" && implementation != "historical")
    return 2;
  std::cout << "implementation,entity_count,mode,samples,p50_ms,p95_ms,p99_ms,passes,components_visited,geometries_visited,constraints_visited,locked_snapshot_size,full_diagnostics_count,allocation_count,allocated_bytes,peak_owned_bytes,retained_bytes,semantic_hash\n";
  run(count, mode, samples, implementation);
}
