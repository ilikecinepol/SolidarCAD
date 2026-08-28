#include "project/ProjectFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cassert>
#include <cstdio>

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  // Keep test artifacts under CTest's writable build directory. This also
  // avoids platform policies that deny atomic QSaveFile replacement in the
  // user's global temporary directory.
  QTemporaryDir directory(
      QDir::current().filePath(QStringLiteral("project-file-tests-XXXXXX")));
  if (!directory.isValid()) {
    std::fprintf(stderr, "QTemporaryDir failed: %s\n",
                 directory.errorString().toUtf8().constData());
    return 1;
  }
  const QString path = directory.filePath("sample.solidar");

  QString error;
  if (!solidar::project::ProjectFile::create(path, &error)) {
    std::fprintf(stderr, "ProjectFile::create failed for %s: %s\n",
                 path.toUtf8().constData(), error.toUtf8().constData());
    return 1;
  }
  assert(QFile::exists(path));
  assert(solidar::project::ProjectFile::validate(path, &error));

  QFile broken(directory.filePath("broken.solidar"));
  assert(broken.open(QIODevice::WriteOnly));
  broken.write("not json");
  broken.close();
  assert(!solidar::project::ProjectFile::validate(broken.fileName(), &error));
  return 0;
}
