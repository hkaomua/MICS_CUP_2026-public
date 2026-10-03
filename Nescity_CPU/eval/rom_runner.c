/* Execute the actual linked PRG ROM using cc65's unmodified sim65 CPU core.
 * This adapter provides bounded internal RAM and records stack accesses.
 * It is a CPU test runner, not a PPU/APU or FPGA timing model. */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "6502.h"
#include "memory.h"
#include "peripherals.h"
#include "trace.h"

uint8_t Mem[65536], TraceMode;
Sim65Peripherals Peripherals;
static unsigned csp, ram_end, min_csp = 0x800, min_hsp = 0xff;
static int checking;
static int boot_mode, ppu_latch, in_nmi;
static unsigned nmi_entry;
static unsigned ppu_addr, ppu_ctrl;
static uint8_t vram[0x4000];
static uint64_t next_nmi = 29781;
static uint64_t pc_cycles[65536];
static int profiling;

static void fail(const char *message) {
    fprintf(stderr, "%s (PC=$%04X)\n", message, Regs.PC);
    exit(1);
}
void Error(const char *format, ...) {
    va_list args; va_start(args, format); vfprintf(stderr, format, args);
    va_end(args); exit(1);
}
void Warning(const char *format, ...) {
    va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args);
}
void PrintTraceNMI(void) {}
void PrintTraceIRQ(void) {}
void PrintTraceInstruction(void) {}
void ParaVirtHooks(CPURegs *regs) { (void)regs; }

uint8_t MemReadByte(uint16_t addr) {
    if (addr < 0x2000) return Mem[addr & 0x7ff];
    if (addr >= 0x8000) return Mem[addr];
    if (boot_mode) {
        if (addr >= 0x2000 && addr < 0x4000 && (addr & 7) == 2) {
            ppu_latch = 0;
            return 0x80; /* Simplified PPU: startup VBlank polling succeeds. */
        }
        return 0; /* No accelerator or cartridge WRAM on the CPU board. */
    }
    fail("Unexpected access outside internal RAM / PRG ROM");
    return 0;
}
void MemWriteByte(uint16_t addr, uint8_t value) {
    if (boot_mode && addr >= 0x2000 && addr < 0x8000) {
        if (addr < 0x4000) {
            switch (addr & 7) {
                case 0: ppu_ctrl = value; break;
                case 5: ppu_latch ^= 1; break;
                case 6:
                    if (!ppu_latch) ppu_addr = (value & 0x3f) << 8;
                    else ppu_addr = (ppu_addr & 0xff00) | value;
                    ppu_latch ^= 1; break;
                case 7:
                    vram[ppu_addr & 0x3fff] = value;
                    ppu_addr += (ppu_ctrl & 4) ? 32 : 1; break;
            }
        }
        if (addr == 0x4014) Peripherals.Counter.ClockCycles +=
            513 + (Peripherals.Counter.ClockCycles & 1);
        return;
    }
    if (addr >= 0x2000) fail("Write outside internal RAM");
    Mem[addr & 0x7ff] = value;
}
uint16_t MemReadWord(uint16_t addr) {
    return MemReadByte(addr) | (MemReadByte(addr + 1) << 8);
}
uint16_t MemReadZPWord(uint8_t addr) {
    return Mem[addr] | (Mem[(uint8_t)(addr + 1)] << 8);
}
void MemWriteWord(uint16_t addr, uint16_t value) {
    MemWriteByte(addr, value); MemWriteByte(addr + 1, value >> 8);
}
void MemInit(void) { memset(Mem, 0, sizeof Mem); }

static unsigned symbol(const char *labels, const char *wanted) {
    FILE *f = fopen(labels, "r");
    char line[512], name[256]; unsigned address;
    if (!f) fail("Cannot open labels");
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "al %x %255s", &address, name) == 2 &&
            name[0] == '.' && !strcmp(name + 1, wanted)) {
            fclose(f); return address;
        }
    }
    fprintf(stderr, "Missing symbol %s\n", wanted); exit(1);
}

static void instruction(void) {
    unsigned pc = Regs.PC, elapsed;
    unsigned op = Mem[Regs.PC], operand = Mem[(uint16_t)(Regs.PC + 1)];
    if (boot_mode && Regs.PC == nmi_entry) in_nmi = 1;
    /* Inspect SP at dereferences rather than during transient low/high-byte
       updates across page boundaries. All cc65 software stack accesses in
       these builds use (c_sp),Y. Calls also capture allocated stack frames. */
    if (checking && (((op & 0x1f) == 0x11 && operand == csp) ||
                     (op == 0x20 && !in_nmi))) {
        unsigned sp = Mem[csp] | (Mem[csp + 1] << 8);
        if (sp < min_csp) min_csp = sp;
        if (sp < ram_end || sp > 0x800) {
            fprintf(stderr, "C SP=$%04X, data end=$%04X\n", sp, ram_end);
            fail("C stack overlaps static data / leaves RAM");
        }
    }
    elapsed = ExecuteInsn();
    if (profiling) pc_cycles[pc] += elapsed;
    if (in_nmi && op == 0x40) in_nmi = 0;
    if (boot_mode && Peripherals.Counter.ClockCycles >= next_nmi) {
        next_nmi += 29781;
        if (ppu_ctrl & 0x80) NMIRequest();
    }
    if (checking && Regs.SP < min_hsp) min_hsp = Regs.SP;
}

static uint64_t call(unsigned entry) {
    uint64_t start = Peripherals.Counter.ClockCycles;
    Regs.SP = 0xfd;
    Mem[0x1fe] = 0xff; Mem[0x1ff] = 0x4f; /* RTS sentinel $5000 */
    Regs.PC = entry;
    while (Regs.PC != 0x5000) {
        instruction();
        if (Peripherals.Counter.ClockCycles - start > 200000000ULL)
            fail("Function exceeded cycle limit");
    }
    if ((Mem[csp] | (Mem[csp + 1] << 8)) != 0x800) fail("Unbalanced C stack");
    return Peripherals.Counter.ClockCycles - start;
}

int main(int argc, char **argv) {
    FILE *f, *out;
    unsigned data_load, data_run, data_size, grid, step, crc, n, i, steps;
    uint8_t header[16];
    uint64_t cycles, total = 0;
    if (argc != 6 && argc != 7) {
        fprintf(stderr, "Usage: rom_runner ROM LABELS STEPS OUTPUT INPUT|initial|boot|math [water_penalty]\n");
        return 1;
    }
    steps = strtoul(argv[3], NULL, 0);
    f = fopen(argv[1], "rb");
    if (!f || fread(header, 1, 16, f) != 16 || memcmp(header, "NES\x1a\x02\x01", 6))
        fail("Expected NROM-256 ROM");
    if (fread(Mem + 0x8000, 1, 32768, f) != 32768) fail("Truncated ROM");
    fclose(f);
    csp = symbol(argv[2], "c_sp");
    ram_end = symbol(argv[2], "__BSS_RUN__") + symbol(argv[2], "__BSS_SIZE__");
    data_load = symbol(argv[2], "__DATA_LOAD__");
    data_run = symbol(argv[2], "__DATA_RUN__");
    data_size = symbol(argv[2], "__DATA_SIZE__");
    memcpy(Mem + data_run, Mem + data_load, data_size);
    MemWriteWord(csp, 0x800);
    Regs.SR = 0x24; CPU = CPU_6502; checking = 1;
    grid = symbol(argv[2], "_grid");
    step = symbol(argv[2], "_sim_step");
    crc = symbol(argv[2], "_compute_grid_crc16_asm");
    if (!strcmp(argv[5], "math")) {
        unsigned average_entry = symbol(argv[2], "_city_compute_average");
        unsigned sum_addr = symbol(argv[2], "_row_weighted_sum");
        unsigned avg_addr = symbol(argv[2], "_row_avg");
        unsigned force_addr = symbol(argv[2], "_city_force_delta");
        unsigned capacity_addr = symbol(argv[2], "_city_capacity_loss");
        for (n = 0; n <= 6500; ++n) {
            MemWriteWord(sum_addr, n);
            call(average_entry);
            if (Mem[avg_addr] != n / 65) fail("Incorrect exact /65");
        }
        for (n = 0; n < 326; ++n) {
            /* C signed division truncates toward zero, including negatives. */
            if ((int8_t)Mem[force_addr + n] != ((int)n - 150) / 20)
                fail("Incorrect signed force division table");
        }
        for (n = 0; n <= 100; ++n) {
            if (Mem[capacity_addr + n] != (n > 55 ? (n - 55) / 3 * 4 : 0))
                fail("Incorrect overcrowding division table");
        }
        printf("math_average 6501\nmath_force 326\nmath_capacity 101\n");
        return 0;
    }
    if (!strcmp(argv[5], "boot")) {
        uint64_t last = 0;
        unsigned step_addr = symbol(argv[2], "_step_count");
        unsigned crc_addr = symbol(argv[2], "_grid_crc16");
        unsigned main_addr = symbol(argv[2], "_main");
        boot_mode = 1; checking = 0; nmi_entry = MemReadWord(0xfffa);
        memset(Mem, 0, 0x800); Reset();
        out = fopen(argv[4], "wb");
        if (!out) fail("Cannot open boot output");
        while (1) {
            if (Regs.PC == main_addr) checking = 1;
            if (Regs.PC == step) {
                n = MemReadWord(step_addr);
                if (fwrite(Mem + grid, 1, 960, out) != 960) fail("Output write failed");
                if (memcmp(vram + 0x2000, Mem + grid, 960)) fail("Nametable differs from grid");
                printf("crc %u %04X\n", n, MemReadWord(crc_addr));
                if (n) printf("cycles %u %" PRIu64 "\n", n,
                              Peripherals.Counter.ClockCycles - last);
                last = Peripherals.Counter.ClockCycles;
                if (n >= steps) break;
            }
            instruction();
            if (Peripherals.Counter.ClockCycles > (steps + 1ULL) * 200000000ULL)
                fail("Boot exceeded cycle limit");
        }
        if (fclose(out)) fail("Output close failed");
        printf("min_csp %u\nmin_hsp %u\nram_end %u\nnmi %" PRIu64 "\n",
               min_csp, min_hsp, ram_end, Peripherals.Counter.NmiEvents);
        return 0;
    }
    if (!strcmp(argv[5], "initial")) call(symbol(argv[2], "_make_initial"));
    else {
        f = fopen(argv[5], "rb");
        if (!f || fread(Mem + grid, 1, 960, f) != 960) fail("Expected 960 input cells");
        fclose(f);
    }
    for (i = 0; i < 960; ++i) if (Mem[grid + i] > 100) fail("Invalid input state");
    if (argc == 7) Mem[symbol(argv[2], "_water_penalty")] = strtoul(argv[6], NULL, 0);
    out = fopen(argv[4], "wb");
    if (!out) fail("Cannot open output");
    for (n = 0; n <= steps; ++n) {
        if (fwrite(Mem + grid, 1, 960, out) != 960) fail("Output write failed");
        call(crc);
        printf("crc %u %04X\n", n, Regs.AC | (Regs.XR << 8));
        if (n == steps) break;
        profiling = getenv("NESCITY_PROFILE") != NULL;
        cycles = call(step); total += cycles;
        profiling = 0;
        printf("cycles %u %" PRIu64 "\n", n + 1, cycles);
        if (MemReadWord(symbol(argv[2], "_step_count")) != (uint16_t)(n + 1))
            fail("Wrong step counter");
    }
    if (fclose(out)) fail("Output close failed");
    if (getenv("NESCITY_PROFILE")) {
        out = fopen(getenv("NESCITY_PROFILE"), "w");
        if (!out) fail("Cannot open profile output");
        fprintf(out, "address,cycles\n");
        for (i = 0; i < 65536; ++i)
            if (pc_cycles[i]) fprintf(out, "%04X,%" PRIu64 "\n", i, pc_cycles[i]);
        if (fclose(out)) fail("Profile close failed");
    }
    printf("total %" PRIu64 "\nmin_csp %u\nmin_hsp %u\nram_end %u\n",
           total, min_csp, min_hsp, ram_end);
    return 0;
}
