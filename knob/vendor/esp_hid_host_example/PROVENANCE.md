# Provenance

Vendored copy of Espressif's official `esp_hid_host` example, unmodified.

- **Source:** https://github.com/espressif/esp-idf/tree/release/v5.5/examples/bluetooth/esp_hid_host
- **Branch:** `release/v5.5`
- **Branch head commit at fetch time:** `bb2188bfb805e284f4b95480691ca0031989400a` (2026-06-18)
- **Fetched:** 2026-07-06, via `raw.githubusercontent.com` (per-file raw downloads)
- **Files vendored (complete example, verified against the GitHub contents API):**
  - `CMakeLists.txt`
  - `README.md`
  - `sdkconfig.defaults`
  - `sdkconfig.defaults.esp32c3`
  - `sdkconfig.defaults.esp32s3`
  - `sdkconfig.ci.nimble`
  - `sdkconfig.ci.test`
  - `main/CMakeLists.txt`
  - `main/Kconfig.projbuild`
  - `main/esp_hid_gap.c`
  - `main/esp_hid_gap.h`
  - `main/esp_hid_host_main.c`

## License

Per the SPDX headers in the vendored source files
(`SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD`),
the example code is licensed:

    SPDX-License-Identifier: Unlicense OR CC0-1.0

i.e. Espressif releases its example code into the public domain
(Unlicense/CC0-1.0), unlike the ESP-IDF component code the example calls into
(e.g. `components/esp_hid`), which is Apache-2.0. ESP-IDF as a whole is
Apache-2.0; the example subtree carries the more permissive grant above.
