idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bus gui net time_service power_service button_service app_console
)
