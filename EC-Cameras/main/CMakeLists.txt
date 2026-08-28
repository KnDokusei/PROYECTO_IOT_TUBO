idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES kundt_config kundt_wifi
)

# Treat warnings as errors in our own code (not in IDF/managed components).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
