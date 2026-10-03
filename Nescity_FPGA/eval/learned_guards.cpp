// Reuse the real MMIO driver; no direct access to DUT internals.
#define main general_regression_main
#include "sim_main.cpp"
#undef main

int main(int argc, char **argv) {
    try {
        Verilated::commandArgs(argc, argv);
        require(argc >= 2, "usage: learned_guards vectors.bin [cpu half-period]");
        Bench b(argc >= 3 ? std::stoul(argv[2]) : 5);
        std::ifstream f(argv[1], std::ios::binary);
        require(bool(f), "guard vectors missing");
        unsigned cases = get32(f), checked = 0;
        for (unsigned c = 0; c < cases; ++c) {
            unsigned mode = get32(f), warm = get32(f), index = get32(f), arg = get32(f);
            bool trained = get32(f);
            Grid g = get_grid(f);
            b.reset(); b.unlock(); b.load(g); b.check(g, c, 0);
            unsigned step = 0;
            for (unsigned s = 1; s <= warm; ++s) {
                g = get_grid(f);
                auto cycles = b.step(++step, s == 87);
                b.check(g, c, step); ++checked;
                if (s >= 87) require(trained ? cycles < 4200 : cycles >= 6400,
                                    "initial identity / learned selection");
            }
            if (mode == 0) {
                // An accepted CPU edit invalidates the learned trajectory even
                // when the same byte is written back (last case tests this).
                b.write(index, arg);
                g[index] = arg;
                b.check(g, c, step);
            } else if (mode == 1) {
                // warm is even, so the retained active board is in bank 0.
                b.write(0x1f10, 2); step = 0;
            } else if (mode == 2) {
                const auto start = b.calc_edges;
                b.write(0x1f10, 1);
                while (b.calc_edges - start < arg) b.tick();
                require(b.dut.busy_out, "learned abort outside calculation");
                b.reset(); b.unlock(); b.load(g); step = 0;
            }
            if (mode == 4) step = 0; // START + soft reset together
            unsigned post = get32(f);
            for (unsigned s = 0; s < post; ++s) {
                g = get_grid(f);
                auto cycles = b.step(++step, s == 0, mode == 4 && s == 0 ? 3 : 1);
                b.check(g, c, step); ++checked;
                require(cycles >= 6400, "edited/reset/untrained board used learned shortcut");
            }
            std::cout << "checked guard case=" << c << " mode=" << mode << std::endl;
        }
        std::cout << "PASS learned guards cases=" << cases << " generations=" << checked
                  << " cpu_half_ns=" << b.cpu_half << std::endl;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << std::endl; return 1;
    }
}
