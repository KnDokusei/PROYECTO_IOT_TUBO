idf_component_register(
    SRCS "main.c" "selftest.c"
    INCLUDE_DIRS "."
    REQUIRES kundt_config kundt_wifi kundt_mqtt kundt_led ad9833 servo esp_driver_gpio esp_timer
)

# Treat warnings as errors in our own code (not in IDF/managed components).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
