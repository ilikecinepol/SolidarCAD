# Qt 6.8.3

Windows MVP динамически поставляет библиотеки и плагины Qt 6.8.3. Каталоги
`qtbase/` и `qtsvg/` содержат неизменённые лицензионные тексты из официальных
тегов Qt Project `v6.8.3`. Каталог `SBOM/` содержит SPDX inventories из
установленной бинарной сборки, использованной для release-пакета.

SolidarCAD использует применимые Qt-модули на условиях LGPL-3.0-only. Наличие в
bundle текстов альтернативных лицензий отражает исходный состав Qt и не меняет
лицензию кода SolidarCAD. Qt DLL и плагины расположены отдельно от
`solidar.exe`, динамически загружаются и могут быть заменены совместимой сборкой.

Источники:

- https://github.com/qt/qtbase/tree/v6.8.3/LICENSES
- https://github.com/qt/qtsvg/tree/v6.8.3/LICENSES
- https://download.qt.io/official_releases/qt/6.8/6.8.3/submodules/
- https://doc.qt.io/qt-6.8/licensing.html
