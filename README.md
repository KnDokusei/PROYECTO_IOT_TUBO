# PROYECTO_IOT_TUBO

Firmware **ESP-IDF** para el Laboratorio Remoto — Tubo de Kundt (USM).

Puerto del firmware original de Arduino IDE (Jose Borquez, ago. 2024;
Prof. Alfredo Navarro) a ESP-IDF v5.5.5.

## El experimento

Parlante fijo en un extremo del tubo, émbolo motorizado en el otro, micrófono
fijo en la entrada. Al barrer la longitud de la cavidad se atraviesan las
resonancias (L = n·λ/2), y de ahí se despeja la velocidad del sonido.

## Módulos

Cada carpeta es un **proyecto ESP-IDF independiente**, con su propia placa,
compilable y flasheable por separado.

| Módulo | Placa | Función | Estado |
|---|---|---|---|
| [E1-Mic](E1-Mic/) | DOIT DEVKIT V1 | Micrófono → WebSocket binario | ✅ **migrado** |
| [E2-SineGen](E2-SineGen/) | DOIT DEVKIT V1 | AD9833 + servo | ✅ **migrado** |
| [E3-StepMotor](E3-StepMotor/) | DOIT DEVKIT V1 | A4988 + fines de carrera | ✅ **migrado** (en validación sobre el riel real) |
| [EC-Cameras](EC-Cameras/) | AI-Thinker ESP32-CAM | Servidor MJPEG ×3 | ⬜ esqueleto |

Los cuatro módulos son **independientes entre sí**: no comparten pines ni
puertos, y cada uno puede migrarse o flashearse sin tocar los demás. Los
esqueletos ya compilan y arrancan: inicializan NVS y avisan por consola de que
su lógica todavía no está portada.

Mientras tanto, el firmware operativo de EC sigue siendo el sketch de Arduino
del proyecto original.

**E3 (émbolo):** una máquina de estados (`switch/case` en
`E3-StepMotor/main/main.c`) calibra al arrancar, espera consignas por MQTT y
mueve el émbolo directo a la posición pedida. Los switches son la verdad
física: el SW derecho está a 26 cm del parlante y el SW izquierdo a 84 cm. La
primera vez recorre el riel completo y guarda en NVS el largo en pasos; desde
ahí, cada arranque sólo busca el SW derecho. La conversión es una recta,
`pasos = m · cm`, con pendiente calibrada o teórica (250 pasos/cm) según cuál
de las dos funciones `pendiente()` quede sin comentar. Todo error mayor a
1 cm sale como WARN: largo medido vs teórico, posición alcanzada vs pedida, o
deriva al tocar un switch. Una deriva así, además, dispara un nuevo barrido.

**Pendiente:** llevar el mismo patrón de máquina de estados a E1 y E2.

## Estructura

```
components/           componentes compartidos por los 4 módulos
  kundt_config/         configuración en NVS (kit, credenciales, servidor)
  kundt_wifi/           WiFi por eventos + reconexión con backoff
  kundt_api/            cliente HTTP del backend (E2 y E3)
  ad9833/               driver SPI del generador DDS
E1-Mic/               proyecto IDF · migrado
  components/
    mic_capture/        adc_continuous + conversión a PCM16 (propio de E1)
  main/
E2-SineGen/           proyecto IDF · migrado
  components/
    servo/                servo de volumen por LEDC (propio de E2)
E3-StepMotor/         proyecto IDF · migrado
  components/
    stepper/            A4988 por alarma de GPTimer (propio de E3)
EC-Cameras/           proyecto IDF · esqueleto
test/host/            tests sin hardware
tools/                bancos de pruebas (WebSocket para E1, HTTP para E2/E3)
```

Los componentes compartidos viven en la raíz precisamente para no repetirlos:
el proyecto Arduino tiene `wifiConnect()` duplicado **cuatro veces** porque el
IDE no permite compartir funciones entre sketches.

## Compilar y flashear

```bash
source ~/esp/esp-idf/export.sh

cd E1-Mic            # o E2-SineGen, E3-StepMotor, EC-Cameras
idf.py menuconfig    # → "Kundt tube configuration"
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Si tu máquina va justa de RAM, limita el paralelismo:

```bash
CMAKE_BUILD_PARALLEL_LEVEL=2 idf.py build
```

### Configuración

SSID, contraseña, IP del servidor y número de kit (1–5) se graban en **NVS** en
el primer arranque y se pueden cambiar después sin recompilar. El puerto del
WebSocket se deriva del kit: `8080 + kit`.

Los símbolos de Kconfig están definidos una sola vez, en el componente
`kundt_config`, así que los cuatro módulos los heredan.

> `sdkconfig` está en `.gitignore`: **las credenciales del laboratorio nunca se
> suben al repositorio**.

## Tests

```bash
make -C test/host        # 553 comprobaciones (E1 + E2 + E3)
make -C test/host e1     # sólo E1: conversión de muestras
make -C test/host e2     # sólo E2: DDS, servo y parseo de la API
make -C test/host e3     # sólo E3: recta pasos↔cm y acotado
make -C test/host asan   # todos bajo AddressSanitizer + UBSan
```

`E1-Mic/components/mic_capture/mic_dsp.c` no depende de ESP-IDF, así que el
firmware y los tests compilan exactamente el mismo código.

## Probar con hardware

Dos bancos de pruebas que sustituyen al backend, ambos sin dependencias
externas (sólo la stdlib de Python 3):

```bash
# E1: recibe el audio, lo analiza en vivo y lo guarda como WAV
python3 tools/ws_test_server.py --port 8081 --expect-hz 1200

# E2/E3: sirve el contrato de la API y lo rompe a propósito
python3 tools/http_test_server.py --port 5000

# E3: barrido de posiciones del émbolo, y acepta su PUT de telemetría
python3 tools/http_test_server.py --port 5000 --embolo
```

El segundo recorre una secuencia de fases con 404, HTML de error, JSON truncado
y valores fuera de rango, para comprobar en hardware que el firmware **mantiene
el último valor válido** en vez de caer a cero.

Ambos módulos se validan en una ESP32 pelada con un solo puente entre GPIO25 y
GPIO34.

## Documentación

El análisis de la migración módulo a módulo, los procedimientos de prueba y el
checklist previo a instalar se mantienen **fuera de este repositorio**, junto al
resto de la documentación del proyecto.

## Autoría y licencia

Este repositorio es un **port a ESP-IDF** del firmware original desarrollado por
**Jose Borquez Gaete** (agosto 2024) para el proyecto de actualización del
Laboratorio Remoto — Tubo de Kundt, dirigido por el **Prof. Alfredo Navarro**,
Universidad Técnica Federico Santa María.

El diseño del experimento, los esquemáticos y la lógica de los cuatro módulos
son obra suya. Este trabajo migra ese firmware a ESP-IDF conservando su
comportamiento, y documenta las diferencias donde las hay.

> **Licencia pendiente.** No se incluye archivo `LICENSE` a propósito: al
> tratarse de un trabajo derivado, la licencia corresponde definirla al autor
> original y al profesor a cargo. Hasta entonces, todos los derechos quedan
> reservados y el código no debe redistribuirse sin su autorización.

## Configuración local

`sdkconfig` está en `.gitignore`, así que tras clonar hay que configurar cada
módulo antes de compilar:

```bash
cd E1-Mic          # o el módulo que toque
idf.py menuconfig  # → "Kundt tube configuration"
idf.py build
```

Las credenciales WiFi y la IP del servidor se graban en NVS en el primer
arranque. **Nunca se versionan.**

