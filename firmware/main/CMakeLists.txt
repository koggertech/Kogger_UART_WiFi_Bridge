idf_component_register(
    SRCS "main.c" "link.c" "frame.c" "proto.c" "sbp.c" "sbpdev.c" "wifistat.c" "bridge.c" "manager.c" "ota.c" "kframe.c" "kpack.c" "relay.c" "mavcrc.c" "radio.c" "netcfg.c" "wifiap.c" "uline.c" "netctl.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi esp_netif esp_event nvs_flash lwip esp_timer esp_app_format app_update bootloader_support
)
