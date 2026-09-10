-- seed-kundt.sql - Siembra local para probar el tipo de experimento `kundt`.
--
-- NO ES PARA PRODUCCIÓN: crea un usuario de prueba con contraseña conocida.
-- Se invoca desde seed-kundt.sh, que le pasa el hash argon2 en :hash.
--
-- Va en SQL y no con el cliente de Prisma porque el generador `prisma-client`
-- de Prisma 7 emite fuentes TypeScript con importaciones de extensión .js, que
-- ni require() ni ts-node en modo CommonJS resuelven.
--
-- Por qué hace falta sembrar: getKundtExp no busca la fila directamente,
-- recorre una cadena de cinco tablas.
--
--   ExpTypes -> Platform -> Controller -> ExpBase -> Kundt
--
-- Y getExpBaseByIndex compara el número real de filas Controller contra el
-- campo n_controllers de la plataforma: si no cuadran lanza un 500 cuyo mensaje
-- habla de "número inesperado de controladores", no de que falte sembrar.
--
-- Es idempotente: se puede volver a ejecutar sin duplicar nada.

\set ON_ERROR_STOP on

CREATE TEMP TABLE _seed_params(hash text, cfg text);
INSERT INTO _seed_params VALUES (:'hash', :'cfg');

DO $seed$
DECLARE
    -- Los tres módulos comparten un único controlador lógico y se distinguen
    -- por los campos que publican, así que la plataforma declara uno solo.
    c_n_controllers CONSTANT int  := 1;
    c_exp_name      CONSTANT text := 'Tubo de Kundt';
    c_plat_name     CONSTANT text := 'Tubo de Kundt - Equipo 1';
    c_ctrl_ip       CONSTANT text := '192.168.0.60';
    -- Dominio .invalid: reservado por RFC 2606, nunca resuelve.
    c_mail          CONSTANT text := 'kundt.local@test.invalid';

    v_exp_id   int;
    v_plat_id  int;
    v_ctrl_id  int;
    v_base_id  int;
    v_user_id  int;
    v_res_id   int;
    v_n_ctrl   int;
    v_hash     text;
    v_cfg      text;
BEGIN
    SELECT hash, cfg INTO v_hash, v_cfg FROM _seed_params;

    ---------------------------------------------------------------- ExpTypes
    SELECT id INTO v_exp_id
      FROM "remote-experiments"."ExpTypes" WHERE exp_name = c_exp_name;
    IF v_exp_id IS NULL THEN
        INSERT INTO "remote-experiments"."ExpTypes" (exp_name, exp_desc)
        VALUES (c_exp_name,
                'Parlante fijo, émbolo motorizado y micrófono. Barrer la '
                || 'longitud atraviesa las resonancias y de ahí se despeja '
                || 'la velocidad del sonido.')
        RETURNING id INTO v_exp_id;
        RAISE NOTICE '  ExpTypes creado          id=%', v_exp_id;
    ELSE
        RAISE NOTICE '  ExpTypes ya existía      id=%', v_exp_id;
    END IF;

    ---------------------------------------------------------------- Platform
    SELECT id INTO v_plat_id
      FROM "remote-experiments"."Platform" WHERE name = c_plat_name;
    IF v_plat_id IS NULL THEN
        INSERT INTO "remote-experiments"."Platform"
               ("expId", name, description, n_controllers, status)
        VALUES (v_exp_id, c_plat_name,
                'Montaje de laboratorio con E1-Mic, E2-SineGen, E3-StepMotor '
                || 'y tres cámaras.',
                c_n_controllers, true)
        RETURNING id INTO v_plat_id;
        RAISE NOTICE '  Platform creada          id=%', v_plat_id;
    ELSE
        RAISE NOTICE '  Platform ya existía      id=%', v_plat_id;
    END IF;

    -- Corregir n_controllers aquí evita el 500 de getExpBaseByIndex, que es
    -- el error más difícil de diagnosticar de toda la cadena.
    UPDATE "remote-experiments"."Platform"
       SET n_controllers = c_n_controllers
     WHERE id = v_plat_id AND n_controllers <> c_n_controllers;

    -------------------------------------------------------------- Controller
    SELECT id INTO v_ctrl_id
      FROM "remote-experiments"."Controller"
     WHERE platform_id = v_plat_id ORDER BY id ASC LIMIT 1;
    IF v_ctrl_id IS NULL THEN
        INSERT INTO "remote-experiments"."Controller" (platform_id, ip, status)
        VALUES (v_plat_id, c_ctrl_ip, true)
        RETURNING id INTO v_ctrl_id;
        RAISE NOTICE '  Controller creado        id=%', v_ctrl_id;
    ELSE
        RAISE NOTICE '  Controller ya existía    id=%', v_ctrl_id;
    END IF;

    SELECT count(*) INTO v_n_ctrl
      FROM "remote-experiments"."Controller" WHERE platform_id = v_plat_id;
    IF v_n_ctrl > c_n_controllers THEN
        RAISE WARNING 'La plataforma tiene % controladores y n_controllers vale %. '
                      'Borra los sobrantes o la API responderá 500.',
                      v_n_ctrl, c_n_controllers;
    END IF;

    ----------------------------------------------------------------- ExpBase
    SELECT id INTO v_base_id
      FROM "remote-experiments"."ExpBase" WHERE controller_id = v_ctrl_id;
    IF v_base_id IS NULL THEN
        INSERT INTO "remote-experiments"."ExpBase" (controller_id)
        VALUES (v_ctrl_id) RETURNING id INTO v_base_id;
        RAISE NOTICE '  ExpBase creado           id=%', v_base_id;
    ELSE
        RAISE NOTICE '  ExpBase ya existía       id=%', v_base_id;
    END IF;

    ------------------------------------------------------------------- Kundt
    IF NOT EXISTS (SELECT 1 FROM "remote-experiments"."Kundt" WHERE id = v_base_id) THEN
        INSERT INTO "remote-experiments"."Kundt" (id) VALUES (v_base_id);
        RAISE NOTICE '  Kundt creado             id=%', v_base_id;
    ELSE
        RAISE NOTICE '  Kundt ya existía         id=%', v_base_id;
    END IF;

    -------------------------------------------------------------------- User
    -- update_date no tiene default en la base: lo gestiona Prisma con
    -- @updatedAt, así que en SQL hay que darlo explícito.
    SELECT id INTO v_user_id FROM users."User" WHERE mail = c_mail;
    IF v_user_id IS NULL THEN
        INSERT INTO users."User"
               (names, surnames, mail, password_hash, type, status, update_date)
        VALUES ('Kundt', 'Local', c_mail, v_hash, 'ADMIN', true, now())
        RETURNING id INTO v_user_id;
        RAISE NOTICE '  User creado              id=%  %', v_user_id, c_mail;
    ELSE
        RAISE NOTICE '  User ya existía          id=%  %', v_user_id, c_mail;
    END IF;

    ------------------------------------------------------------- Reservation
    -- checkReservationAccess pide start_date <= ahora <= end_date y estado
    -- VALID. Un año a cada lado evita tener que resembrar cada día.
    SELECT id INTO v_res_id
      FROM reservations."Reservation"
     WHERE user_id = v_user_id AND platform_id = v_plat_id AND status = 'VALID';
    IF v_res_id IS NULL THEN
        INSERT INTO reservations."Reservation"
               (user_id, platform_id, start_date, end_date, status,
                time_block, updated)
        VALUES (v_user_id, v_plat_id,
                now() - interval '1 year', now() + interval '1 year',
                'VALID', '{}', now())
        RETURNING id INTO v_res_id;
        RAISE NOTICE '  Reservation creada       id=%  válida un año', v_res_id;
    ELSE
        RAISE NOTICE '  Reservation ya existía   id=%', v_res_id;
    END IF;
    -------------------------------------------------------------------- Site
    -- Sin esta fila, getReservationConfigs devuelve undefined y el JSON.parse
    -- de getBlocks lanza una excepción. Como ValidReservationGuard llama a
    -- getBlockStart antes que nada, TODO endpoint protegido responde 500 con
    -- el mensaje genérico "Internal server error", que no apunta a la causa.
    --
    -- La columna es de tipo Json pero el código hace JSON.parse sobre ella,
    -- así que lo que se guarda es una CADENA JSON, no un objeto: de ahí el
    -- to_jsonb sobre el texto.
    IF NOT EXISTS (SELECT 1 FROM site."Site") THEN
        INSERT INTO site."Site" (id, updated_by_user, reservation_config, updated_at)
        VALUES (1, v_user_id, to_jsonb(v_cfg), now());
        RAISE NOTICE '  Site creado              bloques horarios cargados';
    ELSE
        UPDATE site."Site" SET reservation_config = to_jsonb(v_cfg), updated_at = now();
        RAISE NOTICE '  Site actualizado         bloques horarios recargados';
    END IF;
END
$seed$;

-- Datos que necesita el firmware.
SELECT p.id  AS platform_id,
       c.id  AS controller_id,
       'dev-status/kundt/'   || p.id || '/' || c.id AS publica_en,
       'ctrl-channel/kundt/' || p.id || '/' || c.id AS suscrito_a
  FROM "remote-experiments"."Platform"  p
  JOIN "remote-experiments"."Controller" c ON c.platform_id = p.id
 WHERE p.name = 'Tubo de Kundt - Equipo 1'
 ORDER BY c.id ASC LIMIT 1;
