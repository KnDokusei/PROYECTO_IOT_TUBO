# server-kundt

Aporte al servidor **curiousBeagle** para dar de alta el tipo de experimento
`kundt`. Vive aquí y no como pull request porque curiousBeagle pertenece a otro
equipo (DopamineLabsLTDA) y el molde que se copió es suyo y todavía preliminar.

## Qué contiene

| Archivo | Qué es |
|---|---|
| `kundt-skeleton.patch` | Cambios a archivos existentes de curiousBeagleAPI |
| `src/dto/kundt.dto.ts` | DTO del tipo `kundt`, archivo nuevo |
| `prisma/migrations/4_kundt/migration.sql` | Tabla `Kundt` con clave foránea a `ExpBase` |
| `prisma/seed-kundt.sql` | Siembra local idempotente de las cinco tablas encadenadas |
| `prisma/seed-kundt.sh` | Genera el hash argon2 y llama a psql |
| `env.example` | Plantilla de variables, sin valores reales |

## Cómo aplicarlo

```bash
git clone git@github.com:DopamineLabsLTDA/curiousBeagleAPI.git
cd curiousBeagleAPI
git checkout -b feat/kundt-tube
git apply /ruta/a/server-kundt/kundt-skeleton.patch
cp -r /ruta/a/server-kundt/src/dto/kundt.dto.ts src/dto/
cp -r /ruta/a/server-kundt/prisma/* prisma/
```

Luego `npx prisma migrate deploy` y `./prisma/seed-kundt.sh`.

## Contrato MQTT

Los tres módulos comparten un único controlador lógico y se distinguen por los
campos que publican, así que los tres usan el mismo tópico.

```
dev-status/kundt/<platform>/<controller>     dispositivo -> servidor
ctrl-channel/kundt/<platform>/<controller>   servidor -> dispositivo
```

**Asimetría del payload**, verificada ejecutando el servidor y no documentada en
ningún repositorio de curiousBeagle: NestJS envuelve lo que emite en
`{pattern, data}` pero acepta objetos planos al recibir.

```
baja:  {"pattern":"ctrl-channel/kundt/1/1","data":{"id":1,"actuators":{...}}}
sube:  {"device":{"id":1,"platform_id":1},"sensors":{...}}
```

El firmware tiene que leer `data.actuators`, no `actuators`.

## Trampas encontradas al montarlo

| Trampa | Detalle |
|---|---|
| `POSTGRES_USER` tiene que ser `sirius` | El volcado `schema.sql` lleva 15 sentencias `OWNER TO sirius` y no crea el rol. Con otro nombre la base no inicializa. |
| Prisma rechaza migrar sobre base no vacía | Error `P3005`. El volcado del repositorio es de un esquema anterior en kebab-case; hay que borrar esas tablas primero. |
| Falta la fila de `site."Site"` | Sin ella `getReservationConfigs` devuelve `undefined`, revienta el `JSON.parse` de `getBlocks` y **todo endpoint protegido responde 500**. Le pasa igual a estanque y péndulo en una base recién creada. |
| Variables de Valkey con nombres distintos | La API lee `VALKEY_HOST`/`PORT`/`PASS`; el compose usa `VALKEY_PASSWORD`. |
| `start:dev` no lee `.env` | Lee `.env-dev`. |
| El generador de Prisma 7 emite TypeScript | `prisma-client` genera fuentes `.ts` con importaciones de extensión `.js`, que ni `require()` ni `ts-node` en CommonJS resuelven. Por eso la siembra está en SQL. |

## Dos fallos preexistentes en curiousBeagleAPI

**`ValidReservationGuard` lee `request.params.id`.** Toda ruta que declare
`:platform_id` deja ese parámetro indefinido, el guard busca una reserva para
`NaN` y el POST responde 403 siempre. Estanque usa `:id` y funciona; difracción
de electrones usa `:platform_id` y está roto. El tipo `kundt` usa `:id` para no
heredarlo.

**`exp_entry!.id` con aserción de no-nulo** en el manejador MQTT. Un `device.id`
que no corresponde a ningún controlador lanza `TypeError` y el mensaje se
descarta sin que el dispositivo se entere.

## Estado

Verificado de punta a punta el 2026-09-09 contra un banco local con las tres
ESP32 físicas: 19 comprobaciones automáticas y 0 fallos, más la fusión parcial
probada con dos placas publicando a la vez.
