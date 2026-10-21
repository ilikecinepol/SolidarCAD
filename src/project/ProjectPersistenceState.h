#pragma once

namespace solidar::project {

enum class ProjectPersistenceFormat {
  V3,
  V4,
};

// Plain project-layer value. Ownership and application wiring are deliberately
// deferred; a newly created project continues to select the production v3
// persistence path unless a later integration step explicitly chooses v4.
struct ProjectPersistenceState {
  ProjectPersistenceFormat format{ProjectPersistenceFormat::V3};

  friend bool operator==(const ProjectPersistenceState &,
                         const ProjectPersistenceState &) = default;
};

} // namespace solidar::project
