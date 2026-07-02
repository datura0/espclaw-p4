idf_component_register(
    SRCS            "rtsp_server.c"
    INCLUDE_DIRS    "."
    REQUIRES        lwip esp_timer esp_netif
)
