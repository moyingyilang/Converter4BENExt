// Converter4BENExt —— EFMI/EFF -> BEN BEM 迁移工具（C++17，无 .NET / Python）
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
#include "third_party/cJSON.h"

using namespace std;
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

struct Dds { int w, h, mips, fmt; bool srgb; vector<uint8_t> data; };
static uint32_t rd32(const vector<uint8_t>& b, size_t o) { return (uint32_t)b[o] | ((uint32_t)b[o+1] << 8) | ((uint32_t)b[o+2] << 16) | ((uint32_t)b[o+3] << 24); }
static Dds readDds(const string& path) {
    auto b = readFile(path);
    if (b.size() < 128 || memcmp(b.data(), "DDS ", 4)) throw runtime_error("not DDS: " + path);
    Dds d{}; d.h = rd32(b, 12); d.w = rd32(b, 16); d.mips = rd32(b, 28); if (d.mips <= 0) d.mips = 1;
    string fourcc((char*)b.data() + 84, 4);
    size_t off = 128; int code = -1;
    if (fourcc == "DX10") { code = rd32(b, 128); off = 148; }
    else if (fourcc == "DXT1") code = 71; else if (fourcc == "DXT5") code = 77;
    else if (fourcc == "ATI1" || fourcc == "BC4U") code = 80; else if (fourcc == "ATI2" || fourcc == "BC5U") code = 83;
    switch (code) {
        case 71: d.fmt = 10; d.srgb = false; break; case 72: d.fmt = 10; d.srgb = true; break;
        case 77: d.fmt = 12; d.srgb = false; break; case 78: d.fmt = 12; d.srgb = true; break;
        case 80: d.fmt = 26; d.srgb = false; break; case 81: d.fmt = 26; d.srgb = true; break;
        case 83: d.fmt = 27; d.srgb = false; break; case 84: d.fmt = 27; d.srgb = true; break;
        case 98: d.fmt = 25; d.srgb = false; break; case 99: d.fmt = 25; d.srgb = true; break;
        case 28: case 87: d.fmt = 4; d.srgb = false; break;
        default: throw runtime_error("unsupported DDS format " + to_string(code) + " in " + path);
    }
    if (b.size() < off) throw runtime_error("truncated DDS " + path);
    d.data.assign(b.begin() + off, b.end());
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

static void writeBem(const string& path, const vector<uint8_t>& manifest, const vector<vector<uint8_t>>& payloads) {
    // BEM 二进制容器：40B header + manifest + 32B * N entries + payload blocks
    uint64_t fileSize = 40 + manifest.size() + 32ull * payloads.size();
    for (auto& p : payloads) fileSize += p.size();
    vector<uint8_t> out; out.reserve(fileSize);
    auto put16 = [&](uint16_t v) { out.push_back(v & 255); out.push_back(v >> 8); };
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) out.push_back((v >> (8 * i)) & 255); };
    auto put64 = [&](uint64_t v) { for (int i = 0; i < 8; i++) out.push_back((v >> (8 * i)) & 255); };
    const char magic[8] = {'B','E','M',0,'P','K','G',0};
    out.insert(out.end(), magic, magic + 8);
    put16(1); put16(0); put32(40); put64(fileSize); put64(manifest.size()); put32((uint32_t)payloads.size()); put32(0);
    out.insert(out.end(), manifest.begin(), manifest.end());
    uint64_t off = 40 + manifest.size() + 32ull * payloads.size();
    for (auto& p : payloads) {
        put32(0); put32(0); put64(off); put64(p.size()); put64(p.size());
        off += p.size();
    }
    for (auto& p : payloads) out.insert(out.end(), p.begin(), p.end());
    writeFile(path, out);
}

int main(int argc, char** argv) {
    try {
        if (argc < 2 || string(argv[1]) != "convert") {
            cerr << "usage: c4ben convert <mod-dir> --catalog <catalog-dir> --out <out.zip>\n"; return 2;
        }
        string dir, catDir, out;
        for (int i = 2; i < argc; i++) {
            string a = argv[i];
            if (a == "--catalog" && i + 1 < argc) catDir = argv[++i];
            else if (a == "--out" && i + 1 < argc) out = argv[++i];
            else dir = a;
        }
        if (dir.empty() || catDir.empty() || out.empty()) { cerr << "missing args\n"; return 2; }
        auto ini = Ini::parse(dir + "/mod.ini");
        vector<string> hashes;
        for (auto& s : ini.order) {
            if (s.rfind("TextureOverride_EntryPoint_Component", 0) != 0) continue;
            auto it = ini.sec[s].find("hash"); if (it != ini.sec[s].end()) hashes.push_back(lower(it->second));
        }
        auto cat = loadCatalog(catDir, hashes);
        cout << "target: " << cat.character << " (" << jstr(cat.root, "name") << "), matched " << hashes.size() << " entries\n";

        // map mod component -> catalog component id
        map<int, int> mod2cat;
        for (auto& s : ini.order) {
            if (s.rfind("TextureOverride_EntryPoint_Component", 0) != 0) continue;
            int mid = stoi(s.substr(strlen("TextureOverride_EntryPoint_Component")));
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
        map<int, TexRes> texRes;
        for (auto& s : ini.order) {
            if (s.rfind("Resource_Texture", 0) != 0) continue;
            int n = stoi(s.substr(strlen("Resource_Texture")));
            auto it = ini.sec[s].find("filename"); if (it == ini.sec[s].end()) continue;
            auto hs = ini.sec["TextureOverride_Texture" + to_string(n)].find("hash");
            if (hs == ini.sec["TextureOverride_Texture" + to_string(n)].end()) continue;
            auto tn = texName.find(lower(hs->second)); if (tn == texName.end()) continue;
            try { texRes[n] = {tn->second, readDds(dir + "/" + it->second)}; }
            catch (exception& e) { cerr << "texture skip " << it->second << ": " << e.what() << "\n"; }
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
                auto& d = ini.sec[sec]; stride = stoi(d["stride"]);
                data = readFile(dir + "/" + d["filename"]);
            };
            int s0, s1, s2, sib; vector<uint8_t> vb0, vb1, vb2, ib;
            rd(nvb0, s0, vb0); rd(nvb1, s1, vb1); rd(nvb2, s2, vb2); rd(nib, sib, ib);
            if (s2 != 16 && s2 != 8) { cout << "skip comp " << mid << ": VB2 stride " << s2 << "\n"; continue; }
            int nv = (int)vb0.size() / s0;
            if ((int)vb2.size() != nv * s2 || (int)vb1.size() != nv * s1) { cout << "skip comp " << mid << ": buffer mismatch\n"; continue; }
            int ni = (int)ib.size() / 2;

            vector<uint8_t> skin(nv * 12);
            vector<cJSON*> jbones;
            map<pair<int,int>, int> pal;
            bool ok = true;
            for (int v = 0; v < nv && ok; v++) {
                uint16_t w[4] = {0,0,0,0}, gi[4] = {0,0,0,0};
                if (s2 == 16) for (int k = 0; k < 4; k++) {
                    w[k] = (uint16_t)(vb2[v*16 + k*2] | (vb2[v*16 + k*2 + 1] << 8));
                    gi[k] = (uint16_t)(vb2[v*16 + 8 + k*2] | (vb2[v*16 + 8 + k*2 + 1] << 8));
                } else {
                    w[0] = 65535;
                    gi[0] = (uint16_t)(vb2[v*8] | (vb2[v*8 + 1] << 8));
                }
                uint8_t outIdx[4] = {0,0,0,0};
                for (int k = 0; k < 4; k++) {
                    if (gi[k] == 0 && w[k] == 0) continue;
                    int owner, local;
                    if (!ownerOf(gi[k], owner, local)) { ok = false; break; }
                    auto key = make_pair(owner, local);
                    auto it = pal.find(key);
                    int pi;
                    if (it == pal.end()) {
                        pi = (int)pal.size(); if (pi > 255) { ok = false; break; }
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
            if (!ok) { cout << "skip comp " << mid << ": palette/global index\n"; continue; }

            // attributes: catalog 4-tuples -> 5-tuples
            auto* comp = cJSON_GetObjectItemCaseSensitive(cat.comps, to_string(cid).c_str());
            cJSON* jattrs = jarr();
            int fsz[] = {4,2,1,1,2,2,1,1,2,2,4,4}; int offs[3] = {0,0,0};
            for (auto* a = get(comp, "attributes")->child; a; a = a->next) {
                int sem = a->child->valueint, fmt = a->child->next->valueint, dim = a->child->next->next->valueint, stream = a->child->next->next->next->valueint;
                if (stream >= 2) continue;
                cJSON* x = jarr(); jadd(x, jnum(sem)); jadd(x, jnum(fmt)); jadd(x, jnum(dim)); jadd(x, jnum(stream)); jadd(x, jnum(offs[stream]));
                jadd(jattrs, x); offs[stream] += fsz[fmt] * dim;
            }
            cJSON* sx = jarr(); jadd(sx, jnum(12)); jadd(sx, jnum(4)); jadd(sx, jnum(4)); jadd(sx, jnum(2)); jadd(sx, jnum(0)); jadd(jattrs, sx);
            sx = jarr(); jadd(sx, jnum(13)); jadd(sx, jnum(6)); jadd(sx, jnum(4)); jadd(sx, jnum(2)); jadd(sx, jnum(8)); jadd(jattrs, sx);

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
            auto* mats = get(comp, "materials"); auto* m0 = cJSON_GetArrayItem(mats, 0);
            cJSON_AddStringToObject(jd, "material_name", m0 ? m0->valuestring : "M_unknown");
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

        cJSON* mroot = obj(nullptr);
        cJSON_AddNumberToObject(mroot, "schema", 1);
        cJSON_AddStringToObject(mroot, "package_id", ("c4ben." + string(cat.character)).c_str());
        cJSON_AddStringToObject(mroot, "name", ("Converter4BENExt: " + fs::path(dir).filename().string()).c_str());
        cJSON_AddStringToObject(mroot, "author", "Converter4BENExt");
        cJSON_AddStringToObject(mroot, "version", "0.1.0");
        cJSON* caps = jarr(); for (auto s : {"native-materials", "palette-u8", "indices-u32", "fixed-appearances"}) jadd(caps, jstrv(s));
        cJSON_AddItemToObject(mroot, "required_capabilities", caps);
        cJSON_AddItemToObject(mroot, "target", jtarget);
        cJSON_AddItemToObject(mroot, "meshes", jmeshes);
        cJSON_AddItemToObject(mroot, "textures", jtextures);
        cJSON_AddStringToObject(mroot, "default_appearance_id", "default");
        cJSON_AddItemToObject(mroot, "appearances", japps);

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
        else writeBem(out, manifest, payloads);
        cout << "written " << out << ": meshes=" << cJSON_GetArraySize(jmeshes) << " textures=" << cJSON_GetArraySize(jtextures) << " payloads=" << payloads.size() << "\n";
        cJSON_Delete(mroot); cJSON_Delete(cat.root);
        return 0;
    } catch (exception& e) { cerr << "ERROR: " << e.what() << "\n"; return 1; }
}
