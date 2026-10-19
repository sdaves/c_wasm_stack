# Zero Dependency C WASM Application Stack Specification

This specification contains everything needed to generate, compile, run, and benchmark an efficient, custom architecture from scratch. It uses an epoll driven native Linux backend alongside a hardware accelerated WebGL 2.0 WebAssembly user interface, communicating over an inline binary storage matrix.

The project targets Debian. Development happens natively inside the Debian
devcontainer; there is no container orchestration layer. The frontend is
implemented entirely in C. `index.html` is only a loader for the Emscripten
bundle; there is no application JavaScript.

## 1. Project Directory Layout
```text
├── db.h
├── server.c
├── ui.c
├── font8x8.h
├── index.html
├── benchmark_api.c
├── Makefile
└── .devcontainer/
    └── devcontainer.json
```

## 2. Toolchain
* `gcc` - static native builds for the backend and load generator.
* `emcc` (Emscripten) - compiles the WebGL 2.0 frontend and fetch API to WebAssembly.
* `nodejs` - required by Emscripten.
* `python3` - required by Emscripten.

`make setup` installs all of them from apt on Debian. The devcontainer runs that
target automatically through `postCreateCommand`.

## 3. Build
All development tasks go through the Makefile. Run `make help` to list targets.
```bash
make build
```
The underlying commands are:
```bash
gcc -O3 -static server.c -o socket_server
gcc -O3 -static benchmark_api.c -o benchmark_api -lpthread
emcc ui.c -O3 -s MIN_WEBGL_VERSION=2 -s MAX_WEBGL_VERSION=2 -s WASM=1 -s FETCH=1 -s ENVIRONMENT=web -o ui.js
```

### Frontend Notes
* `ui.c` implements the form, record table, keyboard/mouse input, fetch calls,
  and WebGL rendering. No application JavaScript is written or required.
* `index.html` only loads the generated Emscripten bundle.
* The UI reads `/api/users.bin` and copies the raw 48-byte `UserRecord` array,
  so there is no JSON parser in C.
* Text is rendered from the public-domain 8x8 bitmap font in `font8x8.h`.
* `server.c` serves the HTML/JS/WASM assets and the API on the same origin and
  port, so no separate frontend server is needed.

## 4. HTTP API
The C server serves both the API and the compiled frontend on port 8080.

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/api/users` | JSON list of records |
| `GET` | `/api/users.bin` | Raw `UserRecord[]` array consumed by the WASM UI |
| `POST` | `/api/users` | Add a record (`username`, `balance` form fields) |
| `GET` | `/api/bench` | Load-test endpoint |
| `GET` | `/health` | Plain-text health check |
| `GET` | `/` | `index.html` |
| `GET` | `/ui.js`, `/ui.wasm` | Generated Emscripten bundle |

## 5. Run and Test Playbook

### Step 1: Install Dependencies and Build
```bash
make setup
make build
```

### Step 2: Use the WASM Frontend
```bash
make run
```
* The C server serves both the API and the WASM frontend on `:8080`; there is
  no separate frontend process and no port 8000.
* Open http://localhost:8080 in your browser.
* Click the `USERNAME` field and type a name, then click `BALANCE` or press `Tab`.
* Press `Enter` or click `SAVE RECORD`.
* The UI writes the record through `POST /api/users`, reloads the binary record
  array through `GET /api/users.bin`, and redraws the table.
* Reload the page to confirm the records are read back from `data.bin`.

### Step 3: Run High Concurrency API Stress Test
With the server active:
```bash
make benchmark
```

### Step 4: Clean
```bash
make clean
make distclean
```

## 6. Verification Findings
Verified in the Debian devcontainer on 2026-10-19:

* The full build succeeds with no compiler warnings.
* `POST /api/users` returns `201`; records persist across a server restart and
  a browser reload.
* The raw record endpoint reports 48 bytes per record (`96` for two records,
  `144` for three).
* Headless Chromium with WebGL 2.0/SwiftShader ran the C UI, accepted synthetic
  keyboard input, saved a record, and displayed it after reload.
* Benchmark with the browser closed: 40,000 requests at 8 threads with zero
  failures at 63,475.79 TPS; 32,000 requests at 16 threads with zero failures at
  65,521.98 TPS.
* The database must stay on local storage for maximum throughput. On this
  devcontainer the workspace is virtiofs; use `DB_PATH=/tmp/cwasm-data.bin`.
