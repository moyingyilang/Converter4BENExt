// 轻量、纯标量 BC1/BC3 编码器（无 SSE，arm64 可用）。
// 只用于 TGA/PNG/RGBA32 -> BEM fmt 10/12；不做 BC7/BC6H。
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace c4ben_bc {

static inline uint16_t to565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline void from565(uint16_t c, int& r, int& g, int& b) {
    r = (c >> 11) & 0x1F; g = (c >> 5) & 0x3F; b = c & 0x1F;
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
}
static inline int dist2(int r0, int g0, int b0, int r1, int g1, int b1) {
    int dr = r0 - r1, dg = g0 - g1, db = b0 - b1;
    return dr * dr + dg * dg + db * db;
}

static void encodeBc1Block(const uint8_t px[16][4], uint8_t out[8]) {
    int rmin = 255, gmin = 255, bmin = 255, rmax = 0, gmax = 0, bmax = 0;
    for (int i = 0; i < 16; i++) {
        rmin = std::min(rmin, (int)px[i][0]); gmin = std::min(gmin, (int)px[i][1]); bmin = std::min(bmin, (int)px[i][2]);
        rmax = std::max(rmax, (int)px[i][0]); gmax = std::max(gmax, (int)px[i][1]); bmax = std::max(bmax, (int)px[i][2]);
    }
    uint16_t c0 = to565((uint8_t)rmax, (uint8_t)gmax, (uint8_t)bmax);
    uint16_t c1 = to565((uint8_t)rmin, (uint8_t)gmin, (uint8_t)bmin);
    if (c0 < c1) std::swap(c0, c1);
    int r0, g0, b0, r1, g1, b1;
    from565(c0, r0, g0, b0); from565(c1, r1, g1, b1);
    int pal[4][3] = {{r0,g0,b0},{r1,g1,b1},
                     {(2*r0+r1)/3,(2*g0+g1)/3,(2*b0+b1)/3},
                     {(r0+2*r1)/3,(g0+2*g1)/3,(b0+2*b1)/3}};
    uint32_t idx = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bestd = dist2(px[i][0],px[i][1],px[i][2],pal[0][0],pal[0][1],pal[0][2]);
        for (int k = 1; k < 4; k++) {
            int d = dist2(px[i][0],px[i][1],px[i][2],pal[k][0],pal[k][1],pal[k][2]);
            if (d < bestd) { bestd = d; best = k; }
        }
        idx |= ((uint32_t)best) << (2 * i);
    }
    // 若两端点相等，c0<=c1 会进入 BC1 三色模式，但像素仍全同色；保持 c0==c1 即可。
    out[0] = (uint8_t)(c0 & 255); out[1] = (uint8_t)(c0 >> 8);
    out[2] = (uint8_t)(c1 & 255); out[3] = (uint8_t)(c1 >> 8);
    for (int i = 0; i < 4; i++) out[4 + i] = (uint8_t)((idx >> (8 * i)) & 255);
}

static void encodeBc4Block(const uint8_t px[16][4], uint8_t out[8], int channel = 3) {
    int amin = 255, amax = 0;
    for (int i = 0; i < 16; i++) { amin = std::min(amin, (int)px[i][channel]); amax = std::max(amax, (int)px[i][channel]); }
    if (amin == amax) {
        out[0] = out[1] = (uint8_t)amax;
        for (int i = 2; i < 8; i++) out[i] = 0;
        return;
    }
    out[0] = (uint8_t)amax; out[1] = (uint8_t)amin;
    int pal[8];
    pal[0] = amax; pal[1] = amin;
    for (int i = 2; i < 8; i++) {
        // 标准 BC3 8-value 插值：i=2..7 对应 6/7..1/7
        pal[i] = ((8 - i) * amax + (i - 1) * amin + 3) / 7;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bestd = std::abs((int)px[i][channel] - pal[0]);
        for (int k = 1; k < 8; k++) {
            int d = std::abs((int)px[i][channel] - pal[k]);
            if (d < bestd) { bestd = d; best = k; }
        }
        bits |= ((uint64_t)best) << (3 * i);
    }
    for (int i = 0; i < 6; i++) out[2 + i] = (uint8_t)((bits >> (8 * i)) & 255);
}

static void encodeBlocks(const std::vector<uint8_t>& rgba, int w, int h, int mips, bool alpha, std::vector<uint8_t>& out) {
    out.clear();
    for (int m = 0; m < mips; m++) {
        int mw = std::max(1, w >> m), mh = std::max(1, h >> m);
        int bw = (mw + 3) / 4, bh = (mh + 3) / 4;
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
            uint8_t px[16][4];
            for (int py = 0; py < 4; py++) for (int pxx = 0; pxx < 4; pxx++) {
                int x = std::min(bx * 4 + pxx, mw - 1), y = std::min(by * 4 + py, mh - 1);
                const uint8_t* s = &rgba[((size_t)y * mw + x) * 4];
                for (int k = 0; k < 4; k++) px[py * 4 + pxx][k] = s[k];
            }
            uint8_t blk[16];
            if (alpha) {
                // BC3 容器布局：alpha block 在前，color block 在后（bcdec_bc3 同样按此读取）。
                encodeBc4Block(px, blk);
                encodeBc1Block(px, blk + 8);
                out.insert(out.end(), blk, blk + 16);
            } else {
                encodeBc1Block(px, blk);
                out.insert(out.end(), blk, blk + 8);
            }
        }
    }
}

// 返回 BEM fmt：10=BC1，12=BC3；输入 rgba 必须覆盖所有 mip。
static inline int encodeRgba(const std::vector<uint8_t>& rgba, int w, int h, int mips, bool alpha, std::vector<uint8_t>& out) {
    size_t need = 0;
    for (int m = 0; m < mips; m++) need += (size_t)std::max(1, w >> m) * std::max(1, h >> m) * 4;
    if (rgba.size() < need) throw std::runtime_error("bc_encode: RGBA payload shorter than mip chain");
    encodeBlocks(rgba, w, h, mips, alpha, out);
    return alpha ? 12 : 10;
}

// BC5：两个 BC4 通道块（R/G），用于法线类贴图。BEM fmt=27，16B/块。
static inline int encodeRgbaBc5(const std::vector<uint8_t>& rgba, int w, int h, int mips, std::vector<uint8_t>& out) {
    size_t need = 0;
    for (int m = 0; m < mips; m++) need += (size_t)std::max(1, w >> m) * std::max(1, h >> m) * 4;
    if (rgba.size() < need) throw std::runtime_error("bc_encode: RGBA payload shorter than mip chain");
    out.clear();
    for (int m = 0; m < mips; m++) {
        int mw = std::max(1, w >> m), mh = std::max(1, h >> m);
        int bw = (mw + 3) / 4, bh = (mh + 3) / 4;
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
            uint8_t px[16][4];
            for (int py = 0; py < 4; py++) for (int pxx = 0; pxx < 4; pxx++) {
                int x = std::min(bx * 4 + pxx, mw - 1), y = std::min(by * 4 + py, mh - 1);
                const uint8_t* s = &rgba[((size_t)y * mw + x) * 4];
                for (int k = 0; k < 4; k++) px[py * 4 + pxx][k] = s[k];
            }
            uint8_t blk[16];
            encodeBc4Block(px, blk, 0);
            encodeBc4Block(px, blk + 8, 1);
            out.insert(out.end(), blk, blk + 16);
        }
    }
    return 27;
}

// BC4：单通道（R），用于 mask/roughness/metal 类贴图。BEM fmt=26，8B/块。
static inline int encodeRgbaBc4(const std::vector<uint8_t>& rgba, int w, int h, int mips, std::vector<uint8_t>& out) {
    size_t need = 0;
    for (int m = 0; m < mips; m++) need += (size_t)std::max(1, w >> m) * std::max(1, h >> m) * 4;
    if (rgba.size() < need) throw std::runtime_error("bc_encode: RGBA payload shorter than mip chain");
    out.clear();
    for (int m = 0; m < mips; m++) {
        int mw = std::max(1, w >> m), mh = std::max(1, h >> m);
        int bw = (mw + 3) / 4, bh = (mh + 3) / 4;
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
            uint8_t px[16][4];
            for (int py = 0; py < 4; py++) for (int pxx = 0; pxx < 4; pxx++) {
                int x = std::min(bx * 4 + pxx, mw - 1), y = std::min(by * 4 + py, mh - 1);
                const uint8_t* s = &rgba[((size_t)y * mw + x) * 4];
                for (int k = 0; k < 4; k++) px[py * 4 + pxx][k] = s[k];
            }
            uint8_t blk[8];
            encodeBc4Block(px, blk, 0);
            out.insert(out.end(), blk, blk + 8);
        }
    }
    return 26;
}

}
