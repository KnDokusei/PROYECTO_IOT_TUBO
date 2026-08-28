#!/usr/bin/env python3
"""
Servidor HTTP de prueba para los modulos E2 y E3 del Tubo de Kundt.

Sirve el contrato real del backend y, a proposito, tambien lo rompe: recorre
una secuencia de fases que incluye 404, HTML de error y JSON malformado, para
comprobar en hardware que el firmware NO cae a 0 Hz / 0 grados ante una
respuesta mala (hallazgo A4).

Sin dependencias externas.

Uso:
    python3 tools/http_test_server.py                # puerto 5000, secuencia completa
    python3 tools/http_test_server.py --static       # siempre respuesta valida
"""

import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

START = time.time()
STATIC = False
SWEEP = False

# Barrido de volumenes conocidos, para la prueba de integracion E2 -> E1:
# cada escalon cambia el ancho de pulso del PWM que E2 saca por GPIO25, y E1
# deberia ver cambiar el nivel de la senal que capta por GPIO34.
SWEEP_PHASES = [
    (20, 1200, 0),
    (40, 1200, 45),
    (60, 1200, 90),
    (80, 1200, 135),
    (100, 1200, 180),
    (120, 1200, 0),
    (999, 1200, 90),
]

# (hasta_segundo, nombre, frecuencia, volumen)
PHASES = [
    (20, "OK   frecuencia 1200, volumen 45",  1200,  45),
    (35, "OK   frecuencia 2500, volumen 120", 2500, 120),
    (50, "FALLO 404",                         None, None),
    (65, "FALLO pagina HTML de error",        None, None),
    (80, "FALLO JSON malformado",             None, None),
    (95, "FALLO campo 'volumen' ausente",     1800, None),
    (110, "FALLO valores fuera de rango",     99999, 544),
    (999, "OK   vuelta a la normalidad",      1500,  90),
]


def phase_now():
    if SWEEP:
        t = time.time() - START
        for limit, freq, vol in SWEEP_PHASES:
            if t < limit:
                return (limit, f"SWEEP volumen {vol:3d}", freq, vol)
        limit, freq, vol = SWEEP_PHASES[-1]
        return (limit, f"SWEEP volumen {vol:3d}", freq, vol)
    if STATIC:
        return PHASES[0]
    t = time.time() - START
    for p in PHASES:
        if t < p[0]:
            return p
    return PHASES[-1]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # silenciamos el log por defecto; imprimimos el nuestro

    def _send(self, code, body, ctype="application/json"):
        data = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if "/api/kundt/equipo/" not in self.path:
            self._send(404, "no such endpoint", "text/plain")
            return

        limit, name, freq, vol = phase_now()
        t = time.time() - START

        if name.startswith("FALLO 404"):
            self._send(404, "<!DOCTYPE html><html><body>404 Not Found</body></html>",
                       "text/html")
            note = "-> 404 + HTML"
        elif name.startswith("FALLO pagina HTML"):
            self._send(200, "<!DOCTYPE html><html><body>Internal error</body></html>",
                       "text/html")
            note = "-> 200 pero cuerpo HTML"
        elif name.startswith("FALLO JSON malformado"):
            self._send(200, '{"valores": {"frecuencia": 1200,')
            note = "-> JSON truncado"
        elif name.startswith("FALLO campo"):
            self._send(200, json.dumps({"valores": {"frecuencia": freq}}))
            note = f"-> solo frecuencia={freq}, sin volumen"
        elif name.startswith("FALLO valores fuera"):
            self._send(200, json.dumps({"valores": {"frecuencia": freq, "volumen": vol}}))
            note = f"-> frecuencia={freq} volumen={vol} (fuera de rango)"
        else:
            self._send(200, json.dumps({
                "valores": {"frecuencia": freq, "volumen": vol, "embolo": 42.0}
            }))
            note = f"-> frecuencia={freq} volumen={vol}"

        # flush=True: con la salida redirigida a fichero, Python usa buffering de
        # bloque y las lineas se pierden si el proceso muere antes de vaciarlo.
        print(f"[{t:6.1f}s] {self.client_address[0]} {self.path}  {name:38s} {note}",
              flush=True)


def main():
    ap = argparse.ArgumentParser(description="Servidor HTTP de prueba para E2/E3")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--static", action="store_true",
                    help="siempre respuesta valida, sin secuencia de fallos")
    ap.add_argument("--sweep", action="store_true",
                    help="barrido de volumenes conocidos (prueba de integracion E2->E1)")
    args = ap.parse_args()

    global STATIC, SWEEP
    STATIC = args.static
    SWEEP  = args.sweep

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Escuchando en http://{args.host}:{args.port}/api/kundt/equipo/{{kit}}", flush=True)
    if SWEEP:
        print("\nBarrido de volumenes (segundos desde el arranque):", flush=True)
        prev = 0
        for limit, freq, vol in SWEEP_PHASES:
            print(f"  {prev:3d}-{min(limit,140):3d}s  volumen {vol:3d} deg", flush=True)
            prev = min(limit, 140)
    elif not STATIC:
        print("\nSecuencia de fases (segundos desde el arranque):")
        prev = 0
        for limit, name, _, _ in PHASES:
            print(f"  {prev:3d}-{min(limit,120):3d}s  {name}")
            prev = min(limit, 120)
    print("\nCtrl+C para terminar.\n", flush=True)

    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nTerminado.")


if __name__ == "__main__":
    main()
