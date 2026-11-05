#include "../d3d9_world_filter.h"

#include <cstdio>
#include <cstdint>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main() {
    // Wrapped trilinear world texture with the game's anisotropy disabled.
    uint32_t s[14] = {0, 1, 1, 1, 0, 2, 2, 2, 0, 0, 16};
    CHECK(d3d9_world_anisotropy(s, 0) == 1);
    CHECK(d3d9_world_anisotropy(s, 1) == 1);
    CHECK(d3d9_world_anisotropy(s, 2) == 4);
    CHECK(d3d9_world_anisotropy(s, 3) == 8);
    CHECK(d3d9_world_anisotropy(s, 4) == 16);
    s[6] = 3; s[10] = 8;
    CHECK(d3d9_world_anisotropy(s, 0) == 8);
    CHECK(d3d9_world_anisotropy(s, 1) == 1);
    CHECK(d3d9_world_anisotropy(s, 4) == 16);
    // Clamped effects, point samplers and non-trilinear samplers retain the
    // game's choice even when the world policy changes.
    s[1] = 3;
    CHECK(d3d9_world_anisotropy(s, 4) == 8);
    s[1] = 1; s[5] = 1;
    CHECK(d3d9_world_anisotropy(s, 4) == 8);
    s[5] = 2; s[7] = 1;
    CHECK(d3d9_world_anisotropy(s, 4) == 8);
    s[10] = 100;
    CHECK(d3d9_world_anisotropy(s, 0) == 16);
}
