idf_component_register(
    SRCS "app_console.c"
    INCLUDE_DIRS "include"
    REQUIRES console esp_system esp_timer bus bsp net time_service power_service freertos
)
