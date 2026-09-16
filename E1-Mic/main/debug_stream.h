/*
 * debug_stream.h - Audio crudo por WebSocket, sólo para depurar.
 *
 * E1 mide con Goertzel y publica escalares: el audio no forma parte del camino
 * de producción. Esto se conserva porque es la única forma de ver la FORMA DE
 * ONDA desde el PC, que es lo que hace falta el día que el frente analógico se
 * porte raro. Apagado por defecto.
 *
 * Vive aparte de main.c para que el lazo de medida no lleve directivas de
 * compilación condicional: con CONFIG_E1_DEBUG_STREAM apagado, las tres
 * funciones son cuerpos vacíos que el compilador elimina.
 *
 * TRAMPA: sdkconfig.h se incluye explícitamente y va PRIMERO. Sin él los
 * #if CONFIG_ dan falso y el archivo compila vacío sin avisar de nada.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Conecta el cliente si la compilación lo lleva. Si no, no hace nada. */
void debug_stream_start(void);

/** @brief Envía una trama de PCM. Descarta en silencio si no hay enlace. */
void debug_stream_send(const int16_t *pcm, size_t samples);

/** @brief Añade su línea al log periódico. Si no está activo, no imprime nada. */
void debug_stream_log(void);

#ifdef __cplusplus
}
#endif
