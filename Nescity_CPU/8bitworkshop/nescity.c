#include "neslib.h"
#include <string.h>

//#link "city_tiles_chr_direct.s"
//#link "city_draw_direct_vblank6_asm.s"
//#link "city_crc16_asm.s"

/*
  CPU version:
    - simulation uses C and inline 6502 assembly in this file
    - display uses direct-tile CHR and VBlank-split ASM redraw
    - city_compute_asm_fixed.s is intentionally NOT linked, to reduce PRG ROM size
*/

/*
  NES City Cellular Automaton for 8bitworkshop / NES C

  This version does NOT upload CHR data at runtime.
  Instead, custom tiles are linked as CHR ROM via:


  Visual policy:
    - Sea is a fixed simulation cell, drawn as a blue sprite.
    - The upper-left 4 sprite tiles show step_count as 4 hexadecimal digits.
    - Four sprite digits directly below show FPGA status:
        left 2 digits  = accelerator debug code
        right 2 digits = FPGA display/batch interval as 2 hex digits
                         (1..255 -> 01..FF, 256 -> 00)
    - Four hexadecimal sprite digits on the next line show CRC-16 of grid[960].
    - Background uses only four shared colors:
        color 0: dark green
        color 1: yellow green
        color 2: light brown
        color 3: gray
    - All 4 background palettes are identical, so attribute-table boundaries
      should not create visible color mismatches.
    - Houses, shops, apartments, and buildings are distinguished mostly by shape.

  Conditions:
    - 32 x 30 cells
    - 8-bit integer state per cell
    - 5 x 5 weighted neighborhood
    - torus boundary
    - simultaneous update using 3 rolling next rows
    - sea cells are fixed
    - no station, no rail, no factory
*/

#define W 32
#define H 30
#define N 960

#define SEA 0
#define MOUNTAIN 1

#define FORCE_THRESHOLD 20
#define FORCE_SCALE 20
#define MAX_RISE 1
#define MAX_FALL 3

#define DECLINE_STRENGTH 1
#define NATURE_DECAY 1
#define NATURE_RECOVERY 1

#define CAPACITY_LIMIT 55
#define CAPACITY_SCALE 3
#define OVERCROWDING_DECAY 4

#define WATER_PER_SEA 12
#define WATER_PENALTY_SCALE 20

#define FRAME_DELAY 6

/* CPU division: grid and rolling rows live in the 2 KiB internal RAM. */
unsigned char grid[960];

/*
  FPGA city accelerator MMIO.

  Unlock:
    $7FF0 = 'C'
    $7FF1 = 'I'
    $7FF2 = 'T'
    $7FF3 = 'Y'

  After unlock:
    $6000-$63BF : accelerator active grid[960]
    $7F00       : ID0 = 'C'
    $7F01       : ID1 = 'A'
    $7F03       : display/batch interval code: 01..FF = 1..255, 00 = 256
    $7F10       : CONTROL bit0=START, bit1=RESET
    $7F11       : STATUS  bit0=BUSY,  bit1=DONE
    $7F12/$7F13 : hardware step counter, little endian

  During BUSY the FPGA intentionally returns $FF for grid reads and ignores
  grid writes.  This program therefore waits for DONE before drawing.
*/
#define CITY_UNLOCK0 (*(volatile unsigned char *)0x7FF0u)
#define CITY_UNLOCK1 (*(volatile unsigned char *)0x7FF1u)
#define CITY_UNLOCK2 (*(volatile unsigned char *)0x7FF2u)
#define CITY_UNLOCK3 (*(volatile unsigned char *)0x7FF3u)
#define CITY_ID0     (*(volatile unsigned char *)0x7F00u)
#define CITY_ID1     (*(volatile unsigned char *)0x7F01u)
#define CITY_VERSION (*(volatile unsigned char *)0x7F02u)
#define CITY_INTERVAL (*(volatile unsigned char *)0x7F03u)
#define CITY_CONTROL (*(volatile unsigned char *)0x7F10u)
#define CITY_STATUS  (*(volatile unsigned char *)0x7F11u)
#define CITY_STEP_LO (*(volatile unsigned char *)0x7F12u)
#define CITY_STEP_HI (*(volatile unsigned char *)0x7F13u)

#define CITY_CTRL_START 0x01
#define CITY_CTRL_RESET 0x02
#define CITY_STATUS_BUSY 0x01
#define CITY_STATUS_DONE 0x02

unsigned char city_accel_available;
unsigned char accel_debug_code;
unsigned int accel_display_interval;
unsigned char accel_display_interval_code;
unsigned int grid_crc16;

/*
  On-screen accelerator debug codes (two digits below the step counter):
    10 : probing accelerator / before unlock
    11 : CITY unlock writes completed
    21 : ID0 at $7F00 was not 'C' -> CPU fallback
    22 : ID1 at $7F01 was not 'A' -> CPU fallback
    23 : VERSION at $7F02 was not $05 -> CPU fallback
    30 : accelerator detected; RESET command issued
    40 : accelerator detected and ready

  On FPGA, the two digits to the right of the debug code show the batch
  interval in hexadecimal.  01..FF mean 1..255 steps; 00 means 256 steps.

  Failure codes 21..23 are intentionally left on screen while the C fallback
  simulation runs, so the reason for not using hardware remains visible.
*/

unsigned char next_rows[3][W];
unsigned char top_rows[2][W];

unsigned int seed16;
unsigned int step_count;
unsigned char water_penalty;

/*
  Enable the FPGA accelerator if it is present.

  ID bytes are first cleared while the address range is still ordinary WRAM.
  This makes a failed unlock deterministic on the FPGA implementation.
  If the accelerator is absent, the program falls back to the original C
  simulation in internal RAM.
*/
unsigned char city_accel_try_enable(void) {
  accel_debug_code = 10;

  CITY_ID0 = 0;
  CITY_ID1 = 0;

  CITY_UNLOCK0 = 'C';
  CITY_UNLOCK1 = 'I';
  CITY_UNLOCK2 = 'T';
  CITY_UNLOCK3 = 'Y';
  accel_debug_code = 11;

  /* Test each byte separately so the on-screen code identifies the failure. */
  if (CITY_ID0 != 'C') {
    accel_debug_code = 21;
    return 0;
  }
  if (CITY_ID1 != 'A') {
    accel_debug_code = 22;
    return 0;
  }
  if (CITY_VERSION != 0x05) {
    accel_debug_code = 23;
    return 0;
  }

  /*
    $7F03 is an 8-bit encoded interval:
      01..FF = 1..255 steps
      00     = 256 steps
  */
  accel_display_interval_code = CITY_INTERVAL;
  accel_display_interval =
      (accel_display_interval_code == 0) ? 256u
                                         : (unsigned int)accel_display_interval_code;

  /* Select bank 0 and clear the accelerator's step/done state.
     make_initial() runs after this, so its grid[] writes go directly to
     accelerator bank 0 rather than to cartridge WRAM. */
  CITY_CONTROL = CITY_CTRL_RESET;
  accel_debug_code = 30;
  return 1;
}

void update_digit_sprites(void);

unsigned int city_accel_read_step_count(void) {
  unsigned char lo;
  unsigned char hi;
  lo = CITY_STEP_LO;
  hi = CITY_STEP_HI;
  return ((unsigned int)hi << 8) | lo;
}

void city_accel_run_batch(void) {
  unsigned char status;
  unsigned int i;

  /*
    FPGA fast path: run DISPLAY_STEP_INTERVAL simulation steps back-to-back.
    There are intentionally no PPU waits, sprite/debug updates, grid reads,
    or CRC calculations inside this loop.  Each iteration only issues START
    and polls STATUS until DONE, maximizing the accelerator duty cycle.
  */
  for (i = 0; i < accel_display_interval; ++i) {
    CITY_CONTROL = CITY_CTRL_START;
    do {
      status = CITY_STATUS;
    } while ((status & CITY_STATUS_DONE) == 0);
  }

  step_count = city_accel_read_step_count();
}

/*
  Background palette:
    All four BG palettes are identical, so attribute-table color changes are invisible.

    NES colors:
      0x09 dark green
      0x19 yellow green
      0x27 light brown / tan -> 0x17 brown
      0x10 gray
*/
const unsigned char pal_bg_city[16] = {
  0x09, 0x19, 0x17, 0x10,
  0x09, 0x19, 0x17, 0x10,
  0x09, 0x19, 0x17, 0x10,
  0x09, 0x19, 0x17, 0x10
};

/* Sprite palette 0 is reserved for sea. Sprite color 0 is transparent. */
const unsigned char pal_spr_city[16] = {
  0x0f, 0x01, 0x11, 0x21,
  0x0f, 0x00, 0x10, 0x30,
  0x0f, 0x00, 0x10, 0x30,
  0x0f, 0x00, 0x10, 0x30
};

#define TILE_GROUND    0
#define TILE_MOUNTAIN  1
#define TILE_FOREST    2
#define TILE_FIELD     3
#define TILE_HOUSE     4
#define TILE_APT       5
#define TILE_SHOP      6
#define TILE_BUILDING  7
#define TILE_SEA_SPR   120
#define TILE_DIGIT_0    121
#define TILE_HEX_A      131
#define SPR_PAL_SEA     0
#define SPR_PAL_DIGIT   1

unsigned char rng8(void) {
  seed16 = seed16 * 251u + 13849u;
  return (unsigned char)(seed16 >> 8);
}

unsigned char wrap_x(signed char x) {
  return ((unsigned char)x) & 31;
}

unsigned char wrap_y(signed char y) {
  while (y < 0) y += H;
  while (y >= H) y -= H;
  return (unsigned char)y;
}

unsigned int map_idx(signed char x, signed char y) {
  return ((unsigned int)wrap_y(y)) * W + wrap_x(x);
}

unsigned char clamp_u8(signed int v, unsigned char lo, unsigned char hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return (unsigned char)v;
}

/*
  CRC-16/CCITT-FALSE over grid[0..959], implemented in 6502 assembly.
  ca65/cc65 returns unsigned int in A(low)/X(high).
*/
unsigned int compute_grid_crc16_asm(void);
unsigned char hex_tile(unsigned char nibble);

unsigned char bg_tile_for_value(unsigned char v) {
  /* Direct-tile CHR layout: simulation values 0..100 are also BG tile IDs. */
  return v;
}

void copy_row_to(unsigned char *dst, unsigned char row) {
  memcpy(dst, grid + (unsigned int)row * W, W);
}

void copy_row_from(unsigned char row, unsigned char *src) {
  memcpy(grid + (unsigned int)row * W, src, W);
}

void set_cell(signed char x, signed char y, unsigned char v) {
  unsigned int i;
  i = map_idx(x, y);
  if (grid[i] != SEA) grid[i] = v;
}

void add_town(signed char cx, signed char cy, unsigned char radius, unsigned char core) {
  signed char dx, dy;
  unsigned char d;
  signed int v;
  for (dy = -3; dy <= 3; ++dy) {
    for (dx = -3; dx <= 3; ++dx) {
      d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
      if (d <= radius + 1) {
        v = (signed int)core - (signed int)d * 3 + (rng8() & 1);
        set_cell(cx + dx, cy + dy, clamp_u8(v, 35, 58));
      }
    }
  }
}

void make_initial(void) {
  unsigned int i;
  unsigned char y;
  signed char cx;
  unsigned char r;

  seed16 = 6502u;
  step_count = 0;

  for (i = 0; i < N; ++i) {
    r = rng8();
    if (r < 38) grid[i] = 8 + (rng8() % 6);        /* forest */
    else if (r < 90) grid[i] = 25 + (rng8() % 8);  /* field */
    else grid[i] = 17 + (rng8() % 6);              /* grass */
  }

  /* Fixed sea: winding coastline / river.
     Keep it mostly vertical, so sprite-per-scanline pressure stays low. */
  cx = 5;
  for (y = 0; y < H; ++y) {
    r = rng8();
    if (r < 92) --cx;
    else if (r > 174) ++cx;
    if (cx < 1) cx = 1;
    if (cx > 8) cx = 8;
    grid[map_idx(cx, y)] = SEA;
    if ((y % 3) != 0) grid[map_idx(cx + 1, y)] = SEA;
  }

  /* Mountains: mutable, but hard to develop. */
  set_cell(29, 4, MOUNTAIN);
  set_cell(30, 5, MOUNTAIN);
  set_cell(28, 5, MOUNTAIN);
  set_cell(30, 6, MOUNTAIN);
  set_cell(3, 26, MOUNTAIN);
  set_cell(4, 27, MOUNTAIN);
  set_cell(5, 27, MOUNTAIN);

  /* Several residential neighborhoods. */
  add_town(16, 15, 2, 48);
  add_town(21, 18, 2, 52);
  add_town(7, 23, 2, 45);
  add_town(27, 10, 2, 43);
  add_town(12, 8, 1, 41);

  /* Tiny shop seeds. No station, no rail, no factory. */
  set_cell(17, 15, 64);
  set_cell(22, 18, 66);
  set_cell(27, 10, 63);
}

/* CPU hot loop: constant-weight ROM tables replace 6502 multiplication and
   repeated classification calls. Six independent byte counters track kinds.
   Grass (15..24) contributes exactly 2, independent of the weight.
   Force tables add 2*weight, making column sums unsigned bytes. Across the
   full 65-weight neighborhood this adds 130, removed by city_force_delta.
   Keep these tables in ROM: the CPU board has only 2 KiB of internal RAM. */
static const unsigned char city_force1[101] = {
  5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
  5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3,
  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
  3, 3, 3, 3, 3,
};
static const unsigned char city_force2[101] = {
  10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 6, 6, 6, 6, 6, 6, 6, 6, 6,
  6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 8,
  8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
  8, 8, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
  10, 10, 10, 8, 8, 8, 8, 8, 8, 8, 8, 8,
  8, 8, 8, 8, 8, 8, 8, 6, 6, 6, 6, 6,
  6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
  6, 6, 6, 6, 6,
};
static const unsigned char city_force3[101] = {
  15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 8, 8, 8, 8, 8, 8, 8, 8, 8,
  8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 12,
  12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
  12, 12, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
  15, 15, 15, 12, 12, 12, 12, 12, 12, 12, 12, 12,
  12, 12, 12, 12, 12, 12, 12, 9, 9, 9, 9, 9,
  9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
  9, 9, 9, 9, 9,
};

/* Change of influence when weight increases by one, plus the unsigned bias 2.
   Grass has true slope zero, so its entry is 2, not city_force1[grass]. */
static const unsigned char city_force_slope[101] = {
  5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
  5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3,
  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
  3, 3, 3, 3, 3,
};
static const unsigned char city_kind[101] = {
  0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  2, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3,
  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
  3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5,
  5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
  5, 5, 5, 5, 5,
};
/* Exact summaries for a column whose five input values are equal. */
static const unsigned char city_uniform_value_lo[101] = {
  0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55,
  60, 65, 70, 75, 80, 85, 90, 95, 100, 105, 110, 115,
  120, 125, 130, 135, 140, 145, 150, 155, 160, 165, 170, 175,
  180, 185, 190, 195, 200, 205, 210, 215, 220, 225, 230, 235,
  240, 245, 250, 255, 4, 9, 14, 19, 24, 29, 34, 39,
  44, 49, 54, 59, 64, 69, 74, 79, 84, 89, 94, 99,
  104, 109, 114, 119, 124, 129, 134, 139, 144, 149, 154, 159,
  164, 169, 174, 179, 184, 189, 194, 199, 204, 209, 214, 219,
  224, 229, 234, 239, 244,
};
static const unsigned char city_uniform_value_hi[101] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1,
};
static const unsigned char city_uniform_weighted_lo[101] = {
  0, 9, 18, 27, 36, 45, 54, 63, 72, 81, 90, 99,
  108, 117, 126, 135, 144, 153, 162, 171, 180, 189, 198, 207,
  216, 225, 234, 243, 252, 5, 14, 23, 32, 41, 50, 59,
  68, 77, 86, 95, 104, 113, 122, 131, 140, 149, 158, 167,
  176, 185, 194, 203, 212, 221, 230, 239, 248, 1, 10, 19,
  28, 37, 46, 55, 64, 73, 82, 91, 100, 109, 118, 127,
  136, 145, 154, 163, 172, 181, 190, 199, 208, 217, 226, 235,
  244, 253, 6, 15, 24, 33, 42, 51, 60, 69, 78, 87,
  96, 105, 114, 123, 132,
};
static const unsigned char city_uniform_weighted_hi[101] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
  3, 3, 3, 3, 3,
};
static const unsigned char city_uniform_force[101] = {
  25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 10, 10, 10, 10, 10, 10, 10, 10, 10,
  10, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 20,
  20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
  20, 20, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
  25, 25, 25, 20, 20, 20, 20, 20, 20, 20, 20, 20,
  20, 20, 20, 20, 20, 20, 20, 15, 15, 15, 15, 15,
  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
  15, 15, 15, 15, 15,
};
static const unsigned char city_uniform_weighted_force[101] = {
  45, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 28, 28, 28, 28, 28, 28, 28, 28, 28,
  28, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 36,
  36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45,
  45, 45, 45, 36, 36, 36, 36, 36, 36, 36, 36, 36,
  36, 36, 36, 36, 36, 36, 36, 27, 27, 27, 27, 27,
  27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
  27, 27, 27, 27, 27,
};

static const unsigned char city_is_house[101] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0,
};
static const unsigned char city_ring_next[5] = {1, 2, 3, 4, 0};

/* Fixed-rule arithmetic tables: no general multiply/divide in sim_step. */
#if W != 32 || H != 30 || N != 960 || SEA != 0 || MOUNTAIN != 1 || \
    FORCE_THRESHOLD != 20 || FORCE_SCALE != 20 || \
    CAPACITY_LIMIT != 55 || CAPACITY_SCALE != 3 || OVERCROWDING_DECAY != 4 || \
    MAX_RISE != 1 || MAX_FALL != 3 || DECLINE_STRENGTH != 1 || \
    NATURE_DECAY != 1 || NATURE_RECOVERY != 1
#error Update the fixed-rule tables and assembly when changing simulation constants
#endif
/* Exact floor(n/65), for all weighted sums 0..6500. */
#define CITY_FIVE(v) v,v,v,v,v
#define CITY_SIXTYFIVE(v) CITY_FIVE(v),CITY_FIVE(v),CITY_FIVE(v),CITY_FIVE(v),CITY_FIVE(v),CITY_FIVE(v),CITY_FIVE(v),CITY_FIVE(v),CITY_FIVE(v),CITY_FIVE(v),CITY_FIVE(v),CITY_FIVE(v),CITY_FIVE(v)
static const unsigned char city_average_table[6501] = {
  CITY_SIXTYFIVE(0), CITY_SIXTYFIVE(1), CITY_SIXTYFIVE(2), CITY_SIXTYFIVE(3), CITY_SIXTYFIVE(4),
  CITY_SIXTYFIVE(5), CITY_SIXTYFIVE(6), CITY_SIXTYFIVE(7), CITY_SIXTYFIVE(8), CITY_SIXTYFIVE(9),
  CITY_SIXTYFIVE(10), CITY_SIXTYFIVE(11), CITY_SIXTYFIVE(12), CITY_SIXTYFIVE(13), CITY_SIXTYFIVE(14),
  CITY_SIXTYFIVE(15), CITY_SIXTYFIVE(16), CITY_SIXTYFIVE(17), CITY_SIXTYFIVE(18), CITY_SIXTYFIVE(19),
  CITY_SIXTYFIVE(20), CITY_SIXTYFIVE(21), CITY_SIXTYFIVE(22), CITY_SIXTYFIVE(23), CITY_SIXTYFIVE(24),
  CITY_SIXTYFIVE(25), CITY_SIXTYFIVE(26), CITY_SIXTYFIVE(27), CITY_SIXTYFIVE(28), CITY_SIXTYFIVE(29),
  CITY_SIXTYFIVE(30), CITY_SIXTYFIVE(31), CITY_SIXTYFIVE(32), CITY_SIXTYFIVE(33), CITY_SIXTYFIVE(34),
  CITY_SIXTYFIVE(35), CITY_SIXTYFIVE(36), CITY_SIXTYFIVE(37), CITY_SIXTYFIVE(38), CITY_SIXTYFIVE(39),
  CITY_SIXTYFIVE(40), CITY_SIXTYFIVE(41), CITY_SIXTYFIVE(42), CITY_SIXTYFIVE(43), CITY_SIXTYFIVE(44),
  CITY_SIXTYFIVE(45), CITY_SIXTYFIVE(46), CITY_SIXTYFIVE(47), CITY_SIXTYFIVE(48), CITY_SIXTYFIVE(49),
  CITY_SIXTYFIVE(50), CITY_SIXTYFIVE(51), CITY_SIXTYFIVE(52), CITY_SIXTYFIVE(53), CITY_SIXTYFIVE(54),
  CITY_SIXTYFIVE(55), CITY_SIXTYFIVE(56), CITY_SIXTYFIVE(57), CITY_SIXTYFIVE(58), CITY_SIXTYFIVE(59),
  CITY_SIXTYFIVE(60), CITY_SIXTYFIVE(61), CITY_SIXTYFIVE(62), CITY_SIXTYFIVE(63), CITY_SIXTYFIVE(64),
  CITY_SIXTYFIVE(65), CITY_SIXTYFIVE(66), CITY_SIXTYFIVE(67), CITY_SIXTYFIVE(68), CITY_SIXTYFIVE(69),
  CITY_SIXTYFIVE(70), CITY_SIXTYFIVE(71), CITY_SIXTYFIVE(72), CITY_SIXTYFIVE(73), CITY_SIXTYFIVE(74),
  CITY_SIXTYFIVE(75), CITY_SIXTYFIVE(76), CITY_SIXTYFIVE(77), CITY_SIXTYFIVE(78), CITY_SIXTYFIVE(79),
  CITY_SIXTYFIVE(80), CITY_SIXTYFIVE(81), CITY_SIXTYFIVE(82), CITY_SIXTYFIVE(83), CITY_SIXTYFIVE(84),
  CITY_SIXTYFIVE(85), CITY_SIXTYFIVE(86), CITY_SIXTYFIVE(87), CITY_SIXTYFIVE(88), CITY_SIXTYFIVE(89),
  CITY_SIXTYFIVE(90), CITY_SIXTYFIVE(91), CITY_SIXTYFIVE(92), CITY_SIXTYFIVE(93), CITY_SIXTYFIVE(94),
  CITY_SIXTYFIVE(95), CITY_SIXTYFIVE(96), CITY_SIXTYFIVE(97), CITY_SIXTYFIVE(98), CITY_SIXTYFIVE(99),
  100
};
#undef CITY_SIXTYFIVE
#undef CITY_FIVE
static const signed char city_house_bias[26] = {
  -3,-3,2,2,4,4,4,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6
};
static const unsigned char city_capacity_loss[101] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4,
  4, 8, 8, 8, 12, 12, 12, 16, 16, 16, 20, 20,
  20, 24, 24, 24, 28, 28, 28, 32, 32, 32, 36, 36,
  36, 40, 40, 40, 44, 44, 44, 48, 48, 48, 52, 52,
  52, 56, 56, 56, 60,
};
static const signed char city_force_delta[326] = {
  -7, -7, -7, -7, -7, -7, -7, -7, -7, -7, -7, -6,
  -6, -6, -6, -6, -6, -6, -6, -6, -6, -6, -6, -6,
  -6, -6, -6, -6, -6, -6, -6, -5, -5, -5, -5, -5,
  -5, -5, -5, -5, -5, -5, -5, -5, -5, -5, -5, -5,
  -5, -5, -5, -4, -4, -4, -4, -4, -4, -4, -4, -4,
  -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -3,
  -3, -3, -3, -3, -3, -3, -3, -3, -3, -3, -3, -3,
  -3, -3, -3, -3, -3, -3, -3, -2, -2, -2, -2, -2,
  -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2,
  -2, -2, -2, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3,
  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
  3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5,
  5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
  5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6,
  6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
  6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
  7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8,
  8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
  8, 8,
};

/* Relocatable row addresses; cc65 emits these pointers into PRG ROM. */
static unsigned char * const city_grid_rows[H] = {
  grid + 0, grid + 32, grid + 64, grid + 96, grid + 128, grid + 160,
  grid + 192, grid + 224, grid + 256, grid + 288, grid + 320, grid + 352,
  grid + 384, grid + 416, grid + 448, grid + 480, grid + 512, grid + 544,
  grid + 576, grid + 608, grid + 640, grid + 672, grid + 704, grid + 736,
  grid + 768, grid + 800, grid + 832, grid + 864, grid + 896, grid + 928,
};
static unsigned char * const city_next_rows[3] = {next_rows[0], next_rows[1], next_rows[2]};
static unsigned char * const city_saved_rows[2] = {top_rows[0], top_rows[1]};

/* Hot-loop scratch uses linker-allocated zero page, leaving more of
   $0300..$07FF available to the C stack. This function is non-reentrant;
   NMI does not call it. Every value is assigned before use. The NES linker
   checks this allocation together with the runtime/neslib zero-page data. */
#pragma bss-name (push, "ZEROPAGE", "zp")
static unsigned char row_x, row_y, row_yy, row_self, row_avg;
static unsigned char *city_old_rows[5];
static unsigned int row_raw_force; /* force + 130, range 0..325 */
static unsigned int row_weighted_sum;
static unsigned char row_bias; /* signed delta + 128 */
static unsigned char *city_output, *city_math_ptr;
static unsigned char window_counts[6];
static unsigned char row_urban;
static unsigned char row_natural_pressure;
/* Five reusable column summaries (74 bytes including the duplicated prefix).
   Vertical weights are 1,2,3,2,1.
   Weighted population <=900, biased force <=45; plain population <=500 and
   biased force <=25. Each category count is <=5. Low/high arrays avoid a
   runtime multiply-by-two when indexing words. */
static unsigned char col_value_lo[8], col_value_hi[8];
static unsigned char col_weighted_value_lo[5], col_weighted_value_hi[5];
static unsigned char col_force[8], col_weighted_force[5];
static unsigned char col_counts[6][5];
static unsigned char col_uniform[5]; /* 255 for mixed or uninitialized columns */
static unsigned int window_value;
static unsigned char window_force;
static unsigned char row_slot, row_column_x;
static unsigned int new_value, new_weighted_value;
static unsigned char new_force, new_weighted_force, new_counts[7];
static unsigned char column_samples[5], column_pair;
static unsigned char row_house_columns[36], row_pending, city_next_slot, row_last_active;
#pragma bss-name (pop)

/* Unsigned force bias: +2 per unit weight. Column force fits in a byte:
   plain 0..25, weighted 0..45; window_force 0..225. The final force index
   includes +130. X indexes ROM tables; Y stays at the input column. */
#pragma optimize (push, off)
void city_slide_column(void) {
  __asm__("ldy %v", row_column_x);
  /* This shortcut depends only on the five inputs, for every value 0..100. */
  __asm__("lda (%v),y", city_old_rows);
  __asm__("sta %v", column_samples);
  __asm__("tax");
  __asm__("lda (%v+2),y", city_old_rows);
  __asm__("cmp %v", column_samples);
  __asm__("bne %g", mixed_column);
  __asm__("lda (%v+4),y", city_old_rows);
  __asm__("cmp %v", column_samples);
  __asm__("bne %g", mixed_column);
  __asm__("lda (%v+6),y", city_old_rows);
  __asm__("cmp %v", column_samples);
  __asm__("bne %g", mixed_column);
  __asm__("lda (%v+8),y", city_old_rows);
  __asm__("cmp %v", column_samples);
  __asm__("bne %g", mixed_column);
  /* Equal incoming/outgoing columns leave every window summary unchanged,
     including the duplicated prefix used by fixed-offset cell reads. */
  __asm__("ldy %v", row_slot);
  __asm__("txa");
  __asm__("cmp %v,y", col_uniform);
  __asm__("bne %g", changed_uniform_column);
  __asm__("rts");
changed_uniform_column:
  __asm__("sta %v,y", col_uniform);
  __asm__("lda #0");
  __asm__("sta %v+0", new_counts);
  __asm__("sta %v+1", new_counts);
  __asm__("sta %v+2", new_counts);
  __asm__("sta %v+3", new_counts);
  __asm__("sta %v+4", new_counts);
  __asm__("sta %v+5", new_counts);
  __asm__("sta %v+6", new_counts);
  __asm__("lda %v,x", city_uniform_value_lo);
  __asm__("sta %v", new_value);
  __asm__("lda %v,x", city_uniform_value_hi);
  __asm__("sta %v+1", new_value);
  __asm__("lda %v,x", city_uniform_weighted_lo);
  __asm__("sta %v", new_weighted_value);
  __asm__("lda %v,x", city_uniform_weighted_hi);
  __asm__("sta %v+1", new_weighted_value);
  __asm__("lda %v,x", city_uniform_force);
  __asm__("sta %v", new_force);
  __asm__("lda %v,x", city_uniform_weighted_force);
  __asm__("sta %v", new_weighted_force);
  __asm__("lda %v,x", city_kind);
  __asm__("tax");
  __asm__("lda #5");
  __asm__("sta %v,x", new_counts);
  __asm__("jmp %g", update_window);
mixed_column:
  __asm__("lda #0");
  __asm__("sta %v+0", new_counts);
  __asm__("sta %v+1", new_counts);
  __asm__("sta %v+2", new_counts);
  __asm__("sta %v+3", new_counts);
  __asm__("sta %v+4", new_counts);
  __asm__("sta %v+5", new_counts);
  __asm__("sta %v+6", new_counts);
  __asm__("ldx %v", row_slot);
  __asm__("lda #255");
  __asm__("sta %v,x", col_uniform);
  __asm__("clc");
  /* Input row 0, vertical weight 1. */
  __asm__("lda (%v+0),y", city_old_rows);
  __asm__("sta %v+0", column_samples);
  __asm__("tax");
  __asm__("lda %v,x", city_force_slope);
  __asm__("sta %v", new_force);
  __asm__("lda %v,x", city_force1);
  __asm__("sta %v", new_weighted_force);
  __asm__("lda %v,x", city_kind);
  __asm__("tax");
  __asm__("inc %v,x", new_counts);
  /* Input row 1, vertical weight 2. */
  __asm__("lda (%v+2),y", city_old_rows);
  __asm__("sta %v+1", column_samples);
  __asm__("tax");
  __asm__("lda %v,x", city_force_slope);
  __asm__("adc %v", new_force);
  __asm__("sta %v", new_force);
  __asm__("lda %v,x", city_force2);
  __asm__("adc %v", new_weighted_force);
  __asm__("sta %v", new_weighted_force);
  __asm__("lda %v,x", city_kind);
  __asm__("tax");
  __asm__("inc %v,x", new_counts);
  /* Input row 2, vertical weight 3. */
  __asm__("lda (%v+4),y", city_old_rows);
  __asm__("sta %v+2", column_samples);
  __asm__("tax");
  __asm__("lda %v,x", city_force_slope);
  __asm__("adc %v", new_force);
  __asm__("sta %v", new_force);
  __asm__("lda %v,x", city_force3);
  __asm__("adc %v", new_weighted_force);
  __asm__("sta %v", new_weighted_force);
  __asm__("lda %v,x", city_kind);
  __asm__("tax");
  __asm__("inc %v,x", new_counts);
  /* Input row 3, vertical weight 2. */
  __asm__("lda (%v+6),y", city_old_rows);
  __asm__("sta %v+3", column_samples);
  __asm__("tax");
  __asm__("lda %v,x", city_force_slope);
  __asm__("adc %v", new_force);
  __asm__("sta %v", new_force);
  __asm__("lda %v,x", city_force2);
  __asm__("adc %v", new_weighted_force);
  __asm__("sta %v", new_weighted_force);
  __asm__("lda %v,x", city_kind);
  __asm__("tax");
  __asm__("inc %v,x", new_counts);
  /* Input row 4, vertical weight 1. */
  __asm__("lda (%v+8),y", city_old_rows);
  __asm__("sta %v+4", column_samples);
  __asm__("tax");
  __asm__("lda %v,x", city_force_slope);
  __asm__("adc %v", new_force);
  __asm__("sta %v", new_force);
  __asm__("lda %v,x", city_force1);
  __asm__("adc %v", new_weighted_force);
  __asm__("sta %v", new_weighted_force);
  __asm__("lda %v,x", city_kind);
  __asm__("tax");
  __asm__("inc %v,x", new_counts);
  /* Symmetric pairs <=200 never carry. X holds the word's high byte;
     increment it only when a low-byte addition actually overflows. */
  __asm__("ldx #0");
  __asm__("lda %v", column_samples);
  __asm__("clc");
  __asm__("adc %v+4", column_samples);
  __asm__("sta %v", column_pair);
  __asm__("lda %v+1", column_samples);
  __asm__("adc %v+3", column_samples);
  __asm__("sta %v", new_weighted_value);
  __asm__("adc %v", column_pair);
  __asm__("bcc %g", plain_pair_no_carry);
  __asm__("inx");
plain_pair_no_carry:
  __asm__("clc");
  __asm__("adc %v+2", column_samples);
  __asm__("bcc %g", plain_no_carry);
  __asm__("inx");
plain_no_carry:
  __asm__("sta %v", new_value);
  __asm__("stx %v+1", new_value);
  __asm__("lda %v+2", column_samples);
  __asm__("asl a");
  __asm__("adc %v", new_weighted_value);
  __asm__("bcc %g", weighted_pair_no_carry);
  __asm__("inx");
weighted_pair_no_carry:
  __asm__("clc");
  __asm__("adc %v", new_value);
  __asm__("bcc %g", weighted_no_carry);
  __asm__("inx");
weighted_no_carry:
  __asm__("sta %v", new_weighted_value);
  __asm__("stx %v+1", new_weighted_value);
update_window:
  /* Apply each changed count's signed difference modulo 256. The true total
     is always 0..25, so discarding the byte carry gives the exact result. */
  __asm__("ldx %v", row_slot);
  __asm__("lda %v+0", new_counts);
  __asm__("cmp %v+0,x", col_counts);
  __asm__("beq %g", count_0_unchanged);
  __asm__("sec");
  __asm__("sbc %v+0,x", col_counts);
  __asm__("clc");
  __asm__("adc %v+0", window_counts);
  __asm__("sta %v+0", window_counts);
  __asm__("lda %v+0", new_counts);
  __asm__("sta %v+0,x", col_counts);
count_0_unchanged:
  __asm__("lda %v+1", new_counts);
  __asm__("cmp %v+5,x", col_counts);
  __asm__("beq %g", count_1_unchanged);
  __asm__("sec");
  __asm__("sbc %v+5,x", col_counts);
  __asm__("clc");
  __asm__("adc %v+1", window_counts);
  __asm__("sta %v+1", window_counts);
  __asm__("lda %v+1", new_counts);
  __asm__("sta %v+5,x", col_counts);
count_1_unchanged:
  __asm__("lda %v+2", new_counts);
  __asm__("cmp %v+10,x", col_counts);
  __asm__("beq %g", count_2_unchanged);
  __asm__("sec");
  __asm__("sbc %v+10,x", col_counts);
  __asm__("clc");
  __asm__("adc %v+2", window_counts);
  __asm__("sta %v+2", window_counts);
  __asm__("lda %v+2", new_counts);
  __asm__("sta %v+10,x", col_counts);
count_2_unchanged:
  __asm__("lda %v+3", new_counts);
  __asm__("cmp %v+15,x", col_counts);
  __asm__("beq %g", count_3_unchanged);
  __asm__("sec");
  __asm__("sbc %v+15,x", col_counts);
  __asm__("clc");
  __asm__("adc %v+3", window_counts);
  __asm__("sta %v+3", window_counts);
  __asm__("lda %v+3", new_counts);
  __asm__("sta %v+15,x", col_counts);
count_3_unchanged:
  __asm__("lda %v+4", new_counts);
  __asm__("cmp %v+20,x", col_counts);
  __asm__("beq %g", count_4_unchanged);
  __asm__("sec");
  __asm__("sbc %v+20,x", col_counts);
  __asm__("clc");
  __asm__("adc %v+4", window_counts);
  __asm__("sta %v+4", window_counts);
  __asm__("lda %v+4", new_counts);
  __asm__("sta %v+20,x", col_counts);
count_4_unchanged:
  __asm__("lda %v+5", new_counts);
  __asm__("cmp %v+25,x", col_counts);
  __asm__("beq %g", count_5_unchanged);
  __asm__("sec");
  __asm__("sbc %v+25,x", col_counts);
  __asm__("clc");
  __asm__("adc %v+5", window_counts);
  __asm__("sta %v+5", window_counts);
  __asm__("lda %v+5", new_counts);
  __asm__("sta %v+25,x", col_counts);
count_5_unchanged:
  __asm__("lda %v", window_force);
  __asm__("sec");
  __asm__("sbc %v,x", col_weighted_force);
  __asm__("clc");
  __asm__("adc %v", new_weighted_force);
  __asm__("sta %v", window_force);
  __asm__("lda %v", new_weighted_force);
  __asm__("sta %v,x", col_weighted_force);
  __asm__("lda %v", new_force);
  __asm__("sta %v,x", col_force);
  __asm__("lda %v", window_value);
  __asm__("sec");
  __asm__("sbc %v,x", col_weighted_value_lo);
  __asm__("sta %v", window_value);
  __asm__("lda %v+1", window_value);
  __asm__("sbc %v,x", col_weighted_value_hi);
  __asm__("sta %v+1", window_value);
  __asm__("lda %v", window_value);
  __asm__("clc");
  __asm__("adc %v", new_weighted_value);
  __asm__("sta %v", window_value);
  __asm__("lda %v+1", window_value);
  __asm__("adc %v+1", new_weighted_value);
  __asm__("sta %v+1", window_value);
  __asm__("lda %v", new_weighted_value);
  __asm__("sta %v,x", col_weighted_value_lo);
  __asm__("lda %v+1", new_weighted_value);
  __asm__("sta %v,x", col_weighted_value_hi);
  __asm__("lda %v", new_value);
  __asm__("sta %v,x", col_value_lo);
  __asm__("lda %v+1", new_value);
  __asm__("sta %v,x", col_value_hi);
  /* Fixed +1,+2,+3 accesses may cross the end of the five-column ring. */
  __asm__("cpx #3");
  __asm__("bcs %g", column_complete);
  __asm__("lda %v", new_force);
  __asm__("sta %v+5,x", col_force);
  __asm__("lda %v", new_value);
  __asm__("sta %v+5,x", col_value_lo);
  __asm__("lda %v+1", new_value);
  __asm__("sta %v+5,x", col_value_hi);
column_complete:
  __asm__("rts");
}
#pragma optimize (pop)

/* These assembly functions use only the ordinary 6502 instruction set.
   Disable cc65's C optimizer across each block so carry and register lifetime
   follow the instruction sequence, including across C labels. */
#pragma optimize (push, off)
void city_compute_average(void) {
  __asm__("lda %v", row_weighted_sum);
  __asm__("clc");
  __asm__("adc #<%v", city_average_table);
  __asm__("sta %v", city_math_ptr);
  __asm__("lda %v+1", row_weighted_sum);
  __asm__("adc #>%v", city_average_table);
  __asm__("sta %v+1", city_math_ptr);
  __asm__("ldy #0");
  __asm__("lda (%v),y", city_math_ptr);
  __asm__("sta %v", row_avg);
}

void city_reset_columns(void) {
  __asm__("lda #0");
  __asm__("sta %v", window_force);
  __asm__("sta %v", window_value);
  __asm__("sta %v+1", window_value);
  __asm__("sta %v+0", window_counts);
  __asm__("sta %v+1", window_counts);
  __asm__("sta %v+2", window_counts);
  __asm__("sta %v+3", window_counts);
  __asm__("sta %v+4", window_counts);
  __asm__("sta %v+5", window_counts);
  __asm__("ldx #4");
reset_column:
  __asm__("sta %v+0,x", col_counts);
  __asm__("sta %v+5,x", col_counts);
  __asm__("sta %v+10,x", col_counts);
  __asm__("sta %v+15,x", col_counts);
  __asm__("sta %v+20,x", col_counts);
  __asm__("sta %v+25,x", col_counts);
  __asm__("sta %v,x", col_weighted_force);
  __asm__("sta %v,x", col_weighted_value_lo);
  __asm__("sta %v,x", col_weighted_value_hi);
  __asm__("dex");
  __asm__("bpl %g", reset_column);
  __asm__("lda #255");
  __asm__("sta %v", col_uniform);
  __asm__("sta %v+1", col_uniform);
  __asm__("sta %v+2", col_uniform);
  __asm__("sta %v+3", col_uniform);
  __asm__("sta %v+4", col_uniform);
}

/* The average can improve a cell's unbounded delta by at most one; capacity
   loss never improves it. Saturated declines (and non-growing value-1 cells)
   therefore do not need the weighted population or average at all. */
void city_finish_cell(void) {
  /* Assemble force+130. All horizontal terms together are <=100. */
  __asm__("ldx %v", row_slot);
  __asm__("lda %v+2,x", col_force);
  __asm__("asl a");
  __asm__("adc %v+1,x", col_force);
  __asm__("adc %v+3,x", col_force);
  __asm__("adc %v", window_force);
  __asm__("sta %v", row_raw_force);
  __asm__("tay");
  __asm__("lda #0");
  __asm__("adc #0");
  __asm__("sta %v+1", row_raw_force);
  __asm__("beq %g", force_low);
  __asm__("lda %v+256,y", city_force_delta);
  __asm__("jmp %g", force_loaded);
force_low:
  __asm__("lda %v,y", city_force_delta);
force_loaded:
  __asm__("eor #128");
  __asm__("sta %v", row_bias);
  /* Exact local bias, kept biased by 128 to use unsigned comparisons. */
  __asm__("lda %v+3", window_counts);
  __asm__("clc");
  __asm__("adc %v+4", window_counts);
  __asm__("adc %v+5", window_counts);
  __asm__("sta %v", row_urban);
  __asm__("lda %v", row_self);
  __asm__("cmp #2");
  __asm__("bcs %g", not_mountain);
  __asm__("lda %v", row_bias);
  __asm__("sec");
  __asm__("sbc #4");
  __asm__("sta %v", row_bias);
not_mountain:
  __asm__("lda %v", row_self);
  __asm__("cmp #35");
  __asm__("bcc %g", natural_cell);
  __asm__("jmp %g", urban_cell);
natural_cell:
  __asm__("ldx %v+3", window_counts);
  __asm__("lda %v,x", city_house_bias);
  __asm__("clc");
  __asm__("adc %v", row_bias);
  __asm__("sta %v", row_bias);
  __asm__("cpx #3");
  __asm__("bcc %g", no_natural_shop);
  __asm__("lda %v+4", window_counts);
  __asm__("beq %g", no_natural_shop);
  __asm__("inc %v", row_bias);
no_natural_shop:
  __asm__("lda %v", window_counts);
  __asm__("cmp #4");
  __asm__("bcc %g", no_sea_loss);
  __asm__("lda %v", row_bias);
  __asm__("sec");
  __asm__("sbc #2");
  __asm__("sta %v", row_bias);
no_sea_loss:
  __asm__("lda %v+1", window_counts);
  __asm__("cmp #3");
  __asm__("bcc %g", no_mountain_loss);
  __asm__("cpx #4");
  __asm__("bcs %g", no_mountain_loss);
  __asm__("lda %v", row_bias);
  __asm__("sec");
  __asm__("sbc #3");
  __asm__("sta %v", row_bias);
no_mountain_loss:
  __asm__("lda %v+1", row_raw_force);
  __asm__("bne %g", bias_ready);
  __asm__("lda %v", row_self);
  __asm__("cmp #25");
  __asm__("bcc %g", no_recovery);
  __asm__("lda %v", row_urban);
  __asm__("cmp #3");
  __asm__("bcs %g", no_recovery);
  __asm__("lda %v", row_raw_force);
  __asm__("cmp #142");
  __asm__("bcs %g", no_recovery);
  __asm__("dec %v", row_bias);
no_recovery:
  __asm__("lda %v", row_raw_force);
  __asm__("cmp #145");
  __asm__("bcs %g", bias_ready);
  __asm__("dec %v", row_bias);
  __asm__("jmp %g", bias_ready);
urban_cell:
  __asm__("cmp #63");
  __asm__("bcs %g", shop_or_tall);
  __asm__("lda %v+4", window_counts);
  __asm__("beq %g", no_house_shop);
  __asm__("inc %v", row_bias);
  __asm__("inc %v", row_bias);
no_house_shop:
  __asm__("lda %v+5", window_counts);
  __asm__("cmp #2");
  __asm__("bcc %g", decline);
  __asm__("inc %v", row_bias);
  __asm__("jmp %g", decline);
shop_or_tall:
  __asm__("lda %v+4", window_counts);
  __asm__("clc");
  __asm__("adc %v+5", window_counts);
  __asm__("cmp #4");
  __asm__("bcc %g", decline);
  __asm__("inc %v", row_bias);
  __asm__("inc %v", row_bias);
decline:
  __asm__("lda %v", row_urban);
  __asm__("cmp #5");
  __asm__("bcs %g", no_isolation);
  __asm__("lda %v", row_bias);
  __asm__("clc");
  __asm__("adc %v", row_urban);
  __asm__("sec");
  __asm__("sbc #5");
  __asm__("sta %v", row_bias);
no_isolation:
  /* Natural excludes mountain in the cached category; its total coefficient is 3. */
  __asm__("lda %v+1", window_counts);
  __asm__("asl a");
  __asm__("adc %v+1", window_counts);
  __asm__("adc %v+2", window_counts);
  __asm__("sta %v", row_natural_pressure);
  __asm__("lda %v", window_counts);
  __asm__("asl a");
  __asm__("adc %v", row_natural_pressure);
  __asm__("lsr a");
  __asm__("lsr a");
  __asm__("lsr a");
  __asm__("lsr a");
  __asm__("eor #255");
  __asm__("sec");
  __asm__("adc %v", row_bias);
  __asm__("sta %v", row_bias);
  /* A water subtraction that underflows biased zero is already saturated decline. */
  __asm__("sec");
  __asm__("sbc %v", water_penalty);
  __asm__("bcs %g", water_no_underflow);
  __asm__("lda #0");
water_no_underflow:
  __asm__("sta %v", row_bias);
bias_ready:
  __asm__("lda %v", row_bias);
  __asm__("cmp #125");
  __asm__("bcs %g", not_saturated);
  __asm__("jmp %g", maximum_fall);
not_saturated:
  __asm__("lda %v", row_self);
  __asm__("cmp #1");
  __asm__("bne %g", need_average);
  __asm__("lda %v", row_bias);
  __asm__("cmp #128");
  __asm__("bcs %g", need_average);
  __asm__("jmp %g", value_one);
need_average:
  /* Reconstruct weighted population: window + left + 2*center + right. */
  __asm__("ldx %v", row_slot);
  __asm__("lda %v", window_value);
  __asm__("clc");
  __asm__("adc %v+1,x", col_value_lo);
  __asm__("sta %v", row_weighted_sum);
  __asm__("lda %v+1", window_value);
  __asm__("adc %v+1,x", col_value_hi);
  __asm__("sta %v+1", row_weighted_sum);
  __asm__("lda %v+2,x", col_value_lo);
  __asm__("asl a");
  __asm__("sta %v", new_value);
  __asm__("lda %v+2,x", col_value_hi);
  __asm__("rol a");
  __asm__("sta %v+1", new_value);
  __asm__("lda %v", row_weighted_sum);
  __asm__("clc");
  __asm__("adc %v", new_value);
  __asm__("sta %v", row_weighted_sum);
  __asm__("lda %v+1", row_weighted_sum);
  __asm__("adc %v+1", new_value);
  __asm__("sta %v+1", row_weighted_sum);
  __asm__("lda %v", row_weighted_sum);
  __asm__("clc");
  __asm__("adc %v+3,x", col_value_lo);
  __asm__("sta %v", row_weighted_sum);
  __asm__("lda %v+1", row_weighted_sum);
  __asm__("adc %v+3,x", col_value_hi);
  __asm__("sta %v+1", row_weighted_sum);
  __asm__("jsr %v", city_compute_average);
  __asm__("ldx %v", row_avg);
  __asm__("lda %v", row_bias);
  __asm__("sec");
  __asm__("sbc %v,x", city_capacity_loss);
  __asm__("sta %v", row_bias);
  __asm__("lda %v", row_avg);
  __asm__("sec");
  __asm__("sbc %v", row_self);
  __asm__("bcc %g", average_lower);
  __asm__("cmp #13");
  __asm__("bcc %g", final_delta);
  __asm__("inc %v", row_bias);
  __asm__("jmp %g", final_delta);
average_lower:
  __asm__("cmp #236");
  __asm__("bcs %g", final_delta);
  __asm__("dec %v", row_bias);
final_delta:
  __asm__("lda %v", row_bias);
  __asm__("cmp #129");
  __asm__("bcs %g", rise);
  __asm__("cmp #125");
  __asm__("bcs %g", bounded_fall);
maximum_fall:
  __asm__("lda #125");
bounded_fall:
  __asm__("sec");
  __asm__("sbc #128");
  __asm__("clc");
  __asm__("adc %v", row_self);
  __asm__("cmp #101");
  __asm__("bcc %g", nonwrapped_value);
value_one:
  __asm__("lda #1");
  __asm__("jmp %g", store_cell);
nonwrapped_value:
  __asm__("cmp #0");
  __asm__("bne %g", store_cell);
  __asm__("lda #1");
  __asm__("jmp %g", store_cell);
rise:
  __asm__("lda %v", row_self);
  __asm__("cmp #100");
  __asm__("bcs %g", store_cell);
  __asm__("adc #1");
store_cell:
  __asm__("ldy %v", row_x);
  __asm__("sta (%v),y", city_output);
}
#pragma optimize (pop)


void city_duplicate_house_columns(void);
/* Maintain exact house counts for the five old rows. Advance before the
   oldest row is overwritten; saved top rows handle vertical wraparound. */
#pragma optimize (push, off)
void city_mark_house_columns(void) {
  __asm__("ldy #31");
mark_house_column:
  __asm__("lda #0");
  __asm__("sta %v", column_pair);
  __asm__("lda (%v+0),y", city_old_rows);
  __asm__("tax");
  __asm__("lda %v,x", city_is_house);
  __asm__("clc");
  __asm__("adc %v", column_pair);
  __asm__("sta %v", column_pair);
  __asm__("lda (%v+2),y", city_old_rows);
  __asm__("tax");
  __asm__("lda %v,x", city_is_house);
  __asm__("clc");
  __asm__("adc %v", column_pair);
  __asm__("sta %v", column_pair);
  __asm__("lda (%v+4),y", city_old_rows);
  __asm__("tax");
  __asm__("lda %v,x", city_is_house);
  __asm__("clc");
  __asm__("adc %v", column_pair);
  __asm__("sta %v", column_pair);
  __asm__("lda (%v+6),y", city_old_rows);
  __asm__("tax");
  __asm__("lda %v,x", city_is_house);
  __asm__("clc");
  __asm__("adc %v", column_pair);
  __asm__("sta %v", column_pair);
  __asm__("lda (%v+8),y", city_old_rows);
  __asm__("tax");
  __asm__("lda %v,x", city_is_house);
  __asm__("clc");
  __asm__("adc %v", column_pair);
  __asm__("sta %v", column_pair);
  __asm__("sta %v,y", row_house_columns);
  __asm__("dey");
  __asm__("bpl %g", mark_house_column);
  __asm__("jmp %v", city_duplicate_house_columns);
}

void city_duplicate_house_columns(void) {
  __asm__("ldx #3");
duplicate_house_column:
  __asm__("lda %v,x", row_house_columns);
  __asm__("sta %v+32,x", row_house_columns);
  __asm__("dex");
  __asm__("bpl %g", duplicate_house_column);
}

void city_advance_house_columns(void) {
  __asm__("lda %v", row_y);
  __asm__("clc");
  __asm__("adc #3");
  __asm__("cmp #30");
  __asm__("bcc %g", incoming_grid);
  __asm__("sbc #30");
  __asm__("asl a");
  __asm__("tax");
  __asm__("lda %v,x", city_saved_rows);
  __asm__("sta %v", city_math_ptr);
  __asm__("lda %v+1,x", city_saved_rows);
  __asm__("jmp %g", incoming_high);
incoming_grid:
  __asm__("asl a");
  __asm__("tax");
  __asm__("lda %v,x", city_grid_rows);
  __asm__("sta %v", city_math_ptr);
  __asm__("lda %v+1,x", city_grid_rows);
incoming_high:
  __asm__("sta %v+1", city_math_ptr);
  __asm__("ldy #31");
advance_house_column:
  __asm__("lda (%v),y", city_old_rows);
  __asm__("cmp (%v),y", city_math_ptr);
  __asm__("beq %g", same_house_count);
  __asm__("tax");
  __asm__("lda %v,x", city_is_house);
  __asm__("sta %v", column_pair);
  __asm__("lda (%v),y", city_math_ptr);
  __asm__("tax");
  __asm__("lda %v,x", city_is_house);
  __asm__("cmp %v", column_pair);
  __asm__("beq %g", same_house_count);
  __asm__("clc");
  __asm__("adc %v,y", row_house_columns);
  __asm__("sec");
  __asm__("sbc %v", column_pair);
  __asm__("sta %v,y", row_house_columns);
same_house_count:
  __asm__("dey");
  __asm__("bpl %g", advance_house_column);
  __asm__("jmp %v", city_duplicate_house_columns);
}
#pragma optimize (pop)

/* Y retains the horizontal position across fixed cells. Only active cells
   publish row_x; city_finish_cell restores Y before returning. The first
   active cell subtracts the modulo-byte sentinel -5, giving x+5 (5..36). */
#pragma optimize (push, off)
void city_compute_row(void) {
  __asm__("ldy #31");
check_sea_row:
  __asm__("lda (%v+4),y", city_old_rows);
  __asm__("bne %g", land_row);
  __asm__("dey");
  __asm__("bpl %g", check_sea_row);
  __asm__("ldy #31");
clear_sea_row:
  __asm__("sta (%v),y", city_output);
  __asm__("dey");
  __asm__("bpl %g", clear_sea_row);
  __asm__("rts");
land_row:
  __asm__("lda #251");
  __asm__("sta %v", row_last_active);
  __asm__("ldy #0");
next_cell:
  __asm__("lda (%v+4),y", city_old_rows);
  __asm__("bne %g", nonsea_cell);
  __asm__("jmp %g", fixed_cell);
nonsea_cell:
  __asm__("sta %v", row_self);
  /* With <2 houses a mountain cannot grow. If seas>=4, the maximum
     pre-average delta is 7-4-3-2=-2. Otherwise force<=110+4*4=126,
     giving at most 5-4-3=-2. Average adds at most one; capacity only
     subtracts. Clamping to the minimum land value therefore returns 1. */
  __asm__("cmp #1");
  __asm__("bne %g", active_cell);
  __asm__("tya");
  __asm__("clc");
  __asm__("adc #30");
  __asm__("and #31");
  __asm__("tax");
  __asm__("clc");
  __asm__("lda %v+0,x", row_house_columns);
  __asm__("adc %v+1,x", row_house_columns);
  __asm__("adc %v+2,x", row_house_columns);
  __asm__("adc %v+3,x", row_house_columns);
  __asm__("adc %v+4,x", row_house_columns);
  __asm__("cmp #2");
  __asm__("bcs %g", active_cell);
  __asm__("lda #1");
  __asm__("jmp %g", fixed_cell);
active_cell:
  /* A gap >=5 needs five replacements; shorter gaps reuse overlap.
     The old five summaries still sum to the cached window, even across
     row changes. Replacing all five removes every stale contribution. */
  __asm__("sty %v", row_x);
  __asm__("tya");
  __asm__("sec");
  __asm__("sbc %v", row_last_active);
  __asm__("cmp #5");
  __asm__("bcc %g", reuse_window);
  __asm__("lda #5");
reuse_window:
  __asm__("sta %v", row_pending);
  __asm__("sty %v", row_last_active);
  __asm__("lda %v", row_x);
  __asm__("clc");
  __asm__("adc #3");
  __asm__("sec");
  __asm__("sbc %v", row_pending);
  __asm__("and #31");
  __asm__("sta %v", row_column_x);
catch_up_window:
  __asm__("jsr %v", city_slide_column);
  __asm__("ldx %v", row_slot);
  __asm__("lda %v,x", city_ring_next);
  __asm__("sta %v", row_slot);
  __asm__("dec %v", row_pending);
  __asm__("beq %g", window_ready);
  __asm__("lda %v", row_column_x);
  __asm__("clc");
  __asm__("adc #1");
  __asm__("and #31");
  __asm__("sta %v", row_column_x);
  __asm__("jmp %g", catch_up_window);
window_ready:
  __asm__("jsr %v", city_finish_cell);
  __asm__("jmp %g", advance_column);
fixed_cell:
  __asm__("sta (%v),y", city_output);
advance_column:
  __asm__("iny");
  __asm__("cpy #32");
  __asm__("bcs %g", row_done);
  __asm__("jmp %g", next_cell);
row_done:
  __asm__("rts");
}
#pragma optimize (pop)

/* Resolve vertical torus and saved old rows once, before any row writeback. */
#pragma optimize (push, off)
void city_prepare_rows(void) {
  __asm__("lda %v", row_y);
  __asm__("sec");
  __asm__("sbc #2");
  __asm__("bcs %g", valid_first_row);
  __asm__("clc");
  __asm__("adc #30");
valid_first_row:
  __asm__("sta %v", row_yy);
  __asm__("ldy #0");
prepare_row:
  __asm__("lda %v", row_yy);
  __asm__("asl a");
  __asm__("tax");
  __asm__("lda %v", row_y);
  __asm__("cmp #28");
  __asm__("bcc %g", use_grid_row);
  __asm__("cpx #4");
  __asm__("bcs %g", use_grid_row);
  __asm__("lda %v,x", city_saved_rows);
  __asm__("sta %v,y", city_old_rows);
  __asm__("lda %v+1,x", city_saved_rows);
  __asm__("jmp %g", row_address_high);
use_grid_row:
  __asm__("lda %v,x", city_grid_rows);
  __asm__("sta %v,y", city_old_rows);
  __asm__("lda %v+1,x", city_grid_rows);
row_address_high:
  __asm__("sta %v+1,y", city_old_rows);
  __asm__("lda %v", row_yy);
  __asm__("clc");
  __asm__("adc #1");
  __asm__("cmp #30");
  __asm__("bcc %g", next_old_row);
  __asm__("lda #0");
next_old_row:
  __asm__("sta %v", row_yy);
  __asm__("iny");
  __asm__("iny");
  __asm__("cpy #10");
  __asm__("bcc %g", prepare_row);
}
#pragma optimize (pop)

void compute_next_row(unsigned char y, unsigned char *out) {
  city_output = out;
  row_y = y;
  city_prepare_rows();
  if (y == 0) city_mark_house_columns();
  city_compute_row();
  if (y < 29) city_advance_house_columns();
}


/* Fixed 32-byte, non-overlapping row copy. Eight bytes per loop remove
   generic memcpy setup and most loop branches, with ordinary 6502 opcodes. */
#pragma optimize (push, off)
void city_copy32(void) {
  __asm__("ldy #31");
copy_eight:
  __asm__("lda (%v),y", city_output);
  __asm__("sta (%v),y", city_math_ptr);
  __asm__("dey");
  __asm__("lda (%v),y", city_output);
  __asm__("sta (%v),y", city_math_ptr);
  __asm__("dey");
  __asm__("lda (%v),y", city_output);
  __asm__("sta (%v),y", city_math_ptr);
  __asm__("dey");
  __asm__("lda (%v),y", city_output);
  __asm__("sta (%v),y", city_math_ptr);
  __asm__("dey");
  __asm__("lda (%v),y", city_output);
  __asm__("sta (%v),y", city_math_ptr);
  __asm__("dey");
  __asm__("lda (%v),y", city_output);
  __asm__("sta (%v),y", city_math_ptr);
  __asm__("dey");
  __asm__("lda (%v),y", city_output);
  __asm__("sta (%v),y", city_math_ptr);
  __asm__("dey");
  __asm__("lda (%v),y", city_output);
  __asm__("sta (%v),y", city_math_ptr);
  __asm__("dey");
  __asm__("bpl %g", copy_eight);
}

void sim_step(void) {
  /* Save both old top rows before any writeback. House and column caches
     are reset once per generation; row order and three-row staging stay
     identical to the original simultaneous update. */
  __asm__("ldy #63");
save_top_rows:
  __asm__("lda %v,y", grid);
  __asm__("sta %v,y", top_rows);
  __asm__("dey");
  __asm__("bpl %g", save_top_rows);
  __asm__("jsr %v", city_reset_columns);
  __asm__("lda #0");
  __asm__("sta %v", row_slot);
  __asm__("sta %v", row_y);
  __asm__("sta %v", city_next_slot);
  __asm__("jsr %v", city_prepare_rows);
  __asm__("jsr %v", city_mark_house_columns);
next_output_row:
  __asm__("lda %v", city_next_slot);
  __asm__("asl a");
  __asm__("tax");
  __asm__("lda %v,x", city_next_rows);
  __asm__("sta %v", city_output);
  __asm__("lda %v+1,x", city_next_rows);
  __asm__("sta %v+1", city_output);
  __asm__("jsr %v", city_compute_row);
  __asm__("lda %v", row_y);
  __asm__("cmp #29");
  __asm__("bcs %g", last_house_row);
  __asm__("jsr %v", city_advance_house_columns);
last_house_row:
  __asm__("inc %v", city_next_slot);
  __asm__("lda %v", city_next_slot);
  __asm__("cmp #3");
  __asm__("bcc %g", slot_ready);
  __asm__("lda #0");
  __asm__("sta %v", city_next_slot);
slot_ready:
  __asm__("lda %v", row_y);
  __asm__("cmp #2");
  __asm__("bcc %g", no_writeback);
  __asm__("sbc #2");
  __asm__("asl a");
  __asm__("tax");
  __asm__("lda %v,x", city_grid_rows);
  __asm__("sta %v", city_math_ptr);
  __asm__("lda %v+1,x", city_grid_rows);
  __asm__("sta %v+1", city_math_ptr);
  __asm__("lda %v", city_next_slot);
  __asm__("asl a");
  __asm__("tax");
  __asm__("lda %v,x", city_next_rows);
  __asm__("sta %v", city_output);
  __asm__("lda %v+1,x", city_next_rows);
  __asm__("sta %v+1", city_output);
  __asm__("jsr %v", city_copy32);
no_writeback:
  __asm__("inc %v", row_y);
  __asm__("lda %v", row_y);
  __asm__("cmp #30");
  __asm__("bcs %g", flush_rows);
  __asm__("jsr %v", city_prepare_rows);
  __asm__("jmp %g", next_output_row);
flush_rows:
  __asm__("lda #<(%v+32)", next_rows);
  __asm__("sta %v", city_output);
  __asm__("lda #>(%v+32)", next_rows);
  __asm__("sta %v+1", city_output);
  __asm__("lda #<(%v+896)", grid);
  __asm__("sta %v", city_math_ptr);
  __asm__("lda #>(%v+896)", grid);
  __asm__("sta %v+1", city_math_ptr);
  __asm__("jsr %v", city_copy32);
  __asm__("lda #<(%v+64)", next_rows);
  __asm__("sta %v", city_output);
  __asm__("lda #>(%v+64)", next_rows);
  __asm__("sta %v+1", city_output);
  __asm__("lda #<(%v+928)", grid);
  __asm__("sta %v", city_math_ptr);
  __asm__("lda #>(%v+928)", grid);
  __asm__("sta %v+1", city_math_ptr);
  __asm__("jsr %v", city_copy32);
  __asm__("inc %v", step_count);
  __asm__("bne %g", step_done);
  __asm__("inc %v+1", step_count);
step_done:
  __asm__("rts");
}
#pragma optimize (pop)

unsigned char draw_step_sprites(unsigned char sprid) {
  unsigned char h3, h2, h1, h0;

  /* 16-bit step counter shown directly as four hexadecimal digits. */
  h3 = (unsigned char)((step_count >> 12) & 0x0f);
  h2 = (unsigned char)((step_count >> 8)  & 0x0f);
  h1 = (unsigned char)((step_count >> 4)  & 0x0f);
  h0 = (unsigned char)( step_count        & 0x0f);

  /* First four OAM entries have highest sprite priority. */
  sprid = oam_spr(8,  8, hex_tile(h3), SPR_PAL_DIGIT, sprid);
  sprid = oam_spr(16, 8, hex_tile(h2), SPR_PAL_DIGIT, sprid);
  sprid = oam_spr(24, 8, hex_tile(h1), SPR_PAL_DIGIT, sprid);
  sprid = oam_spr(32, 8, hex_tile(h0), SPR_PAL_DIGIT, sprid);
  return sprid;
}

unsigned char draw_debug_sprites(unsigned char sprid) {
  unsigned char tens;
  unsigned char ones;
  unsigned char interval_code;
  unsigned char interval_hi;
  unsigned char interval_lo;

  tens = accel_debug_code / 10;
  ones = accel_debug_code % 10;

  /*
    Right-hand pair is hexadecimal:
      01..FF = 1..255 steps
      00     = 256 steps
    When no accelerator is present, show 00.
  */
  interval_code = city_accel_available ? accel_display_interval_code : 0;
  interval_hi = (unsigned char)(interval_code >> 4);
  interval_lo = (unsigned char)(interval_code & 0x0f);

  /* Second line: [debug tens][debug ones][interval hex high][interval hex low]. */
  sprid = oam_spr(8,  16, TILE_DIGIT_0 + tens, SPR_PAL_DIGIT, sprid);
  sprid = oam_spr(16, 16, TILE_DIGIT_0 + ones, SPR_PAL_DIGIT, sprid);
  sprid = oam_spr(24, 16, hex_tile(interval_hi), SPR_PAL_DIGIT, sprid);
  sprid = oam_spr(32, 16, hex_tile(interval_lo), SPR_PAL_DIGIT, sprid);
  return sprid;
}

unsigned char hex_tile(unsigned char nibble) {
  nibble &= 0x0f;
  if (nibble < 10) return TILE_DIGIT_0 + nibble;
  return TILE_HEX_A + (nibble - 10);
}

unsigned char draw_hash_sprites(unsigned char sprid) {
  unsigned char h3, h2, h1, h0;

  /* Hex avoids decimal divide/modulo and preserves all 16 CRC bits in 4 chars. */
  h3 = (unsigned char)((grid_crc16 >> 12) & 0x0f);
  h2 = (unsigned char)((grid_crc16 >> 8)  & 0x0f);
  h1 = (unsigned char)((grid_crc16 >> 4)  & 0x0f);
  h0 = (unsigned char)( grid_crc16        & 0x0f);

  /* Third line: CRC16, most-significant nibble first. */
  sprid = oam_spr(8,  24, hex_tile(h3), SPR_PAL_DIGIT, sprid);
  sprid = oam_spr(16, 24, hex_tile(h2), SPR_PAL_DIGIT, sprid);
  sprid = oam_spr(24, 24, hex_tile(h1), SPR_PAL_DIGIT, sprid);
  sprid = oam_spr(32, 24, hex_tile(h0), SPR_PAL_DIGIT, sprid);
  return sprid;
}

void update_digit_sprites(void) {
  unsigned char sprid;
  sprid = draw_step_sprites(0);
  sprid = draw_debug_sprites(sprid);
  draw_hash_sprites(sprid);
}

void init_static_sprites(void) {
  unsigned char x, y;
  unsigned char sprid;
  unsigned int i;

  /* Sea sprites never move because SEA cells are fixed.
     Build the OAM buffer once, then update only the step/debug digit sprites. */
  oam_clear();
  sprid = draw_step_sprites(0);
  sprid = draw_debug_sprites(sprid);
  sprid = draw_hash_sprites(sprid);

  for (y = 0; y < H; ++y) {
    for (x = 0; x < W; ++x) {
      i = ((unsigned int)y) * W + x;
      if (grid[i] == SEA) {
        if (sprid >= 252) return; /* 63 sprites. Keep one slot margin. */
        sprid = oam_spr(x << 3, y << 3, TILE_SEA_SPR, SPR_PAL_SEA, sprid);
      }
    }
  }
}

void draw_full_map_asm(void);
void redraw_full_vblank_asm(void);

void clear_attr_table(void) {
  vram_adr(0x23c0);
  vram_fill(0x00, 64);
}

void main(void) {
  unsigned char frame;

  /* No runtime CHR upload here. city_tiles_chr_direct.s provides CHR ROM. */
  ppu_off();
  pal_bg(pal_bg_city);
  pal_spr(pal_spr_city);

  /* Unlock before make_initial(): after a successful unlock $6000-$63BF is
     the accelerator's active grid window. */
  accel_debug_code = 10;
  accel_display_interval = 0;
  accel_display_interval_code = 0;
  city_accel_available = city_accel_try_enable();
  make_initial();
  grid_crc16 = compute_grid_crc16_asm();
  if (city_accel_available) accel_debug_code = 40;
  /* On failure, keep 21/22/23 visible while the CPU fallback runs. */

  /* Direct-tile display:
     grid[] values 0..100 are valid BG tile IDs, so the ASM renderer can copy
     grid[] directly to the nametable without calling bg_tile_for_value().
     Attribute table is constant because all four BG palettes are identical.
     Sea sprites are fixed, so initialize OAM once and update only step digits. */
  clear_attr_table();
  init_static_sprites();
  draw_full_map_asm();
  scroll(0, 0);
  ppu_on_all();

  frame = 0;

  if (city_accel_available) {
    /*
      FPGA path: calculate accel_display_interval steps continuously, then hash
      and redraw once.  No per-step VBlank wait or debug-code animation is used.
      The only VBlank waits are those inherently required by the visible redraw.
    */
    while (1) {
      city_accel_run_batch();
      grid_crc16 = compute_grid_crc16_asm();
      update_digit_sprites();
      redraw_full_vblank_asm();
    }
  } else {
    /* Emulator / no-accelerator fallback: preserve the original CPU cadence. */
    while (1) {
      ppu_wait_nmi();
      update_digit_sprites();

      ++frame;
      if (frame >= FRAME_DELAY) {
        frame = 0;
        sim_step();
        grid_crc16 = compute_grid_crc16_asm();
        update_digit_sprites();
        redraw_full_vblank_asm();
      }
    }
  }
}
