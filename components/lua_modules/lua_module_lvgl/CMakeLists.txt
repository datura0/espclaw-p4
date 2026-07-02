if(CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT)
    set(lvgl_srcs
        "src/lua_module_lvgl.c"
        "src/lua_lvgl_core_widgets.c"
        "src/lua_lvgl_complex_widgets.c"
        "src/lua_lvgl_demos.c"
        "src/lua_lvgl_events.c"
        "src/lua_lvgl_extra_widgets.c"
        "src/lua_lvgl_indev.c"
        "src/lua_lvgl_layout.c"
        "src/lua_lvgl_methods.c"
        "src/lua_lvgl_object.c"
        "src/lua_lvgl_parse.c"
        "src/lua_lvgl_runtime.c"
        "src/lua_lvgl_style.c"
        "src/lua_lvgl_value.c"
    )
    set(lvgl_reqs display_arbiter esp_lcd esp_lcd_touch esp_timer lvgl)
else()
    set(lvgl_srcs "")
    set(lvgl_reqs "")
endif()

idf_component_register(
    SRCS ${lvgl_srcs}
    INCLUDE_DIRS "src"
    REQUIRES
        cap_lua
        ${lvgl_reqs}
)
