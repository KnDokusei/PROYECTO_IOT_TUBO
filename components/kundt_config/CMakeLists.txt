idf_component_register(
    SRCS "kundt_config.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
)

# Treat warnings as errors in our own code (not in IDF/managed components).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
