idf_component_register(
    SRCS "mic_capture.c" "mic_dsp.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_adc
)

# Treat warnings as errors in our own code (not in IDF/managed components).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
