# AGENTS.md

## Repo state

- ATmega8 LED-matrix firmware (24×5 pixels) + docs: `README.md` (Spanish spec, pin maps), datasheets and `Matriz_5x24.ods` (pixel wiring map) in `docs/`.
- Firmware is bare-metal C under `src/`, built with PlatformIO. No Arduino framework, no tests, no CI.

## Commands

- Build: `pio run`
- Flash (USBASP connected): `pio run -e atmega8 -t upload`
- There are no lint/test commands; don't invent them.

## Toolchain quirks

- Board id is **`ATmega8`** (capital letters), not `atmega8`.
- The board JSON defaults to **16 MHz**; `platformio.ini` overrides with `board_build.f_cpu = 8000000L` (MCU runs on internal 8 MHz RC, no crystal). Never let f_cpu drift back to 16 MHz — every timer constant depends on it.
- Board default upload protocol is `urclock`; the repo overrides it to `usbasp`.
- `upload_flags = -B 10` (slow SCK): this USBASP clone corrupts writes at default speed — a fast write "succeeds" but verification fails.
- Fuses (read back and verified on chip): `lfuse=0xC4` → internal RC 8 MHz, `hfuse=0xDD` → BOOTRST unprogrammed (reset starts at 0x0000), SPIEN enabled, RSTDISBL unprogrammed, lock bits unprogrammed. The chip originally had `hfuse=0xDC` (BOOTRST programmed for a urclock bootloader) — corrected, otherwise a USBASP-flashed app would never run. Never program CKSEL away from `0100` (8 MHz) or reprogram BOOTRST.

## Architecture

- Everything runs in one ISR: `TIMER1_COMPA` at **500 Hz (2 ms)** (CTC, prescaler 8, `OCR1A=1999`). It scans one of 8 column groups per tick: all columns off → `OE` high → shift 16 bits → `LE` pulse → `OE` low → active column on. Animation and the status-LED blink are stepped inside the same ISR; `main()` only initializes and loops. Keep it that way — moving work into `main()` requires volatile/atomic discipline.
- Framebuffer: `grp[8]` — one 16-bit word per column group, pixel `(col,row)` = bit `row + 5*(col%3)` in word `col/3` (from `docs/Matriz_5x24.ods`; group = voltage driver C0–C7, bit = constant-current output F0–F14, F15 unused).

## Hardware facts (verified, authoritative sources: `README.md` + `docs/DRV_A6282.PDF`)

- Full pin maps live in `README.md` ("Pines utilizados"); don't duplicate them.
- **A6282 `OE` is active low** (high = all outputs forced off); latches are level-triggered by `LE`; CLK rising edge samples `SDI`; chain is `SDI → I0 → … → I15 → SDO`, so the **first bit shifted ends up in OUT15** — `shift_word()` sends bit 15 first. If the image ever comes out mirrored, this order is the first thing to flip.
- Column switches are P-MOSFETs → **column pins are active low** (low = column on).
- Status LED on **PB2 is active-low**; it is blinked from the 2 ms ISR: 100 ms on / 900 ms off.
- PB5 doubles as ISP `SCK` (harmless at runtime).

## Conventions

- Docs (`README.md`, etc.) are Spanish; `AGENTS.md` stays English.
- Tunables for the test pattern live as `#define`s at the top of `src/main.c` (`VSWEEP_HOLD_FRAMES`, `HSWEEP_HOLD_FRAMES`, `PAUSE_FRAMES`).
