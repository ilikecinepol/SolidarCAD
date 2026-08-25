#pragma once

class QMainWindow;
class QString;

namespace solidar {

// Keeps allocation of MainWindow in the same binary target that defines its
// private layout. Callers depend only on QMainWindow's stable Qt ABI.
QMainWindow* createEditorWindow(const QString& projectPath, QString* error);

}  // namespace solidar
