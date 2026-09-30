# AGENTS.md

## Repo state

- ATmega8 LED-matrix firmware (24×5 pixels) + docs: `README.md` (Spanish spec, pin maps), datasheets and `Matriz_5x24.ods` / `Digitos_5x4.ods` (wiring map, font) in `docs/`.
- Firmware is bare-metal C under `src/`, built with PlatformIO. No Arduino framework, no tests, no CI.

## Commands

- Build: `pio run`
- Flash (USBASP connected): `pio run -e atmega8 -t upload`
- There are no lint/test commands; don't invent them.

## Toolchain quirks

- Board id is **`ATmega8`** (capital letters), not `atmega8`.
- The board JSON defaults to **16 MHz**; `platformio.ini` overrides with `board_build.f_cpu = 8000000L` (MCU runs on internal 8 MHz RC, no crystal). Never let f_cpu drift back to 16 MHz — every timer constant depends on it.
- Board default upload protocol is `urclock`; the repo overrides it to `usbasp`.
- `upload_flags = -B 10` (slow SCK): this USBASP clone corrupts writes at default speed — a fast write "succeeds" but verification fails. Even at `-B 10` PlatformIO's bundled avrdude 6.3.0 often fails verification (mismatch at byte 0x0002/0x0006); the system avrdude 8.2 works reliably: `avrdude -c usbasp -p m8 -B 10 -U flash:w:.pio/build/atmega8/firmware.hex:i`.
- Fuses (read back and verified on chip): `lfuse=0xC4` → internal RC 8 MHz, `hfuse=0xDD` → BOOTRST unprogrammed (reset starts at 0x0000), SPIEN enabled, RSTDISBL unprogrammed, lock bits unprogrammed. The chip originally had `hfuse=0xDC` (BOOTRST programmed for a urclock bootloader) — corrected, otherwise a USBASP-flashed app would never run. Never program CKSEL away from `0100` (8 MHz) or reprogram BOOTRST.

## Architecture

- Clock display app: shows `HH:MM` starting at 12:00 (24 h), rendered from the font table in `docs/Digitos_5x4.ods` (5 rows × 4 cols, LSB = leftmost column; 12 glyphs: 0–9, space, colon).
- Two ISRs, all work happens in ISRs (`main()` only initializes):
  - `TIMER2_COMP_vect` at **2 kHz (0.5 ms)** (CTC, prescaler 32, `OCR2=124`, 250 fps scan) — matrix scan: all columns off → `OE` high → shift 16 bits → `LE` pulse → `OE` low → active column on. The status-LED blink (100 ms on / 900 ms off) is stepped here from its tick counter.
  - `TIMER1_COMPA_vect` at **1 Hz** (CTC, prescaler 256, `OCR1A=31249`) — seconds/minutes/hours tick; a full `render_clock()` rebuild happens only when the minute changes, otherwise only the colon pixels are toggled (1 Hz blink) — keeps per-second ISR stalls near zero. Note ATmega8's Timer2 has a single compare vector (`TIMER2_COMP`, not `COMPA`).
  - **Timer2 prescaler trap**: unlike Timer1, Timer2's `CS22:0` table is 001=/1, 010=/8, **011=/32, 100=/64**, 101=/128, 110=/256, 111=/1024 (Timer1 has 011=/64, 100=/256). Writing `CS22|CS21` (110) means **/256, not /64** — that bug made the scan run at 31 fps and the display visibly flickered for a long time.
  - `grp[]` is written in the Timer1 ISR and read in the Timer2 ISR; AVR ISRs don't nest, so the rebuild is atomic w.r.t. the scan — no locking needed as long as no framebuffer work moves into `main()`.
- Framebuffer: `grp[8]` — one 16-bit word per column group, pixel `(col,row)` = bit `row + 5*(col%3)` in word `col/3` (from `docs/Matriz_5x24.ods`; group = voltage driver C0–C7, bit = constant-current output F0–F14, F15 unused).
- Layout: `HH:MM` spans 23 columns from `CLOCK_COL0` 0: H1 @+0, gap 2, H2 @+6, gap 3 with the colon pixel in the middle (+11), gap 3, M1 @+13, gap 2, M2 @+19 (4+2+4+3+4+2+4). Colon (`COL_COLON` @+9, glyph pixel at +2) blinks once per second (visible on even seconds).

## Hardware facts (verified, authoritative sources: `README.md` + `docs/DRV_A6282.PDF`)

- Full pin maps live in `README.md` ("Pines utilizados"); don't duplicate them.
- **A6282 `OE` is active low** (high = all outputs forced off); latches are level-triggered by `LE`; CLK rising edge samples `SDI`; chain is `SDI → I0 → … → I15 → SDO`, so the **first bit shifted ends up in OUT15** — `shift_word()` sends bit 15 first. If the image ever comes out mirrored, this order is the first thing to flip.
- Column switches are P-MOSFETs → **column pins are active low** (low = column on).
- Status LED on **PB2 is active-low**; it is blinked from the 0.5 ms ISR: 100 ms on / 900 ms off.
- PB5 doubles as ISP `SCK` (harmless at runtime).

## Conventions

- Docs (`README.md`, etc.) are Spanish; `AGENTS.md` stays English.
- Display layout tunables live as `#define`s at the top of `src/main.c` (`CLOCK_COL0`, LED blink window).
