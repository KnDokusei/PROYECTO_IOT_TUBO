idf_component_register(
    SRCS "kundt_api.c" "kundt_api_parse.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client json
)

# Treat warnings as errors in our own code (not in IDF/managed components).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
