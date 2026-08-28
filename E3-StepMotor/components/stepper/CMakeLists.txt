idf_component_register(
    SRCS "stepper.c" "stepper_math.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_gptimer esp_timer
)

# Treat warnings as errors in our own code (not in IDF/managed components).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
