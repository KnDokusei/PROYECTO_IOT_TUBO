idf_component_register(
    SRCS "ad9833.c" "ad9833_regs.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_spi
)

# Treat warnings as errors in our own code (not in IDF/managed components).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
