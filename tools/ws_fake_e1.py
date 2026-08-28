#!/usr/bin/env python3
"""
Simulador de E1-Mic: se conecta al servidor de prueba y envia audio sintetico.

Sirve para dos cosas:
  1. Verificar ws_test_server.py sin tener una ESP32 delante.
  2. Comprobar que el servidor distingue el PCM16 centrado (firmware nuevo)
     del formato unipolar (firmware Arduino antiguo).

Uso:
    python3 tools/ws_fake_e1.py --host 127.0.0.1 --port 8081 --seconds 5
    python3 tools/ws_fake_e1.py --legacy-format     # simula el formato antiguo
"""

import argparse
import base64
import math
import os
import socket
import struct
import sys
import time

SAMPLE_RATE = 44100
BLOCK = 512  # muestras por frame, igual que el firmware


def ws_connect(host, port, path="/"):
    sock = socket.create_connection((host, port), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(
        (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        ).encode()
    )

    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("el servidor cerro durante el handshake")
        data += chunk

    status = data.split(b"\r\n", 1)[0].decode("latin-1")
    if "101" not in status:
        raise ConnectionError(f"handshake rechazado: {status}")
    return sock


def send_binary(sock, payload):
    """El cliente SIEMPRE enmascara, segun RFC 6455."""
    header = bytearray([0x82])  # FIN + opcode binario
    n = len(payload)
    if n < 126:
        header.append(0x80 | n)
    elif n < 65536:
        header.append(0x80 | 126)
        header += struct.pack(">H", n)
    else:
        header.append(0x80 | 127)
        header += struct.pack(">Q", n)

    mask = os.urandom(4)
    header += mask
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    sock.sendall(bytes(header) + masked)


def main():
    ap = argparse.ArgumentParser(description="Simulador de E1-Mic")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8081)
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--freq", type=float, default=1200.0,
                    help="tono a generar (1200 Hz = default del AD9833 en E2)")
    ap.add_argument("--amplitude", type=int, default=6400,
                    help="amplitud pico en cuentas PCM")
    ap.add_argument("--legacy-format", action="store_true",
                    help="emite el formato unipolar del firmware Arduino")
    args = ap.parse_args()

    print(f"Conectando a ws://{args.host}:{args.port}/ ...")
    sock = ws_connect(args.host, args.port)
    print("Handshake OK. Enviando audio sintetico"
          f" ({'formato ANTIGUO unipolar' if args.legacy_format else 'PCM16 centrado'}).")

    phase = 0.0
    step = 2.0 * math.pi * args.freq / SAMPLE_RATE
    t0 = time.time()
    sent = 0

    try:
        while time.time() - t0 < args.seconds:
            samples = []
            for _ in range(BLOCK):
                v = args.amplitude * math.sin(phase)
                phase += step
                if phase > 2 * math.pi:
                    phase -= 2 * math.pi

                if args.legacy_format:
                    # (raw & 0xFFF) * 8 -> unipolar centrado en ~16380
                    v = 16380 + v
                    v = max(0, min(32760, v))
                samples.append(int(v))

            payload = struct.pack(f"<{BLOCK}h", *samples)
            send_binary(sock, payload)
            sent += len(payload)

            # Ritmo real: un bloque cada BLOCK/SAMPLE_RATE segundos.
            # Se planifica contra un reloj absoluto para que el coste de generar
            # el bloque no derive; con sleep() relativo el simulador se queda
            # corto y el servidor avisa de perdida de muestras que no existe.
            blocks_sent = sent // (BLOCK * 2)
            next_at = t0 + (blocks_sent * BLOCK) / SAMPLE_RATE
            delay = next_at - time.time()
            if delay > 0:
                time.sleep(delay)
    except (BrokenPipeError, ConnectionResetError):
        print("El servidor cerro la conexion.")
    finally:
        sock.close()

    elapsed = time.time() - t0
    print(f"Enviados {sent} bytes en {elapsed:.1f} s "
          f"({sent/elapsed/1024:.1f} KiB/s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
