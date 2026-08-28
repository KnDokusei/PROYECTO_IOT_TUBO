# Ejecutor de tests de host para las partes de lógica pura del firmware del tubo
# de Kundt. No requiere ESP32 ni el toolchain de ESP-IDF (sólo las fuentes de
# cJSON que vienen con ESP-IDF, para el parser de respuestas de E2).

CC      ?= gcc
IDF_PATH ?= $(HOME)/esp/esp-idf
CJSON   := $(IDF_PATH)/components/json/cJSON

BASE_CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -g

# ---- E1: conversión de muestras del micrófono -----------------------------
E1_CFLAGS := $(BASE_CFLAGS) -I../../E1-Mic/components/mic_capture/include
E1_SRC    := test_mic_dsp.c ../../E1-Mic/components/mic_capture/mic_dsp.c
E1_BIN    := build/test_mic_dsp

# ---- E2: aritmética del DDS, mapeo del servo, parseo de la API ------------
E2_CFLAGS := $(BASE_CFLAGS) \
             -Ishim \
             -I../../components/ad9833/include \
             -I../../components/kundt_api/include \
             -I../../E2-SineGen/components/servo/include \
             -I$(CJSON)
E2_SRC    := test_e2.c \
             ../../components/ad9833/ad9833_regs.c \
             ../../components/kundt_api/kundt_api_parse.c \
             ../../E2-SineGen/components/servo/servo_map.c \
             $(CJSON)/cJSON.c
E2_BIN    := build/test_e2

# ---- E3: geometria del riel, acotado y unidades (A3) ----------------------
E3_CFLAGS := $(BASE_CFLAGS) -I../../E3-StepMotor/components/stepper/include
E3_SRC    := test_e3.c ../../E3-StepMotor/components/stepper/stepper_math.c
E3_BIN    := build/test_e3

.PHONY: all run e1 e2 e3 asan clean

all: run

build:
	@mkdir -p build

$(E1_BIN): build $(E1_SRC)
	$(CC) $(E1_CFLAGS) $(E1_SRC) -o $(E1_BIN) -lm

$(E2_BIN): build $(E2_SRC)
	$(CC) $(E2_CFLAGS) $(E2_SRC) -o $(E2_BIN) -lm

$(E3_BIN): build $(E3_SRC)
	$(CC) $(E3_CFLAGS) $(E3_SRC) -o $(E3_BIN) -lm

e1: $(E1_BIN)
	@./$(E1_BIN)

e2: $(E2_BIN)
	@./$(E2_BIN)

e3: $(E3_BIN)
	@./$(E3_BIN)

run: e1 e2 e3

# Los mismos tests bajo AddressSanitizer + UBSan: cazan escrituras fuera de
# límites y desbordes con signo que una corrida normal no detectaría.
asan: build
	$(CC) $(E1_CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
	      $(E1_SRC) -o build/test_mic_dsp_asan -lm
	@./build/test_mic_dsp_asan
	$(CC) $(E2_CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
	      $(E2_SRC) -o build/test_e2_asan -lm
	@./build/test_e2_asan
	$(CC) $(E3_CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
	      $(E3_SRC) -o build/test_e3_asan -lm
	@./build/test_e3_asan

clean:
	@rm -rf build
