# esp_hid_gap.c/h are copied from the ESP-IDF esp_hid_host example
# (release/v5.5) — see knob/vendor/esp_hid_host_example/PROVENANCE.md.
# One local change, marked "KingRO4Y modification" in esp_hid_gap.c:
# the BLE scan also accepts devices advertising an HID *appearance*
# (0x03C0..0x03FF) without the 0x1812 service UUID, as the VOL20 does.
idf_component_register(SRCS "ble_input.c" "esp_hid_gap.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES esp_hid
                       PRIV_REQUIRES bt nvs_flash esp_event)
