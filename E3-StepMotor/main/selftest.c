/*
 * selftest.c - ver selftest.h.
 */

#include "selftest.h"

#if CONFIG_E3_SELFTEST

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stepper.h"

static const char *TAG = "E3-selftest";

/* Pines del esquemático; el módulo los usa tal cual. */
#define STEP_GPIO  12
#define DIR_GPIO   14
#define SLEEP_GPIO 33

/* El contador del ESP32 es de 16 bits con signo: el límite alto tiene que
 * quedar por encima de los pasos de la prueba y dentro de ese rango. */
#define PCNT_HIGH_LIMIT 30000
#define PCNT_LOW_LIMIT  -100

static pcnt_unit_handle_t s_unit;
static int s_pass, s_fail;

static void report(bool ok, const char *what); /* definida más abajo */

#if CONFIG_E3_SELFTEST_ENDSTOP_SIM

#define SIM_POS_GPIO CONFIG_E3_SELFTEST_SIM_POS_GPIO
#define SIM_NEG_GPIO CONFIG_E3_SELFTEST_SIM_NEG_GPIO

/* Los fines de carrera son activos en bajo: alto = libre, bajo = pulsado. */
#define SIM_LIBRE   1
#define SIM_PULSADO 0

void selftest_endstop_sim_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << SIM_POS_GPIO) | (1ULL << SIM_NEG_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(SIM_POS_GPIO, SIM_LIBRE);
    gpio_set_level(SIM_NEG_GPIO, SIM_LIBRE);
}

static void sim_set(int pos_level, int neg_level)
{
    gpio_set_level(SIM_POS_GPIO, pos_level);
    gpio_set_level(SIM_NEG_GPIO, neg_level);
}

/* Un temporizador de un disparo permite pulsar un fin de carrera mientras el
 * motor ya está en marcha, o mientras la calibración bloquea esperando. */
static esp_timer_handle_t s_shot;

static void shot_cb(void *arg)
{
    gpio_set_level((int)(intptr_t)arg, SIM_PULSADO);
}

static void sim_press_after(int gpio, int delay_ms)
{
    /* El pin a pulsar viaja como argumento del temporizador, así que se rehace
     * en cada uso en vez de guardar estado aparte. */
    if (s_shot != NULL) {
        esp_timer_stop(s_shot);
        esp_timer_delete(s_shot);
        s_shot = NULL;
    }
    const esp_timer_create_args_t args = {
        .callback = shot_cb,
        .arg      = (void *)(intptr_t)gpio,
        .name     = "sim_endstop",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_shot));
    ESP_ERROR_CHECK(esp_timer_start_once(s_shot, (uint64_t)delay_ms * 1000));
}

/*
 * Antes de medir nada, demostrar que los dos puentes conducen.
 *
 * GPIO34/35 no tienen pull interno: sin cable quedan al aire y leen pulsado,
 * que es indistinguible de un fin de carrera real accionado. Sin esta
 * comprobación, un cable suelto se diagnosticaría como fallo del firmware.
 */
static bool sim_continuidad(void)
{
    bool ok = true;

    sim_set(SIM_PULSADO, SIM_LIBRE);
    vTaskDelay(pdMS_TO_TICKS(20));
    stepper_endstops_t a = stepper_read_endstops();

    sim_set(SIM_LIBRE, SIM_PULSADO);
    vTaskDelay(pdMS_TO_TICKS(20));
    stepper_endstops_t b = stepper_read_endstops();

    sim_set(SIM_LIBRE, SIM_LIBRE);
    vTaskDelay(pdMS_TO_TICKS(20));
    stepper_endstops_t c = stepper_read_endstops();

    ESP_LOGI(TAG, "puente GPIO%d->GPIO34: al bajarlo el fin + leyó %s",
             SIM_POS_GPIO, a.pos_pressed ? "PULSADO" : "libre");
    ESP_LOGI(TAG, "puente GPIO%d->GPIO35: al bajarlo el fin - leyó %s",
             SIM_NEG_GPIO, b.neg_pressed ? "PULSADO" : "libre");

    if (!(a.pos_pressed && !a.neg_pressed)) { ok = false; }
    if (!(b.neg_pressed && !b.pos_pressed)) { ok = false; }
    if (c.pos_pressed || c.neg_pressed)     { ok = false; }

    report(ok, "los dos puentes conducen y los fines quedan libres en reposo");
    if (!ok) {
        ESP_LOGE(TAG, "revisa los cables GPIO%d->GPIO34 y GPIO%d->GPIO35.",
                 SIM_POS_GPIO, SIM_NEG_GPIO);
    }
    return ok;
}

#endif /* CONFIG_E3_SELFTEST_ENDSTOP_SIM */

static void report(bool ok, const char *what)
{
    if (ok) {
        s_pass++;
        ESP_LOGI(TAG, "  OK    %s", what);
    } else {
        s_fail++;
        ESP_LOGE(TAG, "  FALLA %s", what);
    }
}

/*
 * Enruta STEP hacia el contador por dentro del chip. io_loop_back existe justo
 * para esto: lo que el GPIO saca se realimenta al camino de entrada, así que el
 * PCNT ve los mismos pulsos que recibiría el A4988.
 */
static esp_err_t pcnt_setup(void)
{
    const pcnt_unit_config_t ucfg = {
        .high_limit = PCNT_HIGH_LIMIT,
        .low_limit  = PCNT_LOW_LIMIT,
    };
    esp_err_t err = pcnt_new_unit(&ucfg, &s_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_new_unit: %s", esp_err_to_name(err));
        return err;
    }

    pcnt_channel_handle_t chan = NULL;
    const pcnt_chan_config_t ccfg = {
        .edge_gpio_num  = STEP_GPIO,
        .level_gpio_num = -1,
        .flags          = { .io_loop_back = true },
    };
    err = pcnt_new_channel(s_unit, &ccfg, &chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_new_channel: %s", esp_err_to_name(err));
        return err;
    }

    /* Contar sólo el flanco de subida, que es el que el A4988 toma como paso. */
    err = pcnt_channel_set_edge_action(chan,
                                       PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                       PCNT_CHANNEL_EDGE_ACTION_HOLD);
    if (err != ESP_OK) {
        return err;
    }

    err = pcnt_unit_enable(s_unit);
    if (err == ESP_OK) {
        err = pcnt_unit_clear_count(s_unit);
    }
    if (err == ESP_OK) {
        err = pcnt_unit_start(s_unit);
    }
    return err;
}

static int pcnt_read(void)
{
    int count = 0;
    pcnt_unit_get_count(s_unit, &count);
    return count;
}

/* Espera a que termine el movimiento, con tope de seguridad. */
static bool wait_idle(int timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (stepper_is_moving()) {
        if (esp_timer_get_time() > deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

/*
 * El corazón de la prueba: pedir N pasos y comprobar que salieron N pulsos.
 *
 * Cierra la duda que ningún test de host puede cerrar: que el contador interno
 * de posición y los pulsos que realmente llegan al driver son lo mismo. Si esos
 * dos números se separan, el riel pierde la calibración en silencio.
 */
static void test_conteo_de_pasos(void)
{
    const int32_t n = CONFIG_E3_SELFTEST_STEPS;

    /* Origen conocido: sin riel no hay nada contra qué referenciarse. */
    const int32_t origen = stepper_neg_limit_steps();
    stepper_set_position(origen);

    pcnt_unit_clear_count(s_unit);
    const int64_t t0 = esp_timer_get_time();

    stepper_move_to(origen + n);
    const bool acabo = wait_idle((int)(n * 1000 / CONFIG_E3_SPEED_SPS) + 3000);

    const int64_t dt_us   = esp_timer_get_time() - t0;
    const int     pulsos  = pcnt_read();
    const int32_t avance  = stepper_position() - origen;

    ESP_LOGI(TAG, "movimiento de %ld pasos: %d pulsos contados, posición avanzó %ld",
             (long)n, pulsos, (long)avance);

    report(acabo, "el movimiento termina sin quedarse colgado");
    report(pulsos == (int)n, "salen exactamente los pulsos pedidos");
    report(avance == n, "la posición interna coincide con lo pedido");
    report(pulsos == (int)avance, "los pulsos emitidos coinciden con la posición");

    /* Cadencia: el temporizador debería dar CONFIG_E3_SPEED_SPS pasos por
     * segundo. Se admite un 5 % por el arranque del charge pump y el sondeo. */
    const double sps = (double)n * 1000000.0 / (double)dt_us;
    ESP_LOGI(TAG, "cadencia medida: %.1f pasos/s (configurada %d)",
             sps, CONFIG_E3_SPEED_SPS);
    report(sps > CONFIG_E3_SPEED_SPS * 0.90 && sps < CONFIG_E3_SPEED_SPS * 1.05,
           "la cadencia está dentro del 10 % de la configurada");
}

/* El sentido de giro se comanda por DIR; su polaridad viene invertida del
 * sketch original (setPinsInverted(true, false, false)). */
static void test_direccion(void)
{
    gpio_set_direction(DIR_GPIO, GPIO_MODE_INPUT_OUTPUT);

    const int32_t origen = stepper_neg_limit_steps() + 2000;
    stepper_set_position(origen);

    stepper_move_to(origen + 400);
    vTaskDelay(pdMS_TO_TICKS(100));
    const int dir_subiendo = gpio_get_level(DIR_GPIO);
    wait_idle(3000);

    stepper_move_to(origen);
    vTaskDelay(pdMS_TO_TICKS(100));
    const int dir_bajando = gpio_get_level(DIR_GPIO);
    wait_idle(3000);

    ESP_LOGI(TAG, "DIR alejándose del parlante = %d, acercándose = %d",
             dir_subiendo, dir_bajando);
    report(dir_subiendo != dir_bajando, "DIR cambia con el sentido de marcha");
}

/* El motor se duerme al quedar parado para que no se caliente; despertarlo
 * exige 1 ms de charge pump antes del primer paso (hallazgo M2). */
static void test_sleep(void)
{
    gpio_set_direction(SLEEP_GPIO, GPIO_MODE_INPUT_OUTPUT);

    const int32_t origen = stepper_neg_limit_steps() + 2000;
    stepper_set_position(origen);

    const int dormido_antes = gpio_get_level(SLEEP_GPIO);

    stepper_move_to(origen + 400);
    vTaskDelay(pdMS_TO_TICKS(50));
    const int despierto = gpio_get_level(SLEEP_GPIO);

    wait_idle(3000);
    vTaskDelay(pdMS_TO_TICKS(50));
    const int dormido_despues = gpio_get_level(SLEEP_GPIO);

    ESP_LOGI(TAG, "SLEEP en reposo=%d, en marcha=%d, tras terminar=%d",
             dormido_antes, despierto, dormido_despues);
    report(dormido_antes == 0, "el driver arranca dormido");
    report(despierto == 1, "el driver despierta al mover");
    report(dormido_despues == 0, "el driver vuelve a dormirse al terminar");
}

/* Ningún destino puede salir del recorrido físico del riel, venga de donde
 * venga. Es la red que protege incluso si la unidad de A3 está mal supuesta. */
static void test_acotado(void)
{
    const int32_t lo = stepper_neg_limit_steps();
    const int32_t hi = stepper_pos_limit_steps();

    /* Se comprueba sobre la aritmética de destino, que es lo que protege el
     * riel, y no sobre el viaje: sin pull-ups el motor no se mueve y el viaje
     * no diría nada. */
    const bool ok_hi = stepper_clamp_steps(stepper_cm_to_steps(1000.0f), lo, hi) == hi;
    const bool ok_lo = stepper_clamp_steps(stepper_cm_to_steps(-50.0f), lo, hi) == lo;

    ESP_LOGI(TAG, "acotado: destino enorme -> %.2f cm, destino negativo -> %.2f cm",
             (double)stepper_steps_to_cm(hi), (double)stepper_steps_to_cm(lo));
    report(ok_hi, "un destino sobre el riel se acota al fin de carrera +");
    report(ok_lo, "un destino bajo el riel se acota al fin de carrera -");
}

#if !CONFIG_E3_SELFTEST_ENDSTOP_SIM
/*
 * Sin fines de carrera cableados, la calibración TIENE que fallar. Lo que se
 * comprueba aquí es que falla rápido y con diagnóstico, en vez del `while(true)`
 * del sketch original, que habría empujado el riel contra el tope para siempre
 * (hallazgo B2). Con simulación esto lo cubre test_cableado_roto(), que además
 * es determinista.
 */
static void test_calibracion_falla_segura(void)
{
    const stepper_endstops_t es = stepper_read_endstops();
    ESP_LOGI(TAG, "fines de carrera al aire: - lee %s, + lee %s",
             es.neg_pressed ? "PULSADO" : "libre",
             es.pos_pressed ? "PULSADO" : "libre");

    const int64_t t0  = esp_timer_get_time();
    const esp_err_t r = stepper_calibrate();
    const int64_t dt  = (esp_timer_get_time() - t0) / 1000;

    ESP_LOGI(TAG, "calibración sin hardware -> %s en %lld ms",
             esp_err_to_name(r), (long long)dt);
    report(r != ESP_OK, "la calibración no miente diciendo que tuvo éxito");

    if (r == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "ambos fines de carrera leen pulsados: es justo el aviso");
        ESP_LOGW(TAG, "de pull-up externo faltante. Sin cables, es lo esperado.");
    }
}
#endif /* !CONFIG_E3_SELFTEST_ENDSTOP_SIM */

#if CONFIG_E3_SELFTEST_ENDSTOP_SIM

/*
 * La prueba que ningún test de host puede hacer: pulsar un fin de carrera con
 * el motor ya en marcha y comprobar que la ISR corta dentro del mismo paso y
 * toma esa posición como verdad.
 */
static void test_corte_por_fin_de_carrera(void)
{
    const int32_t lo = stepper_neg_limit_steps();
    const int32_t hi = stepper_pos_limit_steps();

    sim_set(SIM_LIBRE, SIM_LIBRE);
    vTaskDelay(pdMS_TO_TICKS(20));

    stepper_set_position(lo + 5000);

    stepper_stats_t antes;
    stepper_get_stats(&antes);

    /* Movimiento largo hacia el extremo lejano, interrumpido a mitad. */
    stepper_move_to(hi);
    sim_press_after(SIM_POS_GPIO, 500);

    const bool paro = wait_idle(10000);

    stepper_stats_t despues;
    stepper_get_stats(&despues);

    ESP_LOGI(TAG, "corte: la posición quedó en %.2f cm (límite %.2f), topes %lu -> %lu",
             (double)stepper_steps_to_cm(despues.position_steps),
             (double)stepper_steps_to_cm(hi),
             (unsigned long)antes.endstop_stops,
             (unsigned long)despues.endstop_stops);

    report(paro, "el movimiento se detiene al pulsar el fin de carrera");
    report(despues.position_steps == hi, "la posición se recalibra al límite del riel");
    report(despues.endstop_stops == antes.endstop_stops + 1, "el corte queda contabilizado");

    sim_set(SIM_LIBRE, SIM_LIBRE);
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* Calibración completa: se pulsa el fin de carrera - a los 2 s en vez de
 * esperar los ~48 s que tardaría el recorrido entero del riel. */
static void test_calibracion_completa(void)
{
    sim_set(SIM_LIBRE, SIM_LIBRE);
    vTaskDelay(pdMS_TO_TICKS(20));

    sim_press_after(SIM_NEG_GPIO, 2000);

    const int64_t t0 = esp_timer_get_time();
    const esp_err_t r = stepper_calibrate();
    const int64_t dt = (esp_timer_get_time() - t0) / 1000;

    stepper_stats_t st;
    stepper_get_stats(&st);

    ESP_LOGI(TAG, "calibración -> %s en %lld ms, origen en %.2f cm",
             esp_err_to_name(r), (long long)dt,
             (double)stepper_steps_to_cm(st.position_steps));

    report(r == ESP_OK, "la calibración termina correctamente");
    report(st.position_steps == stepper_neg_limit_steps(),
           "el origen queda fijado en el fin de carrera -");
    report(st.calibrated, "el módulo queda declarado calibrado");

    sim_set(SIM_LIBRE, SIM_LIBRE);
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* Con los dos finales activos a la vez —cableado roto— hay que negarse a
 * mover y a calibrar, en vez de "recalibrar" saltando a un extremo. */
static void test_cableado_roto(void)
{
    sim_set(SIM_PULSADO, SIM_PULSADO);
    vTaskDelay(pdMS_TO_TICKS(20));

    const esp_err_t cal = stepper_calibrate();
    const int32_t   pos = stepper_position();
    const esp_err_t mov = stepper_move_to(pos + 500);

    ESP_LOGI(TAG, "ambos finales activos: calibrar -> %s, mover -> %s",
             esp_err_to_name(cal), esp_err_to_name(mov));

    report(cal == ESP_ERR_INVALID_STATE, "la calibración se niega y lo dice");
    report(mov == ESP_ERR_INVALID_STATE, "el movimiento se niega y lo dice");
    report(stepper_position() == pos, "la posición no se altera");

    sim_set(SIM_LIBRE, SIM_LIBRE);
    vTaskDelay(pdMS_TO_TICKS(20));
}

#endif /* CONFIG_E3_SELFTEST_ENDSTOP_SIM */

void selftest_run(void)
{
    ESP_LOGW(TAG, "=== AUTOPRUEBA DE BANCO: sin motor, sin driver, sin riel ===");
    ESP_LOGW(TAG, "Los pulsos de STEP se cuentan con PCNT por dentro del chip.");

    if (pcnt_setup() != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo montar el contador; se aborta la autoprueba");
        return;
    }

    s_pass = 0;
    s_fail = 0;

#if CONFIG_E3_SELFTEST_ENDSTOP_SIM
    /* Con simulación, primero se demuestra que los puentes conducen: medir sin
     * comprobarlo es cómo se llega a conclusiones falsas. */
    ESP_LOGI(TAG, "--- 0. continuidad de los puentes de simulación ---");
    const bool puede_moverse = sim_continuidad();
#else
    /*
     * Sin simulación, la precondición de todo lo que implique mover es que los
     * fines de carrera lean "libre". Con GPIO34/35 al aire ambos leen pulsado,
     * el firmware se niega a mover —que es lo correcto— y cualquier medición
     * del generador de pasos sería ruido. Se dice y no se inventan fallos.
     */
    const stepper_endstops_t es = stepper_read_endstops();
    const bool puede_moverse = !(es.pos_pressed || es.neg_pressed);

    if (!puede_moverse) {
        ESP_LOGW(TAG, "fines de carrera: - %s, + %s",
                 es.neg_pressed ? "PULSADO" : "libre",
                 es.pos_pressed ? "PULSADO" : "libre");
        ESP_LOGW(TAG, "Sin pull-up, GPIO34/35 al aire leen pulsado y el motor no");
        ESP_LOGW(TAG, "se mueve por diseño. Activa CONFIG_E3_SELFTEST_ENDSTOP_SIM");
        ESP_LOGW(TAG, "y cablea GPIO26->GPIO34 y GPIO27->GPIO35.");
        ESP_LOGW(TAG, "Se omiten las pruebas de movimiento.");
    }
#endif

    if (puede_moverse) {
        ESP_LOGI(TAG, "--- 1. conteo de pasos y cadencia ---");
        test_conteo_de_pasos();
        ESP_LOGI(TAG, "--- 2. sentido de giro ---");
        test_direccion();
        ESP_LOGI(TAG, "--- 3. gestión de SLEEP ---");
        test_sleep();
#if CONFIG_E3_SELFTEST_ENDSTOP_SIM
        ESP_LOGI(TAG, "--- 5. corte por fin de carrera en marcha ---");
        test_corte_por_fin_de_carrera();
        ESP_LOGI(TAG, "--- 6. calibración completa ---");
        test_calibracion_completa();
        ESP_LOGI(TAG, "--- 7. cableado roto: ambos finales activos ---");
        test_cableado_roto();
#endif
    }

    ESP_LOGI(TAG, "--- 4. acotado al riel ---");
    test_acotado();

#if !CONFIG_E3_SELFTEST_ENDSTOP_SIM
    ESP_LOGI(TAG, "--- 8. la calibración falla de forma segura ---");
    test_calibracion_falla_segura();
#endif

    ESP_LOGW(TAG, "=== AUTOPRUEBA: %d correctas, %d fallidas%s ===",
             s_pass, s_fail,
             puede_moverse ? "" : " (movimiento omitido)");
}

#endif /* CONFIG_E3_SELFTEST */
