idf_component_register(SRCS "app_main.c" "amp_backend_log.c" "input_map.c"
                       INCLUDE_DIRS "."
                       REQUIRES hypex_proto ble_input
                       PRIV_REQUIRES nvs_flash)
