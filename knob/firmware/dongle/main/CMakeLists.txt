# NOTE: component REQUIRES are resolved before Kconfig is available, so they
# cannot depend on CONFIG_DONGLE_APP_MODE_*. Register everything and let the
# #if in app_main.c (plus --gc-sections) drop the unused half from the image.
idf_component_register(SRCS "app_main.c" "amp_backend_log.c" "input_map.c"
                       INCLUDE_DIRS "."
                       REQUIRES hypex_proto ble_input hypex_host ui_display
                       PRIV_REQUIRES nvs_flash)
