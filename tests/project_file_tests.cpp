#include "project/ProjectFile.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cassert>

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir directory;
  assert(directory.isValid());
  const QString path = directory.filePath("sample.solidar");

  QString error;
  assert(solidar::project::ProjectFile::create(path, &error));
  assert(QFile::exists(path));
  assert(solidar::project::ProjectFile::validate(path, &error));

  QFile broken(directory.filePath("broken.solidar"));
  assert(broken.open(QIODevice::WriteOnly));
  broken.write("not json");
  broken.close();
  assert(!solidar::project::ProjectFile::validate(broken.fileName(), &error));
  return 0;
}
