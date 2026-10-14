#include "sketch/EquationKernel.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
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
  std::size_t currentBytes{};
  std::size_t peakBytes{};
};
struct Header {
  void* allocation{};
  std::size_t size{};
  std::uint64_t scopeId{};
};
thread_local Counter* active = nullptr;
thread_local std::uint64_t nextId = 1;
class Scope {
 public:
  explicit Scope(Counter& counter) : previous_(active) {
    counter.id = nextId++;
    active = &counter;
  }
  ~Scope() { active = previous_; }
 private:
  Counter* previous_{};
};
void* allocate(std::size_t size, std::size_t alignment) {
  size = std::max<std::size_t>(size, 1);
  alignment = std::max(alignment, alignof(std::max_align_t));
  if (size > std::numeric_limits<std::size_t>::max() - alignment - sizeof(Header))
    throw std::bad_alloc();
  void* allocation = std::malloc(size + alignment + sizeof(Header));
  if (!allocation) throw std::bad_alloc();
  const auto unaligned = reinterpret_cast<std::uintptr_t>(allocation) + sizeof(Header);
  const auto aligned = (unaligned + alignment - 1) & ~(alignment - 1);
  auto* header = reinterpret_cast<Header*>(aligned - sizeof(Header));
  header->allocation = allocation;
  header->size = size;
  header->scopeId = active ? active->id : 0;
  if (active) {
    ++active->events;
    active->currentBytes += size;
    active->peakBytes = std::max(active->peakBytes, active->currentBytes);
  }
  return reinterpret_cast<void*>(aligned);
}
void deallocate(void* pointer) noexcept {
  if (!pointer) return;
  auto* header = reinterpret_cast<Header*>(
      reinterpret_cast<std::uintptr_t>(pointer) - sizeof(Header));
  if (active && header->scopeId == active->id)
    active->currentBytes -= std::min(active->currentBytes, header->size);
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
void operator delete(void* pointer) noexcept { allocation_probe::deallocate(pointer); }
void operator delete[](void* pointer) noexcept { allocation_probe::deallocate(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { allocation_probe::deallocate(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { allocation_probe::deallocate(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { allocation_probe::deallocate(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { allocation_probe::deallocate(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
  allocation_probe::deallocate(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
  allocation_probe::deallocate(pointer);
}

namespace solidar::sketch {
class SketchTestAccess {
 public:
  static void install(Sketch& sketch, std::vector<Constraint> constraints) {
    sketch.constraints_ = std::move(constraints);
    sketch.nextConstraintId_ = 1;
    for (const auto& constraint : sketch.constraints_)
      sketch.nextConstraintId_ =
          std::max(sketch.nextConstraintId_, constraint.id + 1);
    sketch.invalidateStructureIndexes();
  }
};
}  // namespace solidar::sketch

using namespace solidar::sketch;

namespace {
Constraint withId(Constraint value, ConstraintId id) {
  value.id = id;
  return value;
}

Sketch independentWorkload(std::size_t count) {
  Sketch sketch;
  std::vector<Constraint> constraints;
  constraints.reserve(count * 2);
  ConstraintId id = 1;
  for (std::size_t index = 0; index < count; ++index) {
    const double x = static_cast<double>(index) * 3.0;
    if (index % 2 == 0)
      sketch.addLine({x, 0.0}, {x + 2.0, 0.0});
    else
      sketch.addLine({x, 0.0}, {x, 2.0});
    const auto line = sketch.lineId(index);
    constraints.push_back(withId(
        index % 2 == 0 ? Constraint{HorizontalConstraint{line}}
                       : Constraint{VerticalConstraint{line}}, id++));
    constraints.push_back(withId(Constraint{LengthConstraint{line, 2.0}}, id++));
  }
  SketchTestAccess::install(sketch, std::move(constraints));
  return sketch;
}

Sketch chainWorkload(std::size_t count) {
  Sketch sketch;
  std::vector<Constraint> constraints;
  ConstraintId id = 1;
  for (std::size_t index = 0; index < count; ++index) {
    sketch.addLine({static_cast<double>(index), 0.0},
                   {static_cast<double>(index + 1), 0.0});
    if (index == 0) continue;
    const auto previous = sketch.lineId(index - 1);
    const auto current = sketch.lineId(index);
    constraints.push_back(withId(
        Constraint{CoincidentConstraint{
            PointReference{LineEndpointReference{previous, Endpoint::End}},
            PointReference{LineEndpointReference{current, Endpoint::Start}}}},
        id++));
    constraints.push_back(withId(Constraint{EqualConstraint{previous, current}},
                                 id++));
  }
  SketchTestAccess::install(sketch, std::move(constraints));
  return sketch;
}

Sketch rectangleWorkload(std::size_t rectangleCount) {
  Sketch sketch;
  std::vector<Constraint> constraints;
  ConstraintId id = 1;
  for (std::size_t rectangle = 0; rectangle < rectangleCount; ++rectangle) {
    const double x = static_cast<double>(rectangle) * 20.0;
    const std::size_t element = rectangle + 1;
    const std::size_t firstIndex = sketch.lines().size();
    sketch.addLine({x, 0.0}, {x + 10.0, 0.0}, element);
    sketch.addLine({x + 10.0, 0.0}, {x + 10.0, 5.0}, element);
    sketch.addLine({x + 10.0, 5.0}, {x, 5.0}, element);
    sketch.addLine({x, 5.0}, {x, 0.0}, element);
    const auto bottom = sketch.lineId(firstIndex);
    const auto right = sketch.lineId(firstIndex + 1);
    const auto top = sketch.lineId(firstIndex + 2);
    const auto left = sketch.lineId(firstIndex + 3);
    constraints.push_back(withId(Constraint{HorizontalConstraint{bottom}}, id++));
    constraints.push_back(withId(Constraint{HorizontalConstraint{top}}, id++));
    constraints.push_back(withId(Constraint{VerticalConstraint{right}}, id++));
    constraints.push_back(withId(Constraint{VerticalConstraint{left}}, id++));
    constraints.push_back(withId(Constraint{LengthConstraint{bottom, 10.0}}, id++));
    constraints.push_back(withId(Constraint{LengthConstraint{right, 5.0}}, id++));
  }
  SketchTestAccess::install(sketch, std::move(constraints));
  return sketch;
}

Sketch mixedTangencyWorkload(std::size_t groups) {
  Sketch sketch;
  std::vector<Constraint> constraints;
  ConstraintId id = 1;
  for (std::size_t group = 0; group < groups; ++group) {
    const double x = static_cast<double>(group) * 20.0;
    const std::size_t lineIndex = sketch.lines().size();
    const std::size_t circleIndex = sketch.circles().size();
    const std::size_t arcIndex = sketch.arcs().size();
    sketch.addLine({x + 5.0, -5.0}, {x + 5.0, 5.0});
    sketch.addCircle({x, 0.0}, 5.0);
    sketch.addArc({x + 12.0, 0.0}, 2.0, 0.0, 1.5);
    const auto line = sketch.lineId(lineIndex);
    const auto circle = sketch.circleId(circleIndex);
    const auto arc = sketch.arcId(arcIndex);
    constraints.push_back(withId(Constraint{TangentConstraint{line, circle}}, id++));
    constraints.push_back(withId(Constraint{TangentConstraint{circle, arc}}, id++));
  }
  SketchTestAccess::install(sketch, std::move(constraints));
  return sketch;
}

Sketch bezierWorkload(std::size_t groups) {
  Sketch sketch;
  std::vector<Constraint> constraints;
  ConstraintId id = 1;
  for (std::size_t group = 0; group < groups; ++group) {
    const double x = static_cast<double>(group) * 10.0;
    const std::size_t lineIndex = sketch.lines().size();
    const std::size_t bezierIndex = sketch.beziers().size();
    sketch.addLine({x, 0.0}, {x + 4.0, 0.0});
    sketch.addBezier({x, 0.0}, {x + 1.0, 0.0}, {x + 2.0, 1.0},
                     {x + 3.0, 1.0});
    constraints.push_back(withId(
        Constraint{TangentConstraint{sketch.lineId(lineIndex),
                                     sketch.bezierId(bezierIndex)}},
        id++));
  }
  SketchTestAccess::install(sketch, std::move(constraints));
  return sketch;
}

struct Metrics {
  double milliseconds{};
  std::size_t components{};
  std::size_t parameters{};
  std::size_t equations{};
  std::size_t nonZeros{};
  std::size_t allocations{};
  std::size_t peakBytes{};
  std::size_t ownedBytes{};
  std::uint64_t semanticHash{};
};

Metrics measure(const Sketch& sketch) {
  allocation_probe::Counter counter;
  Metrics result;
  const auto started = std::chrono::steady_clock::now();
  {
    allocation_probe::Scope scope(counter);
    const auto graph = ConstraintGraph::build(sketch);
    result.components = graph.components().size();
    for (const auto& component : graph.components()) {
      const auto system = ConstraintEquationRegistry::assemble(sketch, component);
      result.parameters += system.parameterCount();
      result.equations += system.equationCount();
      result.nonZeros += system.nonZeroJacobianCount();
      result.ownedBytes += system.ownedBytes();
      result.semanticHash ^= system.semanticHash() + 0x9e3779b97f4a7c15ULL +
                             (result.semanticHash << 6) +
                             (result.semanticHash >> 2);
    }
  }
  result.milliseconds = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - started)
                            .count();
  result.allocations = counter.events;
  result.peakBytes = counter.peakBytes;
  return result;
}

void writeRow(std::ostream& output, const std::string& workload,
              std::size_t geometryCount, const Metrics& metrics) {
  output << workload << ',' << geometryCount << ',' << std::fixed
         << std::setprecision(6) << metrics.milliseconds << ','
         << metrics.components << ',' << metrics.parameters << ','
         << metrics.equations << ',' << metrics.nonZeros << ','
         << metrics.allocations << ',' << metrics.peakBytes << ','
         << metrics.ownedBytes << ',' << metrics.semanticHash << '\n';
}
}  // namespace

int main(int argc, char** argv) {
  std::ofstream file;
  std::ostream* output = &std::cout;
  if (argc == 3 && std::string(argv[1]) == "--output") {
    file.open(argv[2], std::ios::trunc);
    if (!file) return EXIT_FAILURE;
    output = &file;
  }
  *output << "workload,geometry_count,assembly_ms,components,parameters,equations,"
             "jacobian_nonzeros,allocations,peak_bytes,owned_bytes,semantic_hash\n";
  for (const std::size_t count : {100u, 500u, 1000u}) {
    writeRow(*output, "independent_hv_length", count,
             measure(independentWorkload(count)));
    writeRow(*output, "coincident_equal_chain", count,
             measure(chainWorkload(count)));
  }
  writeRow(*output, "rectangle_dimensions", 100,
           measure(rectangleWorkload(25)));
  writeRow(*output, "mixed_line_circle_arc_tangency", 99,
           measure(mixedTangencyWorkload(33)));
  writeRow(*output, "bezier_endpoint_tangent", 100,
           measure(bezierWorkload(50)));
  return EXIT_SUCCESS;
}
