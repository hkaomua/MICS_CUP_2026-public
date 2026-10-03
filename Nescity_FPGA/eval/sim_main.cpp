#include "Vcity_accelerator_nowater.h"
#include "verilated.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using Grid = std::array<uint8_t, 960>;
static void require(bool ok, const std::string &why) {
    if (!ok) throw std::runtime_error(why);
}
struct Bench {
    Vcity_accelerator_nowater dut;
    uint64_t now = 0, next_cpu, next_calc = 53, calc_edges = 0;
    unsigned cpu_half;
    explicit Bench(unsigned half): next_cpu(half), cpu_half(half) {
        dut.clk_cpu = dut.clk_calc = 0;
        dut.reset = 0;
        idle(); dut.eval();
    }
    void idle() { dut.cpu_sel = dut.cpu_we = dut.cpu_re = 0; }
    void tick() {
        now = std::min(next_cpu, next_calc);
        if (next_cpu == now) { dut.clk_cpu ^= 1; next_cpu += cpu_half; }
        if (next_calc == now) {
            dut.clk_calc ^= 1; next_calc += 50;
            if (dut.clk_calc) ++calc_edges;
        }
        dut.eval();
    }
    void cpu_fall() {
        do { tick(); } while (dut.clk_cpu || now != next_cpu - cpu_half);
    }
    void cpu_cycles(unsigned n) { while (n--) { cpu_fall(); } }
    void write(unsigned a, unsigned d, unsigned hold = 3) {
        cpu_fall(); dut.cpu_addr = a; dut.cpu_din = d;
        dut.cpu_sel = dut.cpu_we = 1; dut.cpu_re = 0; dut.eval();
        cpu_cycles(hold); idle(); dut.eval(); cpu_cycles(2);
    }
    uint8_t read(unsigned a) {
        cpu_fall(); dut.cpu_addr = a;
        dut.cpu_sel = dut.cpu_re = 1; dut.cpu_we = 0; dut.eval();
        cpu_cycles(3);
        require(dut.cpu_claim && dut.cpu_dout_oe, "MMIO read not claimed");
        uint8_t v = dut.cpu_dout; idle(); dut.eval(); return v;
    }
    void reset() {
        idle(); dut.reset = 1; dut.eval(); cpu_cycles(30);
        dut.reset = 0; dut.eval(); cpu_cycles(30);
        require(!dut.busy_out && !dut.cpu_claim, "reset did not disable device");
    }
    void unlock() {
        // A wrong and an interrupted sequence must leave the overlay disabled.
        write(0x1ff0, 'C'); write(0x1ff1, 'X'); write(0x1ff2, 'T'); write(0x1ff3, 'Y');
        dut.cpu_sel = dut.cpu_re = 1; dut.cpu_addr = 0x1f00; dut.eval();
        require(!dut.cpu_claim && !dut.cpu_dout_oe, "bad unlock accepted"); idle();
        write(0x1ff0, 'C', 12); write(0x1ff1, 'I', 12);
        write(0x1ff2, 'T', 12); write(0x1ff3, 'Y', 12);
        require(read(0x1f00) == 'C' && read(0x1f01) == 'A', "signature");
        require(read(0x1f02) == 5 && read(0x1f03) == 0, "version/interval");
        require(read(0x1f14) == 0, "water penalty");
        dut.cpu_sel = dut.cpu_re = 1; dut.cpu_addr = 0x03c0; dut.eval();
        require(!dut.cpu_claim && !dut.cpu_dout_oe, "grid window exceeds 960 bytes"); idle();
    }
    void load(const Grid &g) {
        for (unsigned i=0; i<g.size(); ++i) write(i, g[i]);
    }
    void check(const Grid &g, unsigned case_id, unsigned step) {
        for (unsigned i=0; i<g.size(); ++i) {
            auto actual = read(i);
            require(actual == g[i], "case=" + std::to_string(case_id) + " step=" +
                std::to_string(step) + " cell=" + std::to_string(i) + " expected=" +
                std::to_string(g[i]) + " actual=" + std::to_string(actual));
        }
    }
    uint64_t step(unsigned step, bool exercise_busy) {
        const auto start = calc_edges;
        write(0x1f10, 1);
        require(dut.busy_out && read(0x1f11) == 1, "START status");
        if (exercise_busy) {
            require(read(0) == 255 && read(959) == 255, "busy grid read");
            write(0, 255); write(959, 255); // Must be ignored.
            write(0x1f10, 3); // START and soft reset must both be ignored while busy.
        }
        while (dut.busy_out) {
            tick(); require(calc_edges - start < 100000, "accelerator timeout");
        }
        const auto elapsed = calc_edges - start;
        require(read(0x1f11) == 2, "DONE status");
        require((read(0x1f12) | (read(0x1f13) << 8)) == (step & 65535), "step counter");
        cpu_cycles(20); require(read(0x1f11) == 2, "DONE is not sticky");
        return elapsed;
    }
};
static uint32_t get32(std::ifstream &f) {
    uint32_t x = 0;
    for (int i=0; i<4; ++i) { int c=f.get(); require(c != EOF, "truncated header"); x |= uint32_t(c) << (8*i); }
    return x;
}
static Grid get_grid(std::ifstream &f) {
    Grid g{}; f.read(reinterpret_cast<char *>(g.data()), g.size());
    require(bool(f), "truncated grid"); return g;
}
int main(int argc, char **argv) {
    try {
        Verilated::commandArgs(argc, argv);
        require(argc >= 2, "usage: sim vectors.bin [cpu half-period ns]");
        std::ifstream f(argv[1], std::ios::binary); require(bool(f), "cannot open vectors");
        Bench b(argc >= 3 ? std::stoul(argv[2]) : 5);
        b.reset(); b.unlock();
        const unsigned cases = get32(f); unsigned generations = 0;
        uint64_t min_cycles=UINT64_MAX, max_cycles=0;
        for (unsigned c=0; c<cases; ++c) {
            unsigned steps = get32(f); Grid g=get_grid(f);
            b.write(0x1f10, 2);
            require(b.read(0x1f11) == 0 && b.read(0x1f12) == 0 && b.read(0x1f13) == 0, "soft reset");
            b.load(g); b.check(g, c, 0);
            for (unsigned s=1; s<=steps; ++s) {
                g=get_grid(f);
                auto cycles=b.step(s, s == 1);
                min_cycles=std::min(min_cycles, cycles); max_cycles=std::max(max_cycles, cycles);
                b.check(g, c, s); ++generations;
            }
            if (c == 0 || (c+1)%32 == 0) std::cout << "checked case " << c << ", generations=" << generations << std::endl;
        }
        // Abort during filling, each arithmetic/stream phase, a row boundary,
        // and the final cell. A reset must cancel both an in-flight sample and
        // its pending output write, then permit a clean start on bank 0.
        const uint64_t abort_offsets[] = {4, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139,
            140, 141, 142, 143, 144, 145, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174,
            min_cycles-6, min_cycles-4, min_cycles-2};
        for (uint64_t offset : abort_offsets) {
            const auto start = b.calc_edges;
            b.write(0x1f10, 1); require(b.dut.busy_out, "abort test start");
            while (b.calc_edges-start < offset) b.tick();
            require(b.dut.busy_out, "abort offset is outside computation");
            b.reset(); b.unlock();
            Grid water{}; b.load(water); b.step(1, true); b.check(water, cases, 1);
        }
        std::cout << "PASS cases=" << cases << " generations=" << generations
                  << " calc_cycles_min=" << min_cycles << " calc_cycles_max=" << max_cycles
                  << " cpu_half_ns=" << b.cpu_half << " reset_recoveries=" << sizeof(abort_offsets)/sizeof(abort_offsets[0]) << std::endl;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << std::endl; return 1;
    }
}
