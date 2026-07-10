# hypex_host talks the USB Host Library, which only exists on USB-OTG-capable
# targets (S2/S3/P4). On other targets the source compiles to a stub (guarded
# by SOC_USB_OTG_SUPPORTED), so the component still registers without `usb`.
idf_build_get_property(target IDF_TARGET)
set(reqs hypex_proto driver)
if(target STREQUAL "esp32s2" OR target STREQUAL "esp32s3" OR target STREQUAL "esp32p4")
    list(APPEND reqs usb)
endif()

idf_component_register(SRCS "hypex_host.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ${reqs})
