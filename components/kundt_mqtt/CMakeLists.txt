idf_component_register(
    SRCS "kundt_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES mqtt json esp_event
)

# Treat warnings as errors in our own code (not in IDF/managed components).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
