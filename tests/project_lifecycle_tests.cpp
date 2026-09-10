#include "project/ProjectFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cassert>
#include <cstdio>
#include <memory>

#include "model/ExtrudeFeature.h"

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
    std::fprintf(stderr, "QTemporaryDir failed: %s\n",
                 directory.errorString().toUtf8().constData());
    return 1;
  }

  for (int cycle = 0; cycle < 10; ++cycle) {
    const QString path = directory.filePath(
        QStringLiteral("lifecycle-%1.solidar").arg(cycle));
    QString error;

    assert(solidar::project::ProjectFile::create(path, &error));

    // A freshly created project is immediately a valid canonical v2 document,
    // not a legacy v1 shell upgraded later on the first save.
    assert(QFile::exists(path));
    assert(solidar::project::ProjectFile::validate(path, &error));

    solidar::Document document;
    assert(solidar::project::ProjectFile::loadDocument(path, &document, &error));
    assert(document.sketches().empty());
    assert(document.bodies().empty());

    // create -> load -> saveDocument -> loadDocument round-trip preserves the
    // same empty, valid canonical state with no lost or fake geometry.
    const QString roundTrip = directory.filePath(
        QStringLiteral("roundtrip-%1.solidar").arg(cycle));
    assert(solidar::project::ProjectFile::saveDocument(roundTrip, document,
                                                       &error));
    solidar::Document reloaded;
    assert(solidar::project::ProjectFile::loadDocument(roundTrip, &reloaded,
                                                       &error));
    assert(reloaded.sketches().empty());
    assert(reloaded.bodies().empty());
    assert(solidar::project::ProjectFile::validate(roundTrip, &error));
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
    assert(document.recompute());

    const QString path =
        directory.filePath(QStringLiteral("featured.solidar"));
    QString error;
    assert(solidar::project::ProjectFile::saveDocument(path, document, &error));

    solidar::Document reloaded;
    assert(solidar::project::ProjectFile::loadDocument(path, &reloaded, &error));
    assert(reloaded.sketches().size() == 1);
    assert(reloaded.bodies().size() == 1);
    assert(reloaded.bodies()[0].features().size() == 1);
    assert(reloaded.bodies()[0].resultShape() != nullptr);
  }

  return 0;
}
