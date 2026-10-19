CC       ?= gcc
EMCC     ?= emcc
CFLAGS   ?= -O3 -static -Wall -Wextra
LDLIBS   ?= -lpthread
UI_FLAGS ?= -O3 -Wall -Wextra -s MIN_WEBGL_VERSION=2 -s MAX_WEBGL_VERSION=2 -s WASM=1 -s FETCH=1 -s ENVIRONMENT=web

DB_PATH        ?= ./data.bin
BENCH_THREADS  ?= 8
BENCH_REQUESTS ?= 5000
BENCH_PORT     ?= 8080
BENCH_IP       ?= 127.0.0.1

.PHONY: all setup build server run benchmark clean distclean help

all: build

## setup: install the Debian toolchain (gcc, emscripten, node, python)
setup:
	@set -eu; \
	if [ "$$(id -u)" -eq 0 ]; then SUDO=""; else SUDO="sudo"; fi; \
	export DEBIAN_FRONTEND=noninteractive; \
	$$SUDO apt-get update; \
	$$SUDO apt-get install -y --no-install-recommends build-essential ca-certificates curl emscripten nodejs python3; \
	printf '\nToolchain versions:\n'; \
	gcc --version | head -n 1; \
	emcc --version | head -n 1; \
	node --version; \
	python3 --version

## build: compile the backend, load generator, and WASM frontend
build: socket_server benchmark_api ui.js

socket_server: server.c db.h Makefile
	$(CC) $(CFLAGS) server.c -o $@

benchmark_api: benchmark_api.c Makefile
	$(CC) $(CFLAGS) benchmark_api.c -o $@ $(LDLIBS)

ui.js ui.wasm &: ui.c Makefile
	$(EMCC) ui.c $(UI_FLAGS) -o ui.js

## server: run the backend and WASM frontend on :8080
server: build
	@DB_PATH="$(DB_PATH)" ./socket_server

## run: build and run the stack on :8080
run: server

## benchmark: load-test the running backend
benchmark: benchmark_api
	@BENCH_THREADS=$(BENCH_THREADS) \
	 BENCH_REQUESTS=$(BENCH_REQUESTS) \
	 BENCH_PORT=$(BENCH_PORT) \
	 BENCH_IP="$(BENCH_IP)" ./benchmark_api

## clean: remove build artifacts
clean:
	rm -f socket_server benchmark_api ui.js ui.wasm

## distclean: remove build artifacts and the local database
distclean: clean
	rm -f data.bin

## help: list available targets
help:
	@printf '%s\n' \
		'make setup       Install Debian dependencies' \
		'make build       Build server, benchmark, and WASM UI' \
		'make server      Run backend + WASM frontend on :8080' \
		'make run         Build and run the stack on :8080' \
		'make benchmark   Load-test the running backend' \
		'make clean       Remove build artifacts' \
		'make distclean   Also remove data.bin'
