#!/usr/bin/env bash
# Siembra local para probar el tipo de experimento `kundt`. Ver seed-kundt.sql.
#
# Uso:  ./prisma/seed-kundt.sh
#
# Genera el hash argon2id de la contraseña de prueba con la librería que ya usa
# la API, y se lo pasa a psql. El hash no se guarda en el .sql para que el
# archivo versionado no lleve credenciales, ni siquiera de laboratorio.
set -euo pipefail

CONTAINER="${PG_CONTAINER:-cb-postgre-db}"
PGUSER="${PGUSER:-sirius}"
PGDB="${PGDB:-curious-beagle-db}"
TEST_PASS="${KUNDT_TEST_PASS:-kundt-local-dev}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Los bloques horarios salen de la plantilla del repositorio, no de valores
# inventados aquí.
CFG_FILE="${CFG_FILE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../curiousBeagle" && pwd)/definitions/time-blocks.json}"
CFG="$(node -e "
  const fs = require('fs');
  process.stdout.write(JSON.stringify(JSON.parse(fs.readFileSync(process.argv[1], 'utf8'))));
" "$CFG_FILE")"

HASH="$(node -e "
  const argon = require('argon2');
  argon.hash(process.argv[1], { type: argon.argon2id })
       .then(h => process.stdout.write(h));
" "$TEST_PASS")"

echo "Siembra local del tubo de Kundt"
echo "==============================="
echo

docker exec -i "$CONTAINER" psql -U "$PGUSER" -d "$PGDB" \
    -v "hash=$HASH" -v "cfg=$CFG" -f - < "$HERE/seed-kundt.sql"

echo
echo "Usuario de prueba: kundt.local@test.invalid / $TEST_PASS"
