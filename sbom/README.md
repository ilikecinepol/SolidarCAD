# SBOM

`solidarcad.spdx.json` — воспроизводимый SPDX 2.3 inventory прямых компонентов
и профиля сборки. Канонический файл создаётся командой:

```text
python scripts/generate_sbom.py
```

Рассинхронизация проверяется через `python scripts/generate_sbom.py --check`.
CI дополнительно создаёт SBOM с фактически установленной версией Qt и публикует
его как build artifact. После CPack команда
`python scripts/finalize_release.py --root . --build-dir build/release`
проверяет наличие в ZIP исполняемого файла, Qt, OCCT, platform plugin и
лицензионных материалов, а затем создаёт `.zip.sha256` и release-profile
`.spdx.json` рядом с архивом.

Этот SPDX-файл фиксирует прямые входы сборки, а SHA-256 защищает весь
бинарный архив. Это не следует выдавать за полный файловый SBOM с
хэшами каждой транзитивной DLL.
