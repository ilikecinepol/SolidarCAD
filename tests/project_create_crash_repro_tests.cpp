// Scratch repro for the "crash on project creation" report.
// Drives the real MainWindow::createProject() (File -> Create) headlessly:
// triggers the "Создать" action, auto-accepts the save dialog with a fixed
// path, pumps events, then quits. Run under ASan to catch heap corruption.
#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QTimer>

#include <crtdbg.h>

#include <iostream>

#ifdef Q_OS_WIN
#include <windows.h>

#include <dbghelp.h>
#include <psapi.h>
#endif

#include "ui/EditorFactory.h"
#include "ui/MainWindow.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return 1;                                                            \
    }                                                                      \
  } while (false)

namespace {

const QString kSeedProject =
    QStringLiteral("C:/Users/drmma/AppData/Local/Temp/opencode/repro.solidar");
const QString kCreatedProject = QStringLiteral(
    "C:/Users/drmma/AppData/Local/Temp/opencode/repro-created.solidar");

#ifdef Q_OS_WIN
// No C++ objects with destructors in here: __try requires POD-only locals.
static bool reproSafeSymFromAddr(HANDLE process, DWORD64 address,
                                 char* nameOut, std::size_t nameCapacity,
                                 DWORD64* displacementOut) {
  __try {
    char symbolBuffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
    memset(symbolBuffer, 0, sizeof(symbolBuffer));
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    if (!SymFromAddr(process, address, &displacement, symbol)) return false;
    size_t length = 0;
    while (length + 1 < nameCapacity && symbol->Name[length] != '\0') {
      nameOut[length] = symbol->Name[length];
      ++length;
    }
    nameOut[length] = '\0';
    *displacementOut = displacement;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static WORD reproSafeCapture(void** frames, WORD capacity) {
  __try {
    return CaptureStackBackTrace(0, capacity, frames, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

LONG WINAPI reproCrashHandler(EXCEPTION_POINTERS* info) {
  std::cerr << "REPRO: fatal exception code=0x" << std::hex
            << info->ExceptionRecord->ExceptionCode << std::dec << "\n"
            << std::flush;
  std::cerr << "REPRO: fault address="
            << info->ExceptionRecord->ExceptionAddress << "\n"
            << std::flush;
  void* frames[64];
  for (int i = 0; i < 64; ++i) frames[i] = nullptr;
  const WORD count = reproSafeCapture(frames, 64);
  std::cerr << "REPRO: captured " << count << " frames\n" << std::flush;
  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
  char searchPath[1024] =
      "C:\\Users\\drmma\\OneDrive\\Documents\\ChatGPT\\SolidarCAD\\build\\"
      "Desktop_Qt_6_11_1_MSVC2022_64bit_RelWithDebInfo\\tests;"
      "C:\\Users\\drmma\\OneDrive\\Documents\\ChatGPT\\SolidarCAD\\build\\"
      "Desktop_Qt_6_11_1_MSVC2022_64bit_RelWithDebInfo\\src";
  const BOOL symOk = SymInitialize(process, searchPath, TRUE);
  if (!symOk) std::cerr << "REPRO: SymInitialize failed\n" << std::flush;
  for (WORD i = 0; i < count; ++i) {
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(frames[i]), &module);
    char modulePath[MAX_PATH];
    modulePath[0] = '\0';
    if (module) GetModuleFileNameA(module, modulePath, MAX_PATH);
    DWORD rva = 0;
    MODULEINFO moduleInfo;
    memset(&moduleInfo, 0, sizeof(moduleInfo));
    if (module && GetModuleInformation(process, module, &moduleInfo,
                                       sizeof(moduleInfo))) {
      rva = static_cast<DWORD>(static_cast<char*>(frames[i]) -
                               static_cast<char*>(moduleInfo.lpBaseOfDll));
    }
    char symName[256];
    symName[0] = '\0';
    DWORD64 displacement = 0;
    if (symOk) {
      if (!reproSafeSymFromAddr(process, reinterpret_cast<DWORD64>(frames[i]),
                                symName, sizeof(symName), &displacement)) {
        symName[0] = '\0';
      }
    }
    std::cerr << "  #" << i << " addr=" << frames[i] << " rva=0x" << std::hex
              << rva << std::dec << " sym=" << symName << "+0x" << std::hex
              << displacement << std::dec << " [" << modulePath << "]\n"
              << std::flush;
  }
  SymCleanup(process);
  return EXCEPTION_EXECUTE_HANDLER;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  std::cerr << "REPRO: start\n" << std::flush;
#ifdef Q_OS_WIN
  SetUnhandledExceptionFilter(reproCrashHandler);
#endif
  qputenv("QT_QPA_PLATFORM", "offscreen");
  // Loud, non-blocking heap diagnostics: validate the CRT heap on every
  // alloc/free and route all CRT reports to stderr instead of a dialog.
  _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) |
                 _CRTDBG_CHECK_ALWAYS_DF | _CRTDBG_ALLOC_MEM_DF |
                 _CRTDBG_LEAK_CHECK_DF);
  QApplication application(argc, argv);

  QString error;
  QMainWindow* editor =
      solidar::createEditorWindow(kSeedProject, &error);
  CHECK(editor != nullptr);
  editor->showMaximized();

  // File -> Create, like a user.
  QTimer::singleShot(500, [&] {
    QAction* create = nullptr;
    for (QAction* action : editor->findChildren<QAction*>()) {
      if (action->text().contains(QString::fromUtf8("Создать"))) {
        create = action;
        break;
      }
    }
    CHECK(create != nullptr);
    std::cerr << "REPRO: triggering File -> Create\n";
    create->trigger();
    std::cerr << "REPRO: createProject returned\n";
  });

  // Auto-accept the modal save dialog with a fixed path.
  auto* watcher = new QTimer(&application);
  QObject::connect(watcher, &QTimer::timeout, [&] {
    QWidget* modal = QApplication::activeModalWidget();
    auto* dialog = qobject_cast<QFileDialog*>(modal);
    if (!dialog) return;
    std::cerr << "REPRO: accepting save dialog\n";
    dialog->selectFile(kCreatedProject);
    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
  });
  watcher->start(100);

  QTimer::singleShot(10000, &application, &QApplication::quit);
  const int result = application.exec();
  std::cerr << "REPRO: event loop exited result=" << result << "\n";
  delete editor;
  std::cerr << "REPRO: done\n";
  return result;
}
