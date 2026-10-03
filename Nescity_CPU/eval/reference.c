/* The distribution's independent Linux model is included without editing it. */
#define main original_display_main
#include "../../linux_city.c"
#undef main

int main(int argc, char **argv) {
    FILE *f;
    unsigned steps, n;
    if (argc != 4 && argc != 5) return 1;
    steps = (unsigned)strtoul(argv[1], NULL, 0);
    if (!strcmp(argv[3], "initial")) make_initial(6502);
    else {
        f = fopen(argv[3], "rb");
        if (!f || fread(grid, 1, N, f) != N) return 1;
        fclose(f);
    }
    if (argc == 5) water_penalty = (uint8_t)strtoul(argv[4], NULL, 0);
    f = fopen(argv[2], "wb");
    if (!f) return 1;
    for (n = 0; n <= steps; ++n) {
        if (fwrite(grid, 1, N, f) != N) return 1;
        if (n < steps) sim_step();
    }
    return fclose(f) != 0;
}
