#include "project/ProjectFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <memory>

#include "model/ExtrudeFeature.h"

// Every critical check uses CHECK (not assert) so it remains active in the
// Release CI build where NDEBUG is defined.
#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

// Crash-free "New Project" lifecycle regression. Repeats the create ->
// validate -> load -> destroy cycle that previously faulted when stale tool
// and B-Rep state outlived a replaced Document. The whole test is model-level
// (no GUI, no platform-specific code) so it is portable across Windows/Linux.
int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);

  // Keep test artifacts under CTest's writable build directory to avoid
  // platform policies that deny atomic file replacement in global temp dirs.
  QTemporaryDir directory(QDir::current().filePath(
      QStringLiteral("project-lifecycle-tests-XXXXXX")));
  if (!directory.isValid()) {
    std::cerr << "QTemporaryDir failed: "
              << directory.errorString().toUtf8().constData() << '\n';
    return EXIT_FAILURE;
  }

  for (int cycle = 0; cycle < 10; ++cycle) {
    const QString path = directory.filePath(
        QStringLiteral("lifecycle-%1.solidar").arg(cycle));
    QString error;

    CHECK(solidar::project::ProjectFile::create(path, &error));

    // A freshly created project is immediately a valid canonical v2 document,
    // not a legacy v1 shell upgraded later on the first save.
    CHECK(QFile::exists(path));
    CHECK(solidar::project::ProjectFile::validate(path, &error));

    solidar::Document document;
    CHECK(solidar::project::ProjectFile::loadDocument(path, &document, &error));
    CHECK(document.sketches().empty());
    CHECK(document.bodies().empty());

    // create -> load -> saveDocument -> loadDocument round-trip preserves the
    // same empty, valid canonical state with no lost or fake geometry.
    const QString roundTrip = directory.filePath(
        QStringLiteral("roundtrip-%1.solidar").arg(cycle));
    CHECK(solidar::project::ProjectFile::saveDocument(roundTrip, document,
                                                      &error));
    solidar::Document reloaded;
    CHECK(solidar::project::ProjectFile::loadDocument(roundTrip, &reloaded,
                                                      &error));
    CHECK(reloaded.sketches().empty());
    CHECK(reloaded.bodies().empty());
    CHECK(solidar::project::ProjectFile::validate(roundTrip, &error));
  }

  // A non-empty project created through the same canonical path persists real
  // parametric history: one sketch, one body, one feature, and a valid B-Rep.
  {
    solidar::Document document;
    auto& baseSketch = document.addSketch("Base");
    baseSketch.geometry.addRectangle({0.0, 0.0}, {40.0, 20.0});
    auto& body = document.addBody("Body");
    body.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        baseSketch.id, 15.0, "Extrude"));
    CHECK(document.recompute());

    const QString path =
        directory.filePath(QStringLiteral("featured.solidar"));
    QString error;
    CHECK(solidar::project::ProjectFile::saveDocument(path, document, &error));

    solidar::Document reloaded;
    CHECK(solidar::project::ProjectFile::loadDocument(path, &reloaded, &error));
    CHECK(reloaded.sketches().size() == 1);
    CHECK(reloaded.bodies().size() == 1);
    CHECK(reloaded.bodies()[0].features().size() == 1);
    CHECK(reloaded.bodies()[0].resultShape() != nullptr);
  }

  return EXIT_SUCCESS;
}
