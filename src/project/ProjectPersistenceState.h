#pragma once

namespace solidar::project {

enum class ProjectPersistenceFormat {
  V3,
  V4,
  V5,
  // V6 can be carried by assembly-aware staging, but is not an editor save
  // target until the assembly workspace owns an active ProductDocument.
  V6,
  // Flat assemblies with embedded last-valid linked caches. Runtime link
  // diagnostics are application state and are deliberately not serialized.
  V7,
  // Recursive assemblies. Runtime link diagnostics remain application-owned;
  // the authored envelope stores only bindings and last-valid caches.
  V8,
  // Recursive assemblies with authored planar mates. Runtime mate solve
  // diagnostics are application state and are never serialized.
  V9,
  // Unified ComponentDefinition envelope without persistent drawings.
  V10,
  // Component definitions with persistent component drawings. Empty V11
  // projects retain V11 on subsequent saves; V10 remains the compact target
  // for component graphs that have never required drawing persistence.
  V11,
  // Component definitions with independently persisted home placements.
  // Once selected, this format is sticky and never downgrades to V10/V11.
  V12,
};

[[nodiscard]] constexpr bool isWritableAssemblyPersistenceFormat(
    ProjectPersistenceFormat format) noexcept {
  return format == ProjectPersistenceFormat::V7 ||
         format == ProjectPersistenceFormat::V8 ||
         format == ProjectPersistenceFormat::V9;
}

[[nodiscard]] constexpr bool isWritableComponentPersistenceFormat(
    ProjectPersistenceFormat format) noexcept {
  return format == ProjectPersistenceFormat::V10 ||
         format == ProjectPersistenceFormat::V11 ||
         format == ProjectPersistenceFormat::V12;
}

// Format-only routing value committed alongside application document
// ownership. Product identity and metadata remain authoritative exclusively
// in ProductDocument; product formats deliberately carry no shadow identity
// state here.
struct ProjectPersistenceState {
  ProjectPersistenceFormat format{ProjectPersistenceFormat::V3};

  friend bool operator==(const ProjectPersistenceState &,
                         const ProjectPersistenceState &) = default;
};

} // namespace solidar::project
