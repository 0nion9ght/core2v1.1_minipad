idf_component_register(
    SRCS "power_service.c"
    INCLUDE_DIRS "include"
    REQUIRES bus bsp freertos
)
