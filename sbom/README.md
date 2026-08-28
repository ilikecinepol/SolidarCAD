# SBOM

`solidarcad.spdx.json` — воспроизводимый SPDX 2.3 inventory прямых компонентов
и профиля сборки. Канонический файл создаётся командой:

```text
python scripts/generate_sbom.py
```

Рассинхронизация проверяется через `python scripts/generate_sbom.py --check`.
CI дополнительно создаёт SBOM с фактически установленной версией Qt и публикует
его как build artifact. Файл пока не является полным release SBOM: транзитивные
Qt/OCCT-компоненты и хэши бинарников добавляются из фактического staging.

Перед релизом нужно:

1. разрешить точные версии Qt, OCCT, toolchain и транзитивных пакетов;
2. просканировать staging-каталог;
3. добавить SHA-256 поставляемых файлов;
4. синхронизировать notices и `LICENSES/`;
5. сохранить SBOM рядом с release artifacts.
