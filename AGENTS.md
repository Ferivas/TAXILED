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

- Clock display app: shows `HH:MM` starting at 12:00:50 (24 h; default chosen so the first digit change lands 10 s after boot for quick hardware checks), rendered from the font table in `docs/Digitos_5x4.ods` (5 rows × 4 cols, LSB = leftmost column; 12 glyphs: 0–9, space, colon).
- ISRs do the timing-critical work; `main()`'s loop only runs the EEPROM write for the orientation flag and applies a complete `$SETCLK` line (both are too slow/long for ISR context):
  - `TIMER2_COMP_vect` at **2 kHz (0.5 ms)** (CTC, prescaler 32, `OCR2=124`, 250 fps scan) — matrix scan: all columns off → `OE` high → shift 16 bits → `LE` pulse → `OE` low → active column on. Also steps the status-LED blink (100 ms on / 900 ms off) and a 1 ms tick counter (`ms_cnt`, for the INT0 debounce).
  - `TIMER1_COMPA_vect` at **200 ms** (CTC, prescaler 256, `OCR1A=6249`) — inside the ISR `tick_div` divides by `TICKS_PER_SEC` (5) to get the 1 Hz seconds/minutes/hours tick; a full `render_clock()` rebuild happens only when the minute changes, otherwise only the colon pixels are toggled (1 Hz blink) — keeps per-second ISR stalls near zero. The other ticks drive the digit scroll (below). Note ATmega8's Timer2 has a single compare vector (`TIMER2_COMP`, not `COMPA`).
  - **Digit scroll**: 1 s before the 59→00 boundary (`second==59`) `prepare_scroll()` snapshots old/new glyphs per slot into `anim_old[]`/`anim_new[]` and sets `anim_mask` (bits: 0=H tens, 1=H units, 2=M tens, 3=M units; only glyphs that actually change). Each 200 ms tick runs `scroll_redraw()`: after `k` steps display row `j` shows `stream[k+j]`, where `stream[0..4]` is the old glyph's rows and `stream[5..9]` the new one's — content scrolls **up** one row per tick while the new glyph's rows enter from the bottom (row 0 first). Step 1 runs in the same ISR as prepare (t0 = −1000 ms), step 5 at −200 ms, so the full `render_clock()` at the boundary causes no visual jump. `draw_glyph_slot()`/`clear_glyph_slot()` decouple sampled font row from display slot (they differ mid-scroll); `slot_pos()` maps slot→col0/row0 per orientation. Animation is cancelled (`anim_mask=0`) by `$SETCLK` and the INT0 orientation toggle.
  - `INT0_vect` (PD2, falling edge, `MCUCR ISC01=1 ISC00=0`) — orientation toggle: 150 ms debounce against `ms_cnt`, flip `orientation`, immediate `render_clock()`, set `eeprom_pending`. The EEPROM byte is written later from `main()` (3.4 ms write must not run in an ISR).
  - `USART_RXC_vect` (9600 8N1, `UBRR=51`) — byte-level state machine only: starts at `$`, collects until CR/LF (second CRLF byte with empty buffer is ignored), over-long lines set `rx_overflow`. Validation + time apply happen in `main()` → `setclk_apply()`, which logs to TX: `RX <line>`, then `OK` or `ERR LEN/CMD/DIGIT/RANGE` (plus `ERR LONG` from the overflow flag, `TAXILED 9600 READY` banner at boot). Polled TX on PD1, called from `main()` only — never from an ISR.
  - **Timer2 prescaler trap**: unlike Timer1, Timer2's `CS22:0` table is 001=/1, 010=/8, **011=/32, 100=/64**, 101=/128, 110=/256, 111=/1024 (Timer1 has 011=/64, 100=/256). Writing `CS22|CS21` (110) means **/256, not /64** — that bug made the scan run at 31 fps and the display visibly flickered for a long time.
  - Shared state (`grp[]`, `orientation`, time fields) is only ever touched from ISR context or inside `cli()/sei()` pairs — that is the concurrency discipline; don't read/write it bare from `main()`.
- Framebuffer: `grp[8]` — one 16-bit word per column group, pixel `(col,row)` = bit `row + 5*(col%3)` in word `col/3` (from `docs/Matriz_5x24.ods`; group = voltage driver C0–C7, bit = constant-current output F0–F14, F15 unused).
- Orientation (persisted in EEPROM address 0, `ee_orientation`; virgin 0xFF → horizontal):
  - **Horizontal** (0): `HH:MM` spans 23 columns from `CLOCK_COL0` 0: H1 @+0, gap 2, H2 @+6, gap 3 with the colon pixel in the middle (+11), gap 3, M1 @+13, gap 2, M2 @+19 (4+2+4+3+4+2+4).
  - **Vertical** (1): digits rotated 90° CW by `draw_glyph_rot(col0,row0,glyph)` — original `(c,r)` → `(col0 + 4 - r, row0 + c)`, so a glyph is 5 col × 4 rows. Read top→bottom (col23→col0) as `Hh:Mm`: H @19-23, h @13-17, colon pixels @11-12 (row 2), M @6-10, m @0-4. Minute glyphs use `row0=1` (rows 1-4, one pixel right of the hours in rows 0-3) — `VC_MROW`. To rotate CCW instead, change the expression to `set_pixel(col0 + r, row0 + 3 - c)` (see comment in code).
  - Colon (`COL_COLON` @+9 horizontal, `VC_COL_A/B` vertical) blinks once per second (visible on even seconds) in both orientations.
- Serial command `$SETCLK,HHMMSS` (13 bytes after `$`): `setclk_apply()` checks exact length + `SETCLK,` prefix + 6 digits, HH 0-23, MM/SS 0-59; sets hour/min/sec + full render inside `cli()`; invalid lines are silently dropped.

## Hardware facts (verified, authoritative sources: `README.md` + `docs/DRV_A6282.PDF`)

- Full pin maps live in `README.md` ("Pines utilizados"); don't duplicate them.
- **A6282 `OE` is active low** (high = all outputs forced off); latches are level-triggered by `LE`; CLK rising edge samples `SDI`; chain is `SDI → I0 → … → I15 → SDO`, so the **first bit shifted ends up in OUT15** — `shift_word()` sends bit 15 first. If the image ever comes out mirrored, this order is the first thing to flip.
- Column switches are P-MOSFETs → **column pins are active low** (low = column on).
- Status LED on **PB2 is active-low**; it is blinked from the 0.5 ms ISR: 100 ms on / 900 ms off.
- PB5 doubles as ISP `SCK` (harmless at runtime).
- PD2 is an input with internal pull-up — the orientation switch must be wired **PD2 → switch → GND** (falling edge = press). PD0 is the UART RX with pull-up; PD1 is the UART TX (activity log, 9600 8N1).

## Conventions

- Docs (`README.md`, etc.) are Spanish; `AGENTS.md` stays English.
- Display layout tunables live as `#define`s at the top of `src/main.c` (`CLOCK_COL0`/`VC_*`, LED blink window, `DEBOUNCE_MS`, `SETCLK_LEN`).
