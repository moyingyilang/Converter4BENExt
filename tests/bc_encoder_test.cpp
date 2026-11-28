// Host test: scalar BC1/BC3 encoder round-trip（不依赖游戏/样本）。
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#include "bc_encode.h"
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

static double roundtrip(bool alpha) {
    const int W = 32, H = 32;
    std::vector<uint8_t> rgba((size_t)W * H * 4);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        size_t i = ((size_t)y * W + x) * 4;
        rgba[i] = (uint8_t)(x * 8); rgba[i + 1] = (uint8_t)(y * 8);
        rgba[i + 2] = (uint8_t)((x + y) * 4); rgba[i + 3] = alpha ? (uint8_t)(x * 8) : 255;
    }
    std::vector<uint8_t> enc;
    int fmt = c4ben_bc::encodeRgba(rgba, W, H, 1, alpha, enc);
    if (fmt != (alpha ? 12 : 10)) { fprintf(stderr, "fmt mismatch\n"); return 1e9; }
    std::vector<uint8_t> dec((size_t)W * H * 4);
    for (int by = 0; by < H; by += 4) for (int bx = 0; bx < W; bx += 4) {
        int bi = (by / 4) * (W / 4) + bx / 4;
        if (alpha) bcdec_bc3(enc.data() + bi * 16, dec.data() + (size_t)by * W * 4 + bx * 4, W * 4);
        else bcdec_bc1(enc.data() + bi * 8, dec.data() + (size_t)by * W * 4 + bx * 4, W * 4);
    }
    long long err = 0;
    for (int i = 0; i < W * H; i++) {
        for (int k = 0; k < 3; k++) { int d = (int)dec[i * 4 + k] - (int)rgba[i * 4 + k]; err += d * d; }
        if (alpha) { int d = (int)dec[i * 4 + 3] - (int)rgba[i * 4 + 3]; err += d * d; }
    }
    return std::sqrt((double)err / (W * H * (alpha ? 4 : 3)));
}

static double roundtripBc5() {
    const int W = 32, H = 32;
    std::vector<uint8_t> rgba((size_t)W * H * 4);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        size_t i = ((size_t)y * W + x) * 4;
        rgba[i] = (uint8_t)(x * 8); rgba[i + 1] = (uint8_t)(y * 8);
        rgba[i + 2] = 0; rgba[i + 3] = 255;
    }
    std::vector<uint8_t> enc;
    int fmt = c4ben_bc::encodeRgbaBc5(rgba, W, H, 1, enc);
    if (fmt != 27) { fprintf(stderr, "bc5 fmt mismatch\n"); return 1e9; }
    std::vector<uint8_t> dec((size_t)W * H * 4);
    for (int by = 0; by < H; by += 4) for (int bx = 0; bx < W; bx += 4) {
        int bi = (by / 4) * (W / 4) + bx / 4;
        bcdec_bc5(enc.data() + bi * 16, dec.data() + (size_t)by * W * 2 + bx * 2, W * 2);
    }
    long long err = 0;
    for (int i = 0; i < W * H; i++) {
        int dr = (int)dec[i * 2] - (int)rgba[i * 4];
        int dg = (int)dec[i * 2 + 1] - (int)rgba[i * 4 + 1];
        err += dr * dr + dg * dg;
    }
    return std::sqrt((double)err / (W * H * 2));
}

static double roundtripBc4() {
    const int W = 32, H = 32;
    std::vector<uint8_t> rgba((size_t)W * H * 4);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        size_t i = ((size_t)y * W + x) * 4;
        rgba[i] = (uint8_t)(x * 8); rgba[i + 1] = 0; rgba[i + 2] = 0; rgba[i + 3] = 255;
    }
    std::vector<uint8_t> enc;
    int fmt = c4ben_bc::encodeRgbaBc4(rgba, W, H, 1, enc);
    if (fmt != 26) { fprintf(stderr, "bc4 fmt mismatch\n"); return 1e9; }
    std::vector<uint8_t> dec((size_t)W * H);
    for (int by = 0; by < H; by += 4) for (int bx = 0; bx < W; bx += 4) {
        int bi = (by / 4) * (W / 4) + bx / 4;
        bcdec_bc4(enc.data() + bi * 8, dec.data() + (size_t)by * W + bx, W);
    }
    long long err = 0;
    for (int i = 0; i < W * H; i++) {
        int dr = (int)dec[i] - (int)rgba[i * 4];
        err += dr * dr;
    }
    return std::sqrt((double)err / (W * H));
}

int main() {
    double o = roundtrip(false), a = roundtrip(true), n = roundtripBc5(), m = roundtripBc4();
    printf("BC1 rmse=%.2f  BC3 rmse=%.2f  BC5 rg-rmse=%.2f  BC4 r-rmse=%.2f\n", o, a, n, m);
    if (o > 35.0 || a > 35.0 || n > 35.0 || m > 35.0) { fprintf(stderr, "BC encoder roundtrip error too high\n"); return 1; }
    return 0;
}
