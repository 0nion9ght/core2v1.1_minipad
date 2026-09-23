idf_component_register(
    SRCS
        "gui.c"
        "shell.c"
        "theme.c"
        "boot_anim.c"
        "boot_frames/boot_path.c"
        "fonts/bebas_neue_176.c"
        "fonts/bebas_neue_20.c"
        "fonts/icons_20.c"
        "fonts/icons_32.c"
        "fonts/cjk_20.c"
    INCLUDE_DIRS "include" "boot_frames"
    REQUIRES bus bsp lvgl esp_timer freertos
)
