idf_component_register(
    SRCS "servo.c" "servo_map.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_ledc
)

# Treat warnings as errors in our own code (not in IDF/managed components).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
