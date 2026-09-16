/*
 * EC-Cameras - Cámaras del tubo de Kundt, port a ESP-IDF.
 *
 * Función: servir el vídeo del OV2640 como MJPEG por HTTP. Tres placas por
 * equipo, cada una con su propia IP. El servidor las consume a través de
 * go2rtc, que reparte a los navegadores.
 *
 * Placa: AI-Thinker ESP32-CAM. El módulo lleva serigrafiado "ESP32-S", que es
 * un ESP32 clásico de dos núcleos Xtensa y NO un ESP32-S2 ni un S3: el target
 * de compilación es "esp32". Se programa con el adaptador ESP32-CAM-MB, que
 * trae CH340 y auto-reset, así que no hace falta puentear IO0 a mano.
 *
 * QUÉ CAMBIA FRENTE AL SKETCH DE ARDUINO
 *
 * 1. La frontera multipart va DELANTE de cada parte. El sketch la emitía
 *    después del JPEG, así que el cuerpo empezaba por las cabeceras de la parte
 *    y no por "--frontera", que es lo que exige el RFC 2046. Los navegadores lo
 *    toleran; el lector mime/multipart de Go que usa go2rtc no, y rechazaba
 *    cada fotograma con "multipart: wrong boundary". Medido: 0 fotogramas antes,
 *    63 en 8 s después.
 *
 * 2. Se comprueba la PSRAM antes de pedir XGA con dos búferes (hallazgo B10).
 *    Sin ella el driver falla al inicializar; ahora se baja la resolución y el
 *    módulo arranca igual, diciendo por qué.
 *
 * 3. Desaparece el PUT de registro contra el puerto 6001. Ese servidor ya no
 *    existe: en curiousBeagle las cámaras se dan de alta desde el servidor,
 *    porque su controlador de vídeo va tras JwtGuard y una ESP32-CAM no
 *    sostiene un JWT. Con el PUT se van también los hallazgos A4 y B5.
 *
 * 4. Un solo binario para las tres cámaras y los cinco kits. El sketch
 *    codificaba el número de cámara en la ruta, así que hacía falta recompilar
 *    por cámara. Aquí cualquier ruta sirve el vídeo: quien identifica a cada
 *    cámara es su IP, que es justamente lo que el servidor ya guarda.
 *
 * 5. La reconexión de WiFi la lleva kundt_wifi por eventos, y las credenciales
 *    salen de NVS en vez de estar en un header del árbol de fuentes.
 */

#include <stdio.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kundt_config.h"
#include "kundt_led.h"
#include "kundt_wifi.h"

static const char *TAG = "EC-Cameras";

/* --------------------------------------------------------------------- */
/* Trama MJPEG                                                            */
/* --------------------------------------------------------------------- */

#define PART_BOUNDARY "kundtframe"

static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;

/* El delimitador abre cada parte; el RFC 2046 pide que el cuerpo empiece por
 * él. Ver el punto 1 de la cabecera: ponerlo al final es lo que rompía go2rtc. */
static const char *STREAM_BOUNDARY = "--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART_HDR = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
static const char *STREAM_TAIL     = "\r\n";

/* --------------------------------------------------------------------- */
/* Cámara                                                                 */
/* --------------------------------------------------------------------- */

/* Patillaje de la AI-Thinker ESP32-CAM. Va con nombre propio y no como
 * "config" a secas: en el sketch esa variable global quedaba tapada por una
 * local homónima dentro del arranque del servidor (hallazgo B6). */
static camera_config_t s_camera_cfg = {
    .pin_pwdn     = 32,
    .pin_reset    = -1,
    .pin_xclk     = 0,
    .pin_sccb_sda = 26,
    .pin_sccb_scl = 27,
    .pin_d7       = 35,
    .pin_d6       = 34,
    .pin_d5       = 39,
    .pin_d4       = 36,
    .pin_d3       = 21,
    .pin_d2       = 19,
    .pin_d1       = 18,
    .pin_d0       = 5,
    .pin_vsync    = 25,
    .pin_href     = 23,
    .pin_pclk     = 22,
    .xclk_freq_hz = 20000000,
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size   = FRAMESIZE_XGA,  /* se ajusta en camera_start() según PSRAM */
    .jpeg_quality = CONFIG_EC_JPEG_QUALITY,
    .fb_count     = 2,
    .grab_mode    = CAMERA_GRAB_LATEST,
};

#if CONFIG_EC_FRAMESIZE_VGA
#define WANTED_FRAMESIZE FRAMESIZE_VGA
#elif CONFIG_EC_FRAMESIZE_SVGA
#define WANTED_FRAMESIZE FRAMESIZE_SVGA
#else
#define WANTED_FRAMESIZE FRAMESIZE_XGA
#endif

/*
 * Arranca el sensor, degradando la configuración si no hay PSRAM.
 *
 * Hallazgo B10: el sketch pedía XGA con dos búferes sin comprobar nada. Un
 * fotograma XGA no cabe en la RAM interna, así que en una placa sin PSRAM
 * esp_camera_init() falla y el módulo se queda sin hacer nada, sin decir por
 * qué. Bajar a VGA con un búfer deja la cámara funcionando y el problema
 * anotado en el log.
 */
static esp_err_t camera_start(void)
{
    const size_t psram = esp_psram_get_size();

    if (psram > 0) {
        s_camera_cfg.frame_size = WANTED_FRAMESIZE;
        s_camera_cfg.fb_count   = 2;
        s_camera_cfg.fb_location = CAMERA_FB_IN_PSRAM;
        ESP_LOGI(TAG, "PSRAM: %u KiB", (unsigned)(psram / 1024));
    } else {
        s_camera_cfg.frame_size = FRAMESIZE_VGA;
        s_camera_cfg.fb_count   = 1;
        s_camera_cfg.fb_location = CAMERA_FB_IN_DRAM;
        ESP_LOGW(TAG, "sin PSRAM: se baja a VGA con un solo búfer (hallazgo B10)");
    }

    const esp_err_t err = esp_camera_init(&s_camera_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init falló: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        ESP_LOGI(TAG, "sensor 0x%04x listo, %d de calidad JPEG",
                 s->id.PID, CONFIG_EC_JPEG_QUALITY);
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------- */
/* Servidor HTTP                                                          */
/* --------------------------------------------------------------------- */

static uint32_t s_frames;
static uint32_t s_clients;

static esp_err_t stream_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "cliente %lu pide \"%s\"", (unsigned long)(++s_clients), req->uri);

    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        return res;
    }
    /* go2rtc y los navegadores abren la conexión y la sostienen; sin esto un
     * proxy intermedio podría cachear el primer fotograma. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

#if CONFIG_EC_MAX_FPS > 0
    const int64_t min_period_us = 1000000 / CONFIG_EC_MAX_FPS;
    int64_t next_us = 0;
#endif

    char part_hdr[64];

    for (;;) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            ESP_LOGE(TAG, "no se pudo capturar el fotograma");
            res = ESP_FAIL;
            break;
        }

        /* pixel_format está fijado a JPEG, así que el sensor ya entrega JPEG y
         * no hace falta la conversión que el sketch llevaba por si acaso. Si
         * algún día se cambia el formato, esto lo dice en vez de mandar basura. */
        if (fb->format != PIXFORMAT_JPEG) {
            ESP_LOGE(TAG, "el sensor no entregó JPEG (formato %d)", fb->format);
            esp_camera_fb_return(fb);
            res = ESP_FAIL;
            break;
        }

        const size_t hlen = snprintf(part_hdr, sizeof(part_hdr),
                                     STREAM_PART_HDR, (unsigned)fb->len);

        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, part_hdr, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, STREAM_TAIL, strlen(STREAM_TAIL));
        }

        esp_camera_fb_return(fb);

        if (res != ESP_OK) {
            break;  /* el cliente cerró: es lo normal, no un error */
        }
        s_frames++;

#if CONFIG_EC_MAX_FPS > 0
        const int64_t now = esp_timer_get_time();
        if (now < next_us) {
            vTaskDelay(pdMS_TO_TICKS((next_us - now) / 1000));
        }
        next_us = esp_timer_get_time() + min_period_us;
#endif
    }

    ESP_LOGI(TAG, "cliente desconectado tras %lu fotogramas en total",
             (unsigned long)s_frames);
    return res;
}

static esp_err_t server_start(httpd_handle_t *out)
{
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();

    /*
     * El manejador del stream no retorna mientras haya cliente, y httpd atiende
     * en una sola tarea: la cámara sirve a un espectador cada vez. No es
     * limitación en la práctica, porque el único consumidor es go2rtc y es él
     * quien reparte a los navegadores.
     *
     * lru_purge_enable es lo que evita el caso feo: si go2rtc se reconecta
     * mientras el socket anterior sigue medio abierto, sin esto la conexión
     * nueva se rechaza y la cámara queda muda hasta que alguien la reinicie.
     */
    http_cfg.lru_purge_enable = true;
    http_cfg.stack_size       = 8192;

    /* Cualquier ruta sirve el vídeo: así un mismo binario vale para las tres
     * cámaras y para las rutas heredadas /Equipo{kit}/Cam{n} que el servidor
     * viejo tuviera registradas. Ver el punto 4 de la cabecera. */
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;

    ESP_RETURN_ON_ERROR(httpd_start(out, &http_cfg), TAG, "httpd_start");

    const httpd_uri_t stream_uri = {
        .uri      = "/*",
        .method   = HTTP_GET,
        .handler  = stream_handler,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(*out, &stream_uri);
}

/* --------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "Tubo de Kundt - módulo EC de cámaras (ESP-IDF)");

    /* GPIO33 es el LED rojo de la AI-Thinker. Va en lógica invertida, así que
     * el patrón se ve al revés que en los otros módulos; sigue sirviendo para
     * saber desde lejos que la placa vive, que es para lo que está. GPIO2 no
     * vale aquí: lo usa la ranura de microSD. */
    ESP_ERROR_CHECK(kundt_led_init(33));

    ESP_ERROR_CHECK(kundt_config_init());
    kundt_config_log();

    if (!kundt_config_is_provisioned()) {
        ESP_LOGE(TAG, "Falta el SSID de WiFi o la IP del servidor.");
        ESP_LOGE(TAG, "Configúralos con 'idf.py menuconfig', menú 'Kundt tube configuration',");
        ESP_LOGE(TAG, "y luego borra NVS una vez con 'idf.py erase-flash' para que carguen.");
        return;
    }

    kundt_config_t cfg;
    ESP_ERROR_CHECK(kundt_config_get(&cfg));

    /* La cámara se levanta antes que la red: si el sensor no responde conviene
     * saberlo ya, sin que el fallo quede tapado por un problema de WiFi. */
    ESP_ERROR_CHECK(camera_start());

    ESP_ERROR_CHECK(kundt_wifi_init());
    ESP_ERROR_CHECK(kundt_wifi_connect(cfg.wifi_ssid, cfg.wifi_password));

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(server_start(&server));
    ESP_LOGI(TAG, "servidor MJPEG en el puerto 80, cualquier ruta");

    kundt_led_set_state(KUNDT_LED_NO_WIFI);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        if (!kundt_wifi_is_connected()) {
            kundt_led_set_state(KUNDT_LED_NO_WIFI);
            continue;  /* kundt_wifi reintenta por su cuenta */
        }
        kundt_led_set_state(KUNDT_LED_RUNNING);

        ESP_LOGI(TAG, "kit %u | ip=%s | clientes=%lu fotogramas=%lu | heap=%u",
                 (unsigned)cfg.kit, kundt_wifi_ip(),
                 (unsigned long)s_clients, (unsigned long)s_frames,
                 (unsigned)esp_get_free_heap_size());
    }
}
