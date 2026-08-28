#!/usr/bin/env python3
"""
Servidor WebSocket de prueba para el modulo E1-Mic del Tubo de Kundt.

Sustituye al backend real para poder probar E1 de forma aislada: recibe el
audio, lo analiza en vivo y lo guarda como WAV. Sin dependencias externas
(solo stdlib), porque una maquina de laboratorio no siempre permite pip.

Que verifica:
  - Que E1 conecta y sostiene el enlace.
  - La tasa de datos real (deben ser ~86 KiB/s a 44.1 kHz).
  - Si el PCM llega centrado en cero (correcto) o con offset DC (formato
    antiguo del firmware Arduino). Este es el punto pendiente #1.
  - La frecuencia dominante, para contrastarla con la que genera E2.

Uso:
    python3 tools/ws_test_server.py                 # escucha en 0.0.0.0:8081
    python3 tools/ws_test_server.py --port 8083     # kit 3
    python3 tools/ws_test_server.py --seconds 10    # graba 10 s y termina
"""

import argparse
import base64
from collections import deque
import hashlib
import math
import socket
import struct
import sys
import time
import wave

WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

OP_CONT, OP_TEXT, OP_BIN, OP_CLOSE, OP_PING, OP_PONG = 0x0, 0x1, 0x2, 0x8, 0x9, 0xA


# --------------------------------------------------------------------------- #
# WebSocket minimo (RFC 6455), lado servidor
# --------------------------------------------------------------------------- #

def handshake(conn):
    """Completa el upgrade HTTP -> WebSocket. Devuelve la ruta solicitada."""
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            raise ConnectionError("el cliente cerro durante el handshake")
        data += chunk
        if len(data) > 65536:
            raise ConnectionError("cabecera de handshake demasiado grande")

    head = data.split(b"\r\n\r\n", 1)[0].decode("latin-1")
    lines = head.split("\r\n")
    path = lines[0].split(" ")[1] if len(lines[0].split(" ")) > 1 else "/"

    key = None
    for line in lines[1:]:
        if ":" in line:
            name, _, value = line.partition(":")
            if name.strip().lower() == "sec-websocket-key":
                key = value.strip()

    if key is None:
        raise ConnectionError("falta Sec-WebSocket-Key: el cliente no habla WebSocket")

    accept = base64.b64encode(
        hashlib.sha1((key + WS_MAGIC).encode()).digest()
    ).decode()

    conn.sendall(
        (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept}\r\n"
            "\r\n"
        ).encode()
    )
    return path


def recv_exact(conn, n):
    """Lee exactamente n bytes; None si el peer cierra o resetea.

    Un cliente embebido que se reinicia o pierde el WiFi corta la conexion sin
    avisar, y eso llega como ConnectionResetError. Tratarlo como fin de datos
    -- y no como excepcion -- es lo que permite al servidor sobrevivir a las
    reconexiones del firmware.
    """
    buf = bytearray()
    while len(buf) < n:
        try:
            chunk = conn.recv(n - len(buf))
        except (ConnectionResetError, ConnectionAbortedError, OSError):
            return None
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


def send_frame(conn, opcode, payload=b""):
    """Envia un frame sin mascara (el servidor nunca enmascara)."""
    header = bytearray([0x80 | opcode])
    n = len(payload)
    if n < 126:
        header.append(n)
    elif n < 65536:
        header.append(126)
        header += struct.pack(">H", n)
    else:
        header.append(127)
        header += struct.pack(">Q", n)
    conn.sendall(bytes(header) + payload)


def read_frame(conn):
    """Devuelve (opcode, payload) o (None, None) al cerrar el peer."""
    head = recv_exact(conn, 2)
    if head is None:
        return None, None

    opcode = head[0] & 0x0F
    masked = bool(head[1] & 0x80)
    length = head[1] & 0x7F

    if length == 126:
        ext = recv_exact(conn, 2)
        if ext is None:
            return None, None
        length = struct.unpack(">H", ext)[0]
    elif length == 127:
        ext = recv_exact(conn, 8)
        if ext is None:
            return None, None
        length = struct.unpack(">Q", ext)[0]

    mask_key = b""
    if masked:
        mask_key = recv_exact(conn, 4)
        if mask_key is None:
            return None, None

    payload = recv_exact(conn, length) if length else b""
    if payload is None:
        return None, None

    if masked:
        payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload))

    return opcode, payload


# --------------------------------------------------------------------------- #
# Analisis de la senal
# --------------------------------------------------------------------------- #

def goertzel(samples, sample_rate, target_hz):
    """Energia en una frecuencia concreta. Mas barato que una FFT completa
    cuando solo interesa comprobar unos pocos candidatos."""
    n = len(samples)
    if n == 0:
        return 0.0
    k = int(0.5 + (n * target_hz) / sample_rate)
    omega = (2.0 * math.pi * k) / n
    coeff = 2.0 * math.cos(omega)
    s0 = s1 = s2 = 0.0
    for x in samples:
        s0 = x + coeff * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(abs(s1 * s1 + s2 * s2 - coeff * s1 * s2)) / n


def dominant_frequency(samples, sample_rate):
    """Busca la frecuencia dominante barriendo Goertzel en pasos de 25 Hz."""
    if len(samples) < 512:
        return None, 0.0
    window = samples[:4096]
    best_hz, best_mag = 0.0, 0.0
    hz = 100.0
    while hz <= 8000.0:
        mag = goertzel(window, sample_rate, hz)
        if mag > best_mag:
            best_mag, best_hz = mag, hz
        hz += 25.0
    return best_hz, best_mag


def analyse(samples):
    """Estadisticas basicas del bloque PCM."""
    n = len(samples)
    if n == 0:
        return None
    mean = sum(samples) / n
    rms = math.sqrt(sum((s - mean) ** 2 for s in samples) / n)
    return {
        "n": n,
        "mean": mean,
        "rms": rms,
        "min": min(samples),
        "max": max(samples),
        "peak": max(abs(min(samples)), abs(max(samples))),
    }


def verdict_on_format(mean, peak):
    """El punto pendiente #1: distinguir PCM centrado del formato unipolar
    que enviaba el firmware Arduino."""
    if peak < 200:
        return "senal demasiado debil para juzgar (revisa el microfono y E2)"
    if abs(mean) < 1500:
        return "OK - PCM16 centrado en cero, como espera un reproductor estandar"
    if mean > 8000:
        return ("ATENCION - offset DC alto: parece el formato unipolar antiguo. "
                "Revisa track_dc en mic_capture")
    return f"offset DC intermedio ({mean:.0f}): el tracker puede no haber convergido aun"


# --------------------------------------------------------------------------- #

def accept_client(srv):
    """Espera un cliente, reintentando ante interrupciones."""
    while True:
        try:
            return srv.accept()[0], srv.getsockname()
        except OSError:
            continue


def serve(host, port, seconds, wav_path, sample_rate, expect_hz):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)

    print(f"Escuchando en ws://{host}:{port}/")
    print("Configura E1 con la IP de esta maquina y el kit correspondiente.")
    print("Ctrl+C para terminar.\n")

    conn, _ = accept_client(srv)
    conn.settimeout(30.0)
    # Un buffer de recepcion holgado: a 86 KiB/s el socket se llena enseguida
    # si el proceso se entretiene, y el cliente aborta la conexion.
    conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 256 * 1024)
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"Conexion desde {conn.getpeername()[0]}", flush=True)

    try:
        path = handshake(conn)
        print(f"Handshake OK (ruta solicitada: {path})\n")
    except Exception as exc:
        print(f"Handshake fallido: {exc}")
        conn.close()
        return 1

    wav = wave.open(wav_path, "wb")
    wav.setnchannels(1)
    wav.setsampwidth(2)
    wav.setframerate(sample_rate)

    t_start = time.time()
    t_last = t_start
    total_bytes = 0
    total_frames = 0
    window_bytes = 0
    recent = deque(maxlen=8192)

    try:
        while True:
            if seconds and (time.time() - t_start) >= seconds:
                print("\nTiempo de captura alcanzado.")
                break

            try:
                opcode, payload = read_frame(conn)
            except socket.timeout:
                print("Sin datos durante 30 s: el enlace parece caido.")
                break

            if opcode is None:
                print("\nEl cliente cerro; esperando reconexion...", flush=True)
                try:
                    conn.close()
                except OSError:
                    pass
                if seconds and (time.time() - t_start) >= seconds:
                    break
                conn, _ = accept_client(srv)
                conn.settimeout(30.0)
                conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 256 * 1024)
                try:
                    handshake(conn)
                    print("Reconectado.", flush=True)
                except Exception as exc:
                    print(f"Handshake fallido en la reconexion: {exc}", flush=True)
                    break
                continue
            if opcode == OP_CLOSE:
                print("\nFrame de cierre recibido.")
                break
            if opcode == OP_PING:
                send_frame(conn, OP_PONG, payload)
                continue
            if opcode == OP_TEXT:
                print(f"  [texto inesperado] {payload[:120]!r}")
                continue
            if opcode != OP_BIN:
                continue

            total_frames += 1
            total_bytes += len(payload)
            window_bytes += len(payload)

            if len(payload) % 2:
                print(f"  AVISO: frame de {len(payload)} bytes, impar - no es PCM16 alineado")
                payload = payload[: len(payload) - 1]

            # El WAV se escribe siempre (bytes crudos, barato). Desempaquetar
            # para estadisticas es lo caro, asi que se hace 1 de cada 8 frames:
            # a ~86 frames/s eso sigue dando ~10 muestreos por segundo, de sobra
            # para las medias, y deja el socket drenando a tiempo.
            wav.writeframes(payload)
            if total_frames % 8 == 0:
                recent.extend(struct.unpack(f"<{len(payload)//2}h", payload))

            now = time.time()
            if now - t_last >= 2.0:
                st = analyse(list(recent))
                kbs = window_bytes / (now - t_last) / 1024.0
                sps = window_bytes / 2 / (now - t_last)
                print(
                    f"[{now - t_start:5.1f}s] {kbs:6.1f} KiB/s  "
                    f"{sps:7.0f} muestras/s  frame={len(payload)}B  "
                    f"pico={st['peak']:6d}  rms={st['rms']:7.1f}  dc={st['mean']:8.1f}",
                    flush=True
                )
                window_bytes = 0
                t_last = now
    except KeyboardInterrupt:
        print("\nInterrumpido.")
    finally:
        wav.close()
        try:
            send_frame(conn, OP_CLOSE)
        except OSError:
            pass
        conn.close()
        srv.close()

    elapsed = time.time() - t_start
    print("\n" + "=" * 62)
    print("RESUMEN")
    print("=" * 62)

    if total_bytes == 0:
        print("No se recibio audio. Revisa que E1 apunte a esta IP y puerto.")
        return 1

    n_samples = total_bytes // 2
    print(f"Duracion            : {elapsed:.1f} s")
    print(f"Frames recibidos    : {total_frames}")
    print(f"Bytes totales       : {total_bytes} ({total_bytes/1024:.1f} KiB)")
    print(f"Tasa media          : {total_bytes/elapsed/1024:.1f} KiB/s")
    print(f"Muestras/s efectivas: {n_samples/elapsed:.0f}  (esperado ~{sample_rate})")

    drift = (n_samples / elapsed) / sample_rate
    if drift < 0.95:
        print(f"  AVISO: solo el {drift*100:.0f}% del ritmo esperado.")
        print("         Puede ser perdida en la placa (mira pool_overflows y")
        print("         failed_sends) o que el enlace se cortara a mitad de la captura.")
    elif drift > 1.05:
        print(f"  AVISO: {drift*100:.0f}% del ritmo esperado - revisa sample_rate_hz.")
    else:
        print("  Ritmo de muestreo correcto.")

    st = analyse(list(recent))
    if st:
        print(f"\nUltimas {st['n']} muestras:")
        print(f"  min/max : {st['min']} / {st['max']}")
        print(f"  pico    : {st['peak']}   rms: {st['rms']:.1f}   dc medio: {st['mean']:.1f}")
        print(f"\nFormato : {verdict_on_format(st['mean'], st['peak'])}")

        hz, mag = dominant_frequency(list(recent), sample_rate)
        if hz:
            print(f"Tono dominante: {hz:.0f} Hz (magnitud {mag:.1f})")
            if expect_hz:
                err = abs(hz - expect_hz)
                if err <= 50:
                    print(f"  Coincide con los {expect_hz:.0f} Hz esperados.")
                else:
                    print(f"  No coincide con los {expect_hz:.0f} Hz esperados (error {err:.0f} Hz).")

    print(f"\nAudio guardado en: {wav_path}")
    print("Escuchalo para confirmar que el tono suena limpio.")
    return 0


def main():
    ap = argparse.ArgumentParser(description="Servidor WebSocket de prueba para E1-Mic")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8081, help="8080 + numero de kit")
    ap.add_argument("--seconds", type=float, default=0, help="0 = hasta Ctrl+C")
    ap.add_argument("--wav", default="e1_capture.wav")
    ap.add_argument("--rate", type=int, default=44100)
    ap.add_argument("--expect-hz", type=float, default=0,
                    help="frecuencia que deberia estar generando E2, p.ej. 1200")
    args = ap.parse_args()
    return serve(args.host, args.port, args.seconds, args.wav, args.rate, args.expect_hz)


if __name__ == "__main__":
    sys.exit(main())
