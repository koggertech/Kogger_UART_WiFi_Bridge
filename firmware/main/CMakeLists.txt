# Line translators that are not part of the open tree (private/, kept out of the export): absent there.
file(GLOB private_srcs "${CMAKE_CURRENT_LIST_DIR}/private/*.c")
idf_component_register(
    SRCS ${private_srcs} "main.c" "link.c" "frame.c" "proto.c" "sbp.c" "sbpdev.c" "wifistat.c" "bridge.c" "manager.c" "ota.c" "kframe.c" "kpack.c" "relay.c" "mavcrc.c" "radio.c" "netcfg.c" "wifiap.c" "uline.c" "netctl.c" "portinfo.c" "ports.c" "bootkey.c" "led.c" "survey.c" "linkrate.c" "rfoff.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi esp_netif esp_event nvs_flash lwip esp_timer esp_app_format app_update bootloader_support
)
