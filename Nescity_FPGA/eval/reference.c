/* Compile the untouched contest C model into a small test-only library. */
#define main city_reference_main
#include "../../linux_city.c"
#undef main

void reference_initial(uint32_t seed, uint8_t *out) {
    make_initial(seed);
    memcpy(out, grid, N);
}

void reference_step(uint8_t *cells) {
    memcpy(grid, cells, N);
    water_penalty = 0;
    sim_step();
    memcpy(cells, grid, N);
}
