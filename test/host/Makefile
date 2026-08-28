# Host test runner for the pure-logic parts of the Kundt tube firmware.
# No ESP32 and no ESP-IDF toolchain required (only the cJSON sources shipped
# with ESP-IDF, for the E2 response parser).

CC      ?= gcc
IDF_PATH ?= $(HOME)/esp/esp-idf
CJSON   := $(IDF_PATH)/components/json/cJSON

BASE_CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -g

# ---- E1: microphone sample conversion -------------------------------------
E1_CFLAGS := $(BASE_CFLAGS) -I../../E1-Mic/components/mic_capture/include
E1_SRC    := test_mic_dsp.c ../../E1-Mic/components/mic_capture/mic_dsp.c
E1_BIN    := build/test_mic_dsp

# ---- E2: DDS maths, servo mapping, API parsing ----------------------------
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

.PHONY: all run e1 e2 asan clean

all: run

build:
	@mkdir -p build

$(E1_BIN): build $(E1_SRC)
	$(CC) $(E1_CFLAGS) $(E1_SRC) -o $(E1_BIN) -lm

$(E2_BIN): build $(E2_SRC)
	$(CC) $(E2_CFLAGS) $(E2_SRC) -o $(E2_BIN) -lm

e1: $(E1_BIN)
	@./$(E1_BIN)

e2: $(E2_BIN)
	@./$(E2_BIN)

run: e1 e2

# Same tests under AddressSanitizer + UBSan: catches out-of-bounds writes and
# signed-overflow that a plain run would miss.
asan: build
	$(CC) $(E1_CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
	      $(E1_SRC) -o build/test_mic_dsp_asan -lm
	@./build/test_mic_dsp_asan
	$(CC) $(E2_CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
	      $(E2_SRC) -o build/test_e2_asan -lm
	@./build/test_e2_asan

clean:
	@rm -rf build
