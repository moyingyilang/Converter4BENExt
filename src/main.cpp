// Converter4BENeXt —— EFMI/EFF -> BEN BEM 迁移工具（C++17，无 .NET / Python）
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <zlib.h>
#if __has_include(<archive.h>)
#include <archive.h>
#include <archive_entry.h>
#define C4BEN_HAVE_LIBARCHIVE 1
#endif
#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include "bcdec.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "stb_image.h"
#include "bc_encode.h"
#include "third_party/cJSON.h"
#include "c4ben.h"

using namespace std;
static bool g_textureFingerprint = false;
static bool g_autoEntries = false; // --auto-entries：布局-only catalog 的唯一指纹锚定
static string g_optionReport;      // --option-report <file>：EFMI 选项/组件条件导出
static bool g_emitOptions = false; // --emit-options：输出 BEM 1.1 option_groups
static bool g_textureCompress = false; // --texture-compress：RGBA32 -> BC1/BC3
// 显式开启的旧布局兼容：源 VB 缺少目标尾部顶点属性时补零，而不是直接拒绝。
// 只允许 source stride < target stride；不做删除/重排，避免静默猜布局。
static bool g_compatLayout = false;
// 显式开启的旧全局骨骼兼容：源组件使用的全局 bone id 数量与目标组件 bone_names
// 数量一致时，按 source id 数值序 -> target bone_names 顺序做排名映射。
static bool g_compatBones = false;
// 外部 global skeleton 表：global bone id -> bone name（avtos TSV）。
static map<int, string> g_skeleton;
static string g_forceCharacter;
static cJSON* g_identities = nullptr;
static void loadSkeletonTable(const string& path) {
    ifstream f(path);
    if (!f) throw runtime_error("cannot open skeleton table: " + path);
    string line;
    while (getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        vector<string> cols; size_t a = 0;
        while (a <= line.size()) {
            size_t b = line.find('\t', a);
            if (b == string::npos) b = line.size();
            cols.push_back(line.substr(a, b - a)); a = b + 1;
        }
        if (cols.size() < 4) continue;
        try { g_skeleton[stoi(cols[0])] = cols.back(); } catch (...) {}
    }
    cerr << "skeleton: loaded " << g_skeleton.size() << " global bones from " << path << "\n";
}
namespace fs = std::filesystem;

struct Ini {
    map<string, map<string, string>> sec;
    vector<string> order;
    static Ini parse(const string& path) {
        Ini ini; ifstream f(path); string cur, line;
        while (getline(f, line)) {
            auto trim = [](string s) { size_t a = s.find_first_not_of(" \t\r\n"); size_t b = s.find_last_not_of(" \t\r\n"); return a == string::npos ? string() : s.substr(a, b - a + 1); };
            line = trim(line); if (line.empty() || line[0] == ';') continue;
            if (line.size() > 2 && line[0] == '[' && line.back() == ']') {
                cur = trim(line.substr(1, line.size() - 2));
                if (!ini.sec.count(cur)) { ini.sec[cur]; ini.order.push_back(cur); }
                continue;
            }
            auto eq = line.find('='); if (eq == string::npos) continue;
            string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            auto& d = ini.sec[cur];
            if (!d.count(k)) d[k] = v;
        }
        return ini;
    }
};

static void writeFile(const string& p, const vector<uint8_t>& b) {
    ofstream f(p, ios::binary); if (!f) throw runtime_error("write failed: " + p);
    f.write((const char*)b.data(), b.size());
}

static vector<uint8_t> readFile(const string& p) {
    ifstream f(p, ios::binary); if (!f) throw runtime_error("open failed: " + p);
    return vector<uint8_t>((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
}

static string lower(string s);
// srgbKnown：只有 DX10 头自带色彩空间才算已知。legacy fourCC（DXT1/3/5）没有这个
// 字段，必须回退到贴图名启发式，不能默认按线性输出，否则 albedo 会被标成 linear。
struct Dds { int w, h, mips, fmt; bool srgb; bool srgbKnown = false; vector<uint8_t> data; };

// 某个 BEM 纹理工况的期望字节数；返回 0 表示格式未知，跳过校验。
static size_t expectedTextureBytes(int fmt, int w, int h, int mips) {
    if (w <= 0 || h <= 0 || mips <= 0) return 0;
    size_t total = 0;
    for (int m = 0; m < mips; m++) {
        const int mw = std::max(1, w >> m), mh = std::max(1, h >> m);
        const size_t blocks = (size_t)((mw + 3) / 4) * (size_t)((mh + 3) / 4);
        switch (fmt) {
            case 10: case 26: total += blocks * 8; break;            // BC1 / BC4
            case 12: case 25: case 27: total += blocks * 16; break;  // BC3 / BC7 / BC5
            case 4: total += (size_t)mw * (size_t)mh * 4; break;     // RGBA32
            default: return 0;
        }
    }
    return total;
}
static uint32_t rd32(const vector<uint8_t>& b, size_t o) { return (uint32_t)b[o] | ((uint32_t)b[o+1] << 8) | ((uint32_t)b[o+2] << 16) | ((uint32_t)b[o+3] << 24); }

static void checkDdsPayload(const Dds& d, const string& path) {
    const size_t need = expectedTextureBytes(d.fmt, d.w, d.h, d.mips);
    if (need && d.data.size() < need) {
        throw runtime_error("truncated DDS payload: " + path + " has " + to_string(d.data.size()) +
                            " bytes, format needs " + to_string(need));
    }
}

// Decode a BC2/DXT3 chain to RGBA32: the BEM runtime does not accept BC2, so the
// converter expands it instead of dropping the texture.
static void decodeBc2ToRgba(Dds& d, const vector<uint8_t>& in, size_t off) {
    vector<uint8_t> out;
    size_t src = off;
    int w = d.w, h = d.h;
    for (int m = 0; m < d.mips; m++) {
        int mw = std::max(1, w >> m), mh = std::max(1, h >> m);
        int bw = (mw + 3) / 4, bh = (mh + 3) / 4;
        size_t need = (size_t)bw * bh * 16;
        if (src + need > in.size()) throw runtime_error("truncated BC2 DDS");
        size_t base = out.size();
        out.resize(base + (size_t)mw * mh * 4);
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
            uint8_t block[64];
            bcdec_bc2(in.data() + src + ((size_t)by * bw + bx) * 16, block, 16);
            for (int py = 0; py < 4; py++) for (int px = 0; px < 4; px++) {
                int x = bx * 4 + px, y = by * 4 + py;
                if (x >= mw || y >= mh) continue;
                memcpy(&out[base + ((size_t)y * mw + x) * 4], &block[(py * 4 + px) * 4], 4);
            }
        }
        src += need;
    }
    d.data.swap(out);
    d.fmt = 4;
}

static void bgraToRgba(vector<uint8_t>& data) {
    for (size_t i = 0; i + 3 < data.size(); i += 4) std::swap(data[i], data[i + 2]);
}

static Dds readDds(const string& path) {
    auto b = readFile(path);
    if (b.size() < 128 || memcmp(b.data(), "DDS ", 4)) throw runtime_error("not DDS: " + path);
    Dds d{}; d.h = rd32(b, 12); d.w = rd32(b, 16); d.mips = rd32(b, 28); if (d.mips <= 0) d.mips = 1;
    const uint32_t caps2 = rd32(b, 116);
    if (caps2 & 0x200u) throw runtime_error("DDS cubemap is not representable as BEM 2D texture: " + path);
    if (rd32(b, 24) > 1) throw runtime_error("DDS volume texture is not representable as BEM 2D texture: " + path);
    string fourcc((char*)b.data() + 84, 4);
    size_t off = 128; int code = -1;
    if (fourcc == "DX10") {
        code = rd32(b, 128); off = 148;
        d.srgbKnown = true; // DX10 的格式码已区分 UNORM / UNORM_SRGB
        const uint32_t dimension = rd32(b, 132), misc = rd32(b, 136), arraySize = rd32(b, 140);
        if (dimension == 4) throw runtime_error("DDS volume texture is not representable as BEM 2D texture: " + path);
        if (dimension == 5) throw runtime_error("DDS cubemap is not representable as BEM 2D texture: " + path);
        if (arraySize > 1 || (misc & 0x4u)) throw runtime_error("DDS texture array is not representable as BEM 2D texture: " + path);
    }
    else if (fourcc == "DXT1") code = 71; else if (fourcc == "DXT5") code = 77;
    else if (fourcc == "DXT3") code = 74;
    else if (fourcc == "ATI1" || fourcc == "BC4U") code = 80; else if (fourcc == "ATI2" || fourcc == "BC5U") code = 83;
    switch (code) {
        case 71: d.fmt = 10; d.srgb = false; break; case 72: d.fmt = 10; d.srgb = true; break;
        case 77: d.fmt = 12; d.srgb = false; break; case 78: d.fmt = 12; d.srgb = true; break;
        case 80: d.fmt = 26; d.srgb = false; break; case 81: d.fmt = 26; d.srgb = true; break;
        case 83: d.fmt = 27; d.srgb = false; break; case 84: d.fmt = 27; d.srgb = true; break;
        case 98: d.fmt = 25; d.srgb = false; break; case 99: d.fmt = 25; d.srgb = true; break;
        case 74: case 75: {
            if (b.size() < off) throw runtime_error("truncated DDS " + path);
            decodeBc2ToRgba(d, b, off);
            d.srgb = (code == 75);
            return d;
        }
        case 95: case 96: throw runtime_error("BC6H HDR is not representable as BEM RGBA32 (8-bit): " + path);
        case 28: case 87: {
            d.fmt = 4; d.srgb = false;
            d.data.assign(b.begin() + off, b.end());
            if (code == 87) bgraToRgba(d.data);
            checkDdsPayload(d, path);
            return d;
        }
        default: throw runtime_error("unsupported DDS format " + to_string(code) + " in " + path);
    }
    if (b.size() < off) throw runtime_error("truncated DDS " + path);
    d.data.assign(b.begin() + off, b.end());
    checkDdsPayload(d, path);
    return d;
}

// --texture-compress：把 BEM fmt4 (RGBA32) 编码成 BC1/BC3。可选、默认关闭；
// 不改变 DDS 原生 BC1/3/4/5/7 直通路径。arm64 纯标量实现。
static void applyTextureCompression(Dds& d) {
    if (!g_textureCompress || d.fmt != 4 || d.data.empty()) return;
    bool hasAlpha = false;
    for (size_t i = 3; i < d.data.size(); i += 4)
        if (d.data[i] != 255) { hasAlpha = true; break; }
    vector<uint8_t> out;
    const size_t before = d.data.size();
    try {
        int fmt = c4ben_bc::encodeRgba(d.data, d.w, d.h, d.mips, hasAlpha, out);
        d.data.swap(out);
        d.fmt = fmt;
        cerr << "texture compress: " << (hasAlpha ? "BC3" : "BC1")
             << " " << before << " -> " << d.data.size() << " bytes\n";
    } catch (exception& e) {
        cerr << "texture compress skipped: " << e.what() << "\n";
    }
}

static bool linearTextureName(const string& name) {
    string s = lower(name);
    for (const char* token : {"_n", "normal", "_nm", "_mask", "rough", "metal", "_mra", "_orm", "_p"}) {
        if (s.find(token) != string::npos) return true;
    }
    return false;
}

// DDS keeps its native format (BC1/3/4/5/7, RGBA); TGA/PNG/JPG are decoded to
// RGBA32, which BEM accepts. sRGB is inferred from the slot/asset name.
static Dds readTexture(const string& path, const string& hint) {
    string p = lower(path);
    if (p.size() >= 4 && p.substr(p.size() - 4) == ".dds") {
        Dds d = readDds(path);
        // legacy DDS 头不带色彩空间；沿用与 PNG/TGA 相同的历史启发式补齐。
        if (!d.srgbKnown) d.srgb = !linearTextureName(hint.empty() ? path : hint);
        applyTextureCompression(d);
        return d;
    }
    int w = 0, h = 0, n = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!pixels) throw runtime_error("not DDS and image decode failed: " + path);
    Dds d{}; d.w = w; d.h = h; d.mips = 1; d.fmt = 4;
    d.srgb = !linearTextureName(hint.empty() ? path : hint);
    d.data.assign(pixels, pixels + (size_t)w * h * 4);
    stbi_image_free(pixels);
    applyTextureCompression(d);
    return d;
}

static cJSON* get(cJSON* o, const char* k) { return cJSON_GetObjectItemCaseSensitive(o, k); }
static string jstr(cJSON* o, const char* k) { auto v = get(o, k); return cJSON_IsString(v) ? v->valuestring : ""; }
static int jint(cJSON* o, const char* k) { auto v = get(o, k); return cJSON_IsNumber(v) ? v->valueint : 0; }
static string lower(string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

struct Cat {
    cJSON* root = nullptr;
    string character; cJSON* comps = nullptr; cJSON* entries = nullptr; cJSON* textures = nullptr;
    vector<int> ids; vector<int> base; int totalBones = 0;
};

static Cat loadCatalog(const string& dir, const vector<string>& hashes) {
    Cat best; int bestHit = -1;
    for (auto& p : fs::directory_iterator(dir)) {
        if (p.path().extension() != ".json") continue;
        auto txt = readFile(p.path().string());
        cJSON* root = cJSON_ParseWithLength((const char*)txt.data(), txt.size());
        if (!root) continue;
        auto kind = get(root, "kind");
        if (!cJSON_IsString(kind) || string(kind->valuestring) != "bem-character-catalog") { cJSON_Delete(root); continue; }
        auto entries = get(root, "entries"); int hit = 0;
        if (cJSON_IsObject(entries)) for (auto& h : hashes) if (cJSON_GetObjectItemCaseSensitive(entries, h.c_str())) hit++;
        if (hit > bestHit) {
            if (best.root) cJSON_Delete(best.root);
            best.root = root; bestHit = hit;
            best.character = jstr(root, "character_id"); best.comps = get(root, "components");
            best.entries = entries; best.textures = get(root, "textures");
        } else cJSON_Delete(root);
    }
    if (!best.root || bestHit <= 0) throw runtime_error("no BEN catalog character matched");
    best.ids.clear();
    for (auto* c = best.comps->child; c; c = c->next) best.ids.push_back(stoi(string(c->string)));
    sort(best.ids.begin(), best.ids.end());
    best.base.resize(best.ids.size()); int acc = 0;
    for (size_t i = 0; i < best.ids.size(); i++) {
        best.base[i] = acc;
        acc += cJSON_GetArraySize(get(cJSON_GetObjectItemCaseSensitive(best.comps, to_string(best.ids[i]).c_str()), "bone_names"));
    }
    best.totalBones = acc;
    return best;
}

static cJSON* jarr() { return cJSON_CreateArray(); }
static void jadd(cJSON* a, cJSON* v) { cJSON_AddItemToArray(a, v); }
static cJSON* jnum(int v) { return cJSON_CreateNumber(v); }
static cJSON* jstrv(const string& s) { return cJSON_CreateString(s.c_str()); }
static cJSON* obj(const char* k) { return cJSON_CreateObject(); }

struct Entry { string name; vector<uint8_t> data; uint32_t crc; uint32_t off; };
static void writeZip(const string& path, vector<Entry>& es) {
    ofstream f(path, ios::binary); if (!f) throw runtime_error("open out failed");
    auto u16 = [&](uint16_t v) { f.put(v & 255); f.put(v >> 8); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) f.put((v >> (8 * i)) & 255); };
    for (auto& e : es) {
        e.off = (uint32_t)f.tellp(); e.crc = crc32(0L, Z_NULL, 0); e.crc = crc32(e.crc, e.data.data(), e.data.size());
        u32(0x04034b50); u16(20); u16(0); u16(0); u16(0); u16(0); u32(e.crc); u32(e.data.size()); u32(e.data.size());
        u16(e.name.size()); u16(0); f.write(e.name.data(), e.name.size()); f.write((char*)e.data.data(), e.data.size());
    }
    uint32_t cdStart = (uint32_t)f.tellp();
    for (auto& e : es) {
        u32(0x02014b50); u16(20); u16(20); u16(0); u16(0); u16(0); u16(0); u32(e.crc); u32(e.data.size()); u32(e.data.size());
        u16(e.name.size()); u16(0); u16(0); u16(0); u16(0); u32(0); u32(e.off); f.write(e.name.data(), e.name.size());
    }
    uint32_t cdSize = (uint32_t)f.tellp() - cdStart;
    u32(0x06054b50); u16(0); u16(0); u16(es.size()); u16(es.size()); u32(cdSize); u32(cdStart); u16(0);
}

// 失败不交付：没有任何 mesh / texture 时不写空包（历史行为：legacy-auto 全组件被拒时
// 仍会写出一个 0 内容的 BEM 并返回成功）。
static void requireNonEmptyOutput(cJSON* jmeshes, cJSON* jtextures, const string& out) {
    const int nm = cJSON_GetArraySize(jmeshes), nt = cJSON_GetArraySize(jtextures);
    if (nm == 0 && nt == 0)
        throw runtime_error("no meshes or textures converted; refusing to write an empty package: " + out);
}

static void writeBem(const string& path, const vector<uint8_t>& manifest, const vector<vector<uint8_t>>& payloads, int minor = 0) {    // BEM 二进制容器：40B header + manifest + 32B * N entries + payload blocks
    uint64_t fileSize = 40 + manifest.size() + 32ull * payloads.size();
    for (auto& p : payloads) fileSize += p.size();
    vector<uint8_t> out; out.reserve(fileSize);
    auto put16 = [&](uint16_t v) { out.push_back(v & 255); out.push_back(v >> 8); };
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) out.push_back((v >> (8 * i)) & 255); };
    auto put64 = [&](uint64_t v) { for (int i = 0; i < 8; i++) out.push_back((v >> (8 * i)) & 255); };
    const char magic[8] = {'B','E','M',0,'P','K','G',0};
    out.insert(out.end(), magic, magic + 8);
    put16(1); put16((uint16_t)minor); put32(40); put64(fileSize); put64(manifest.size()); put32((uint32_t)payloads.size()); put32(0);
    out.insert(out.end(), manifest.begin(), manifest.end());
    uint64_t off = 40 + manifest.size() + 32ull * payloads.size();
    for (auto& p : payloads) {
        put32(0); put32(0); put64(off); put64(p.size()); put64(p.size());
        off += p.size();
    }
    for (auto& p : payloads) out.insert(out.end(), p.begin(), p.end());
    writeFile(path, out);
}


// ============================ EFF Format 3 加载 ============================
// EFF .effres kind=1 的 payload = 原生 Unity 输入 + EFFMESH v6 作者快照。
// EFFMESH 是完整几何（位置/法线/切线/UV/index/skin/bone paths），直接用于 BEM。
struct EffSkin { float w[4]; uint32_t b[4]; };
struct EffSub { int topology; uint32_t start, count, base, first, vcount; };
struct EffMesh {
    int version = 0;
    string coordinate, source, name;
    int vcount = 0, icount = 0;
    vector<float> pos, nrm, tan, col;
    vector<vector<float>> uv = vector<vector<float>>(8);
    vector<uint32_t> idx;
    vector<EffSub> subs;
    vector<EffSkin> skin;
    vector<string> bonePaths;
};

struct EffReader {
    const vector<uint8_t>& d; size_t p = 0;
    EffReader(const vector<uint8_t>& data) : d(data) {}
    void need(size_t n) { if (p + n > d.size()) throw runtime_error("truncated EFFMESH"); }
    uint8_t u8() { need(1); return d[p++]; }
    int32_t i32() { need(4); int32_t v; memcpy(&v, &d[p], 4); p += 4; return v; }
    uint32_t u32() { need(4); uint32_t v; memcpy(&v, &d[p], 4); p += 4; return v; }
    float f32() { need(4); float v; memcpy(&v, &d[p], 4); p += 4; return v; }
    string str() {
        uint64_t n = 0; int shift = 0;
        while (true) { uint8_t b = u8(); n |= uint64_t(b & 0x7f) << shift; if (!(b & 0x80)) break; shift += 7; if (shift > 28) throw runtime_error("bad EFFMESH string"); }
        need((size_t)n); string s((const char*)&d[p], (size_t)n); p += (size_t)n; return s;
    }
    vector<float> floats() {
        int n = i32(); if (n < 0) throw runtime_error("bad EFFMESH float count");
        need((size_t)n * 4); vector<float> v((size_t)n);
        if (n) memcpy(v.data(), &d[p], (size_t)n * 4); p += (size_t)n * 4; return v;
    }
    vector<uint32_t> u32s() {
        int n = i32(); if (n < 0) throw runtime_error("bad EFFMESH uint count");
        need((size_t)n * 4); vector<uint32_t> v((size_t)n);
        if (n) memcpy(v.data(), &d[p], (size_t)n * 4); p += (size_t)n * 4; return v;
    }
    void skip(size_t n) { need(n); p += n; }
};

static EffMesh decodeEffMeshData(const vector<uint8_t>& b) {
    EffReader r(b);
    const uint8_t magic[8] = {'E','F','F','M','E','S','H',0};
    r.need(8); if (memcmp(&b[0], magic, 8)) throw runtime_error("not EFFMESH");
    r.p = 8;
    EffMesh m; m.version = r.i32();
    m.coordinate = r.str(); m.source = r.str(); m.name = r.str();
    m.vcount = r.i32(); if (m.vcount < 0) throw runtime_error("bad EFFMESH vertex count");
    m.pos = r.floats(); m.nrm = r.floats(); m.tan = r.floats(); m.col = r.floats();
    for (int i = 0; i < 8; i++) m.uv[i] = r.floats();
    m.icount = r.i32(); if (m.icount < 0) throw runtime_error("bad EFFMESH index count");
    r.need((size_t)m.icount * 4); m.idx.resize((size_t)m.icount);
    if (m.icount) memcpy(m.idx.data(), &b[r.p], (size_t)m.icount * 4); r.p += (size_t)m.icount * 4;
    int subn = r.i32(); if (subn < 0) throw runtime_error("bad EFFMESH submesh count");
    for (int i = 0; i < subn; i++) { EffSub s; s.topology = r.i32(); s.start = r.u32(); s.count = r.u32(); s.base = r.u32(); s.first = r.u32(); s.vcount = r.u32(); m.subs.push_back(s); }
    int skn = r.i32(); if (skn < 0) throw runtime_error("bad EFFMESH skin count");
    for (int i = 0; i < skn; i++) { EffSkin s{}; for (float &w : s.w) w = r.f32(); for (uint32_t &x : s.b) x = r.u32(); m.skin.push_back(s); }
    int bindn = r.i32(); if (bindn < 0) throw runtime_error("bad EFFMESH bindpose count"); r.skip((size_t)bindn * 64);
    int hashn = r.i32(); if (hashn < 0) throw runtime_error("bad EFFMESH bone hash count"); r.skip((size_t)hashn * 4);
    int pathn = r.i32(); if (pathn < 0) throw runtime_error("bad EFFMESH bone path count");
    for (int i = 0; i < pathn; i++) m.bonePaths.push_back(r.str());
    if ((int)m.pos.size() < m.vcount * 3) throw runtime_error("EFFMESH position incomplete");
    if (!m.nrm.empty() && (int)m.nrm.size() < m.vcount * 3) throw runtime_error("EFFMESH normal incomplete");
    if (!m.tan.empty() && (int)m.tan.size() < m.vcount * 4) throw runtime_error("EFFMESH tangent incomplete");
    if (!m.uv[0].empty() && (int)m.uv[0].size() < m.vcount * 2) throw runtime_error("EFFMESH uv incomplete");
    return m;
}

static EffMesh readEffMesh(const string& path) {
    auto b = readFile(path);
    if (b.size() < 72 || memcmp(b.data(), "EFF_AUTHOR_FILE3", 16)) throw runtime_error("not EFF_AUTHOR_FILE3: " + path);
    if (rd32(b, 16) != 3) throw runtime_error("unsupported EFF version");
    if (rd32(b, 20) != 1) throw runtime_error("not a Mesh EFF resource: " + path);
    uint64_t meta = 0, pay = 0; memcpy(&meta, &b[24], 8); memcpy(&pay, &b[32], 8);
    if (72 + meta + pay != b.size()) throw runtime_error("EFF mesh extent differs: " + path);
    vector<uint8_t> payload(b.begin() + 72 + meta, b.end());
    const uint8_t magic[8] = {'E','F','F','M','E','S','H',0};
    auto it = std::search(payload.begin(), payload.end(), magic, magic + 8);
    if (it == payload.end()) throw runtime_error("EFFMESH snapshot not found in " + path);
    vector<uint8_t> author(it, payload.end());
    return decodeEffMeshData(author);
}

static string effKey(const Ini& ini, const string& sec, const string& key) {
    auto s = ini.sec.find(sec); if (s == ini.sec.end()) return "";
    auto i = s->second.find(key); if (i != s->second.end()) return i->second;
    for (auto& kv : s->second) if (kv.first.size() == key.size()) {
        bool eq = true; for (size_t j = 0; j < key.size(); j++) if (tolower((unsigned char)kv.first[j]) != tolower((unsigned char)key[j])) { eq = false; break; }
        if (eq) return kv.second;
    }
    return "";
}
static bool effSection(const Ini& ini, const string& sec, map<string,string>& out) {
    auto it = ini.sec.find(sec); if (it == ini.sec.end()) return false; out = it->second; return true;
}
static vector<string> effSplit(const string& s, char c) {
    vector<string> out; string cur;
    for (char ch : s) { if (ch == c) { if (!cur.empty()) out.push_back(cur); cur.clear(); } else cur += ch; }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static Cat loadCatalogByCharacter(const string& dir, const string& character);
static int applyIdentities(Cat& cat);
static string catalogTextureByDescriptor(cJSON* textures, int w, int h, int mips);

// 按 catalog 的 mesh_name 选择角色（EFF target.asset 就是 mesh_name）
static Cat loadCatalogByMeshes(const string& dir, const set<string>& meshes) {
    Cat best; int bestHit = -1;
    for (auto& p : fs::directory_iterator(dir)) {
        if (p.path().extension() != ".json") continue;
        auto txt = readFile(p.path().string());
        cJSON* root = cJSON_ParseWithLength((const char*)txt.data(), txt.size());
        if (!root) continue;
        auto kind = get(root, "kind");
        if (!cJSON_IsString(kind) || string(kind->valuestring) != "bem-character-catalog") { cJSON_Delete(root); continue; }
        auto comps = get(root, "components"); int hit = 0;
        if (cJSON_IsObject(comps)) for (auto* c = comps->child; c; c = c->next) {
            auto mn = get(c, "mesh_name"); if (cJSON_IsString(mn) && meshes.count(mn->valuestring)) hit++;
        }
        if (hit > bestHit) {
            if (best.root) cJSON_Delete(best.root);
            best.root = root; bestHit = hit; best.character = jstr(root, "character_id");
            best.comps = comps; best.entries = get(root, "entries"); best.textures = get(root, "textures");
        } else cJSON_Delete(root);
    }
    if (!best.root || bestHit <= 0) throw runtime_error("no BEN catalog character matched EFF targets");
    best.ids.clear();
    for (auto* c = best.comps->child; c; c = c->next) best.ids.push_back(stoi(string(c->string)));
    sort(best.ids.begin(), best.ids.end());
    best.base.resize(best.ids.size()); int acc = 0;
    for (size_t i = 0; i < best.ids.size(); i++) {
        best.base[i] = acc; acc += cJSON_GetArraySize(get(cJSON_GetObjectItemCaseSensitive(best.comps, to_string(best.ids[i]).c_str()), "bone_names"));
    }
    best.totalBones = acc;
    return best;
}

static int effCid(Cat& cat, const string& meshName) {
    for (int cid : cat.ids) {
        auto* c = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(cid).c_str());
        if (jstr(c, "mesh_name") == meshName) return cid;
    }
    return -1;
}
static bool effFindBone(Cat& cat, const string& path, int prefer, int& ocid, int& oidx) {
    // EFF 的辅助骨骼（飘带/裙摆）未必出现在 BEN 目标组件里；从叶子往上找最近祖先，
    // 保证 cloth 能跟随父骨骼动，而不是整块部件被丢掉。
    auto tryComp = [&](int cid, const string& name) -> bool {
        auto* c = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(cid).c_str());
        auto* bn = get(c, "bone_names"); int n = cJSON_GetArraySize(bn);
        for (int i = 0; i < n; i++) { auto* x = cJSON_GetArrayItem(bn, i); if (cJSON_IsString(x) && name == x->valuestring) { ocid = cid; oidx = i; return true; } }
        return false;
    };
    vector<string> segs = effSplit(path, '/');
    for (int i = (int)segs.size() - 1; i >= 0; i--) {
        if (prefer >= 0 && tryComp(prefer, segs[i])) return true;
        for (int cid : cat.ids) if (cid != prefer && tryComp(cid, segs[i])) return true;
    }
    return false;
}
static bool effCatalogTexture(Cat& cat, const string& name, string& original) {
    if (!cJSON_IsObject(cat.textures) || name.empty()) return false;
    const string wanted = lower(name);
    for (auto* t = cat.textures->child; t; t = t->next) {
        auto* n = get(t, "name");
        if (cJSON_IsString(n) && lower(n->valuestring) == wanted) { original = n->valuestring; return true; }
    }
    return false;
}

struct EffMatData {
    bool overrides = false;
    map<string,double> floats;
    map<string,array<double,4>> colors;
    vector<string> enableKeywords, disableKeywords;
    map<string,bool> shaderPasses;
    int renderQueue = -1;
};

static bool validMatProp(const string& name) {
    if (name.size() < 2 || name.size() > 96 || name[0] != '_') return false;
    for (char c : name) if (!(isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}
static bool validKeyword(const string& name) {
    if (name.size() < 2 || name.size() > 96) return false;
    for (char c : name) if (!(isupper((unsigned char)c) || isdigit((unsigned char)c) || c == '_')) return false;
    return true;
}
static double parseDouble(const string& v) {
    char* end = nullptr; double d = strtod(v.c_str(), &end);
    if (end == v.c_str()) throw runtime_error("bad number: " + v);
    return d;
}
// EFF material resource (kind=2) payload is a small line format:
//   format=EFFMAT / float._P=v / value4._C=r,g,b,a / keyword._K=1|0 / render_queue=N
// The trailing ";EFF_MATERIAL_IMPORT={...}" comment is ignored.
static EffMatData parseEffMaterial(const string& path) {
    auto b = readFile(path);
    if (b.size() < 72 || memcmp(b.data(), "EFF_AUTHOR_FILE3", 16)) throw runtime_error("not EFF material: " + path);
    if (rd32(b, 20) != 2) throw runtime_error("not a material resource: " + path);
    size_t off = 72 + rd32(b, 24);
    if (off >= b.size()) throw runtime_error("bad material payload: " + path);
    EffMatData d;
    string text((const char*)b.data() + off, b.size() - off);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        string line = text.substr(pos, nl == string::npos ? string::npos : nl - pos);
        pos = nl == string::npos ? text.size() : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\0')) line.pop_back();
        if (line.empty()) continue;
        if (line[0] == ';') break;
        if (line.rfind("overrides=", 0) == 0) { d.overrides = line.find("true") != string::npos; continue; }
        if (line.rfind("render_queue=", 0) == 0) { d.renderQueue = atoi(line.c_str() + 13); continue; }
        if (line.rfind("float.", 0) == 0) {
            auto eq = line.find('='); if (eq == string::npos) continue;
            string name = line.substr(6, eq - 6);
            if (validMatProp(name)) d.floats[name] = parseDouble(line.substr(eq + 1));
            continue;
        }
        if (line.rfind("value4.", 0) == 0) {
            auto eq = line.find('='); if (eq == string::npos) continue;
            string name = line.substr(7, eq - 7);
            if (!validMatProp(name)) continue;
            auto parts = effSplit(line.substr(eq + 1), ',');
            if (parts.size() != 4) continue;
            array<double,4> c{}; bool ok = true;
            for (int i = 0; i < 4; i++) { c[i] = parseDouble(parts[i]); if (c[i] < 0 || c[i] > 8) ok = false; }
            if (ok) d.colors[name] = c;
            continue;
        }
        if (line.rfind("keyword.", 0) == 0) {
            auto eq = line.find('='); if (eq == string::npos) continue;
            string name = line.substr(8, eq - 8);
            if (!validKeyword(name)) continue;
            (parseDouble(line.substr(eq + 1)) > 0.5 ? d.enableKeywords : d.disableKeywords).push_back(name);
        }
    }
    auto marker = text.find(";EFF_MATERIAL_IMPORT=");
    if (marker != string::npos) {
        const string js = text.substr(marker + strlen(";EFF_MATERIAL_IMPORT="));
        cJSON* root = cJSON_ParseWithLength(js.data(), js.size());
        if (root) {
            auto* value = get(root, "value");
            if (cJSON_IsObject(value)) {
                auto* passes = get(value, "disabledShaderPasses");
                if (cJSON_IsArray(passes)) for (auto* pass = passes->child; pass; pass = pass->next) {
                    if (!cJSON_IsString(pass)) continue;
                    string name = pass->valuestring;
                    if (name.empty() || name.size() > 96 || name.front() == ' ' || name.back() == ' ') continue;
                    bool printable = true; for (unsigned char c : name) if (c < 0x20 || c > 0x7e) printable = false;
                    if (printable) d.shaderPasses[name] = false;
                }
                auto addKeywords = [&](const char* field, bool enabled) {
                    auto* arr = get(value, field);
                    if (!cJSON_IsArray(arr)) return;
                    for (auto* kw = arr->child; kw; kw = kw->next) {
                        if (!cJSON_IsString(kw) || !validKeyword(kw->valuestring)) continue;
                        string name = kw->valuestring;
                        auto& list = enabled ? d.enableKeywords : d.disableKeywords;
                        if (find(list.begin(), list.end(), name) == list.end()) list.push_back(name);
                    }
                };
                addKeywords("m_ValidKeywords", true);
                addKeywords("m_InvalidKeywords", false);
            }
            cJSON_Delete(root);
        }
    }
    return d;
}

static int convertEff(const string& dir, const string& catDir, const string& out) {
    auto ini = Ini::parse(dir + "/mod.ini");
    auto format = effKey(ini, "Mod", "format");
    if (format != "3") throw runtime_error("not an EFF format-3 package (format=" + format + ")");

    // resources
    struct EffTex { string dds, asset; bool srgb = false; };
    struct EffMat { string asset; vector<pair<string,string>> slots; };
    map<string, EffTex> tex;
    map<string, EffMat> mat;
    map<string, EffMatData> matData;
    map<string, string> meshPath;
    map<string, string> partMesh, partMat;
    vector<string> replaceKeys;
    set<string> targetMeshes;
    for (auto& s : ini.order) {
        map<string,string> d; effSection(ini, s, d);
        if (s.rfind("Texture", 0) == 0 && s.rfind("TextureOverride", 0) != 0) {
            auto dds = effKey(ini, s, "dds"), asset = effKey(ini, s, "source.asset");
            bool srgbHint = false, srgbKnown = false;
            if (asset.empty()) {
                // Most EFF packages declare only source.path; the catalog key is the
                // basename without extension (e.g. .../t_actor_laevat_body_01_d.tga).
                auto path = effKey(ini, s, "source.path");
                if (!path.empty()) {
                    for (auto& ch : path) if (ch == '\\') ch = '/';
                    auto slash = path.find_last_of('/');
                    asset = slash == string::npos ? path : path.substr(slash + 1);
                    auto dot = asset.find_last_of('.');
                    if (dot != string::npos) asset.resize(dot);
                }
            }
            if (asset.empty()) {
                // EFF3 另一种写法：身份直接写在段名里
                // [Texture<asset>_<12位hash>_srgb|linear]
                string sec = s.substr(7); // 去掉 "Texture"
                for (const char* sfx : {"_srgb", "_linear"}) {
                    const size_t n = strlen(sfx);
                    if (sec.size() > n && sec.compare(sec.size() - n, n, sfx) == 0) {
                        srgbHint = (string(sfx) == "_srgb");
                        srgbKnown = true;
                        sec.resize(sec.size() - n);
                        break;
                    }
                }
                const size_t us = sec.find_last_of('_');
                if (us != string::npos && us > 0 && sec.size() - us - 1 == 12 &&
                    sec.find_first_not_of("0123456789abcdefABCDEF", us + 1) == string::npos) {
                    sec.resize(us);
                    if (!sec.empty()) asset = sec;
                }
            }
            if (!dds.empty()) tex[s] = {dds, asset, srgbKnown ? srgbHint : false};
        } else if (s.rfind("Material", 0) == 0 && s.rfind("MaterialTrack", 0) != 0) {
            EffMat m; m.asset = effKey(ini, s, "source.asset");
            for (auto& kv : d) if (kv.first.rfind("texture.", 0) == 0) m.slots.push_back({kv.first, kv.second});
            if (!m.asset.empty()) mat[s] = m;
            auto resPath = effKey(ini, s, "path");
            if (!resPath.empty()) {
                try { matData[s] = parseEffMaterial(dir + "/" + resPath); }
                catch (exception& e) { cerr << "material skip " << s << ": " << e.what() << "\n"; }
            }
        } else if (s.rfind("Mesh", 0) == 0) {
            auto p = effKey(ini, s, "path"); if (!p.empty()) meshPath[s] = p;
        } else if (s.rfind("Part", 0) == 0) {
            partMesh[s] = effKey(ini, s, "mesh"); partMat[s] = effKey(ini, s, "material");
        } else if (s.rfind("Replace", 0) == 0) {
            replaceKeys.push_back(s);
            auto ta = effKey(ini, s, "target.asset");
            if (!ta.empty()) targetMeshes.insert(ta);
        }
    }
    Cat cat = g_forceCharacter.empty() ? loadCatalogByMeshes(catDir, targetMeshes)
                                       : loadCatalogByCharacter(catDir, g_forceCharacter);
    applyIdentities(cat);
    cout << "target: " << cat.character << " (" << jstr(cat.root, "name") << "), target meshes " << targetMeshes.size() << "\n";

    vector<vector<uint8_t>> payloads;
    auto addPayload = [&](vector<uint8_t> b) { payloads.push_back(std::move(b)); return (int)payloads.size() - 1; };
    cJSON* jtextures = jarr();
    map<string,int> texIndex; // original_name -> textures[] index
    auto addTexture = [&](const string& asset, const string& ddsPath, bool srgbOverride = false) -> int {
        auto it = texIndex.find(asset); if (it != texIndex.end()) return it->second;
        Dds d;
        try { d = readTexture(ddsPath, asset); } catch (exception& e) { cerr << "texture skip " << asset << ": " << e.what() << "\n"; return -1; }
        if (srgbOverride) d.srgb = true; // EFF3 段名 _srgb 后缀是权威来源
        string original;
        if (!effCatalogTexture(cat, asset, original)) {
            if (!g_textureFingerprint) { cerr << "texture skip " << asset << ": no catalog identity\n"; return -1; }
            original = catalogTextureByDescriptor(cat.textures, d.w, d.h, d.mips);
            if (original.empty()) { cerr << "texture skip " << asset << ": no unique descriptor match\n"; return -1; }
            cout << "texture fingerprint: " << asset << " -> " << original << "\n";
        }
        int pid = addPayload(d.data);
        cJSON* t = obj(nullptr);
        cJSON_AddNumberToObject(t, "width", d.w); cJSON_AddNumberToObject(t, "height", d.h);
        cJSON_AddNumberToObject(t, "mips", d.mips); cJSON_AddNumberToObject(t, "format", d.fmt);
        cJSON_AddBoolToObject(t, "srgb", d.srgb); cJSON_AddStringToObject(t, "original_name", original.c_str());
        cJSON_AddNumberToObject(t, "payload", pid); jadd(jtextures, t);
        int idx = cJSON_GetArraySize(jtextures) - 1; texIndex[asset] = idx; return idx;
    };

    cJSON* jmeshes = jarr();
    map<int,int> meshIndex;
    bool hasMatOverrides = false;
    bool hasRenderState = false;
    map<int,int> cidToMesh;
    // 先决定目标/部件，再逐个目标合并
    set<int> hiddenTargets;
    set<int> keptTargets;
    for (auto& rkey : replaceKeys) {
        const string handling = effKey(ini, rkey, "handling");
        string targetAsset = effKey(ini, rkey, "target.asset");
        int cid = effCid(cat, targetAsset);
        if (handling == "hide" || handling == "keep") {
            if (cid < 0) { cout << "skip " << handling << " " << rkey << ": no catalog component for " << targetAsset << "\n"; continue; }
            if (handling == "hide") hiddenTargets.insert(cid); else keptTargets.insert(cid);
            continue;
        }
        if (handling == "skip") continue;
        if (cid < 0) { cout << "skip replace " << rkey << ": no catalog component for " << targetAsset << "\n"; continue; }
        auto parts = effSplit(effKey(ini, rkey, "parts"), ',');
        if (parts.empty()) { cout << "skip replace " << rkey << ": no parts\n"; continue; }

        // Pre-scan parts: the merged BEM mesh needs one attribute layout, so use
        // the union of authored UV sets (UV0..UV7) and colour presence.
        map<string,EffMesh> meshCache;
        set<int> uvSetIndices;
        bool anyColor = false;
        for (auto& pname : parts) {
            auto meshSec = partMesh.count(pname) ? partMesh[pname] : "";
            if (meshSec.empty() || !meshPath.count(meshSec)) continue;
            try {
                if (!meshCache.count(meshSec)) meshCache[meshSec] = readEffMesh(dir + "/" + meshPath[meshSec]);
                const EffMesh& m = meshCache[meshSec];
                for (int uv = 0; uv < 8; uv++) if ((int)m.uv[uv].size() >= m.vcount * 2 && m.vcount > 0) uvSetIndices.insert(uv);
                if ((int)m.col.size() >= m.vcount * 4 && m.vcount > 0) anyColor = true;
            } catch (exception&) {}
        }
        if (uvSetIndices.empty()) uvSetIndices.insert(0);
        if (!uvSetIndices.count(0)) uvSetIndices.insert(0);
        vector<int> uvList(uvSetIndices.begin(), uvSetIndices.end());
        const int s1stride = (int)uvList.size() * 8 + (anyColor ? 16 : 0);
        vector<uint8_t> s0, s1, s2; vector<uint32_t> indices;
        vector<cJSON*> bones; map<pair<int,int>,int> pal; bool ok = true; int totalIdx = 0;
        cJSON* jdraws = jarr();
        for (auto& pname : parts) {
            auto meshSec = partMesh.count(pname) ? partMesh[pname] : "";
            if (meshSec.empty() || !meshPath.count(meshSec)) { cout << "skip part " << pname << ": no mesh path\n"; ok = false; break; }
            EffMesh m;
            try { m = meshCache.count(meshSec) ? meshCache[meshSec] : readEffMesh(dir + "/" + meshPath[meshSec]); }
            catch (exception& e) { cout << "skip part " << pname << ": " << e.what() << "\n"; ok = false; break; }
            if (m.vcount <= 0 || m.idx.empty()) { cout << "skip part " << pname << ": empty mesh\n"; ok = false; break; }
            // 材质/贴图
            auto mname = partMat.count(pname) ? partMat[pname] : "";
            EffMat em; if (mat.count(mname)) em = mat[mname];
            auto* comp = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(cid).c_str());
            auto* mats = get(comp, "materials"); int slot = 0; string materialName = jstr(comp, "materials") ;
            auto* m0 = cJSON_GetArrayItem(mats, 0); if (m0) materialName = m0->valuestring;
            for (int i = 0; i < cJSON_GetArraySize(mats); i++) { auto* x = cJSON_GetArrayItem(mats, i); if (cJSON_IsString(x) && em.asset == x->valuestring) { slot = i; materialName = x->valuestring; break; } }
            cJSON* jtexIds = jarr(); set<int> seenTex;
            for (auto& [prop, ts] : em.slots) {
                auto it = tex.find(ts); if (it == tex.end()) continue;
                auto tpath = it->second.dds; if (tpath.empty()) continue;
                int ti = addTexture(it->second.asset, dir + "/" + tpath, it->second.srgb);
                if (ti >= 0 && !seenTex.count(ti)) { seenTex.insert(ti); jadd(jtexIds, jnum(ti)); }
            }
            // 骨骼 palette
            vector<int> local(m.bonePaths.size(), -1);
            if (m.bonePaths.empty()) { cout << "skip part " << pname << ": no bones\n"; ok = false; break; }
            for (size_t bi = 0; bi < m.bonePaths.size(); bi++) {
                int ocid, oidx;
                if (!effFindBone(cat, m.bonePaths[bi], cid, ocid, oidx)) { cout << "skip part " << pname << ": bone not in catalog: " << m.bonePaths[bi] << "\n"; ok = false; break; }
                auto key = make_pair(ocid, oidx);
                auto it = pal.find(key);
                if (it == pal.end()) {
                    int pi = (int)pal.size(); if (pi > 255) { ok = false; break; }
                    pal[key] = pi; local[bi] = pi;
                    auto* oc = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(ocid).c_str());
                    auto* bn = get(oc, "bone_names"); auto* nm = cJSON_GetArrayItem(bn, oidx);
                    cJSON* b = obj(nullptr); cJSON_AddNumberToObject(b, "component", ocid); cJSON_AddNumberToObject(b, "index", oidx); cJSON_AddStringToObject(b, "name", nm->valuestring);
                    bones.push_back(b);
                } else local[bi] = it->second;
            }
            if (!ok) break;
            size_t baseVert = s0.size() / 40;
            for (int v = 0; v < m.vcount; v++) {
                auto pushf = [&](vector<uint8_t>& dst, float x) { uint8_t b[4]; memcpy(b, &x, 4); dst.insert(dst.end(), b, b + 4); };
                // stream0: position, normal, tangent
                for (int k = 0; k < 3; k++) pushf(s0, m.pos[v*3+k]);
                for (int k = 0; k < 3; k++) pushf(s0, m.nrm.empty() ? 0.f : m.nrm[v*3+k]);
                for (int k = 0; k < 4; k++) pushf(s0, m.tan.empty() ? (k == 3 ? 1.f : 0.f) : m.tan[v*4+k]);
                // stream1: all authored UV sets (union) + colour when present
                for (int uv : uvList) {
                    const bool has = (int)m.uv[uv].size() >= m.vcount * 2;
                    for (int k = 0; k < 2; k++) pushf(s1, has ? m.uv[uv][v*2+k] : 0.f);
                }
                if (anyColor) {
                    const bool has = (int)m.col.size() >= m.vcount * 4;
                    for (int k = 0; k < 4; k++) pushf(s1, has ? m.col[v*4+k] : (k == 3 ? 1.f : 0.f));
                }
                // stream2: u16 weights + u8 palette
                float w[4] = {1,0,0,0}; uint32_t gb[4] = {0,0,0,0};
                if (v < (int)m.skin.size()) { for (int k = 0; k < 4; k++) { w[k] = m.skin[v].w[k]; gb[k] = m.skin[v].b[k]; } }
                for (int k = 0; k < 4; k++) {
                    float f = std::max(0.f, std::min(1.f, w[k])); uint16_t u = (uint16_t)std::lround(f * 65535.f);
                    s2.push_back(u & 255); s2.push_back(u >> 8);
                }
                for (int k = 0; k < 4; k++) {
                    int pi = 0;
                    if (gb[k] < local.size() && local[gb[k]] >= 0 && w[k] > 0.f) pi = local[gb[k]];
                    s2.push_back((uint8_t)pi);
                }
            }
            size_t baseIdx = indices.size();
            for (uint32_t x : m.idx) indices.push_back(x + (uint32_t)baseVert);
            cJSON* jd = obj(nullptr);
            cJSON_AddNumberToObject(jd, "start", (int)baseIdx); cJSON_AddNumberToObject(jd, "count", (int)m.idx.size());
            cJSON_AddNumberToObject(jd, "material_component", cid); cJSON_AddNumberToObject(jd, "material_slot", slot);
            cJSON_AddStringToObject(jd, "material_name", materialName.c_str()); cJSON_AddItemToObject(jd, "textures", jtexIds);
            auto mdIt = matData.find(mname);
            if (mdIt != matData.end()) {
                auto& md = mdIt->second;
                cJSON* mo = obj(nullptr); bool any = false;
                if (!md.floats.empty()) {
                    cJSON* fl = obj(nullptr);
                    for (auto& kv : md.floats) {
                        if (kv.second < -128.0 || kv.second > 128.0) {
                            cerr << "material skip float out of range: " << kv.first << "=" << kv.second << "\n";
                            continue;
                        }
                        cJSON_AddNumberToObject(fl, kv.first.c_str(), kv.second);
                    }
                    if (fl->child) { cJSON_AddItemToObject(mo, "floats", fl); any = true; } else cJSON_Delete(fl);
                }
                if (!md.colors.empty()) {
                    cJSON* cl = obj(nullptr);
                    for (auto& kv : md.colors) {
                        cJSON* v = jarr();
                        for (double x : kv.second) jadd(v, cJSON_CreateNumber(x));
                        cJSON_AddItemToObject(cl, kv.first.c_str(), v);
                    }
                    if (cl->child) { cJSON_AddItemToObject(mo, "colors", cl); any = true; } else cJSON_Delete(cl);
                }
                if (!md.enableKeywords.empty()) {
                    cJSON* arr = jarr(); for (auto& k : md.enableKeywords) jadd(arr, jstrv(k));
                    cJSON_AddItemToObject(mo, "enable_keywords", arr); any = true;
                }
                if (!md.disableKeywords.empty()) {
                    cJSON* arr = jarr(); for (auto& k : md.disableKeywords) jadd(arr, jstrv(k));
                    cJSON_AddItemToObject(mo, "disable_keywords", arr); any = true;
                }
                if (!md.shaderPasses.empty()) {
                    cJSON* sp = obj(nullptr);
                    for (auto& kv : md.shaderPasses) cJSON_AddBoolToObject(sp, kv.first.c_str(), kv.second);
                    cJSON_AddItemToObject(mo, "shader_passes", sp);
                    any = true; hasRenderState = true;
                }
                if (md.renderQueue >= -1 && md.renderQueue <= 5000) {
                    cJSON_AddNumberToObject(mo, "render_queue", md.renderQueue);
                    any = true;
                    hasRenderState = true;
                }
                if (any) { cJSON_AddItemToObject(jd, "material_overrides", mo); hasMatOverrides = true; }
                else cJSON_Delete(mo);
            }
            jadd(jdraws, jd); totalIdx += (int)m.idx.size();
            cout << "part " << pname << ": v=" << m.vcount << " idx=" << m.idx.size() << " bonePalette=" << pal.size() << " tex=" << cJSON_GetArraySize(jtexIds) << "\n";
        }
        if (!ok || totalIdx == 0) { cout << "skip target " << targetAsset << ": incomplete\n"; continue; }

        // BEM 1.0 skin 要求 12 字节：UNorm16 x4 + UInt8 x4
        cJSON* jattrs = jarr();
        auto attr = [&](int sem, int fmt, int dim, int stream, int off) { cJSON* a = jarr(); jadd(a,jnum(sem)); jadd(a,jnum(fmt)); jadd(a,jnum(dim)); jadd(a,jnum(stream)); jadd(a,jnum(off)); jadd(jattrs,a); };
        attr(0,0,3,0,0); attr(1,0,3,0,12); attr(2,0,4,0,24);
        // stream1 layout follows the UV-set union; every attribute is tightly packed.
        int uvOffset = 0;
        for (int uv : uvList) { attr(4 + uv, 0, 2, 1, uvOffset); uvOffset += 8; }
        if (anyColor) attr(3,0,4,1,uvOffset);
        attr(12,4,4,2,0); attr(13,6,4,2,8);
        cJSON* jstreams = jarr();
        auto stream = [&](int stride, int payload) { cJSON* x = obj(nullptr); cJSON_AddNumberToObject(x,"stride",stride); cJSON_AddNumberToObject(x,"payload",payload); jadd(jstreams,x); };
        int p0 = addPayload(s0), p1 = addPayload(s1), p2 = addPayload(s2);
        vector<uint8_t> ib(indices.size()*4); if (!indices.empty()) memcpy(ib.data(), indices.data(), ib.size());
        int pib = addPayload(ib);
        stream(40,p0); stream(s1stride,p1); stream(12,p2);
        cJSON* jmesh = obj(nullptr);
        cJSON_AddNumberToObject(jmesh,"vertex_count",(int)(s0.size()/40));
        cJSON_AddNumberToObject(jmesh,"index_size",4);
        cJSON_AddItemToObject(jmesh,"streams",jstreams);
        cJSON_AddItemToObject(jmesh,"attributes",jattrs);
        cJSON* jbones = jarr(); for (auto* b : bones) jadd(jbones,b); cJSON_AddItemToObject(jmesh,"bones",jbones);
        cJSON_AddNumberToObject(jmesh,"indices",pib); cJSON_AddNumberToObject(jmesh,"index_count",totalIdx);
        cJSON_AddItemToObject(jmesh,"draws",jdraws);
        cidToMesh[cid] = cJSON_GetArraySize(jmeshes); jadd(jmeshes,jmesh);
    }

    // target + appearance
    cJSON* jtarget = obj(nullptr);
    cJSON_AddStringToObject(jtarget,"character_id",cat.character.c_str());
    cJSON_AddStringToObject(jtarget,"platform","windows-x64");
    cJSON_AddStringToObject(jtarget,"profile_id",jstr(cat.root,"profile_id").c_str());
    cJSON_AddStringToObject(jtarget,"revision",jstr(cat.root,"revision").c_str());
    cJSON_AddStringToObject(jtarget,"snapshot","");
    cJSON_AddStringToObject(jtarget,"world_resource",jstr(cat.root,"world_resource").c_str());
    cJSON_AddStringToObject(jtarget,"ui_resource",jstr(cat.root,"ui_resource").c_str());
    cJSON* jcomps = jarr();
    for (int cid : cat.ids) {
        auto* c = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(cid).c_str());
        cJSON* x = obj(nullptr); cJSON_AddNumberToObject(x,"id",cid);
        cJSON_AddStringToObject(x,"mesh_name",jstr(c,"mesh_name").c_str());
        cJSON_AddNumberToObject(x,"original_index_count",jint(c,"original_index_count"));
        cJSON* bn = jarr(); for (auto* b = get(c,"bone_names")->child; b; b = b->next) jadd(bn,jstrv(b->valuestring)); cJSON_AddItemToObject(x,"bone_names",bn);
        cJSON* mm = jarr(); for (auto* m = get(c,"materials")->child; m; m = m->next) jadd(mm,jstrv(m->valuestring)); cJSON_AddItemToObject(x,"materials",mm);
        jadd(jcomps,x);
    }
    cJSON_AddItemToObject(jtarget,"components",jcomps);
    cJSON* japps = jarr(); cJSON* japp = obj(nullptr);
    cJSON_AddStringToObject(japp,"id","default"); cJSON_AddStringToObject(japp,"name","默认外观");
    cJSON* jac = jarr();
    for (int cid : cat.ids) {
        cJSON* x = obj(nullptr); cJSON_AddNumberToObject(x,"target",cid);
        auto it = cidToMesh.find(cid);
        if (it != cidToMesh.end()) { cJSON_AddStringToObject(x,"operation","replace"); cJSON_AddNumberToObject(x,"mesh",it->second); }
        else if (hiddenTargets.count(cid)) cJSON_AddStringToObject(x,"operation","hide");
        else cJSON_AddStringToObject(x,"operation","keep");
        jadd(jac,x);
    }
    cJSON_AddItemToObject(japp,"components",jac); jadd(japps,japp);

    cJSON* mroot = obj(nullptr);
    cJSON_AddNumberToObject(mroot,"schema",1);
    cJSON_AddStringToObject(mroot,"package_id",("c4ben."+cat.character).c_str());
    cJSON_AddStringToObject(mroot,"name",("Converter4BENeXt EFF: "+fs::path(dir).filename().string()).c_str());
    cJSON_AddStringToObject(mroot,"author","Converter4BENeXt");
    cJSON_AddStringToObject(mroot,"version","0.1.0");
    cJSON* caps = jarr(); for (auto s : {"native-materials","palette-u8","indices-u32"}) jadd(caps,jstrv(s));
    // minor>=1 的包必须是「组合选项」形态：需要 option_groups + component_rules，
    // 且 draw 自带 indices/count；否则 bem_v1 会报 BEM version/manifest mismatch。
    // 1.5 的 material_overrides 需要完整的 1.1 结构（option_groups/component_rules/
    // draw 级 indices）。EFF 的 draw 切分尚未对齐 validator，先降级为合法的 BEM 1.0，
    // 并明确告知 overrides 未写入——不产出会被拒绝的包。
    const bool asV11 = false;
    if (hasMatOverrides) {
        cout << "warning: material_overrides 需要 BEM 1.5 结构，当前降级为 1.0 并丢弃该字段\n";
        for (auto* mesh = jmeshes->child; mesh; mesh = mesh->next)
            for (auto* d = (get(mesh, "draws") ? get(mesh, "draws")->child : nullptr); d; d = d->next)
                cJSON_DeleteItemFromObject(d, "material_overrides");
        hasMatOverrides = false;
        hasRenderState = false;
    }
    if (asV11) {
        jadd(caps, jstrv("composable-options"));
        for (auto* mesh = jmeshes->child; mesh; mesh = mesh->next) {
            auto* draws = get(mesh, "draws");
            auto* idx = get(mesh, "indices");
            if (!cJSON_IsArray(draws) || !idx) continue;
            const int basePayload = idx->valueint;
            if (basePayload < 0 || basePayload >= (int)payloads.size()) continue;
            const vector<uint8_t> ibytes = payloads[basePayload];
            for (auto* d = draws->child; d; d = d->next) {
                int start = jint(d, "start");
                int count = jint(d, "count");
                if (count <= 0) count = jint(d, "index_count");
                if (start < 0) start = 0;
                const size_t off = std::min<size_t>((size_t)start * 2, ibytes.size());
                const size_t len = std::min<size_t>((size_t)count * 2, ibytes.size() - off);
                vector<uint8_t> slice(ibytes.begin() + off, ibytes.begin() + off + len);
                const int pid = addPayload(std::move(slice));
                cJSON_DeleteItemFromObject(d, "start");
                cJSON_DeleteItemFromObject(d, "count");
                cJSON_DeleteItemFromObject(d, "indices");
                cJSON_AddNumberToObject(d, "indices", pid);
                cJSON_AddNumberToObject(d, "count", (int)(len / 2));
            }
            cJSON_DeleteItemFromObject(mesh, "indices");
            cJSON_DeleteItemFromObject(mesh, "index_count");
        }
    } else {
        jadd(caps, jstrv("fixed-appearances"));
    }
    if (hasMatOverrides) jadd(caps,jstrv("material-overrides-v1"));
    if (hasRenderState) jadd(caps,jstrv("material-render-state-v1"));
    cJSON_AddItemToObject(mroot,"required_capabilities",caps);
    cJSON_AddItemToObject(mroot,"target",jtarget);
    requireNonEmptyOutput(jmeshes, jtextures, out);
    cJSON_AddItemToObject(mroot,"meshes",jmeshes);
    cJSON_AddItemToObject(mroot,"textures",jtextures);
    if (asV11) {
        cJSON_AddItemToObject(mroot, "option_groups", jarr()); // 1.5 允许空数组
        cJSON* jrules = jarr();
        for (size_t ti = 0; ti < cat.ids.size(); ti++) {
            const int cid = cat.ids[ti];
            cJSON* rule = obj(nullptr);
            cJSON_AddNumberToObject(rule, "target", (int)ti);
            cJSON* cands = jarr();
            cJSON* c = obj(nullptr);
            auto mi = cidToMesh.find(cid);
            if (mi != cidToMesh.end()) {
                cJSON_AddStringToObject(c, "operation", "replace");
                cJSON_AddNumberToObject(c, "mesh", mi->second);
            } else if (hiddenTargets.count(cid)) {
                cJSON_AddStringToObject(c, "operation", "hide");
            } else {
                cJSON_AddStringToObject(c, "operation", "keep");
            }
            jadd(cands, c); cJSON_AddItemToObject(rule, "candidates", cands);
            jadd(jrules, rule);
        }
        cJSON_AddItemToObject(mroot, "component_rules", jrules);
    } else {
        cJSON_AddStringToObject(mroot,"default_appearance_id","default");
        cJSON_AddItemToObject(mroot,"appearances",japps);
    }
    char* json = cJSON_PrintUnformatted(mroot); if (!json) throw runtime_error("manifest serialize failed");
    vector<uint8_t> manifest((uint8_t*)json, (uint8_t*)json + strlen(json)); free(json);
    // 与 EFMI 路径一致：输出名以 .zip 结尾时写 BEM 原生 ZIP 容器，否则写二进制容器。
    if (out.size() >= 4 && string(out).substr(string(out).size() - 4) == ".zip") {
        vector<Entry> es; es.push_back({"manifest.json", manifest, 0, 0});
        for (size_t i = 0; i < payloads.size(); i++) {
            char nm[64]; snprintf(nm, sizeof nm, "payloads/%04zu.bin", i);
            es.push_back({nm, payloads[i], 0, 0});
        }
        writeZip(out, es);
    } else {
        writeBem(out, manifest, payloads, hasMatOverrides ? 5 : 0);
    }
    cout << "written " << out << ": meshes=" << cJSON_GetArraySize(jmeshes) << " textures=" << cJSON_GetArraySize(jtextures) << " payloads=" << payloads.size() << "\n";
    cJSON_Delete(mroot); cJSON_Delete(cat.root);
    return 0;
}

static int effiComponentMid(const string& s) {
    const char* a = "TextureOverride_EntryPoint_Component";
    const char* b = "TextureOverride_Component";
    if (s.rfind(a, 0) == 0) return atoi(s.c_str() + strlen(a));
    if (s.rfind(b, 0) == 0) return atoi(s.c_str() + strlen(b));
    return -1;
}


// ============================ 旧 EFMI 兼容层 ============================
// 不依赖旧 catalog：把旧包拆成“组件指纹”，和当前角色 catalog 打分，输出可审计的 mapping 草稿。
struct LegacyComp {
    int id = -1;
    string hash;
    int match_index_count = 0;
    int index_count = 0, vertex_count = 0;
    int vb0_stride = 0, vb1_stride = 0, vb2_stride = 0;
    int bone_max = -1, bone_used = 0;
    bool bones_known = false; // 只有 stride12/旧 alpha 的 u8 本地索引能直接数出骨骼数
    vector<pair<int,double>> candidates; // target cid, score
};

static Cat loadCatalogByCharacter(const string& dir, const string& character) {
    Cat best;
    for (auto& p : fs::directory_iterator(dir)) {
        if (p.path().extension() != ".json") continue;
        auto txt = readFile(p.path().string());
        cJSON* root = cJSON_ParseWithLength((const char*)txt.data(), txt.size());
        if (!root) continue;
        auto kind = get(root, "kind");
        if (!cJSON_IsString(kind) || string(kind->valuestring) != "bem-character-catalog") { cJSON_Delete(root); continue; }
        if (jstr(root, "character_id") == character) { best.root = root; break; }
        cJSON_Delete(root);
    }
    if (!best.root) throw runtime_error("catalog character not found: " + character);
    best.character = character; best.comps = get(best.root, "components");
    best.entries = get(best.root, "entries"); best.textures = get(best.root, "textures");
    best.ids.clear();
    for (auto* c = best.comps->child; c; c = c->next) best.ids.push_back(stoi(string(c->string)));
    sort(best.ids.begin(), best.ids.end());
    best.base.resize(best.ids.size()); int acc = 0;
    for (size_t i = 0; i < best.ids.size(); i++) {
        best.base[i] = acc;
        acc += cJSON_GetArraySize(get(cJSON_GetObjectItemCaseSensitive(best.comps, to_string(best.ids[i]).c_str()), "bone_names"));
    }
    best.totalBones = acc;
    return best;
}

static int legacyIndexCount(const string& dir, Ini& ini, int mid, int& vcount, int& b0, int& b1, int& b2, int& maxBone, int& usedBone) {
    auto rd = [&](const string& sec, int& stride, vector<uint8_t>& data) -> bool {
        auto it = ini.sec.find(sec); if (it == ini.sec.end()) return false;
        auto st = it->second.find("stride"); if (st == it->second.end()) return false;
        stride = atoi(st->second.c_str());
        auto fn = it->second.find("filename"); if (fn == it->second.end()) return false;
        data = readFile(dir + "/" + fn->second); return true;
    };
    int s0=0,s1=0,s2=0,sib=0; vector<uint8_t> vb0,vb1,vb2,ib;
    if (!rd("Resource_Component"+to_string(mid)+"_VB0", s0, vb0) ||
        !rd("Resource_Component"+to_string(mid)+"_IB", sib, ib)) return -1;
    rd("Resource_Component"+to_string(mid)+"_VB1", s1, vb1);
    if (!rd("Resource_Component"+to_string(mid)+"_VB2", s2, vb2)) return -1;
    if (s0 <= 0) return -1;
    vcount = (int)vb0.size() / s0;
    b0 = s0; b1 = s1; b2 = s2;
    int ni = (int)ib.size() / 2;
    // 旧 alpha: u8 本地骨骼；stride16 是合并骨架 u16，stride8 是刚性 u16。
    set<int> used;
    if (s2 == 12) {
        for (int v = 0; v < vcount; v++) for (int k = 0; k < 4; k++) {
            uint8_t x = vb2[v*12 + 8 + k];
            uint16_t w = (uint16_t)(vb2[v*12 + k*2] | (vb2[v*12 + k*2 + 1] << 8));
            if (w || x) { used.insert(x); if ((int)x > maxBone) maxBone = x; }
        }
    } else if (s2 == 16) {
        for (int v = 0; v < vcount; v++) for (int k = 0; k < 4; k++) {
            uint16_t w = (uint16_t)(vb2[v*16 + k*2] | (vb2[v*16 + k*2 + 1] << 8));
            uint16_t x = (uint16_t)(vb2[v*16 + 8 + k*2] | (vb2[v*16 + 8 + k*2 + 1] << 8));
            if (w) { used.insert(x); if ((int)x > maxBone) maxBone = x; }
        }
    } else if (s2 == 8) {
        for (int v = 0; v < vcount; v++) {
            uint16_t x = (uint16_t)(vb2[v*8] | (vb2[v*8 + 1] << 8));
            used.insert(x); if ((int)x > maxBone) maxBone = x;
        }
    }
    usedBone = (int)used.size();
    return ni;
}

static int legacyInspect(const string& dir, const string& catDir, const string& character, const string& outPath) {
    auto ini = Ini::parse(dir + "/mod.ini");
    if (!character.empty()) {
        // 只接受旧布局；新格式会先走精确 hash 路径。
    } else {
        throw runtime_error("legacy mapping needs --character <chr_...> (旧包没有当前 hash，必须显式指定目标角色)");
    }
    auto cat = loadCatalogByCharacter(catDir, character);
    vector<LegacyComp> comps;
    for (auto& s : ini.order) {
        int mid = effiComponentMid(s); if (mid < 0) continue;
        auto it = ini.sec[s].find("hash"); if (it == ini.sec[s].end()) continue;
        LegacyComp c; c.id = mid; c.hash = lower(it->second);
        auto mic = ini.sec[s].find("match_index_count");
        if (mic != ini.sec[s].end()) c.match_index_count = atoi(mic->second.c_str());
        c.index_count = legacyIndexCount(dir, ini, mid, c.vertex_count, c.vb0_stride, c.vb1_stride, c.vb2_stride, c.bone_max, c.bone_used);
        c.bones_known = (c.vb2_stride == 12 || c.vb2_stride == 8);
        if (c.index_count < 0) { cerr << "skip component " << mid << ": missing VB/IB\n"; continue; }
        comps.push_back(c);
    }
    if (comps.empty()) throw runtime_error("no legacy ComponentN resources found");

    // 为每个旧组件和当前 catalog 组件打分：index count 为主，骨骼数为辅。
    for (auto& c : comps) {
        int oldCount = c.match_index_count > 0 ? c.match_index_count : c.index_count;
        int oldBones = c.bone_max + 1;
        for (int cid : cat.ids) {
            auto* t = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(cid).c_str());
            int tc = jint(t, "original_index_count");
            int tb = cJSON_GetArraySize(get(t, "bone_names"));
            if (tc <= 0) continue;
            double countScore = 1.0 - std::min(1.0, std::fabs((double)oldCount - tc) / std::max(oldCount, tc));
            double boneScore = 1.0 - std::min(1.0, std::fabs((double)oldBones - tb) / std::max(oldBones, tb));
            // 旧 EFMI 的组件顺序也有一点先验，但不作为主证据。
            double orderBonus = (c.id == cid) ? 0.03 : 0.0;
            double score = std::max(0.0, std::min(1.0, 0.68 * countScore + 0.29 * boneScore + orderBonus));
            c.candidates.push_back({cid, score});
        }
        sort(c.candidates.begin(), c.candidates.end(), [](auto& a, auto& b) { return a.second > b.second; });
    }

    cJSON* root = obj(nullptr);
    cJSON_AddStringToObject(root, "status", "draft");
    cJSON_AddStringToObject(root, "source_format", "efmi-legacy");
    cJSON_AddStringToObject(root, "note", "draft mapping; bones/textures require review; no silent guessing");
    cJSON_AddStringToObject(root, "source_dir", fs::path(dir).filename().string().c_str());
    cJSON* target = obj(nullptr);
    cJSON_AddStringToObject(target, "character_id", character.c_str());
    cJSON_AddStringToObject(target, "revision", jstr(cat.root, "revision").c_str());
    cJSON_AddItemToObject(root, "target", target);
    cJSON* jcomps = jarr();
    cJSON* jbones = jarr();
    cout << "legacy components: " << comps.size() << " -> " << character << " (" << cat.ids.size() << " target comps)\n";
    for (auto& c : comps) {
        cJSON* x = obj(nullptr);
        cJSON_AddNumberToObject(x, "source_id", c.id);
        cJSON_AddStringToObject(x, "source_hash", c.hash.c_str());
        cJSON_AddNumberToObject(x, "match_index_count", c.match_index_count);
        cJSON_AddNumberToObject(x, "index_count", c.index_count);
        cJSON_AddNumberToObject(x, "vertex_count", c.vertex_count);
        cJSON_AddNumberToObject(x, "vb0_stride", c.vb0_stride);
        cJSON_AddNumberToObject(x, "vb2_stride", c.vb2_stride);
        cJSON_AddNumberToObject(x, "bone_max", c.bone_max);
        cJSON_AddNumberToObject(x, "bone_used", c.bone_used);
        cJSON* alts = jarr();
        for (int i = 0; i < (int)std::min<size_t>(3, c.candidates.size()); i++) {
            cJSON* a = obj(nullptr);
            cJSON_AddNumberToObject(a, "target", c.candidates[i].first);
            cJSON_AddNumberToObject(a, "score", c.candidates[i].second);
            auto* t = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(c.candidates[i].first).c_str());
            cJSON_AddStringToObject(a, "mesh_name", jstr(t, "mesh_name").c_str());
            cJSON_AddNumberToObject(a, "original_index_count", jint(t, "original_index_count"));
            cJSON_AddNumberToObject(a, "target_bones", cJSON_GetArraySize(get(t, "bone_names")));
            jadd(alts, a);
        }
        cJSON_AddItemToObject(x, "alternatives", alts);
        if (!c.candidates.empty()) {
            cJSON_AddNumberToObject(x, "target", c.candidates[0].first);
            cJSON_AddNumberToObject(x, "confidence", c.candidates[0].second);
            cJSON_AddBoolToObject(x, "needs_review", c.candidates[0].second < 0.78);
        }
        jadd(jcomps, x);
        if (!c.candidates.empty() && c.bones_known && c.bone_max >= 0) {
            int tcid = c.candidates[0].first;
            auto* t = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(tcid).c_str());
            int tb = cJSON_GetArraySize(get(t, "bone_names"));
            if (c.bone_max + 1 == tb) {
                for (int bi = 0; bi <= c.bone_max; bi++) {
                    cJSON* b = obj(nullptr);
                    cJSON_AddNumberToObject(b, "source_id", c.id);
                    cJSON_AddNumberToObject(b, "source_index", bi);
                    cJSON_AddNumberToObject(b, "target_component", tcid);
                    cJSON_AddNumberToObject(b, "target_index", bi);
                    cJSON_AddStringToObject(b, "mode", "exact-count-index-order");
                    jadd(jbones, b);
                }
            }
        }
        cout << "  comp " << c.id << " hash=" << c.hash << " idx=" << c.index_count
             << " bones<=" << (c.bone_max + 1) << " ->";
        for (int i = 0; i < (int)std::min<size_t>(3, c.candidates.size()); i++)
            cout << " " << c.candidates[i].first << "(" << c.candidates[i].second << ")";
        cout << "\n";
    }
    cJSON_AddItemToObject(root, "components", jcomps);
    cJSON* notes = jarr();
    jadd(notes, jstrv("旧 EFMI 没有骨骼名；只有 stride12(u8 本地索引) 才能直接数出骨骼数"));
    jadd(notes, jstrv("组件匹配置信度 < 0.78 必须人工复核，不自动写入 BEM"));
    jadd(notes, jstrv("贴图映射需要 DDS 内容指纹或人工 profile，本命令只输出空表"));
    cJSON_AddItemToObject(root, "notes", notes);
    cJSON* jtex = jarr(); cJSON_AddItemToObject(root, "textures", jtex);
    cJSON_AddItemToObject(root, "bones", jbones);

    char* text = cJSON_Print(root);
    if (!text) throw runtime_error("mapping serialize failed");
    if (!outPath.empty()) writeFile(outPath, vector<uint8_t>((uint8_t*)text, (uint8_t*)text + strlen(text)));
    else cout << text << "\n";
    free(text); cJSON_Delete(root); cJSON_Delete(cat.root);
    return 0;
}


// ============================ 旧 EFMI profile 转换 ============================
// 严格按 mapping.json 转换：组件/骨骼/贴图没有显式映射就拒绝，不按 hash 或编号猜。
static int convertLegacyProfile(const string& dir, const string& catDir, const string& profilePath,
                                const string& out, bool allowReview) {
    auto ini = Ini::parse(dir + "/mod.ini");
    auto pb = readFile(profilePath);
    cJSON* pr = cJSON_ParseWithLength((const char*)pb.data(), pb.size());
    if (!pr) throw runtime_error("profile JSON parse failed");
    auto* tgt = get(pr, "target");
    string character = jstr(tgt, "character_id");
    if (character.empty()) throw runtime_error("profile.target.character_id missing");
    auto cat = loadCatalogByCharacter(catDir, character);

    map<int,int> compMap;
    map<int,bool> review;
    auto* pcs = get(pr, "components");
    if (!cJSON_IsArray(pcs)) throw runtime_error("profile.components missing");
    for (auto* c = pcs->child; c; c = c->next) {
        int sid = jint(c, "source_id"), tid = jint(c, "target");
        if (tid <= 0 && !get(c, "target")) continue;
        bool needs = cJSON_IsTrue(get(c, "needs_review"));
        if (needs && !allowReview) { cerr << "skip source " << sid << ": needs_review (use --allow-review to force)\n"; continue; }
        bool exists = false; for (int cid : cat.ids) if (cid == tid) exists = true;
        if (!exists) { cerr << "skip source " << sid << ": target " << tid << " not in catalog\n"; continue; }
        if (compMap.count(sid)) continue;
        compMap[sid] = tid; review[sid] = needs;
    }
    if (compMap.empty()) throw runtime_error("no usable component mapping (need --allow-review?)");

    map<pair<int,int>, pair<int,int>> boneMap;
    auto* pbs = get(pr, "bones");
    if (cJSON_IsArray(pbs)) for (auto* b = pbs->child; b; b = b->next) {
        int sid = jint(b, "source_id"), si = jint(b, "source_index");
        int tc = jint(b, "target_component"), ti = jint(b, "target_index");
        boneMap[{sid, si}] = {tc, ti};
    }

    // 旧贴图 hash -> DDS；profile.textures 决定 hash -> catalog original_name
    map<string, string> texDds;
    for (auto& sec : ini.order) {
        if (sec.rfind("Resource_Texture", 0) != 0) continue;
        int n = atoi(sec.c_str() + strlen("Resource_Texture"));
        auto fn = effKey(ini, sec, "filename"); if (fn.empty()) continue;
        auto hash = effKey(ini, "TextureOverride_Texture" + to_string(n), "hash");
        if (!hash.empty()) texDds[lower(hash)] = fn;
    }
    struct ProfileTex { string name, dds; };
    vector<ProfileTex> profileTex;
    set<string> usedTexNames;
    auto addProfileTex = [&](const string& hash, const string& name) {
        if (hash.empty() || name.empty() || !texDds.count(hash) || usedTexNames.count(name)) return;
        usedTexNames.insert(name);
        profileTex.push_back({name, texDds[hash]});
    };
    // 1) explicit profile mapping wins
    auto* pts = get(pr, "textures");
    if (cJSON_IsArray(pts)) for (auto* t = pts->child; t; t = t->next) {
        string h = lower(jstr(t, "source_hash")), name = jstr(t, "target_name");
        if (h.empty() || name.empty() || !texDds.count(h)) continue;
        bool known = false; if (cJSON_IsObject(cat.textures)) for (auto* ct = cat.textures->child; ct; ct = ct->next) {
            auto* nm = get(ct, "name"); if (cJSON_IsString(nm) && name == nm->valuestring) { known = true; break; }
        }
        if (known) addProfileTex(h, name);
    }
    // 2) direct hash identity from catalog/identities, then optional descriptor fingerprint
    for (auto& kv : texDds) {
        auto* ct = cJSON_IsObject(cat.textures) ? cJSON_GetObjectItemCaseSensitive(cat.textures, kv.first.c_str()) : nullptr;
        auto* nm = ct ? get(ct, "name") : nullptr;
        if (cJSON_IsString(nm)) { addProfileTex(kv.first, nm->valuestring); continue; }
        if (!g_textureFingerprint) continue;
        try {
            Dds d = readTexture(dir + "/" + kv.second, "");
            string name = catalogTextureByDescriptor(cat.textures, d.w, d.h, d.mips);
            if (!name.empty()) { cout << "legacy texture fingerprint: " << kv.first << " -> " << name << "\n"; addProfileTex(kv.first, name); }
        } catch (exception&) {}
    }

    vector<vector<uint8_t>> payloads;
    auto addPayload = [&](vector<uint8_t> b) { payloads.push_back(std::move(b)); return (int)payloads.size() - 1; };
    cJSON* jtextures = jarr();
    map<string,int> texIndex;
    vector<int> textureIds;
    for (auto& t : profileTex) {
        try {
            Dds d = readTexture(dir + "/" + t.dds, t.name);
            int pid = addPayload(d.data);
            cJSON* x = obj(nullptr);
            cJSON_AddNumberToObject(x, "width", d.w); cJSON_AddNumberToObject(x, "height", d.h);
            cJSON_AddNumberToObject(x, "mips", d.mips); cJSON_AddNumberToObject(x, "format", d.fmt);
            cJSON_AddBoolToObject(x, "srgb", d.srgb); cJSON_AddStringToObject(x, "original_name", t.name.c_str());
            cJSON_AddNumberToObject(x, "payload", pid); jadd(jtextures, x);
            int ti = cJSON_GetArraySize(jtextures) - 1; texIndex[t.name] = ti; textureIds.push_back(ti);
        } catch (exception& e) { cerr << "texture skip " << t.dds << ": " << e.what() << "\n"; }
    }

    cJSON* jmeshes = jarr();
    map<int,int> meshIndex;
    for (auto& [mid, cid] : compMap) {
        auto nvb0 = "Resource_Component" + to_string(mid) + "_VB0";
        auto nvb1 = "Resource_Component" + to_string(mid) + "_VB1";
        auto nvb2 = "Resource_Component" + to_string(mid) + "_VB2";
        auto nib  = "Resource_Component" + to_string(mid) + "_IB";
        if (!ini.sec.count(nvb0) || !ini.sec.count(nvb1) || !ini.sec.count(nvb2) || !ini.sec.count(nib)) { cout << "skip comp " << mid << ": missing resources\n"; continue; }
        auto rd = [&](const string& sec, int& stride, vector<uint8_t>& data) {
            auto& d = ini.sec[sec]; stride = atoi(d["stride"].c_str()); data = readFile(dir + "/" + d["filename"]);
        };
        int s0,s1,s2,sib; vector<uint8_t> vb0,vb1,vb2,ib;
        rd(nvb0,s0,vb0); rd(nvb1,s1,vb1); rd(nvb2,s2,vb2); rd(nib,sib,ib);
        if (s0 <= 0 || s1 <= 0) { cout << "skip comp " << mid << ": bad stride\n"; continue; }
        int nv = (int)vb0.size()/s0;
        if ((int)vb1.size()/s1 != nv) { cout << "skip comp " << mid << ": VB1 size mismatch\n"; continue; }
        if (s2 != 4 && s2 != 8 && s2 != 12 && s2 != 16 && s2 != 24) { cout << "skip comp " << mid << ": legacy skin stride " << s2 << "\n"; continue; }
        if ((int)vb2.size() != nv*s2) { cout << "skip comp " << mid << ": VB2 size mismatch\n"; continue; }
        int ni = (int)ib.size()/2;

        auto* comp = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(cid).c_str());
        auto* attrs = get(comp, "attributes");
        int fsz[] = {4,2,1,1,2,2,1,1,2,2,4,4}; int offs[3] = {0,0,0};
        int astrides[3] = {0,0,0};
        cJSON* jattrs = jarr();
        for (auto* a = attrs->child; a; a = a->next) {
            int sem = a->child->valueint, fmt = a->child->next->valueint, dim = a->child->next->next->valueint, stream = a->child->next->next->next->valueint;
            if (stream >= 2) continue;
            cJSON* x = jarr(); jadd(x,jnum(sem)); jadd(x,jnum(fmt)); jadd(x,jnum(dim)); jadd(x,jnum(stream)); jadd(x,jnum(offs[stream]));
            jadd(jattrs,x); offs[stream] += fsz[fmt]*dim; astrides[stream] = offs[stream];
        }
        cJSON* sx = jarr(); jadd(sx,jnum(12)); jadd(sx,jnum(4)); jadd(sx,jnum(4)); jadd(sx,jnum(2)); jadd(sx,jnum(0)); jadd(jattrs,sx);
        sx = jarr(); jadd(sx,jnum(13)); jadd(sx,jnum(6)); jadd(sx,jnum(4)); jadd(sx,jnum(2)); jadd(sx,jnum(8)); jadd(jattrs,sx);
        if (astrides[0] != s0 || astrides[1] != s1) {
            if (!g_compatLayout) {
                cout << "skip comp " << mid << ": target attrs " << astrides[0] << "/" << astrides[1]
                     << " != source " << s0 << "/" << s1 << " (add --compat-layout to pad trailing gaps)\n";
                continue;
            }
            auto padStream = [&](vector<uint8_t>& vb, int& stride, int targetStride, int stream) {
                if (stride == targetStride) return true;
                if (stride > targetStride) {
                    cout << "skip comp " << mid << ": compat-layout refuses shrinking stream" << stream
                         << " " << stride << " -> " << targetStride << "\n";
                    return false;
                }
                vector<uint8_t> out((size_t)nv * targetStride, 0);
                for (int v = 0; v < nv; v++)
                    memcpy(&out[(size_t)v * targetStride], &vb[(size_t)v * stride], stride);
                vb.swap(out);
                cout << "compat-layout comp " << mid << ": stream" << stream << " "
                     << stride << " -> " << targetStride << " (zero-fill trailing)\n";
                stride = targetStride;
                return true;
            };
            if (!padStream(vb0, s0, astrides[0], 0) || !padStream(vb1, s1, astrides[1], 1)) continue;
        }

        int targetBones = cJSON_GetArraySize(get(comp, "bone_names"));
        // Decode the source skin layout into (u16 weight, source bone id) pairs.
        int srcBones = 0;
        auto decodeSkin = [&](int v, uint16_t w[4], uint16_t gi[4]) {
            if (s2 == 12) {
                for (int k = 0; k < 4; k++) {
                    w[k] = (uint16_t)(vb2[v*12 + k*2] | (vb2[v*12 + k*2 + 1] << 8));
                    gi[k] = vb2[v*12 + 8 + k];
                }
            } else if (s2 == 16) {
                for (int k = 0; k < 4; k++) {
                    w[k] = (uint16_t)(vb2[v*16 + k*2] | (vb2[v*16 + k*2 + 1] << 8));
                    gi[k] = (uint16_t)(vb2[v*16 + 8 + k*2] | (vb2[v*16 + 8 + k*2 + 1] << 8));
                }
            } else if (s2 == 24) {
                float f[4]; memcpy(f, &vb2[v*24], 16);
                float sum = 0; for (int k = 0; k < 4; k++) { if (!std::isfinite(f[k]) || f[k] < 0) f[k] = 0; sum += f[k]; }
                if (sum <= 1e-6f) { for (int k = 0; k < 4; k++) { w[k] = 0; gi[k] = 0; } return; }
                for (int k = 0; k < 4; k++) {
                    w[k] = (uint16_t)std::lround(std::min(1.0f, f[k] / sum) * 65535.0f);
                    gi[k] = (uint16_t)(vb2[v*24 + 16 + k*2] | (vb2[v*24 + 16 + k*2 + 1] << 8));
                }
            } else if (s2 == 8) {
                for (int k = 0; k < 4; k++) gi[k] = (uint16_t)(vb2[v*8 + k*2] | (vb2[v*8 + k*2 + 1] << 8));
                w[0] = 65535;
            } else {
                gi[0] = (uint16_t)(vb2[v*4] | (vb2[v*4 + 1] << 8));
                w[0] = 65535;
            }
            for (int k = 0; k < 4; k++) if (w[k]) srcBones = std::max(srcBones, (int)gi[k] + 1);
        };
        uint16_t tw[4], tgi[4];
        for (int v = 0; v < nv; v++) { decodeSkin(v, tw, tgi); }

        // --compat-bones：源是全局骨骼 id，但目标 catalog 只给组件局部 bone_names。
        // 仅当「源实际使用的 unique bone id 数」恰好等于目标 bone 数时，按数值序做排名映射。
        set<int> usedIds;
        for (int v = 0; v < nv; v++) {
            uint16_t w[4], gi[4]; decodeSkin(v, w, gi);
            for (int k = 0; k < 4; k++) if (w[k]) usedIds.insert((int)gi[k]);
        }
        vector<int> usedVec(usedIds.begin(), usedIds.end());
        bool rankBones = g_compatBones && (int)usedVec.size() == targetBones;
        // stride12 是 u8 本地骨骼索引：源骨骼范围不超过目标组件骨骼数时，允许显式直映。
        bool directLocal = g_compatBones && s2 == 12 && srcBones <= targetBones;
        if (directLocal) cout << "compat-bones comp " << mid << ": direct local 0.." << (srcBones - 1)
                              << " -> target 0.." << (srcBones - 1) << "\n";
        if (g_compatBones && !rankBones && (int)usedVec.size() != targetBones && !directLocal)
            cout << "compat-bones comp " << mid << ": used " << usedVec.size()
                 << " != target " << targetBones << " (skip rank mapping)\n";

        vector<uint8_t> skin(nv*12);
        vector<cJSON*> bones; map<pair<int,int>,int> pal; bool ok = true; string fail;
        for (int v = 0; v < nv && ok; v++) {
            uint16_t w[4], gi[4]; decodeSkin(v, w, gi);
            uint8_t outIdx[4] = {0,0,0,0};
            for (int k = 0; k < 4; k++) {
                if (!w[k]) continue;
                pair<int,int> tgt;
                auto it = boneMap.find({mid, (int)gi[k]});
                if (it != boneMap.end()) tgt = it->second;
                else if (rankBones) {
                    auto rit = lower_bound(usedVec.begin(), usedVec.end(), (int)gi[k]);
                    if (rit != usedVec.end() && *rit == (int)gi[k])
                        tgt = {cid, (int)(rit - usedVec.begin())};
                    else { fail = "bone " + to_string(gi[k]) + " not in used set"; ok = false; break; }
                }
                else if (directLocal) tgt = {cid, (int)gi[k]};
                else if (!g_skeleton.empty() && (s2 == 16 || s2 == 8 || s2 == 4)) {
                    auto sit = g_skeleton.find((int)gi[k]);
                    if (sit == g_skeleton.end()) { fail = "bone " + to_string(gi[k]) + " not in skeleton table"; ok = false; break; }
                    bool found = false;
                    auto* bn = get(cat.comps, to_string(cid).c_str());
                    bn = bn ? get(bn, "bone_names") : nullptr;
                    for (int bi = 0; bn && bi < cJSON_GetArraySize(bn); bi++) {
                        auto* nm = cJSON_GetArrayItem(bn, bi);
                        if (cJSON_IsString(nm) && sit->second == nm->valuestring) { tgt = {cid, bi}; found = true; break; }
                    }
                    if (!found) { fail = "bone " + sit->second + " not in target component"; ok = false; break; }
                }
                else if (srcBones == targetBones) tgt = {cid, (int)gi[k]};
                else { fail = "bone " + to_string(gi[k]) + " unmapped (src bones " + to_string(srcBones) + " vs target " + to_string(targetBones) + ")"; ok = false; break; }
                auto pit = pal.find(tgt);
                int pi;
                if (pit == pal.end()) {
                    pi = (int)pal.size(); if (pi > 255) { fail = "palette exceeds 256"; ok = false; break; }
                    pal[tgt] = pi;
                    auto* tc = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(tgt.first).c_str());
                    auto* bn = get(tc, "bone_names"); auto* nm = cJSON_GetArrayItem(bn, tgt.second);
                    if (!cJSON_IsString(nm)) { fail = "target bone out of table"; ok = false; break; }
                    cJSON* b = obj(nullptr); cJSON_AddNumberToObject(b,"component",tgt.first); cJSON_AddNumberToObject(b,"index",tgt.second); cJSON_AddStringToObject(b,"name",nm->valuestring);
                    bones.push_back(b);
                } else pi = pit->second;
                outIdx[k] = (uint8_t)pi;
            }
            if (!ok) break;
            for (int k = 0; k < 4; k++) { skin[v*12+k*2]=w[k]&255; skin[v*12+k*2+1]=w[k]>>8; }
            memcpy(&skin[v*12+8], outIdx, 4);
        }
        if (!ok) { cout << "skip comp " << mid << ": " << fail << "\n"; continue; }

        int p0=addPayload(vb0), p1=addPayload(vb1), p2=addPayload(skin), pib=addPayload(ib);
        cJSON* jstreams = jarr();
        auto stream = [&](int stride,int payload){ cJSON* q=obj(nullptr); cJSON_AddNumberToObject(q,"stride",stride); cJSON_AddNumberToObject(q,"payload",payload); jadd(jstreams,q); };
        stream(s0,p0); stream(s1,p1); stream(12,p2);
        cJSON* jmesh = obj(nullptr);
        cJSON_AddNumberToObject(jmesh,"vertex_count",nv);
        cJSON_AddNumberToObject(jmesh,"index_size",2);
        cJSON_AddItemToObject(jmesh,"streams",jstreams);
        cJSON_AddItemToObject(jmesh,"attributes",jattrs);
        cJSON* jbones = jarr(); for (auto* b : bones) jadd(jbones,b); cJSON_AddItemToObject(jmesh,"bones",jbones);
        cJSON_AddNumberToObject(jmesh,"indices",pib);
        cJSON_AddNumberToObject(jmesh,"index_count",ni);
        cJSON* jdraws = jarr(); cJSON* jd = obj(nullptr);
        cJSON_AddNumberToObject(jd,"start",0); cJSON_AddNumberToObject(jd,"count",ni);
        cJSON_AddNumberToObject(jd,"material_component",cid); cJSON_AddNumberToObject(jd,"material_slot",0);
        auto* mats = get(comp,"materials"); auto* m0 = cJSON_GetArrayItem(mats,0);
        cJSON_AddStringToObject(jd,"material_name",m0?m0->valuestring:"M_unknown");
        cJSON* jtex = jarr(); for (int ti : textureIds) jadd(jtex,jnum(ti)); cJSON_AddItemToObject(jd,"textures",jtex);
        jadd(jdraws,jd); cJSON_AddItemToObject(jmesh,"draws",jdraws);
        meshIndex[cid] = cJSON_GetArraySize(jmeshes); jadd(jmeshes,jmesh);
        cout << "legacy comp " << mid << " -> " << cid << ": v=" << nv << " idx=" << ni << " bones=" << pal.size() << "\n";
    }
    if (meshIndex.empty()) throw runtime_error("no component converted from profile");

    // target + appearance
    cJSON* jtarget = obj(nullptr);
    cJSON_AddStringToObject(jtarget,"character_id",character.c_str());
    cJSON_AddStringToObject(jtarget,"platform","windows-x64");
    cJSON_AddStringToObject(jtarget,"profile_id",jstr(cat.root,"profile_id").c_str());
    cJSON_AddStringToObject(jtarget,"revision",jstr(cat.root,"revision").c_str());
    cJSON_AddStringToObject(jtarget,"snapshot","");
    cJSON_AddStringToObject(jtarget,"world_resource",jstr(cat.root,"world_resource").c_str());
    cJSON_AddStringToObject(jtarget,"ui_resource",jstr(cat.root,"ui_resource").c_str());
    cJSON* jcomps = jarr();
    for (int cid : cat.ids) {
        auto* c = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(cid).c_str());
        cJSON* x = obj(nullptr); cJSON_AddNumberToObject(x,"id",cid);
        cJSON_AddStringToObject(x,"mesh_name",jstr(c,"mesh_name").c_str());
        cJSON_AddNumberToObject(x,"original_index_count",jint(c,"original_index_count"));
        cJSON* bn = jarr(); for (auto* b = get(c,"bone_names")->child; b; b = b->next) jadd(bn,jstrv(b->valuestring)); cJSON_AddItemToObject(x,"bone_names",bn);
        cJSON* mm = jarr(); for (auto* m = get(c,"materials")->child; m; m = m->next) jadd(mm,jstrv(m->valuestring)); cJSON_AddItemToObject(x,"materials",mm);
        jadd(jcomps,x);
    }
    cJSON_AddItemToObject(jtarget,"components",jcomps);
    cJSON* japps = jarr(); cJSON* japp = obj(nullptr);
    cJSON_AddStringToObject(japp,"id","default"); cJSON_AddStringToObject(japp,"name","默认外观");
    cJSON* jac = jarr();
    for (int cid : cat.ids) {
        cJSON* x = obj(nullptr); cJSON_AddNumberToObject(x,"target",cid);
        auto it = meshIndex.find(cid);
        if (it == meshIndex.end()) cJSON_AddStringToObject(x,"operation","keep");
        else { cJSON_AddStringToObject(x,"operation","replace"); cJSON_AddNumberToObject(x,"mesh",it->second); }
        jadd(jac,x);
    }
    cJSON_AddItemToObject(japp,"components",jac); jadd(japps,japp);

    cJSON* root = obj(nullptr);
    cJSON_AddNumberToObject(root,"schema",1);
    cJSON_AddStringToObject(root,"package_id",("c4ben."+character+".legacy").c_str());
    cJSON_AddStringToObject(root,"name",("Converter4BENeXt legacy: "+fs::path(dir).filename().string()).c_str());
    cJSON_AddStringToObject(root,"author","Converter4BENeXt");
    cJSON_AddStringToObject(root,"version","0.2.0");
    cJSON* caps = jarr(); for (auto s : {"native-materials","palette-u8","indices-u32","fixed-appearances"}) jadd(caps,jstrv(s));
    cJSON_AddItemToObject(root,"required_capabilities",caps);
    cJSON_AddItemToObject(root,"target",jtarget);
    requireNonEmptyOutput(jmeshes, jtextures, out);
    cJSON_AddItemToObject(root,"meshes",jmeshes);
    cJSON_AddItemToObject(root,"textures",jtextures);
    cJSON_AddStringToObject(root,"default_appearance_id","default");
    cJSON_AddItemToObject(root,"appearances",japps);
    char* json = cJSON_PrintUnformatted(root); if (!json) throw runtime_error("manifest serialize failed");
    vector<uint8_t> manifest((uint8_t*)json, (uint8_t*)json + strlen(json)); free(json);
    // 与 EFMI 路径一致：输出名以 .zip 结尾时写 BEM 原生 ZIP 容器，否则写二进制容器。
    if (out.size() >= 4 && string(out).substr(string(out).size() - 4) == ".zip") {
        vector<Entry> es; es.push_back({"manifest.json", manifest, 0, 0});
        for (size_t i = 0; i < payloads.size(); i++) {
            char nm[64]; snprintf(nm, sizeof nm, "payloads/%04zu.bin", i);
            es.push_back({nm, payloads[i], 0, 0});
        }
        writeZip(out, es);
    } else {
        writeBem(out, manifest, payloads);
    }
    cout << "written " << out << ": meshes=" << cJSON_GetArraySize(jmeshes) << " textures=" << cJSON_GetArraySize(jtextures) << " payloads=" << payloads.size() << "\n";
    cJSON_Delete(root); cJSON_Delete(pr); cJSON_Delete(cat.root);
    return 0;
}


// EFMI 转换主流程（从 CLI 中抽出，供 C ABI 复用）。
// Unique-ish fallback for catalog hash drift: match a source DDS against catalog
// texture descriptors (width/height/mip count). Opt-in via --texture-fingerprint;
// only a single surviving candidate is accepted, anything else is reported.
static string catalogTextureByDescriptor(cJSON* textures, int w, int h, int mips) {
    if (!cJSON_IsObject(textures)) return "";
    string match; int hits = 0;
    for (auto* t = textures->child; t; t = t->next) {
        auto* name = get(t, "name");
        if (!cJSON_IsString(name)) continue;
        auto* evidence = get(t, "evidence");
        auto* descriptor = evidence ? get(evidence, "descriptor") : nullptr;
        if (!cJSON_IsArray(descriptor) || cJSON_GetArraySize(descriptor) < 2) continue;
        int dw = cJSON_GetArrayItem(descriptor, 0)->valueint;
        int dh = cJSON_GetArrayItem(descriptor, 1)->valueint;
        int dm = cJSON_GetArraySize(descriptor) > 3 ? cJSON_GetArrayItem(descriptor, 3)->valueint : 1;
        if (dw == w && dh == h && (dm <= 0 || dm == mips)) { ++hits; match = name->valuestring; }
    }
    return hits == 1 ? match : "";
}

// Optional identity map supplied by an external extractor (ForTMR/BEN tools):
//   {"characters":{"chr_x":{"entries":{"<hash>":3},"textures":{"<hash>":"T_actor_..."}}}}
// It only fills hash→component / hash→texture-name gaps; layouts still come from
// the BEN catalog, so unsupported native layouts still fail loudly.
static int applyIdentities(Cat& cat) {
    if (!g_identities) return 0;
    auto* characters = get(g_identities, "characters");
    auto* entry = characters ? get(characters, cat.character.c_str()) : nullptr;
    if (!cJSON_IsObject(entry)) return 0;
    int applied = 0;
    auto* entries = get(entry, "entries");
    if (cJSON_IsObject(entries) && cJSON_IsObject(cat.entries)) for (auto* e = entries->child; e; e = e->next) {
        if (!cJSON_IsNumber(e)) continue;
        auto* old = cJSON_GetObjectItemCaseSensitive(cat.entries, e->string);
        if (old) {
            if (!cJSON_IsNumber(old) || old->valueint != e->valueint)
                throw runtime_error("identities conflict for component hash " + string(e->string));
            continue;
        }
        cJSON* copy = cJSON_CreateNumber(e->valueint);
        cJSON_AddItemToObject(cat.entries, e->string, copy); ++applied;
    }
    auto* textures = get(entry, "textures");
    if (cJSON_IsObject(textures) && cJSON_IsObject(cat.textures)) for (auto* t = textures->child; t; t = t->next) {
        string name = cJSON_IsString(t) ? t->valuestring : jstr(t, "name");
        if (name.empty()) continue;
        auto* old = cJSON_GetObjectItemCaseSensitive(cat.textures, t->string);
        if (old) {
            auto* oldName = get(old, "name");
            if (cJSON_IsString(oldName) && name != oldName->valuestring)
                throw runtime_error("identities conflict for texture hash " + string(t->string));
            continue;
        }
        cJSON* obj = cJSON_CreateObject(); cJSON_AddStringToObject(obj, "name", name.c_str());
        cJSON_AddItemToObject(cat.textures, t->string, obj);
        cJSON_AddStringToObject(obj, "source", "external-identities");
        ++applied;
    }
    cout << "identities: applied " << applied << " entries for " << cat.character << "\n";
    return applied;
}

// EFMI 把「变体 → 组件」的条件写成常量：
//   $draw_component_2_ba3f54e3_mask = ($swapvar_toggle_0 == 0)
// 解析出来即可知道每个组件在哪个组、取哪个值时被绘制。
struct EfmiDrawCond { string group; int value = 0; };
static map<string, vector<EfmiDrawCond>> parseEfmiDrawConditions(const Ini& ini) {
    map<string, vector<EfmiDrawCond>> out; // component hash -> conditions
    for (auto& s : ini.order) {
        auto it = ini.sec.find(s);
        if (it == ini.sec.end()) continue;
        for (auto& kv : it->second) {
            if (kv.first.rfind("$draw_", 0) != 0) continue;
            // 变量名里带 8 位组件 hash：$draw_component_<n>_<hash>...
            string hash;
            size_t pos = 0;
            while ((pos = kv.first.find('_', pos)) != string::npos) {
                const string cand = kv.first.substr(pos + 1, 8);
                if (cand.size() == 8 && cand.find_first_not_of("0123456789abcdefABCDEF") == string::npos) { hash = lower(cand); break; }
                pos++;
            }
            if (hash.empty()) continue;
            // 条件形如 ($swapvar_x == 0)
            const string& v = kv.second;
            const size_t eq = v.find("==");
            if (eq == string::npos) continue;
            string group = v.substr(0, eq);
            const size_t gs = group.find("$swapvar");
            if (gs == string::npos) continue;
            group = group.substr(gs);
            while (!group.empty() && !isalnum((unsigned char)group[0]) && group[0] != '$') group.erase(group.begin());
            for (auto& ch : group) if (!(isalnum((unsigned char)ch) || ch == '_' || ch == '$')) ch = '_';
            while (!group.empty() && group.back() == '_') group.pop_back();
            const int value = atoi(v.c_str() + eq + 2);
            auto& bucket = out[hash];
            bool dup = false;
            for (auto& c : bucket) if (c.group == group && c.value == value) { dup = true; break; }
            if (!dup) bucket.push_back({group, value});
        }
    }
    return out;
}

// EFMI option surfaces (KeySwap/KeyToggle) are detected but not yet mapped to
// BEM 1.1 option_groups. Report them instead of silently collapsing to one state.
static vector<pair<string, vector<int>>> detectEfmiOptions(const Ini& ini) {
    vector<pair<string, vector<int>>> groups;
    for (auto& s : ini.order) {
        if (s.rfind("KeySwap", 0) != 0 && s.rfind("KeyToggle", 0) != 0) continue;
        auto section = ini.sec.find(s);
        if (section == ini.sec.end()) continue;
        auto it = section->second.find("key");
        if (it == section->second.end()) continue;
        // cycle sections declare the variable and its values on a line like
        //   $swapkey1 = 0, 1, 2
        string variable, values;
        for (auto& kv : section->second) {
            if (kv.first.rfind("$", 0) == 0 && kv.first.find("swap") != string::npos) { variable = kv.first; values = kv.second; break; }
        }
        if (variable.empty()) continue;
        vector<int> choices;
        string cur;
        for (char c : values + ",") {
            if (c == ',') { if (!cur.empty()) choices.push_back(atoi(cur.c_str())); cur.clear(); }
            else if (!isspace((unsigned char)c)) cur += c;
        }
        if (choices.empty()) choices.push_back(0);
        groups.push_back({variable, choices});
    }
    return groups;
}

// 布局-only catalog（新版 VFS 回填只给 components，entries 为空）的自动锚定。
// 只有“指纹唯一命中”时才写 entries，多个候选一律不猜，符合项目“不猜”的约定。
// 指纹 = 顶点数 + 索引数 + 三条流的 stride。
static int inferEntriesByLayout(Cat& cat, const string& dir, Ini& ini) {
    if (!cJSON_IsObject(cat.entries) || !cJSON_IsObject(cat.comps)) return 0;
    int added = 0;
    for (auto& s : ini.order) {
        int mid = effiComponentMid(s);
        if (mid < 0) continue;
        auto hit = ini.sec[s].find("hash");
        if (hit == ini.sec[s].end()) continue;
        const string hash = lower(hit->second);
        if (cJSON_GetObjectItemCaseSensitive(cat.entries, hash.c_str())) continue;

        int vcount = 0, s0 = 0, s1 = 0, s2 = 0, maxBone = -1, usedBone = 0;
        int icount = legacyIndexCount(dir, ini, mid, vcount, s0, s1, s2, maxBone, usedBone);
        if (icount < 0 || vcount <= 0) continue;

        vector<int> matches;
        int exactV = 0;
        for (auto* c = cat.comps->child; c; c = c->next) {
            auto* comp = c;
            auto* subs = get(comp, "submeshes");
            int ci = (cJSON_IsArray(subs) && cJSON_GetArraySize(subs) > 0) ? jint(cJSON_GetArrayItem(subs, 0), "index_count") : 0;
            // EFMI 替换网格的 draw 索引数必须与目标完全一致；顶点数允许比目标多
            // （作者常在 UV 缝处拆分顶点），所以按 index_count 精确 + vertex_count 上限匹配。
            if (ci != icount || ci == 0) continue;
            const int cv = jint(comp, "vertex_count");
            if (cv > vcount) continue;
            // 有 source_streams 的组件再校验一遍 stride（有则更可信）
            auto* streams = get(comp, "source_streams");
            if (cJSON_IsArray(streams) && cJSON_GetArraySize(streams) >= 3) {
                int cs[3] = {0, 0, 0};
                bool okStreams = true;
                for (int i = 0; i < 3; i++) {
                    auto* sem = get(cJSON_GetArrayItem(streams, i), "semantics");
                    if (!cJSON_IsArray(sem)) { okStreams = false; break; }
                    for (auto* q = sem->child; q; q = q->next) cs[i] += jint(q, "stride");
                }
                if (okStreams && (cs[0] != s0 || cs[1] != s1 || cs[2] != s2)) continue;
            }
            matches.push_back(stoi(string(c->string)));
            if (cv == vcount) exactV++;
        }
        if (matches.size() > 1 && exactV == 1) {
            // 顶点数完全相等的那个更可信
            int keep = -1;
            for (auto* c = cat.comps->child; c; c = c->next) {
                auto* subs = get(c, "submeshes");
                int ci = (cJSON_IsArray(subs) && cJSON_GetArraySize(subs) > 0) ? jint(cJSON_GetArrayItem(subs, 0), "index_count") : 0;
                if (ci == icount && jint(c, "vertex_count") == vcount) { keep = stoi(string(c->string)); break; }
            }
            matches.clear();
            if (keep >= 0) matches.push_back(keep);
        }
        if (matches.size() == 1) {
            cJSON_AddItemToObject(cat.entries, hash.c_str(), cJSON_CreateNumber(matches[0]));
            cout << "auto-entry: " << hash << " -> component " << matches[0]
                 << " (v=" << vcount << " idx=" << icount << " strides=" << s0 << "/" << s1 << "/" << s2 << ")\n";
            added++;
        } else if (matches.size() > 1) {
            cout << "auto-entry: " << hash << " ambiguous (" << matches.size() << " candidates), skipped\n";
        }
    }
    return added;
}

static int convertEfmi(const string& dir, const string& catDir, const string& out) {        auto ini = Ini::parse(dir + "/mod.ini");
        if (effKey(ini, "Mod", "format") == "3") return convertEff(dir, catDir, out);
        int componentEntries = 0;
        for (auto& section : ini.order) if (effiComponentMid(section) >= 0) ++componentEntries;
        if (componentEntries == 0) {
            bool dynamicPool = false;
            for (auto& section : ini.order)
                if (section.find("Pool_MergedSkeleton") != string::npos ||
                    section.find("CommandList_MergedSkeleton") != string::npos) dynamicPool = true;
            if (dynamicPool)
                throw runtime_error("dynamic EFMI merged-skeleton/pool mod (no static Component resources); not supported yet");
            throw runtime_error("not an EFMI model package (no TextureOverride_Component entry points)");
        }
        auto optionGroups = detectEfmiOptions(ini);
        auto drawConds = parseEfmiDrawConditions(ini);
        if (!optionGroups.empty()) {
            cout << "options detected: " << optionGroups.size() << " groups, "
                 << drawConds.size() << " component binding(s) (BEM option_groups 输出尚未实现)\n";
            for (auto& g : optionGroups) {
                cout << "  " << g.first << ":";
                for (int v : g.second) cout << " " << v;
                cout << "\n";
            }
            for (auto& kv : drawConds) {
                cout << "    component " << kv.first << " drawn when";
                for (auto& c : kv.second) cout << " " << c.group << "==" << c.value;
                cout << "\n";
            }
        }
        if (!g_optionReport.empty()) {
            cJSON* rep = obj(nullptr);
            cJSON* jg = jarr();
            for (auto& g : optionGroups) {
                cJSON* x = obj(nullptr);
                cJSON_AddStringToObject(x, "id", g.first.c_str());
                cJSON* ch = jarr();
                for (int v : g.second) jadd(ch, jnum(v));
                cJSON_AddItemToObject(x, "values", ch);
                jadd(jg, x);
            }
            cJSON_AddItemToObject(rep, "groups", jg);
            cJSON* jc = obj(nullptr);
            for (auto& kv : drawConds) {
                cJSON* arr = jarr();
                for (auto& c : kv.second) {
                    cJSON* w = obj(nullptr);
                    cJSON_AddStringToObject(w, "group", c.group.c_str());
                    cJSON_AddNumberToObject(w, "value", c.value);
                    jadd(arr, w);
                }
                cJSON_AddItemToObject(jc, kv.first.c_str(), arr);
            }
            cJSON_AddItemToObject(rep, "component_draw_conditions", jc);
            char* js = cJSON_PrintUnformatted(rep);
            if (js) { writeFile(g_optionReport, vector<uint8_t>(js, js + strlen(js))); free(js); }
            cJSON_Delete(rep);
            cout << "option report written: " << g_optionReport << "\n";
        }
        vector<string> hashes;
        map<int, string> midHash; // 组件序号 -> EFMI hash，用于把选项条件关联回组件
        for (auto& s : ini.order) {
            int mid0 = effiComponentMid(s);
            if (mid0 < 0) continue;
            auto it = ini.sec[s].find("hash");
            if (it == ini.sec[s].end()) continue;
            const string h = lower(it->second);
            hashes.push_back(h);
            midHash[mid0] = h;
        }
        Cat cat = g_forceCharacter.empty() ? loadCatalog(catDir, hashes)
                                           : loadCatalogByCharacter(catDir, g_forceCharacter);
        applyIdentities(cat);
        if (g_autoEntries) {
            const int added = inferEntriesByLayout(cat, dir, ini);
            if (added > 0) cout << "auto-entries: " << added << " component(s) anchored by layout fingerprint\n";
        }
        cout << "target: " << cat.character << " (" << jstr(cat.root, "name") << "), matched " << hashes.size() << " entries\n";

        // map mod component -> catalog component id
        map<int, int> mod2cat;
        for (auto& s : ini.order) {
            int mid = effiComponentMid(s);
            if (mid < 0) continue;
            auto it = ini.sec[s].find("hash"); if (it == ini.sec[s].end()) continue;
            auto h = lower(it->second); auto e = cJSON_GetObjectItemCaseSensitive(cat.entries, h.c_str());
            if (cJSON_IsNumber(e)) mod2cat[mid] = e->valueint;
            else if (cJSON_IsString(e)) mod2cat[mid] = stoi(e->valuestring);
        }

        // texture hash -> catalog name
        map<string, string> texName;
        if (cJSON_IsObject(cat.textures)) for (auto* t = cat.textures->child; t; t = t->next) {
            auto nm = get(t, "name"); if (cJSON_IsString(nm)) texName[lower(string(t->string))] = nm->valuestring;
        }
        struct TexRes { string name; Dds dds; };
        map<string, TexRes> texRes;
        // RabbitFX：纹理走自己的命名空间，资源段是 [ResourceTextureN]（无下划线），
        // 且把 catalog 用的 8 位 hash 编码在文件名里：
        //   filename = Textures/c=0-1 f=BC7_UNORM t=908140d5 d=8cb4....dds
        // 组件里用 `Resource\RabbitFX\<Slot> = ref ResourceTextureN` 绑定。
        // 这里先按 hash 解析出来，绑定仍复用下面按 catalog 名匹配的通用逻辑。
        auto rabbitFxHash = [](const string& filename) -> string {
            const string key = "t=";
            const size_t at = filename.find(key);
            if (at == string::npos) return "";
            size_t i = at + key.size(), n = 0;
            while (i + n < filename.size() && n < 8 && isxdigit((unsigned char)filename[i + n])) n++;
            return n == 8 ? lower(filename.substr(i, 8)) : string();
        };
        for (auto& s : ini.order) {
            if (s.size() <= 15 || s.compare(0, 15, "ResourceTexture") != 0) continue;
            const string suffix = s.substr(15);
            if (suffix.empty() || suffix.find_first_not_of("0123456789") != string::npos) continue;
            auto it = ini.sec[s].find("filename"); if (it == ini.sec[s].end()) continue;
            const string hash = rabbitFxHash(it->second);
            if (hash.empty()) continue;
            auto tn = texName.find(hash);
            try {
                Dds d = readTexture(dir + "/" + it->second, tn != texName.end() ? tn->second : "");
                string name = tn != texName.end() ? tn->second : "";
                if (name.empty() && g_textureFingerprint) {
                    name = catalogTextureByDescriptor(cat.textures, d.w, d.h, d.mips);
                    if (!name.empty()) cout << "texture fingerprint: " << hash << " -> " << name << "\n";
                }
                if (name.empty()) continue;
                texRes[hash] = {name, d};
            } catch (exception& e) { cerr << "texture skip " << it->second << ": " << e.what() << "\n"; }
        }
        for (auto& s : ini.order) {
            if (s.rfind("Resource_Texture", 0) != 0) continue;
            const string suffix = s.substr(strlen("Resource_Texture"));
            string hash;
            if (!suffix.empty() && suffix[0] == '_') {
                // 新 EFMI：Resource_Texture_<hash> / TextureOverride_Texture_<hash>
                hash = lower(suffix.substr(1));
            } else if (!suffix.empty() && suffix.find_first_not_of("0123456789") == string::npos) {
                auto hs = ini.sec["TextureOverride_Texture" + suffix].find("hash");
                if (hs != ini.sec["TextureOverride_Texture" + suffix].end()) hash = lower(hs->second);
            }
            if (hash.empty()) continue;
            auto it = ini.sec[s].find("filename"); if (it == ini.sec[s].end()) continue;
            auto tn = texName.find(hash);
            try {
                Dds d = readTexture(dir + "/" + it->second, tn != texName.end() ? tn->second : "");
                string name = tn != texName.end() ? tn->second : "";
                if (name.empty() && g_textureFingerprint) {
                    name = catalogTextureByDescriptor(cat.textures, d.w, d.h, d.mips);
                    if (!name.empty()) cout << "texture fingerprint: " << hash << " -> " << name << "\n";
                }
                if (name.empty()) continue;
                texRes[hash] = {name, d};
            } catch (exception& e) { cerr << "texture skip " << it->second << ": " << e.what() << "\n"; }
        }
        cout << "textures: " << texRes.size() << " matched\n";

        vector<vector<uint8_t>> payloads;
        auto addPayload = [&](vector<uint8_t> b) { payloads.push_back(std::move(b)); return (int)payloads.size() - 1; };
        map<string, int> texIndex;
        cJSON* jtextures = jarr();
        cJSON* jmeshes = jarr();
        map<int, int> meshIndex;
        vector<cJSON*> appComps;

        // global -> (owner,local)
        auto ownerOf = [&](int g, int& owner, int& local) -> bool {
            for (size_t i = 0; i < cat.ids.size(); i++) {
                int c = cat.ids[i]; auto* comp = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(c).c_str());
                int nb = cJSON_GetArraySize(get(comp, "bone_names"));
                if (g >= cat.base[i] && g < cat.base[i] + nb) { owner = c; local = g - cat.base[i]; return true; }
            }
            return false;
        };

        for (auto& kv : mod2cat) {
            int mid = kv.first, cid = kv.second;
            auto nvb0 = "Resource_Component" + to_string(mid) + "_VB0";
            auto nvb1 = "Resource_Component" + to_string(mid) + "_VB1";
            auto nvb2 = "Resource_Component" + to_string(mid) + "_VB2";
            auto nib  = "Resource_Component" + to_string(mid) + "_IB";
            if (!ini.sec.count(nvb0) || !ini.sec.count(nvb2)) { cout << "skip comp " << mid << ": no VB\n"; continue; }
            auto rd = [&](const string& sec, int& stride, vector<uint8_t>& data) {
                auto& d = ini.sec[sec];
                const string strideText = d.count("stride") ? d["stride"] : string{};
                if (strideText.empty() || strideText.find_first_not_of("0123456789") != string::npos)
                    throw runtime_error("missing/invalid stride in [" + sec + "]");
                stride = stoi(strideText);
                data = readFile(dir + "/" + d["filename"]);
            };
            int s0, s1, s2, sib; vector<uint8_t> vb0, vb1, vb2, ib;
            rd(nvb0, s0, vb0); rd(nvb1, s1, vb1); rd(nvb2, s2, vb2); rd(nib, sib, ib);
            if (s2 != 4 && s2 != 8 && s2 != 12 && s2 != 16 && s2 != 24) { cout << "skip comp " << mid << ": VB2 stride " << s2 << "\n"; continue; }
            int nv = (int)vb0.size() / s0;
            if ((int)vb2.size() != nv * s2 || (int)vb1.size() != nv * s1) { cout << "skip comp " << mid << ": buffer mismatch\n"; continue; }
            int ni = (int)ib.size() / 2;

            // catalog 的属性布局决定流 stride。mod 的 VB stride 与它不同时必须重排，
            // 否则会出现「属性声明与流 stride 不一致」的非法包（BEN validator 直接拒绝）。
            {
                int fmtSize[12] = {4,2,1,1,2,2,1,1,2,2,4,4};
                auto* compInfo = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(cid).c_str());
                auto* cattrs0 = get(compInfo, "attributes");
                int off[3] = {0,0,0};
                if (cJSON_IsArray(cattrs0)) for (auto* a = cattrs0->child; a; a = a->next) {
                    if (cJSON_GetArraySize(a) < 4) continue;
                    int sem = cJSON_GetArrayItem(a, 0)->valueint, fmt = cJSON_GetArrayItem(a, 1)->valueint;
                    int dim = cJSON_GetArrayItem(a, 2)->valueint, st = cJSON_GetArrayItem(a, 3)->valueint;
                    if (st < 0 || st > 2 || fmt < 0 || fmt > 11) continue;
                    if (sem == 12 || sem == 13) continue; // 蒙皮属性统一追加
                    off[st] += fmtSize[fmt] * dim;
                }
                const int t0 = off[0] ? off[0] : s0;
                const int t1 = off[1] ? off[1] : s1;
                if (t0 != s0 || t1 != s1) {
                    auto repack = [](const vector<uint8_t>& src, int from, int to, int count) {
                        vector<uint8_t> out((size_t)count * (size_t)to, 0);
                        const int copy = std::min(from, to);
                        for (int v = 0; v < count; v++)
                            memcpy(&out[(size_t)v * to], &src[(size_t)v * from], (size_t)copy);
                        return out;
                    };
                    if (t0 != s0) vb0 = repack(vb0, s0, t0, nv);
                    if (t1 != s1) vb1 = repack(vb1, s1, t1, nv);
                    cout << "repack comp " << mid << ": stream0 " << s0 << "->" << t0
                         << ", stream1 " << s1 << "->" << t1 << "\n";
                    s0 = t0; s1 = t1;
                }
            }

            vector<uint8_t> skin(nv * 12);
            vector<cJSON*> jbones;
            map<pair<int,int>, int> pal;
            bool ok = true;
            string skinError;
            for (int v = 0; v < nv && ok; v++) {
                uint16_t w[4] = {0,0,0,0}, gi[4] = {0,0,0,0};
                if (s2 == 16) {
                    // 合并骨架：4×UNORM16 权重 + 4×UINT16 全局骨骼 id
                    for (int k = 0; k < 4; k++) {
                        w[k] = (uint16_t)(vb2[v*16 + k*2] | (vb2[v*16 + k*2 + 1] << 8));
                        gi[k] = (uint16_t)(vb2[v*16 + 8 + k*2] | (vb2[v*16 + 8 + k*2 + 1] << 8));
                    }
                } else if (s2 == 12) {
                    // 旧 EFMI ALPHA：u16 权重 + u8 组件本地骨骼索引
                    for (int k = 0; k < 4; k++) w[k] = (uint16_t)(vb2[v*12 + k*2] | (vb2[v*12 + k*2 + 1] << 8));
                    for (int k = 0; k < 4; k++) gi[k] = vb2[v*12 + 8 + k];
                } else if (s2 == 24) {
                    // 4×FLOAT32 权重 + 4×UINT16 全局骨骼 id；权重先归一化再量化到 UNORM16。
                    float f[4];
                    memcpy(f, &vb2[v*24], 16);
                    float sum = 0;
                    for (int k = 0; k < 4; k++) {
                        if (!std::isfinite(f[k]) || f[k] < 0) f[k] = 0;
                        sum += f[k];
                    }
                    if (sum <= 1e-6f) { cout << "skip comp " << mid << ": zero skin weights\n"; ok = false; break; }
                    for (int k = 0; k < 4; k++) {
                        w[k] = (uint16_t)std::lround(std::min(1.0f, f[k] / sum) * 65535.0f);
                        gi[k] = (uint16_t)(vb2[v*24 + 16 + k*2] | (vb2[v*24 + 16 + k*2 + 1] << 8));
                    }
                } else if (s2 == 8) {
                    // Playbook：4×UINT16 全局骨骼 id 的刚性布局；首个 id 生效。
                    for (int k = 0; k < 4; k++) gi[k] = (uint16_t)(vb2[v*8 + k*2] | (vb2[v*8 + k*2 + 1] << 8));
                    w[0] = 65535;
                } else {
                    // stride 4：2×UINT16，首个是骨骼 id，其余为填充；刚性绑定。
                    gi[0] = (uint16_t)(vb2[v*4] | (vb2[v*4 + 1] << 8));
                    w[0] = 65535;
                }
                uint8_t outIdx[4] = {0,0,0,0};
                for (int k = 0; k < 4; k++) {
                    if (gi[k] == 0 && w[k] == 0) continue;
                    int owner, local;
                    if (s2 == 12) {
                        owner = cid; local = (int)gi[k];
                        auto* cc = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(cid).c_str());
                        if (local >= cJSON_GetArraySize(get(cc, "bone_names"))) {
                            skinError = "local bone " + to_string(local) + " >= target bones " +
                                to_string(cJSON_GetArraySize(get(cc, "bone_names")));
                            ok = false; break;
                        }
                    } else if (!ownerOf(gi[k], owner, local)) {
                        skinError = "global bone id " + to_string(gi[k]) + " is not in the merged target skeleton";
                        ok = false; break;
                    }
                    auto key = make_pair(owner, local);
                    auto it = pal.find(key);
                    int pi;
                    if (it == pal.end()) {
                        pi = (int)pal.size(); if (pi > 255) {
                            skinError = "bone palette exceeds 256 slots";
                            ok = false; break;
                        }
                        pal[key] = pi;
                        auto* ownerComp = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(owner).c_str());
                        auto* bn = get(ownerComp, "bone_names");
                        auto* name = cJSON_GetArrayItem(bn, local);
                        cJSON* b = obj(nullptr); cJSON_AddNumberToObject(b, "component", owner);
                        cJSON_AddNumberToObject(b, "index", local); cJSON_AddStringToObject(b, "name", name->valuestring);
                        jbones.push_back(b);
                    } else pi = it->second;
                    outIdx[k] = (uint8_t)pi;
                }
                if (!ok) break;
                for (int k = 0; k < 4; k++) { skin[v*12 + k*2] = w[k] & 255; skin[v*12 + k*2 + 1] = w[k] >> 8; }
                memcpy(&skin[v*12 + 8], outIdx, 4);
            }
            if (!ok) { cout << "skip comp " << mid << ": " << (skinError.empty() ? "palette/global index" : skinError) << "\n"; continue; }

            // attributes: catalog 4-tuples -> 5-tuples
            auto* comp = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(cid).c_str());
            cJSON* jattrs = jarr();
            int fsz[] = {4,2,1,1,2,2,1,1,2,2,4,4}; int offs[3] = {0,0,0};
            // catalog 的 attributes 可能缺失（1.7.x 的 VFS 回填只给 components 布局），
            // 直接解引用会崩；缺失时退化为不写 catalog 属性，仅保留下面强制补的两条。
            cJSON* cattrs = get(comp, "attributes");
            set<int> seenSem;
            if (cJSON_IsArray(cattrs)) for (auto* a = cattrs->child; a; a = a->next) {
                if (cJSON_GetArraySize(a) < 4) continue;
                int sem = a->child->valueint, fmt = a->child->next->valueint, dim = a->child->next->next->valueint, stream = a->child->next->next->next->valueint;
                if (stream < 0 || stream > 2) continue;
                if (fmt < 0 || fmt > 11) continue;
                // 蒙皮属性统一由下面追加：validator 要求 BLENDWEIGHTS 紧跟 BLENDINDICES
                // 且位于 stream 2 末尾，catalog 的顺序未必满足。
                if (sem == 12 || sem == 13) continue;
                if (seenSem.count(sem)) continue;
                cJSON* x = jarr(); jadd(x, jnum(sem)); jadd(x, jnum(fmt)); jadd(x, jnum(dim)); jadd(x, jnum(stream)); jadd(x, jnum(offs[stream]));
                jadd(jattrs, x); offs[stream] += fsz[fmt] * dim;
                seenSem.insert(sem);
            }
            // 只有 catalog 没给蒙皮属性时才补；否则会与自带属性重复，validator 会拒绝。
            if (!seenSem.count(12)) { cJSON* sx = jarr(); jadd(sx, jnum(12)); jadd(sx, jnum(4)); jadd(sx, jnum(4)); jadd(sx, jnum(2)); jadd(sx, jnum(offs[2])); jadd(jattrs, sx); offs[2] += 8; }
            if (!seenSem.count(13)) { cJSON* sx = jarr(); jadd(sx, jnum(13)); jadd(sx, jnum(6)); jadd(sx, jnum(4)); jadd(sx, jnum(2)); jadd(sx, jnum(offs[2])); jadd(jattrs, sx); offs[2] += 4; }

            int pvb0 = addPayload(vb0), pvb1 = addPayload(vb1), pskin = addPayload(skin), pib = addPayload(ib);
            // textures for this component
            cJSON* jtexIds = jarr(); set<int> seenTex;
            auto* tprops = get(comp, "material_texture_properties");
            if (cJSON_IsArray(tprops)) for (auto* mat = tprops->child; mat; mat = mat->next) {
                if (!cJSON_IsArray(mat)) continue;
                for (auto* pr = mat->child; pr; pr = pr->next) {
                    auto name = jstr(pr, "name"); if (name.empty()) continue;
                    int tidx = -1;
                    auto tp = texIndex.find(name);
                    if (tp != texIndex.end()) tidx = tp->second;
                    else {
                        for (auto& tr : texRes) if (tr.second.name == name) {
                            int pid = addPayload(tr.second.dds.data);
                            cJSON* t = obj(nullptr);
                            cJSON_AddNumberToObject(t, "width", tr.second.dds.w);
                            cJSON_AddNumberToObject(t, "height", tr.second.dds.h);
                            cJSON_AddNumberToObject(t, "mips", tr.second.dds.mips);
                            cJSON_AddNumberToObject(t, "format", tr.second.dds.fmt);
                            cJSON_AddBoolToObject(t, "srgb", tr.second.dds.srgb);
                            cJSON_AddStringToObject(t, "original_name", name.c_str());
                            cJSON_AddNumberToObject(t, "payload", pid);
                            jadd(jtextures, t);
                            tidx = cJSON_GetArraySize(jtextures) - 1;
                            texIndex[name] = tidx; break;
                        }
                    }
                    if (tidx >= 0 && !seenTex.count(tidx)) { seenTex.insert(tidx); jadd(jtexIds, jnum(tidx)); }
                }
            }
            cJSON* jstreams = jarr();
            auto addStream = [&](int stride, int payload) { cJSON* s = obj(nullptr); cJSON_AddNumberToObject(s, "stride", stride); cJSON_AddNumberToObject(s, "payload", payload); jadd(jstreams, s); };
            addStream(s0, pvb0); addStream(s1, pvb1); addStream(12, pskin);
            cJSON* jmesh = obj(nullptr);
            cJSON_AddNumberToObject(jmesh, "vertex_count", nv);
            cJSON_AddNumberToObject(jmesh, "index_size", 2);
            cJSON_AddItemToObject(jmesh, "streams", jstreams);
            cJSON_AddItemToObject(jmesh, "attributes", jattrs);
            cJSON* jbonesArr = jarr(); for (auto* b : jbones) jadd(jbonesArr, b); cJSON_AddItemToObject(jmesh, "bones", jbonesArr);
            cJSON_AddNumberToObject(jmesh, "indices", pib);
            cJSON_AddNumberToObject(jmesh, "index_count", ni);
            cJSON* jdraws = jarr(); cJSON* jd = obj(nullptr);
            cJSON_AddNumberToObject(jd, "start", 0); cJSON_AddNumberToObject(jd, "count", ni);
            cJSON_AddNumberToObject(jd, "material_component", cid); cJSON_AddNumberToObject(jd, "material_slot", 0);
            auto* mats = get(comp, "materials");
            auto* m0 = cJSON_IsArray(mats) ? cJSON_GetArrayItem(mats, 0) : nullptr;
            cJSON_AddStringToObject(jd, "material_name", (m0 && cJSON_IsString(m0)) ? m0->valuestring : "M_unknown");
            cJSON_AddItemToObject(jd, "textures", jtexIds); jadd(jdraws, jd);
            cJSON_AddItemToObject(jmesh, "draws", jdraws);
            meshIndex[cid] = cJSON_GetArraySize(jmeshes); jadd(jmeshes, jmesh);
            cout << "comp " << mid << "->" << cid << ": v=" << nv << " idx=" << ni << " bones=" << pal.size() << " tex=" << cJSON_GetArraySize(jtexIds) << "\n";
        }

        // target components + appearance
        cJSON* jtarget = obj(nullptr);
        cJSON_AddStringToObject(jtarget, "character_id", cat.character.c_str());
        cJSON_AddStringToObject(jtarget, "platform", "windows-x64");
        cJSON_AddStringToObject(jtarget, "profile_id", jstr(cat.root, "profile_id").c_str());
        cJSON_AddStringToObject(jtarget, "revision", jstr(cat.root, "revision").c_str());
        cJSON_AddStringToObject(jtarget, "snapshot", "");
        cJSON_AddStringToObject(jtarget, "world_resource", jstr(cat.root, "world_resource").c_str());
        cJSON_AddStringToObject(jtarget, "ui_resource", jstr(cat.root, "ui_resource").c_str());
        cJSON* jcomps = jarr();
        for (int cid : cat.ids) {
            auto* c = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(cid).c_str());
            cJSON* x = obj(nullptr); cJSON_AddNumberToObject(x, "id", cid);
            cJSON_AddStringToObject(x, "mesh_name", jstr(c, "mesh_name").c_str());
            cJSON_AddNumberToObject(x, "original_index_count", jint(c, "original_index_count"));
            cJSON* bones = jarr(); for (auto* b = get(c, "bone_names")->child; b; b = b->next) jadd(bones, jstrv(b->valuestring)); cJSON_AddItemToObject(x, "bone_names", bones);
            cJSON* mats = jarr(); for (auto* m = get(c, "materials")->child; m; m = m->next) jadd(mats, jstrv(m->valuestring)); cJSON_AddItemToObject(x, "materials", mats);
            jadd(jcomps, x);
        }
        cJSON_AddItemToObject(jtarget, "components", jcomps);

        cJSON* japps = jarr(); cJSON* japp = obj(nullptr);
        cJSON_AddStringToObject(japp, "id", "default"); cJSON_AddStringToObject(japp, "name", "默认外观");
        cJSON* jac = jarr();
        for (int cid : cat.ids) {
            cJSON* x = obj(nullptr); cJSON_AddNumberToObject(x, "target", cid);
            auto it = meshIndex.find(cid);
            if (it == meshIndex.end()) cJSON_AddStringToObject(x, "operation", "keep");
            else { cJSON_AddStringToObject(x, "operation", "replace"); cJSON_AddNumberToObject(x, "mesh", it->second); }
            jadd(jac, x);
        }
        cJSON_AddItemToObject(japp, "components", jac); jadd(japps, japp);

        bool emit11 = g_emitOptions && !optionGroups.empty();
        if (emit11) {
            // 1.1 的 attributes 必须与三条流 stride 严格吻合；catalog 缺 attributes 时
            // 无法合成合法声明，宁可不发 1.1，也不写出会被 validator 拒绝的包。
            for (auto& kv : meshIndex) {
                auto* c = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(kv.first).c_str());
                auto* a = get(c, "attributes");
                if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) == 0) {
                    cout << "emit-options: component " << kv.first
                         << " has no catalog attributes; staying on BEM 1.0\n";
                    emit11 = false;
                    break;
                }
            }
            if (emit11) {
                // validator 要求 stream stride == 属性按格式累加出来的偏移；
                // mod 的 VB stride 与 catalog 布局不一致时说明布局对不上，不硬发 1.1。
                const int sizes[12] = {4,2,1,1,2,2,1,1,2,2,4,4};
                for (auto* mesh = jmeshes->child; mesh; mesh = mesh->next) {
                    int off[3] = {0, 0, 0};
                    auto* attrs = get(mesh, "attributes");
                    if (cJSON_IsArray(attrs)) for (auto* a = attrs->child; a; a = a->next) {
                        if (cJSON_GetArraySize(a) < 5) continue;
                        int fmt = cJSON_GetArrayItem(a, 1)->valueint;
                        int dim = cJSON_GetArrayItem(a, 2)->valueint;
                        int st = cJSON_GetArrayItem(a, 3)->valueint;
                        if (st >= 0 && st < 3 && fmt >= 0 && fmt < 12) off[st] += sizes[fmt] * dim;
                    }
                    auto* streams = get(mesh, "streams");
                    for (int i = 0; i < 3 && cJSON_IsArray(streams); i++) {
                        auto* s = cJSON_GetArrayItem(streams, i);
                        if (s && jint(s, "stride") != off[i]) {
                            cout << "emit-options: stream " << i << " stride " << jint(s, "stride")
                                 << " != attribute layout " << off[i] << "; staying on BEM 1.0\n";
                            emit11 = false;
                            break;
                        }
                    }
                    if (!emit11) break;
                }
            }
        }
        cJSON* mroot = obj(nullptr);
        cJSON_AddNumberToObject(mroot, "schema", 1);
        cJSON_AddStringToObject(mroot, "package_id", ("c4ben." + string(cat.character)).c_str());
        cJSON_AddStringToObject(mroot, "name", ("Converter4BENeXt: " + fs::path(dir).filename().string()).c_str());
        cJSON_AddStringToObject(mroot, "author", "Converter4BENeXt");
        cJSON_AddStringToObject(mroot, "version", "0.1.0");
        cJSON* caps = jarr(); for (auto s : {"native-materials", "palette-u8", "indices-u32", "fixed-appearances"}) jadd(caps, jstrv(s));
        cJSON_AddItemToObject(mroot, "required_capabilities", caps);
        cJSON_AddItemToObject(mroot, "target", jtarget);
        requireNonEmptyOutput(jmeshes, jtextures, out);
        cJSON_AddItemToObject(mroot, "meshes", jmeshes);
        cJSON_AddItemToObject(mroot, "textures", jtextures);
        cJSON_AddStringToObject(mroot, "default_appearance_id", "default");
        cJSON_AddItemToObject(mroot, "appearances", japps);

        // ---------- BEM 1.1：组合选项（--emit-options）----------
        if (emit11) {
            cJSON_DeleteItemFromObject(mroot, "default_appearance_id");
            cJSON_DeleteItemFromObject(mroot, "appearances");
            cJSON_DeleteItemFromObject(mroot, "required_capabilities");
            cJSON* caps11 = jarr();
            for (auto s : {"native-materials", "palette-u8", "indices-u32", "composable-options"}) jadd(caps11, jstrv(s));
            cJSON_AddItemToObject(mroot, "required_capabilities", caps11);

            // 1.1 的索引挂在 draw 上；本转换器每个 mesh 只有一条 draw，直接搬过去。
            for (auto* mesh = jmeshes->child; mesh; mesh = mesh->next) {
                auto* draws = get(mesh, "draws");
                auto* idx = get(mesh, "indices");
                auto* cnt = get(mesh, "index_count");
                if (cJSON_IsArray(draws) && cJSON_GetArraySize(draws) == 1 && idx && cnt) {
                    auto* d0 = cJSON_GetArrayItem(draws, 0);
                    // 1.0 的 draw 已经带 start/count，直接再加会重复键；先删再写。
                    cJSON_DeleteItemFromObject(d0, "start");
                    cJSON_DeleteItemFromObject(d0, "count");
                    cJSON_DeleteItemFromObject(d0, "indices");
                    cJSON_AddItemToObject(d0, "indices", cJSON_Duplicate(idx, 1));
                    cJSON_AddItemToObject(d0, "count", cJSON_Duplicate(cnt, 1));
                    cJSON_DeleteItemFromObject(mesh, "indices");
                    // 保留 mesh 级 index_count：BEN Rust 校验器（native/bem/src/lib.rs:346）
                    // 无条件要求它；bem_tool.py 的 1.1 路径不要求、但多余字段不校验。
                    // 保留后 Rust 能读 index_count、又因 mesh.indices 已删除而跳过长度检查；
                    // draw 级 indices/count 继续输出，因此两份校验器都满足。
                    // 注意 BEM_V1_1_SPEC 说「不再用网格级 indices 和固定 index_count」，
                    // 这里保留 index_count 是实测兼容取舍，不是重新引入 mesh.indices。
                }
            }

            map<string, string> gidOf;                // $swapvar_x -> group id
            map<string, map<int, string>> choiceOf;   // group id -> value -> choice id
            cJSON* jgroups = jarr();
            for (size_t gi = 0; gi < optionGroups.size(); gi++) {
                string gid = optionGroups[gi].first;
                if (!gid.empty() && gid[0] == '$') gid.erase(0, 1);
                for (auto& ch : gid) if (!(isalnum((unsigned char)ch) || ch == '_' || ch == '.' || ch == '-')) ch = '_';
                while (!gid.empty() && !isalnum((unsigned char)gid[0])) gid.erase(0, 1);
                while (!gid.empty() && gid.back() == '_') gid.pop_back();
                if (gid.empty()) gid = "group" + to_string(gi);
                gidOf[optionGroups[gi].first] = gid;
                cJSON* g = obj(nullptr);
                cJSON_AddStringToObject(g, "id", gid.c_str());
                cJSON_AddStringToObject(g, "name", gid.c_str());
                cJSON* jch = jarr();
                string def;
                for (int v : optionGroups[gi].second) {
                    const string cidv = (v >= 0) ? ("v" + to_string(v)) : ("vm" + to_string(-v));
                    cJSON* c = obj(nullptr);
                    cJSON_AddStringToObject(c, "id", cidv.c_str());
                    cJSON_AddStringToObject(c, "name", cidv.c_str());
                    jadd(jch, c);
                    choiceOf[gid][v] = cidv;
                    if (def.empty() || v == 0) def = cidv; // EFMI 的 persist 默认通常是 0
                }
                if (def.empty() && !optionGroups[gi].second.empty())
                    def = choiceOf[gid][optionGroups[gi].second[0]];
                cJSON_AddStringToObject(g, "default", def.c_str());
                cJSON_AddItemToObject(g, "choices", jch);
                jadd(jgroups, g);
            }
            cJSON_AddItemToObject(mroot, "option_groups", jgroups);

            // 组件 -> 条件（用模组组件 hash 反查 $draw_* 条件）
            map<int, vector<EfmiDrawCond>> cidConds;
            for (auto& kv : mod2cat) {
                auto h = midHash.find(kv.first);
                if (h == midHash.end()) continue;
                auto dc = drawConds.find(h->second);
                if (dc == drawConds.end() || dc->second.empty()) continue;
                if (!cidConds.count(kv.second)) cidConds[kv.second] = dc->second;
            }

            cJSON* jrules = jarr();
            for (size_t ti = 0; ti < cat.ids.size(); ti++) {
                const int cid = cat.ids[ti];
                cJSON* rule = obj(nullptr);
                cJSON_AddNumberToObject(rule, "target", (int)ti);
                cJSON* cands = jarr();
                auto mi = meshIndex.find(cid);
                bool gated = false;
                if (mi != meshIndex.end()) {
                    auto ci = cidConds.find(cid);
                    if (ci != cidConds.end()) {
                        for (auto& cond : ci->second) {
                            auto gk = gidOf.find(cond.group);
                            if (gk == gidOf.end()) continue;
                            auto& byVal = choiceOf[gk->second];
                            auto vk = byVal.find(cond.value);
                            if (vk == byVal.end()) continue;
                            cJSON* c1 = obj(nullptr);
                            cJSON* when = obj(nullptr);
                            cJSON* eq = jarr(); jadd(eq, jstrv(gk->second)); jadd(eq, jstrv(vk->second));
                            cJSON_AddItemToObject(when, "eq", eq);
                            cJSON_AddItemToObject(c1, "when", when);
                            cJSON_AddStringToObject(c1, "operation", "replace");
                            cJSON_AddNumberToObject(c1, "mesh", mi->second);
                            jadd(cands, c1);

                            cJSON* c2 = obj(nullptr);
                            cJSON* neg = obj(nullptr); cJSON* inner = obj(nullptr);
                            cJSON* eq2 = jarr(); jadd(eq2, jstrv(gk->second)); jadd(eq2, jstrv(vk->second));
                            cJSON_AddItemToObject(inner, "eq", eq2);
                            cJSON_AddItemToObject(neg, "not", inner);
                            cJSON_AddItemToObject(c2, "when", neg);
                            cJSON_AddStringToObject(c2, "operation", "keep");
                            jadd(cands, c2);
                            gated = true;
                            break;
                        }
                    }
                }
                if (!gated) {
                    cJSON* c = obj(nullptr);
                    if (mi != meshIndex.end()) {
                        cJSON_AddStringToObject(c, "operation", "replace");
                        cJSON_AddNumberToObject(c, "mesh", mi->second);
                    } else {
                        cJSON_AddStringToObject(c, "operation", "keep");
                    }
                    jadd(cands, c);
                }
                cJSON_AddItemToObject(rule, "candidates", cands);
                jadd(jrules, rule);
            }
            cJSON_AddItemToObject(mroot, "component_rules", jrules);
            cout << "emitting BEM 1.1: " << cJSON_GetArraySize(jgroups) << " option group(s), "
                 << cJSON_GetArraySize(jrules) << " component rule(s)\n";
        }

        char* json = cJSON_PrintUnformatted(mroot);
        if (!json) throw runtime_error("manifest serialize failed");
        vector<uint8_t> manifest((uint8_t*)json, (uint8_t*)json + strlen(json));
        free(json);

        vector<Entry> es; es.push_back({"manifest.json", manifest, 0, 0});
        for (size_t i = 0; i < payloads.size(); i++) {
            char nm[64]; snprintf(nm, sizeof nm, "payloads/%04zu.bin", i);
            es.push_back({nm, payloads[i], 0, 0});
        }
        if (out.size() >= 4 && string(out).substr(string(out).size() - 4) == ".zip") writeZip(out, es);
        else writeBem(out, manifest, payloads, emit11 ? 1 : 0);
        cout << "written " << out << ": meshes=" << cJSON_GetArraySize(jmeshes) << " textures=" << cJSON_GetArraySize(jtextures) << " payloads=" << payloads.size() << "\n";
        cJSON_Delete(mroot); cJSON_Delete(cat.root);
        return 0;
}

// ============================ 压缩包输入 ============================
// mod 包在发布时基本都是 .zip/.7z/.rar。这里先支持 .zip（zlib 已链接，无新依赖）：
// 解压到临时目录后走原有流程。解压目录会在进程结束时清理。
static uint32_t zr16(const vector<uint8_t>& b, size_t o) { return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8); }
static uint32_t zr32(const vector<uint8_t>& b, size_t o) {
    return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}

static bool looksLikeZip(const string& path, const vector<uint8_t>& head) {
    if (head.size() >= 4 && head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4) return true;
    const string p = lower(path);
    return p.size() > 4 && p.compare(p.size() - 4, 4, ".zip") == 0;
}

static vector<uint8_t> inflateRaw(const uint8_t* src, size_t srcLen, size_t expect) {
    vector<uint8_t> out(expect ? expect : 1);
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) throw runtime_error("zlib inflateInit failed");
    zs.next_in = const_cast<Bytef*>(src); zs.avail_in = (uInt)srcLen;
    zs.next_out = out.data(); zs.avail_out = (uInt)out.size();
    int rc = inflate(&zs, Z_FINISH);
    const size_t produced = out.size() - zs.avail_out;
    inflateEnd(&zs);
    if (rc != Z_STREAM_END && rc != Z_OK) throw runtime_error("zip deflate stream corrupt");
    out.resize(produced);
    return out;
}

// 解压 zip 到 outDir；返回包含 mod.ini 的最浅目录（相对 outDir），找不到返回空。
static string unzipTo(const string& zipPath, const string& outDir) {
    const auto b = readFile(zipPath);
    if (b.size() < 22) throw runtime_error("zip too small: " + zipPath);

    // End of central directory：在最后 64KB 内回扫
    size_t eocd = string::npos;
    const size_t scanFrom = b.size() > 65558 ? b.size() - 65558 : 0;
    for (size_t i = b.size() - 22 + 1; i-- > scanFrom;) {
        if (zr32(b, i) == 0x06054b50u) { eocd = i; break; }
    }
    if (eocd == string::npos) throw runtime_error("zip: end-of-central-directory not found: " + zipPath);
    const uint32_t count = zr16(b, eocd + 10);
    const uint32_t cdOff = zr32(b, eocd + 16);
    if (cdOff >= b.size()) throw runtime_error("zip: bad central directory offset");

    string modIniDir;
    vector<string> modIniHits;
    size_t p = cdOff;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 46 > b.size() || zr32(b, p) != 0x02014b50u) throw runtime_error("zip: bad central directory entry");
        const uint32_t method = zr16(b, p + 10);
        const uint32_t csize = zr32(b, p + 20);
        const uint32_t usize = zr32(b, p + 24);
        const uint32_t nlen = zr16(b, p + 28);
        const uint32_t elen = zr16(b, p + 30);
        const uint32_t clen = zr16(b, p + 32);
        const uint32_t lho = zr32(b, p + 42);
        const string name(b.begin() + p + 46, b.begin() + p + 46 + nlen);
        p += 46 + nlen + elen + clen;

        if (!name.empty() && (name.back() == '/' || name.back() == '\\')) continue; // 目录项
        // 与 BEN 的 Source 一致：整包内路径必须相对，不允许越界或盘符
        if (name.find("..") != string::npos) throw runtime_error("zip: refusing entry with '..': " + name);
        if (!name.empty() && (name[0] == '/' || name[0] == '\\')) throw runtime_error("zip: refusing absolute entry: " + name);
        if (name.size() > 1 && name[1] == ':') throw runtime_error("zip: refusing drive-letter entry: " + name);
        if (lho + 30 > b.size() || zr32(b, lho) != 0x04034b50u) throw runtime_error("zip: bad local header for " + name);
        const uint32_t lnlen = zr16(b, lho + 26), lelen = zr16(b, lho + 28);
        const size_t dataOff = lho + 30 + lnlen + lelen;
        if (dataOff + csize > b.size()) throw runtime_error("zip: truncated data for " + name);

        vector<uint8_t> data;
        if (method == 0) data.assign(b.begin() + dataOff, b.begin() + dataOff + csize);
        else if (method == 8) data = inflateRaw(b.data() + dataOff, csize, usize);
        else throw runtime_error("zip: unsupported compression method " + to_string(method) + " for " + name);

        const string dest = outDir + "/" + name;
        fs::create_directories(fs::path(dest).parent_path());
        writeFile(dest, data);

        // 只认精确的 mod.ini 文件名（DISABLED_mod.ini 不算），出现多处则报歧义
        const string ln = lower(name);
        if (ln.size() >= 7 && ln.compare(ln.size() - 7, 7, "mod.ini") == 0) {
            const string base = name.substr(name.find_last_of('/') == string::npos ? 0 : name.find_last_of('/') + 1);
            const string baseLower = lower(base);
            if (baseLower == "mod.ini") {
                modIniHits.push_back(name);
                const string dir = name.substr(0, name.size() - base.size());
                if (modIniDir.empty() || dir.size() < modIniDir.size()) modIniDir = dir;
            }
        }
    }
    if (modIniHits.size() > 1) {
        string list;
        for (size_t i = 0; i < modIniHits.size() && i < 8; i++) list += "\n  " + modIniHits[i];
        throw runtime_error("zip: mod.ini 在包内有 " + to_string(modIniHits.size()) + " 处，无法确定目标包:" + list);
    }
    return modIniDir; // 可能是 ""（mod.ini 在根）
}

#ifdef C4BEN_HAVE_LIBARCHIVE
// 7z / rar（含 RAR5）走 libarchive（BSD-2，可同时读两种格式）。
// 解压语义与 zip 路径一致：拒绝越界条目、精确匹配 mod.ini、重复则报歧义。
static string unarchiveTo(const string& path, const string& outDir) {
    struct archive* a = archive_read_new();
    if (!a) throw runtime_error("archive_read_new failed");
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, path.c_str(), 1 << 16) != ARCHIVE_OK) {
        const string msg = archive_error_string(a) ? archive_error_string(a) : "unknown error";
        archive_read_free(a);
        throw runtime_error("archive open failed: " + path + ": " + msg);
    }

    vector<string> modIniHits;
    string modIniDir;
    struct archive_entry* e = nullptr;
    vector<char> buf(1 << 16);
    while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
        const char* pn = archive_entry_pathname(e);
        if (!pn) { archive_read_data_skip(a); continue; }
        string name = pn;
        for (auto& ch : name) if (ch == '\\') ch = '/';
        // 只解普通文件：目录、符号链接、设备等一律跳过（避免符号链接穿越）
        if (archive_entry_filetype(e) != AE_IFREG) { archive_read_data_skip(a); continue; }
        if (!name.empty() && name.back() == '/') { archive_read_data_skip(a); continue; }
        if (name.find("..") != string::npos) { archive_read_free(a); throw runtime_error("archive: refusing entry with '..': " + name); }
        if (!name.empty() && name[0] == '/') { archive_read_free(a); throw runtime_error("archive: refusing absolute entry: " + name); }
        if (name.size() > 1 && name[1] == ':') { archive_read_free(a); throw runtime_error("archive: refusing drive-letter entry: " + name); }

        const string dest = outDir + "/" + name;
        fs::create_directories(fs::path(dest).parent_path());
        ofstream f(dest, ios::binary);
        if (!f) { archive_read_free(a); throw runtime_error("write failed: " + dest); }
        for (;;) {
            const la_ssize_t n = archive_read_data(a, buf.data(), buf.size());
            if (n > 0) f.write(buf.data(), (streamsize)n);
            else if (n == 0) break;
            else { archive_read_free(a); throw runtime_error("archive: read data failed for " + name); }
        }
        f.close();

        const string ln = lower(name);
        if (ln.size() >= 7 && ln.compare(ln.size() - 7, 7, "mod.ini") == 0) {
            const size_t slash = name.find_last_of('/');
            const string base = slash == string::npos ? name : name.substr(slash + 1);
            if (lower(base) == "mod.ini") {
                modIniHits.push_back(name);
                const string dir = name.substr(0, name.size() - base.size());
                if (modIniDir.empty() || dir.size() < modIniDir.size()) modIniDir = dir;
            }
        }
    }
    archive_read_free(a);
    if (modIniHits.size() > 1) {
        string list;
        for (size_t i = 0; i < modIniHits.size() && i < 8; i++) list += "\n  " + modIniHits[i];
        throw runtime_error("archive: mod.ini 在包内有 " + to_string(modIniHits.size()) + " 处，无法确定目标包:" + list);
    }
    return modIniDir;
}
#endif  /* C4BEN_HAVE_LIBARCHIVE */

static string g_extractedRoot; // 进程结束前清理的解压目录
static void cleanupExtracted() {
    if (g_extractedRoot.empty()) return;
    std::error_code ec;
    fs::remove_all(g_extractedRoot, ec);
}

// 输入是压缩包时解压到临时目录，并把 dir 指向含 mod.ini 的根目录。
// 语义与 BEN tools/CustomModel 的 Source 一致：目录与 .zip 直接读，
// .rar/.7z 需要外部解包后端，这里明确拒绝而不是半读。
static void resolveArchiveInput(string& dir) {
    std::error_code ec;
    if (!fs::is_regular_file(dir, ec)) return; // 普通目录，保持原行为
    const string lowerPath = lower(dir);
    auto endsWith = [&](const char* ext) {
        const size_t n = strlen(ext);
        return lowerPath.size() >= n && lowerPath.compare(lowerPath.size() - n, n, ext) == 0;
    };
    vector<uint8_t> head(8);
    { ifstream f(dir, ios::binary); if (f) { f.read((char*)head.data(), (streamsize)head.size()); head.resize((size_t)f.gcount()); } }
    if (!looksLikeZip(dir, head)) {
        const bool rar4 = head.size() >= 7 && head[0] == 'R' && head[1] == 'a' && head[2] == 'r' && head[3] == '!' &&
                          head[4] == 0x1A && head[5] == 0x07 && head[6] == 0x00;
        const bool rar5 = head.size() >= 8 && head[0] == 'R' && head[1] == 'a' && head[2] == 'r' && head[3] == '!' &&
                          head[4] == 0x1A && head[5] == 0x07 && head[6] == 0x01 && head[7] == 0x00;
        const bool sevenZ = head.size() >= 6 && head[0] == '7' && head[1] == 'z' && head[2] == 0xBC &&
                            head[3] == 0xAF && head[4] == 0x27 && head[5] == 0x1C;
        const bool archiveByName = endsWith(".rar") || endsWith(".7z");
        if (rar4 || rar5 || sevenZ || archiveByName) {
#ifdef C4BEN_HAVE_LIBARCHIVE
            const string stamp = to_string((unsigned long long)chrono::steady_clock::now().time_since_epoch().count());
            const string tmp = (fs::temp_directory_path() / ("c4ben-" + stamp)).string();
            fs::create_directories(tmp);
            g_extractedRoot = tmp;
            const string sub = unarchiveTo(dir, tmp);
            dir = sub.empty() ? tmp : (tmp + "/" + sub);
            cerr << "extracted " << dir << "\n";
            return;
#else
            throw runtime_error("ARCHIVE_BACKEND: 构建时未找到 libarchive，无法读取 .rar/.7z；"
                                "请安装 libarchive 开发包后重新编译，或提供 .zip / 已解包的目录: " + dir);
#endif
        }
        throw runtime_error("source must be an EFMI mod folder, .zip, .rar or .7z: " + dir);
    }

    const string stamp = to_string((unsigned long long)chrono::steady_clock::now().time_since_epoch().count());
    const string tmp = (fs::temp_directory_path() / ("c4ben-" + stamp)).string();
    fs::create_directories(tmp);
    g_extractedRoot = tmp;
    const string sub = unzipTo(dir, tmp);
    dir = sub.empty() ? tmp : (tmp + "/" + sub);
    cerr << "extracted " << dir << "\n";
}

// ============================ 合成 EFMI 固定件 ============================
// 回归用：按 catalog 的真实 hash 生成最小 EFMI 包。几何是程序生成的网格，贴图是
// 常量色 BC 块，不读取任何游戏资源，可在 CI 里重复生成。
static void fxU32(vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; i++) o.push_back((uint8_t)((v >> (8 * i)) & 255));
}

static vector<uint8_t> fxDds(const string& fam, int w, int h) {
    vector<uint8_t> o;
    const size_t blocks = (size_t)((w + 3) / 4) * (size_t)((h + 3) / 4);
    const bool dx10 = (fam == "BC5" || fam == "BC7");
    o.push_back('D'); o.push_back('D'); o.push_back('S'); o.push_back(' ');
    fxU32(o, 124); fxU32(o, 0x1u | 0x2u | 0x4u | 0x1000u | 0x80000u);
    fxU32(o, (uint32_t)h); fxU32(o, (uint32_t)w); fxU32(o, 0); fxU32(o, 0); fxU32(o, 1);
    for (int i = 0; i < 11; i++) fxU32(o, 0);
    fxU32(o, 32); fxU32(o, 4);
    const string four = fam == "BC1" ? "DXT1" : fam == "BC2" ? "DXT3" : fam == "BC3" ? "DXT5" : "DX10";
    o.insert(o.end(), four.begin(), four.end());
    for (int i = 0; i < 5; i++) fxU32(o, 0);
    fxU32(o, 0x1000); for (int i = 0; i < 4; i++) fxU32(o, 0);
    if (dx10) { fxU32(o, fam == "BC5" ? 83u : 98u); fxU32(o, 3); fxU32(o, 0); fxU32(o, 1); fxU32(o, 0); }

    const uint32_t color = ((200u >> 3) << 11) | ((120u >> 2) << 5) | (60u >> 3);
    const uint32_t cpair = color | (color << 16);
    for (size_t i = 0; i < blocks; i++) {
        if (fam == "BC1") { fxU32(o, cpair); fxU32(o, 0); }
        else if (fam == "BC2" || fam == "BC3") { fxU32(o, 0xFFFFFFFFu); fxU32(o, 0xFFFFFFFFu); fxU32(o, cpair); fxU32(o, 0); }
        else if (fam == "BC5") { for (int k = 0; k < 4; k++) fxU32(o, 0xFFFFFFFFu); }
        else { fxU32(o, 0x40u); fxU32(o, 0); fxU32(o, 0); fxU32(o, 0); }
    }
    return o;
}

static int genFixture(const string& outDir, const string& catDir, const string& character) {
    Cat cat = loadCatalogByCharacter(catDir, character);
    if (!cJSON_IsObject(cat.entries)) throw runtime_error("catalog has no entries map");
    string compHash; int cid = -1;
    for (auto* e = cat.entries->child; e; e = e->next) {
        if (!e->string) continue;
        compHash = lower(string(e->string)); cid = (int)e->valueint; break;
    }
    if (cid < 0 || compHash.empty()) throw runtime_error("catalog entries is empty");
    cJSON* comp = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(cid).c_str());
    if (!comp) throw runtime_error("catalog component " + to_string(cid) + " missing");
    const int vcount = jint(comp, "vertex_count");
    int icount = 0;
    cJSON* subs = get(comp, "submeshes");
    if (cJSON_IsArray(subs) && cJSON_GetArraySize(subs) > 0) icount = jint(cJSON_GetArrayItem(subs, 0), "index_count");
    if (vcount <= 0 || icount <= 0) throw runtime_error("component has no usable geometry counts");

    vector<int> strides;
    cJSON* streams = get(comp, "source_streams");
    if (cJSON_IsArray(streams)) for (auto* st = streams->child; st; st = st->next) {
        int s = 0; cJSON* sem = get(st, "semantics");
        if (cJSON_IsArray(sem)) for (auto* q = sem->child; q; q = q->next) s += jint(q, "stride");
        strides.push_back(s);
    }
    while (strides.size() < 3) strides.push_back(strides.empty() ? 16 : 12);

    // 组件实际绑定的贴图名，反查 catalog 里的 hash
    map<string, string> nameToHash;
    if (cJSON_IsObject(cat.textures)) for (auto* t = cat.textures->child; t; t = t->next) {
        const string nm = jstr(t, "name");
        if (!nm.empty() && !nameToHash.count(nm)) nameToHash[nm] = lower(string(t->string));
    }
    vector<string> bound;
    cJSON* mats = get(comp, "material_textures");
    if (cJSON_IsArray(mats)) for (auto* grp = mats->child; grp; grp = grp->next) {
        if (!cJSON_IsArray(grp)) continue;
        for (auto* nm = grp->child; nm; nm = nm->next) {
            if (!cJSON_IsString(nm)) continue;
            auto it = nameToHash.find(nm->valuestring);
            if (it != nameToHash.end() && find(bound.begin(), bound.end(), it->second) == bound.end()) bound.push_back(it->second);
        }
    }
    if (bound.empty() && cJSON_IsObject(cat.textures))
        for (auto* t = cat.textures->child; t && bound.size() < 5; t = t->next) bound.push_back(lower(string(t->string)));

    vector<uint8_t> vb0, vb1, vb2, ib;
    for (int i = 0; i < vcount; i++) {
        const float x = (float)(i % 64) * 0.1f, y = (float)((i / 64) % 64) * 0.1f;
        const float p3[3] = {x, y, 0.0f};
        vb0.insert(vb0.end(), (const uint8_t*)p3, (const uint8_t*)p3 + 12);
        fxU32(vb0, 0); // ENCODEDDATA
        const float uv[2] = {(float)(i % 64) / 64.0f, (float)((i / 64) % 64) / 64.0f};
        vb1.insert(vb1.end(), (const uint8_t*)uv, (const uint8_t*)uv + 8);
        for (int k = 0; k < 4; k++) vb1.push_back(255); // COLOR
        vb2.push_back(255); vb2.push_back(255);         // weight0 = 65535
        for (int k = 0; k < 6; k++) vb2.push_back(0);
        for (int k = 0; k < 4; k++) vb2.push_back(0);   // bone indices
    }
    for (int i = 0; i < icount; i++) {
        const uint16_t v = (uint16_t)(i % vcount);
        ib.push_back((uint8_t)(v & 255)); ib.push_back((uint8_t)(v >> 8));
    }

    fs::create_directories(outDir + "/Meshes");
    fs::create_directories(outDir + "/Textures");
    writeFile(outDir + "/Meshes/vb0.buf", vb0);
    writeFile(outDir + "/Meshes/vb1.buf", vb1);
    writeFile(outDir + "/Meshes/vb2.buf", vb2);
    writeFile(outDir + "/Meshes/ib.buf", ib);

    const char* fams[5] = {"BC1", "BC2", "BC3", "BC5", "BC7"};
    vector<pair<string, string>> texOut;
    for (size_t i = 0; i < bound.size(); i++) {
        const string fam = fams[i % 5];
        writeFile(outDir + "/Textures/" + bound[i] + ".dds", fxDds(fam, 64, 64));
        texOut.push_back(make_pair(bound[i], fam));
    }

    string ini;
    ini += "[Mod]\nnamespace = c4ben-fixture\n\n";
    ini += "[TextureOverride_EntryPoint_Component" + to_string(cid) + "]\n";
    ini += "hash = " + compHash + "\n";
    ini += "match_first_index = 0\n";
    ini += "match_index_count = " + to_string(icount) + "\n";
    ini += "drawindexed = " + to_string(icount) + ", 0, 0\n\n";
    const char* names[3] = {"VB0", "VB1", "VB2"};
    for (int i = 0; i < 3; i++) {
        ini += string("[Resource_Component") + to_string(cid) + "_" + names[i] + "]\n";
        ini += "stride = " + to_string(strides[i]) + "\n";
        ini += string("filename = Meshes/") + lower(string(names[i])) + ".buf\n\n";
    }
    ini += "[Resource_Component" + to_string(cid) + "_IB]\nstride = 2\nfilename = Meshes/ib.buf\n\n";
    for (size_t i = 0; i < texOut.size(); i++) {
        ini += "[Resource_Texture_" + texOut[i].first + "]\nfilename = Textures/" + texOut[i].first + ".dds\n\n";
        ini += "[TextureOverride_Texture_" + texOut[i].first + "]\nhash = " + texOut[i].first +
               "\nps-t0 = Resource_Texture_" + texOut[i].first + "\n\n";
    }
    writeFile(outDir + "/mod.ini", vector<uint8_t>(ini.begin(), ini.end()));

    cout << "fixture written to " << outDir << ": component=" << cid << " hash=" << compHash
         << " v=" << vcount << " idx=" << icount << " textures=" << texOut.size() << "\n";
    for (size_t i = 0; i < texOut.size(); i++) cout << "  " << texOut[i].first << " " << texOut[i].second << "\n";
    cJSON_Delete(cat.root);
    return 0;
}


#ifndef C4BEN_NO_MAIN
int main(int argc, char** argv) {
    atexit(cleanupExtracted);
    try {
        if (argc < 2 || (string(argv[1]) != "convert" && string(argv[1]) != "inspect-legacy" &&
                         string(argv[1]) != "map" && string(argv[1]) != "fixture")) {
            cerr << "usage: c4ben convert <mod-dir> --catalog <catalog-dir> --out <out.bem>\n"
                    "       c4ben inspect-legacy|map <old-mod-dir> --catalog <dir> --character <chr_...> [--out mapping.json]\n"
                    "       c4ben convert <old-mod-dir> --catalog <dir> --character <chr_...> --legacy-auto --out <out.bem>\n"
                    "       c4ben fixture <out-dir> --catalog <dir> --character <chr_...>   (合成回归包，不含游戏资源)\n"
                    "       add --texture-fingerprint to map hash-drifted DDS by unique descriptor\n"; return 2;
        }
        if (string(argv[1]) == "fixture") {
            string outDir, catDir, character;
            for (int i = 2; i < argc; i++) {
                string a = argv[i];
                if (a == "--catalog" && i + 1 < argc) catDir = argv[++i];
                else if (a == "--character" && i + 1 < argc) character = argv[++i];
                else outDir = a;
            }
            if (outDir.empty() || catDir.empty() || character.empty()) {
                cerr << "usage: c4ben fixture <out-dir> --catalog <dir> --character <chr_...>\n"; return 2;
            }
            return genFixture(outDir, catDir, character);
        }
        if (string(argv[1]) == "inspect-legacy" || string(argv[1]) == "map") {
            string dir, catDir, character, outPath;
            for (int i = 2; i < argc; i++) {
                string a = argv[i];
                if (a == "--catalog" && i + 1 < argc) catDir = argv[++i];
                else if (a == "--character" && i + 1 < argc) { character = argv[++i]; g_forceCharacter = character; }
                else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
                else dir = a;
            }
            if (dir.empty() || catDir.empty()) { cerr << "missing mod/catalog\n"; return 2; }
            resolveArchiveInput(dir);
            return legacyInspect(dir, catDir, character, outPath);
        }
        string dir, catDir, out, profile, character, skeletonPath;
        bool allowReview = false, legacyAuto = false;
        for (int i = 2; i < argc; i++) {
            string a = argv[i];
            if (a == "--catalog" && i + 1 < argc) catDir = argv[++i];
            else if (a == "--out" && i + 1 < argc) out = argv[++i];
            else if (a == "--profile" && i + 1 < argc) profile = argv[++i];
            else if (a == "--character" && i + 1 < argc) character = argv[++i];
            else if (a == "--allow-review") allowReview = true;
            else if (a == "--legacy-auto") legacyAuto = true;
            else if (a == "--texture-fingerprint") g_textureFingerprint = true;
            else if (a == "--texture-compress") g_textureCompress = true;
            else if (a == "--auto-entries") g_autoEntries = true;
            else if (a == "--option-report" && i + 1 < argc) g_optionReport = argv[++i];
            else if (a == "--emit-options") g_emitOptions = true;
            else if (a == "--compat-layout") g_compatLayout = true;
            else if (a == "--compat-bones") g_compatBones = true;
            else if (a == "--skeleton" && i + 1 < argc) skeletonPath = argv[++i];
            else if (a == "--identities" && i + 1 < argc) {
                auto bytes = readFile(argv[++i]);
                g_identities = cJSON_ParseWithLength((const char*)bytes.data(), bytes.size());
                if (!g_identities) { cerr << "identities JSON parse failed\n"; return 2; }
            }
            else dir = a;
        }
        if (dir.empty() || catDir.empty() || out.empty()) { cerr << "missing args\n"; return 2; }
        resolveArchiveInput(dir);
        if (!skeletonPath.empty()) loadSkeletonTable(skeletonPath);
        if (!character.empty()) g_forceCharacter = character;
        if (!profile.empty()) return convertLegacyProfile(dir, catDir, profile, out, allowReview);
        if (legacyAuto && !character.empty()) {
            try { return convertEfmi(dir, catDir, out); }
            catch (exception& first) {
                cerr << "catalog conversion failed (" << first.what() << "); trying legacy profile for " << character << "\n";
                string mapping = out + ".mapping.json";
                int rc = legacyInspect(dir, catDir, character, mapping);
                if (rc != 0) throw;
                cerr << "legacy mapping written: " << mapping << "\n";
                return convertLegacyProfile(dir, catDir, mapping, out, true);
            }
        }
        return convertEfmi(dir, catDir, out);
    } catch (exception& e) { cerr << "ERROR: " << e.what() << "\n"; return 1; }
}

#endif  /* C4BEN_NO_MAIN */

#ifdef C4BEN_NO_MAIN
// 供 BEN/ForTMR4A 导入链复用的 C ABI。与 CLI 共用同一套转换实现。
extern "C" {

static void c4ben_set_error(char* err, unsigned cap, const string& message) {
    if (!err || cap == 0) return;
    unsigned n = (unsigned)message.size();
    if (n >= cap) n = cap - 1;
    memcpy(err, message.data(), n);
    err[n] = 0;
}

// 0 = 不是已知 mod 目录；1 = EFMI；2 = EFF format 3。
int c4ben_probe(const char* dir) {
    if (!dir || !*dir) return 0;
    try {
        string base(dir);
        if (!fs::is_directory(base)) return 0;
        string iniPath = base + "/mod.ini";
        if (!fs::is_regular_file(iniPath)) return 0;
        auto ini = Ini::parse(iniPath);
        if (effKey(ini, "Mod", "format") == "3") return 2;
        for (auto& s : ini.order) {
            if (effiComponentMid(s) >= 0) return 1;
        }
    } catch (...) {
    }
    return 0;
}

int c4ben_version(void) { return 1; }

int c4ben_convert(const char* dir, const char* catalog_dir, const char* out_path,
                  char* err, unsigned err_cap) {
    if (!dir || !*dir || !catalog_dir || !*catalog_dir || !out_path || !*out_path) {
        c4ben_set_error(err, err_cap, "missing dir/catalog/out");
        return C4BEN_BAD_ARG;
    }
    try {
        int kind = c4ben_probe(dir);
        if (kind == 2) return convertEff(dir, catalog_dir, out_path) == 0 ? C4BEN_OK : C4BEN_ERR;
        if (kind == 1) return convertEfmi(dir, catalog_dir, out_path) == 0 ? C4BEN_OK : C4BEN_ERR;
        c4ben_set_error(err, err_cap, "unsupported mod directory (no mod.ini components, not EFF 3)");
        return C4BEN_UNSUPPORTED;
    } catch (std::exception& e) {
        c4ben_set_error(err, err_cap, e.what());
        return C4BEN_ERR;
    } catch (...) {
        c4ben_set_error(err, err_cap, "unknown conversion failure");
        return C4BEN_ERR;
    }
}

int c4ben_convert_profile(const char* dir, const char* catalog_dir, const char* profile,
                          const char* out_path, int allow_review, char* err, unsigned err_cap) {
    if (!dir || !catalog_dir || !profile || !out_path) {
        c4ben_set_error(err, err_cap, "missing dir/catalog/profile/out");
        return C4BEN_BAD_ARG;
    }
    try {
        return convertLegacyProfile(dir, catalog_dir, profile, out_path, allow_review != 0) == 0
                   ? C4BEN_OK : C4BEN_ERR;
    } catch (std::exception& e) {
        c4ben_set_error(err, err_cap, e.what());
        return C4BEN_ERR;
    }
}

}  // extern "C"
#endif  /* C4BEN_NO_MAIN */
