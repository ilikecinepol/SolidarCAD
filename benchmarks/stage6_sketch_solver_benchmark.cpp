#include "sketch/Sketch.h"
#include "sketch/SketchConstraintDiagnostics.h"
#include "sketch/SketchSolver.h"
#include "sketch/EquationKernel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
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

namespace solidar::sketch {

// Benchmark fixtures must measure the solve/assembly selected by the mode,
// not spend setup time solving every exact constraint through the persistence
// boundary. This is the same friend seam used by deterministic model tests.
class SketchTestAccess final {
 public:
  static void installConstraints(Sketch& sketch,
                                 std::vector<Constraint> constraints) {
    sketch.constraints_ = std::move(constraints);
    sketch.nextConstraintId_ = 1;
    for (const auto& constraint : sketch.constraints_)
      sketch.nextConstraintId_ =
          std::max(sketch.nextConstraintId_, constraint.id + 1);
    sketch.invalidateStructureIndexes();
  }
};

}  // namespace solidar::sketch

namespace {

using Clock = std::chrono::steady_clock;
using solidar::sketch::SketchSolver;
using solidar::sketch::Constraint;
using solidar::sketch::ConstraintId;
using solidar::sketch::ConstraintType;
using solidar::sketch::GeometryId;
using solidar::sketch::PointReference;
using solidar::sketch::Sketch;

std::uint64_t semanticHash(const Sketch& sketch) {
  return sketch.semanticFingerprint();
}

Sketch makeSketch(std::size_t count, bool connected) {
  Sketch sketch;
  std::vector<Constraint> constraints;
  constraints.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const double x = static_cast<double>(index) * 2.0;
    // R2 built the same sloped source and normalized it before the timer. Keep
    // the resulting committed pose directly: this preserves the timed fixture
    // and semantic hashes without paying the obsolete Solver-v1 setup cost.
    sketch.addLine({x, 0.0}, {x + 1.0, 0.0});
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
  solidar::sketch::SketchTestAccess::installConstraints(
      sketch, std::move(constraints));
  return sketch;
}

Sketch makeTangencySketch(std::size_t count) {
  Sketch sketch;
  std::vector<Constraint> constraints;
  ConstraintId nextId = 1;
  std::size_t created = 0;
  for (std::size_t pair = 0; created + 1 < count; ++pair) {
    // Keep coordinates numerically local; overlapping independent components
    // are intentional and must not become topology without a constraint.
    const double x = static_cast<double>(pair % 8) * 24.0;
    GeometryId first = solidar::sketch::kInvalidGeometryId;
    GeometryId second = solidar::sketch::kInvalidGeometryId;
    switch (pair % 4) {
      case 0:
        sketch.addLine({x - 8.0, 0.0}, {x + 8.0, 0.0});
        sketch.addCircle({x, 5.0}, 5.0);
        first = sketch.lineId(sketch.lines().size() - 1);
        second = sketch.circleId(sketch.circles().size() - 1);
        break;
      case 1:
        sketch.addLine({x + 5.0, -5.0}, {x + 5.0, 5.0});
        sketch.addArc({x, 0.0}, 5.0, -0.5, 1.0);
        first = sketch.lineId(sketch.lines().size() - 1);
        second = sketch.arcId(sketch.arcs().size() - 1);
        break;
      case 2: {
        constexpr double diagonal = 3.5355339059327378;
        sketch.addArc({x, 0.0}, 5.0, 0.0,
                      0.5 * 3.14159265358979323846);
        sketch.addBezier({x + diagonal, diagonal},
                         {x + diagonal - 1.0, diagonal + 1.0},
                         {x + 1.5, 5.0}, {x + 0.5, 5.0});
        first = sketch.arcId(sketch.arcs().size() - 1);
        second = sketch.bezierId(sketch.beziers().size() - 1);
        break;
      }
      default:
        sketch.addBezier({x, 0.0}, {x + 1.0, 0.0},
                         {x + 2.0, 1.0}, {x + 3.0, 1.0});
        sketch.addBezier({x, 0.0}, {x - 1.0, 0.0},
                         {x - 2.0, -1.0}, {x - 3.0, -1.0});
        first = sketch.bezierId(sketch.beziers().size() - 2);
        second = sketch.bezierId(sketch.beziers().size() - 1);
        break;
    }
    constraints.emplace_back(solidar::sketch::TangentConstraint{first, second},
                             solidar::sketch::ConstraintMode::Driving,
                             nextId++);
    created += 2;
  }
  if (created < count)
    sketch.addLine({0.0, 20.0}, {10.0, 20.0});
  solidar::sketch::SketchTestAccess::installConstraints(
      sketch, std::move(constraints));
  return sketch;
}

Sketch makeBezierSketch(std::size_t count) {
  Sketch sketch;
  std::vector<Constraint> constraints;
  ConstraintId nextId = 1;
  std::size_t created = 0;
  for (std::size_t pair = 0; created + 1 < count; ++pair) {
    const double x = static_cast<double>(pair % 8) * 12.0;
    GeometryId first = solidar::sketch::kInvalidGeometryId;
    GeometryId second = solidar::sketch::kInvalidGeometryId;
    if ((pair % 2) == 0) {
      sketch.addLine({x, 0.0}, {x + 10.0, 0.0});
      sketch.addBezier({x + 5.0, 0.0}, {x + 6.0, 0.0},
                       {x + 7.0, 1.0}, {x + 8.0, 1.0});
      first = sketch.lineId(sketch.lines().size() - 1);
      second = sketch.bezierId(sketch.beziers().size() - 1);
    } else {
      sketch.addBezier({x, 0.0}, {x + 1.0, 0.0},
                       {x + 2.0, 1.0}, {x + 3.0, 1.0});
      sketch.addBezier({x, 0.0}, {x - 1.0, 0.0},
                       {x - 2.0, -1.0}, {x - 3.0, -1.0});
      first = sketch.bezierId(sketch.beziers().size() - 2);
      second = sketch.bezierId(sketch.beziers().size() - 1);
    }
    constraints.emplace_back(solidar::sketch::TangentConstraint{first, second},
                             solidar::sketch::ConstraintMode::Driving,
                             nextId++);
    created += 2;
  }
  if (created < count)
    sketch.addBezier({0.0, 20.0}, {1.0, 20.0},
                     {2.0, 21.0}, {3.0, 21.0});
  solidar::sketch::SketchTestAccess::installConstraints(
      sketch, std::move(constraints));
  return sketch;
}

double percentile(std::vector<double> values, double p) {
  std::sort(values.begin(), values.end());
  const auto index = static_cast<std::size_t>(
      std::ceil(p * static_cast<double>(values.size())) - 1.0);
  return values[std::min(index, values.size() - 1)];
}

void run(std::size_t count, const std::string& mode, int samples) {
  Sketch source = mode == "tangency_diagnostics"
                      ? makeTangencySketch(count)
                  : mode == "bezier_diagnostics" ? makeBezierSketch(count)
                                                   : makeSketch(
                                                         count,
                                                         mode == "connected_drag");
  const auto fixtureGraph = solidar::sketch::ConstraintGraph::build(source);
  std::size_t fixtureParameters = 0;
  std::size_t fixtureEquations = 0;
  for (const auto& component : fixtureGraph.components()) {
    const auto system =
        solidar::sketch::ConstraintEquationRegistry::assemble(source, component);
    fixtureParameters += system.variableCount();
    fixtureEquations += system.equationCount();
  }
  // The source is the canonical committed pose produced by R2 fixture setup.
  // Each measured drag opens its own press-time journal in the helper below.
  std::vector<double> elapsed;
  elapsed.reserve(static_cast<std::size_t>(samples));
  solidar::sketch::SolveResult last;
  std::uint64_t hash{};
  std::optional<std::uint64_t> expectedHash;
  std::size_t retainedBytes{};
  for (int sample = 0; sample < samples; ++sample) {
    Sketch sketch = source;
    if (mode == "unchanged_solve")
      static_cast<void>(SketchSolver::solveStable(sketch));
    allocation_probe::Counter allocations;
    const auto begin = Clock::now();
    {
      allocation_probe::Scope allocationScope(allocations);
      if (mode == "localized_drag") {
        last = SketchSolver::translateThenSolveForBenchmark(
            sketch, {sketch.lineId(0)}, 0.1, 0.0);
      } else if (mode == "connected_drag") {
        last = SketchSolver::translateThenSolveForBenchmark(
            sketch, {sketch.lineId(0)}, 0.1, 0.0);
      } else if (mode == "unchanged_solve") {
        last = SketchSolver::solveStable(sketch);
      } else if (mode.ends_with("_diagnostics")) {
        last = {};
        const auto diagnostics =
            solidar::sketch::analyzeConstraintSystem(sketch, true);
        last.status = diagnostics.status;
        last.converged = !diagnostics.conflicting;
        last.violatedConstraints = diagnostics.violations.size();
        last.maxNormalizedResidual = diagnostics.maxNormalizedResidual;
        last.fullDiagnosticsCount = 1;
      }
    }
    const auto end = Clock::now();
    last.allocationCount = allocations.events;
    last.allocatedBytes = allocations.bytes;
    last.peakOwnedBytes = allocations.peakBytes;
    elapsed.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
    hash = semanticHash(sketch);
    if (expectedHash && hash != *expectedHash) {
      std::cerr << "non-deterministic semantic hash for mode " << mode << '\n';
      std::exit(3);
    }
    expectedHash = hash;
    retainedBytes = sketch.ownedBytes();
  }
  std::cout << "production," << count << ',' << mode << ',' << samples << ','
            << std::fixed << std::setprecision(6)
            << percentile(elapsed, 0.50) << ',' << percentile(elapsed, 0.95)
            << ',' << percentile(elapsed, 0.99) << ',' << last.passes << ','
            << last.componentsVisited << ',' << last.geometriesVisited << ','
            << last.constraintsVisited << ','
            << fixtureGraph.components().size() << ','
            << fixtureParameters << ',' << fixtureEquations << ','
            << last.lockedSnapshotSize << ','
            << last.fullDiagnosticsCount << ',' << last.allocationCount << ','
            << last.allocatedBytes << ',' << last.peakOwnedBytes << ','
            << retainedBytes << ',' << hash
            << ',' << static_cast<int>(last.status)
            << ',' << last.maxNormalizedResidual
            << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t count = argc > 1 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 100;
  const std::string mode = argc > 2 ? argv[2] : "localized_drag";
  const int samples = argc > 3 ? std::max(1, std::atoi(argv[3])) : 21;
  if (argc > 4 && std::string(argv[4]) != "production") return 2;
  std::cout << "implementation,entity_count,mode,samples,p50_ms,p95_ms,p99_ms,iterations,components_visited,geometries_visited,constraints_visited,fixture_components,parameters,equations,locked_snapshot_size,full_diagnostics_count,allocation_count,allocated_bytes,peak_owned_bytes,retained_bytes,semantic_hash,status,max_normalized_residual\n";
  run(count, mode, samples);
}
