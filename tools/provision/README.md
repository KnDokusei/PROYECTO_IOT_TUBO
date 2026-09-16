# Provisión por placa

Un binario por módulo, la identidad por placa. Sin recompilar.

## Por qué funciona

`kundt_config` lee de NVS y sólo usa los valores de Kconfig cuando la clave no
está. NVS gana. Así que basta grabar una partición NVS con los valores de esa
placa concreta: la aplicación no se toca.

Sin esto, cinco kits obligarían a cinco compilaciones distintas de E1, E2 y E3
sólo porque cambia un número. **15 binarios contra 4.**

## Por qué el `platform_id` no es opcional

El tópico MQTT es `dev-status/kundt/<platform_id>/<controller_id>`.

Con los cinco kits compartiendo `platform_id`:

- Una consigna de frecuencia al kit 1 **cambia el tono de los cinco**.
- Los quince módulos de medida escriben en **la misma fila** de PostgreSQL.
- El micrófono de un kit pisa la medida de otro.

No es un problema de rendimiento: es corrección.

## Uso

```bash
source ~/esp/esp-idf/export.sh
~/kundt-tools/provision/kundt-provision.sh --kit 3 --port /dev/ttyUSB0
```

Por omisión `platform_id = kit` y `controller_id = 1`: un kit es una plataforma
reservable en curiousBeagle, y sus tres módulos comparten controlador a
propósito (decisión "un solo controlador lógico").

Opciones: `--platform`, `--controller`, `--ssid`, `--server`, `--port`,
`--baud`, `--dry-run`.

La contraseña se lee de `~/.kundt-wifi-pass` (permisos 600) o se pide por
teclado. **No pasarla por `--pass`** salvo en scripts: quedaría en el historial
del shell y en la lista de procesos.

El SSID y la IP del servidor salen de `~/.kundt-provision.conf`, fuera del
repositorio, para no versionar la red del laboratorio:

```sh
KUNDT_SSID=MiRed
KUNDT_SERVER=10.0.0.5
```

## Orden al montar una placa nueva

```bash
idf.py -p /dev/ttyUSB0 -b 115200 erase-flash      # sólo la primera vez
idf.py -p /dev/ttyUSB0 -b 115200 flash            # el binario, igual para todas
~/kundt-tools/provision/kundt-provision.sh --kit N --port /dev/ttyUSB0
```

Reprovisionar después no exige `erase-flash` ni reflashear la aplicación: la
partición NVS se sobrescribe sola en 0,1 s.

## Comprobación

En el arranque:

```
kundt_config: kit=3  ssid="MiRed"  server=10.0.0.5  ws_port=8083
kundt_config: curiousBeagle: platform=3 controller=1  ->  kundt/3/1
```

## Detalles

| Cosa | Valor |
|---|---|
| Namespace NVS | `kundt` |
| Claves | `ssid`, `pass`, `srv_ip`, `kit`, `plat`, `ctrl` |
| Offset de la partición | `0x9000` |
| Tamaño | 24 KiB (`0x6000`) |

Verificado en los cuatro módulos: aunque EC-Cameras use
`CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE`, la partición `nvs` está en `0x9000`
con 24 KB en ambas tablas. Lo único que cambia es el tamaño de la partición de
aplicación. Si algún día se añade una tabla propia, comprobar con:

```bash
python $IDF_PATH/components/partition_table/gen_esp32part.py \
       build/partition_table/partition-table.bin
```

## Qué necesita cada módulo

| Módulo | Usa `platform_id` | Por qué |
|---|---|---|
| E1, E2, E3 | **sí** | Forma su tópico MQTT |
| EC-Cameras | no | No usa MQTT; se identifica por IP. Se provisiona igual, por inventario |
