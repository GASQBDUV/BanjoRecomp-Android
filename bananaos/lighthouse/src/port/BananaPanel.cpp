// BananaOS: a companion screen on the bottom panel (DSI-1), drawn straight to KMS from inside the game process.
// The game draws on the top panel through SDL's KMSDRM driver as usual; SDL holds DRM master and gives us its fd
// (SDL_GetWindowWMInfo), with which the bottom panel's connector gets a CRTC, a dumb buffer and a mode of its own.
// The bottom panel's touchscreen is read straight from evdev. Enabled with BANANA_PANEL=1.
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <algorithm>
#include <fcntl.h>
#include <map>
#include <math.h>
#include <time.h>
#include <vector>
#include <linux/input.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

struct Buf {
    uint32_t* px = nullptr;
    int w = 0, h = 0, pitch = 0; // pitch in pixels
};

Buf g_buf;
uint32_t* g_front = nullptr; // the mapped scanout buffer; g_buf.px is a plain back buffer copied over it per frame
size_t g_front_bytes = 0;

void Fill(int x, int y, int w, int h, uint32_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_buf.w) w = g_buf.w - x;
    if (y + h > g_buf.h) h = g_buf.h - y;
    for (int j = 0; j < h; j++) {
        uint32_t* row = g_buf.px + (size_t)(y + j) * g_buf.pitch + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

struct Panel {
    int fd = -1;
    uint32_t crtc = 0, conn = 0, fb = 0;
    drmModeModeInfo mode{};
    int w = 0, h = 0;
};

bool SetupPanel(SDL_Window* win, Panel& p) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_KMSDRM) {
        fprintf(stderr, "BananaPanel: window is not a KMSDRM window\n");
        return false;
    }
    p.fd = info.info.kmsdrm.drm_fd;
    if (p.fd < 0) return false;

    drmModeRes* res = drmModeGetResources(p.fd);
    if (!res) { fprintf(stderr, "BananaPanel: drmModeGetResources failed\n"); return false; }

    int want_id = getenv("BANANA_PANEL_ID") ? atoi(getenv("BANANA_PANEL_ID")) : 1; // DSI-1 is the bottom panel
    drmModeConnector* conn = nullptr;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector* c = drmModeGetConnector(p.fd, res->connectors[i]);
        if (!c) continue;
        if (c->connector_type == DRM_MODE_CONNECTOR_DSI && (int)c->connector_type_id == want_id &&
            c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
            conn = c;
        else
            drmModeFreeConnector(c);
    }
    if (!conn) { fprintf(stderr, "BananaPanel: DSI-%d not found\n", want_id); drmModeFreeResources(res); return false; }

    p.conn = conn->connector_id;
    p.mode = conn->modes[0];
    p.w = p.mode.hdisplay;
    p.h = p.mode.vdisplay;

    // the CRTCs the other connectors are on (SDL's) must be left alone
    uint32_t busy = 0;
    for (int i = 0; i < res->count_connectors; i++) {
        if (res->connectors[i] == p.conn) continue;
        drmModeConnector* c = drmModeGetConnector(p.fd, res->connectors[i]);
        if (!c) continue;
        if (c->encoder_id) {
            drmModeEncoder* e = drmModeGetEncoder(p.fd, c->encoder_id);
            if (e) {
                for (int k = 0; k < res->count_crtcs; k++)
                    if (res->crtcs[k] == e->crtc_id) busy |= 1u << k;
                drmModeFreeEncoder(e);
            }
        }
        drmModeFreeConnector(c);
    }

    uint32_t possible = 0;
    uint32_t cur = 0;
    for (int i = 0; i < conn->count_encoders; i++) {
        drmModeEncoder* e = drmModeGetEncoder(p.fd, conn->encoders[i]);
        if (!e) continue;
        possible |= e->possible_crtcs;
        if (conn->encoder_id == e->encoder_id) cur = e->crtc_id;
        drmModeFreeEncoder(e);
    }
    p.crtc = 0;
    for (int k = 0; k < res->count_crtcs; k++) {
        if (!(possible & (1u << k)) || (busy & (1u << k))) continue;
        if (cur && res->crtcs[k] != cur) continue;
        p.crtc = res->crtcs[k];
        break;
    }
    if (!p.crtc)
        for (int k = 0; k < res->count_crtcs; k++)
            if ((possible & (1u << k)) && !(busy & (1u << k))) { p.crtc = res->crtcs[k]; break; }
    fprintf(stderr, "BananaPanel: DSI-%d connector %u, mode %dx%d, crtc %u (possible 0x%x, busy 0x%x, current %u)\n",
            want_id, p.conn, p.w, p.h, p.crtc, possible, busy, cur);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    if (!p.crtc) { fprintf(stderr, "BananaPanel: no free CRTC\n"); return false; }

    drm_mode_create_dumb cd{};
    cd.width = p.w; cd.height = p.h; cd.bpp = 32;
    if (drmIoctl(p.fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { perror("BananaPanel: create dumb"); return false; }
    if (drmModeAddFB(p.fd, p.w, p.h, 24, 32, cd.pitch, cd.handle, &p.fb)) { perror("BananaPanel: addfb"); return false; }
    drm_mode_map_dumb md{};
    md.handle = cd.handle;
    if (drmIoctl(p.fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { perror("BananaPanel: map dumb"); return false; }
    void* m = mmap(nullptr, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, p.fd, md.offset);
    if (m == MAP_FAILED) { perror("BananaPanel: mmap"); return false; }
    g_front = (uint32_t*)m;
    g_front_bytes = cd.size;
    g_buf.px = (uint32_t*)calloc(1, cd.size);
    g_buf.w = p.w; g_buf.h = p.h; g_buf.pitch = cd.pitch / 4;
    memset(m, 0, cd.size);
    if (drmModeSetCrtc(p.fd, p.crtc, p.fb, 0, 0, &p.conn, 1, &p.mode)) { perror("BananaPanel: setcrtc"); return false; }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Game state and sprites. The layout follows the Android companion screen of BanjoRecomp-Android
// (DualScreenStatsView): a 1240x1080 design space with Banjo and the lives, the honeycombs, note/egg, feathers,
// Jiggy/Mumbo token, the five Jinjos and the level's name in the game's own bold font. The pictures come from the
// game's own sprite assets (assetcache_get) of the player's ROM, nothing is bundled.
// ---------------------------------------------------------------------------------------------------------------------
extern "C" {
void* assetcache_get(int assetId);
void assetcache_release(void* asset);
int item_getCount(int item);
int level_get(void);
int GetCurrentGameMode(void);
int gsworld_getMap(void);
int jiggyscore_total(void);
int itemscore_noteScores_getTotal(void);
int honeycombscore_get_total(void);
int fileProgressFlag_get(int flag);
const char* port_getLevelName(int map_id);
}

enum { ITEM_NOTE = 0xC, ITEM_EGGS = 0xD, ITEM_JIGGY = 0xE, ITEM_RED_FEATHER = 0xF, ITEM_GOLD_FEATHER = 0x10,
       ITEM_JINJOS = 0x12, ITEM_MUMBO_TOKEN_TOTAL = 0x25, ITEM_JIGGY_TOTAL = 0x26, ITEM_HEALTH = 0x14, ITEM_HEALTH_TOTAL = 0x15, ITEM_LIFE = 0x16, ITEM_MUMBO_TOKEN = 0x1C };

// the port's in-memory sprite (src/port/Resource/Type/Sprite.cpp: BKSprite, BKSpriteFrame and BKSpriteTextureBlock)
struct SpriteHdr { int16_t frameCnt, type, unk4, unk6, unk8, unkA; uint32_t unkC; void* frames[]; };
struct SpriteFrameHdr { int16_t x, y, w, h, chunkCnt, unkA, unkC, unkE, unk10, unk12; };
struct SpriteBlockHdr { int16_t x, y, w, h; };
static_assert(sizeof(SpriteHdr) == 16 && sizeof(SpriteFrameHdr) == 20 && sizeof(SpriteBlockHdr) == 8, "sprite layout");

enum { FMT_CI4 = 0x1, FMT_CI8 = 0x4, FMT_I4 = 0x20, FMT_I8 = 0x40, FMT_RGBA16 = 0x400, FMT_RGBA32 = 0x800 };

struct Image {
    int w = 0, h = 0;
    int advance = 0; // font chunks: the block's x
    std::vector<uint32_t> px;
};

struct Glyph {
    Image img;
    float advance = 1.0f;
    bool tight = false;
};

std::vector<Image> g_sprite[16];
std::map<char, Glyph> g_glyphs;
bool g_loaded = false, g_started = false;

enum { S_HEALTH, S_BANJO, S_LIFE, S_EGG, S_RED, S_GOLD, S_NOTE, S_JIGGY, S_MUMBO, S_JY, S_JG, S_JB, S_JP, S_JO };
const int kSpriteAsset[] = { 0x7DD, 0x7EF, 0x80E, 0x81E, 0x820, 0x81F, 0x81B, 0x80D, 0x808, 0x802, 0x803, 0x804, 0x805, 0x806 };

int Align8(int v) { return (v + 7) & ~7; }

int BitsPerPixel(int fmt) {
    switch (fmt) {
    case FMT_CI4: case FMT_I4: return 4;
    case FMT_CI8: case FMT_I8: return 8;
    case FMT_RGBA16: return 16;
    case FMT_RGBA32: return 32;
    }
    return 0;
}

uint32_t Rgba16(const uint8_t* p) {
    int v = (p[0] << 8) | p[1];
    int r5 = (v >> 11) & 31, g5 = (v >> 6) & 31, b5 = (v >> 1) & 31;
    uint32_t a = (v & 1) ? 255 : 0;
    return (a << 24) | (uint32_t)(((r5 << 3) | (r5 >> 2)) << 16) | (uint32_t)(((g5 << 3) | (g5 >> 2)) << 8) |
           (uint32_t)((b5 << 3) | (b5 >> 2));
}

void DecodePixels(const uint8_t* raw, const uint8_t* pal, int fmt, int w, int h, uint32_t* out) {
    int n = w * h;
    switch (fmt) {
    case FMT_CI4:
        for (int i = 0; i < n; i++) {
            int b = raw[i / 2], idx = (i & 1) ? (b & 15) : (b >> 4);
            out[i] = Rgba16(pal + idx * 2);
        }
        break;
    case FMT_CI8:
        for (int i = 0; i < n; i++) out[i] = Rgba16(pal + raw[i] * 2);
        break;
    case FMT_I4:
        for (int i = 0; i < n; i++) {
            int b = raw[i / 2], v = ((i & 1) ? (b & 15) : (b >> 4)) * 17;
            out[i] = 0xFF000000u | (v << 16) | (v << 8) | v;
        }
        break;
    case FMT_I8:
        for (int i = 0; i < n; i++) out[i] = 0xFF000000u | (raw[i] << 16) | (raw[i] << 8) | raw[i];
        break;
    case FMT_RGBA16:
        for (int i = 0; i < n; i++) out[i] = Rgba16(raw + i * 2);
        break;
    case FMT_RGBA32:
        for (int i = 0; i < n; i++)
            out[i] = ((uint32_t)raw[i * 4 + 3] << 24) | ((uint32_t)raw[i * 4] << 16) | ((uint32_t)raw[i * 4 + 1] << 8) | raw[i * 4 + 2];
        break;
    }
}

// Every texture block of a frame, either blitted into one frame image or (glyphs) kept apart.
bool DecodeFrame(const SpriteHdr* spr, int frame, bool separate, std::vector<Image>& out) {
    int fmt = spr->type;
    int bpp = BitsPerPixel(fmt);
    if (!bpp || frame < 0 || frame >= spr->frameCnt || !spr->frames[frame]) return false;
    const uint8_t* base = (const uint8_t*)spr;
    const SpriteFrameHdr* fh = (const SpriteFrameHdr*)spr->frames[frame];
    int cursor = (int)((const uint8_t*)fh - base) + (int)sizeof(SpriteFrameHdr);
    const uint8_t* pal = nullptr;
    if (fmt == FMT_CI4 || fmt == FMT_CI8) {
        cursor = Align8(cursor);
        pal = base + cursor;
        cursor += fmt == FMT_CI4 ? 0x20 : 0x200;
    }
    Image whole;
    if (!separate) {
        whole.w = fh->w;
        whole.h = fh->h;
        if (whole.w <= 0 || whole.h <= 0 || whole.w > 512 || whole.h > 512) return false;
        whole.px.assign((size_t)whole.w * whole.h, 0);
    }
    int chunks = fh->chunkCnt;
    if (chunks <= 0 || chunks > 256) return false;
    for (int c = 0; c < chunks; c++) {
        const SpriteBlockHdr* b = (const SpriteBlockHdr*)(base + cursor);
        cursor = Align8(cursor + (int)sizeof(SpriteBlockHdr));
        const uint8_t* raw = base + cursor;
        int bw = b->w, bh = b->h;
        if (bw <= 0 || bh <= 0 || bw > 1024 || bh > 1024) return false;
        cursor += bw * bh * bpp / 8;
        Image blk;
        blk.w = bw;
        blk.h = bh;
        blk.advance = b->x;
        blk.px.resize((size_t)bw * bh);
        DecodePixels(raw, pal, fmt, bw, bh, blk.px.data());
        if (separate) {
            out.push_back(std::move(blk));
        } else {
            int ox = chunks == 1 ? 0 : b->x, oy = chunks == 1 ? 0 : b->y;
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++) {
                    int dx = ox + x, dy = oy + y;
                    if (dx >= 0 && dx < whole.w && dy >= 0 && dy < whole.h) whole.px[(size_t)dy * whole.w + dx] = blk.px[(size_t)y * bw + x];
                }
        }
    }
    if (!separate) out.push_back(std::move(whole));
    return true;
}

Image CropTransparent(const Image& im) {
    int l = im.w, t = im.h, r = -1, b = -1;
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++)
            if (im.px[(size_t)y * im.w + x] >> 24) { if (x < l) l = x; if (x > r) r = x; if (y < t) t = y; if (y > b) b = y; }
    if (r < l || b < t) return im;
    Image o;
    o.w = r - l + 1;
    o.h = b - t + 1;
    o.advance = im.advance;
    o.px.resize((size_t)o.w * o.h);
    for (int y = 0; y < o.h; y++)
        for (int x = 0; x < o.w; x++) o.px[(size_t)y * o.w + x] = im.px[(size_t)(t + y) * im.w + l + x];
    return o;
}

int Lerp(int a, int b, float t) { return (int)(a + (b - a) * t + 0.5f); }

// the letters are an alpha mask in the ROM; the game colours them with the honey gradient
void TintHoney(Image& im) {
    for (int y = 0; y < im.h; y++) {
        float v = im.h <= 1 ? 0.0f : y / (float)(im.h - 1);
        int r, g, b;
        if (v < 0.55f) { float t = v / 0.55f; r = Lerp(255, 238, t); g = Lerp(241, 167, t); b = Lerp(89, 31, t); }
        else { float t = (v - 0.55f) / 0.45f; r = Lerp(238, 132, t); g = Lerp(167, 67, t); b = Lerp(31, 5, t); }
        for (int x = 0; x < im.w; x++) {
            uint32_t& p = im.px[(size_t)y * im.w + x];
            int a = p >> 24;
            if (!a) continue;
            float hl = 0.62f + (a / 255.0f) * 0.38f;
            int rr = (int)(r * hl), gg = (int)(g * hl), bb = (int)(b * hl);
            p = ((uint32_t)a << 24) | (uint32_t)((rr > 255 ? 255 : rr) << 16) | (uint32_t)((gg > 255 ? 255 : gg) << 8) | (uint32_t)(bb > 255 ? 255 : bb);
        }
    }
}

void LoadFont(int assetId, float nominal, bool tight, bool letters) {
    void* a = assetcache_get(assetId);
    if (!a) { fprintf(stderr, "BananaPanel: font asset 0x%x missing\n", assetId); return; }
    std::vector<Image> chunks;
    if (DecodeFrame((const SpriteHdr*)a, 0, true, chunks)) {
        auto put = [&](char ch, size_t i, bool apostrophe) {
            if (i >= chunks.size()) return;
            Glyph g;
            g.img = CropTransparent(chunks[i]);
            TintHoney(g.img);
            g.advance = std::max(1.0f, chunks[i].advance / nominal);
            g.tight = tight;
            (void)apostrophe;
            g_glyphs[ch] = std::move(g);
        };
        if (letters) {
            for (int i = 1; i <= 26; i++) put((char)('A' + i - 1), i, false);
            put('\'', 40, true);
            put('/', 39, false);
        } else {
            for (int i = 0; i < 10; i++) put((char)('0' + i), i, false);
        }
    } else {
        fprintf(stderr, "BananaPanel: font asset 0x%x did not decode\n", assetId);
    }
    assetcache_release(a);
}

void DumpPpm(const char* name, const Image& im) {
    const char* dir = getenv("BANANA_PANEL_DUMP");
    if (!dir || im.px.empty()) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", im.w, im.h);
    for (uint32_t p : im.px) {
        int a = p >> 24;
        uint8_t rgb[3] = { (uint8_t)((((p >> 16) & 255) * a + 255 * (255 - a)) / 255), (uint8_t)((((p >> 8) & 255) * a) / 255),
                           (uint8_t)(((p & 255) * a + 255 * (255 - a)) / 255) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

// the game thread loads once, the draw thread only reads
void LoadAssets() {
    for (size_t i = 0; i < sizeof kSpriteAsset / sizeof *kSpriteAsset; i++) {
        void* a = assetcache_get(kSpriteAsset[i]);
        if (!a) { fprintf(stderr, "BananaPanel: sprite asset 0x%x missing\n", kSpriteAsset[i]); continue; }
        const SpriteHdr* s = (const SpriteHdr*)a;
        for (int f = 0; f < s->frameCnt && f < 64; f++)
            if (!DecodeFrame(s, f, false, g_sprite[i])) break;
        char name[32];
        snprintf(name, sizeof name, "sprite_%x", kSpriteAsset[i]);
        if (!g_sprite[i].empty()) DumpPpm(name, g_sprite[i][0]);
        fprintf(stderr, "BananaPanel: sprite 0x%x: %d frames, %dx%d\n", kSpriteAsset[i], (int)g_sprite[i].size(),
                g_sprite[i].empty() ? 0 : g_sprite[i][0].w, g_sprite[i].empty() ? 0 : g_sprite[i][0].h);
        assetcache_release(a);
    }
    LoadFont(0x6ED, 20.0f, false, false);
    LoadFont(0x6EC, 23.0f, true, true);
    for (auto& g : g_glyphs) { char n[16]; snprintf(n, sizeof n, "glyph_%d", (int)g.first); DumpPpm(n, g.second.img); }
    fprintf(stderr, "BananaPanel: %d glyphs\n", (int)g_glyphs.size());
}

const int kAreas[][3] = {
    { 0x01, 0x14CF, 71 },
    { 0x02, 0x14AA, 0 },
    { 0x05, 0x146F, 0 },
    { 0x06, 0x146D, 0 },
    { 0x07, 0x146B, 0 },
    { 0x8F, 0x1473, 0 },
    { 0x0A, 0x1471, 0 },
    { 0x0B, 0x14ED, 0 },
    { 0x0C, 0x14AC, 0 },
    { 0x0D, 0x14D1, 0 },
    { 0x0E, 0x14AE, 0 },
    { 0x10, 0x14D3, 0 },
    { 0x11, 0x14D4, 0 },
    { 0x12, 0x1474, 0 },
    { 0x92, 0x147E, 0 },
    { 0x13, 0x1476, 0 },
    { 0x14, 0x1478, 0 },
    { 0x15, 0x147A, 0 },
    { 0x16, 0x147C, 0 },
    { 0x1A, 0x147D, 0 },
    { 0x1B, 0x147F, 0 },
    { 0x1C, 0x1486, 0 },
    { 0x1D, 0x1482, 0 },
    { 0x1E, 0x149D, 0 },
    { 0x1F, 0x14A0, 0 },
    { 0x20, 0x14A9, 0 },
    { 0x21, 0x14EF, 0 },
    { 0x22, 0x14F0, 0 },
    { 0x23, 0x14F2, 0 },
    { 0x24, 0x1488, 0 },
    { 0x25, 0x1495, 0 },
    { 0x26, 0x1485, 0 },
    { 0x27, 0x14C8, 0 },
    { 0x28, 0x1489, 0 },
    { 0x29, 0x148B, 0 },
    { 0x2A, 0x148D, 0 },
    { 0x2B, 0x1483, 0 },
    { 0x2C, 0x148F, 0 },
    { 0x2D, 0x1491, 0 },
    { 0x2E, 0x1493, 0 },
    { 0x2F, 0x1481, 0 },
    { 0x30, 0x14AE, 0 },
    { 0x8D, 0x1498, 0 },
    { 0x31, 0x14B0, 0 },
    { 0x8B, 0x14C5, 0 },
    { 0x34, 0x14B2, 0 },
    { 0x35, 0x14B4, 0 },
    { 0x36, 0x14B6, 0 },
    { 0x37, 0x14B8, 0 },
    { 0x38, 0x14BA, 0 },
    { 0x39, 0x14BD, 0 },
    { 0x3A, 0x14BE, 0 },
    { 0x3B, 0x14C1, 0 },
    { 0x3C, 0x14C3, 0 },
    { 0x3D, 0x14C0, 0 },
    { 0x3E, 0x14B9, 0 },
    { 0x3F, 0x14BB, 0 },
    { 0x40, 0x14D8, 0 },
    { 0x41, 0x14CA, 0 },
    { 0x7F, 0x14CC, 0 },
    { 0x43, 0x14D9, 0 },
    { 0x44, 0x14DA, 0 },
    { 0x45, 0x14DB, 0 },
    { 0x46, 0x14DC, 0 },
    { 0x47, 0x14AE, 0 },
    { 0x48, 0x14AE, 0 },
    { 0x4A, 0x14AE, 0 },
    { 0x4B, 0x14AE, 0 },
    { 0x4C, 0x14AE, 0 },
    { 0x4D, 0x14AE, 0 },
    { 0x53, 0x14CB, 0 },
    { 0x5A, 0x14DD, 0 },
    { 0x5B, 0x14DD, 0 },
    { 0x5C, 0x14DD, 0 },
    { 0x5E, 0x14DE, 0 },
    { 0x5F, 0x14DE, 0 },
    { 0x60, 0x14DE, 0 },
    { 0x61, 0x14DE, 0 },
    { 0x62, 0x14E0, 0 },
    { 0x63, 0x14E1, 0 },
    { 0x64, 0x14E1, 0 },
    { 0x65, 0x14DF, 0 },
    { 0x66, 0x14DF, 0 },
    { 0x67, 0x14DF, 0 },
    { 0x68, 0x14DF, 0 },
    { 0x69, 0x14F3, 0 },
    { 0x6A, 0x14F4, 0 },
    { 0x6B, 0x14F5, 0 },
    { 0x6C, 0x14F6, 0 },
    { 0x6D, 0x14F7, 0 },
    { 0x6E, 0x14F8, 0 },
    { 0x6F, 0x14F9, 0 },
    { 0x74, 0x14FD, 0 },
    { 0x70, 0x14FB, 0 },
    { 0x75, 0x14FE, 0 },
    { 0x7A, 0x14FF, 0 },
    { 0x71, 0x1500, 0 },
    { 0x72, 0x1501, 0 },
    { 0x76, 0x1502, 0 },
    { 0x77, 0x1503, 0 },
    { 0x78, 0x1504, 0 },
    { 0x79, 0x1505, 0 },
    { 0x80, 0x1506, 0 },
    { 0x93, 0x150F, 0 },
    { 0x90, 0x14FC, 0 },
    { 0x7B, 0x150F, 0 },
    { 0x7C, 0x14A2, 0 },
    { 0x7D, 0x14CF, 0 },
    { 0x7E, 0x14CF, 0 },
    { 0x81, 0x150F, 0 },
    { 0x82, 0x150F, 0 },
    { 0x83, 0x150F, 0 },
    { 0x84, 0x150F, 0 },
    { 0x85, 0x14CF, 0 },
    { 0x86, 0x14CF, 0 },
    { 0x87, 0x14A6, 0 },
    { 0x88, 0x14CF, 0 },
    { 0x94, 0x14CF, 0 },
    { 0x98, 0x149F, 0 },
    { 0x99, 0x149F, 0 },
    { 0x95, 0x14A9, 0 },
    { 0x89, 0x14A2, 0 },
    { 0x8A, 0x14A2, 0 },
    { 0x96, 0x14A9, 0 },
    { 0x97, 0x14A9, 0 },
    { 0x91, 0x14A2, 0 },
    { 0x8C, 0x14A2, 0 },
    { 0x8E, 0x14E8, 0 },
};

// a model's texture, for the world backdrops (the port keeps the model's texture list as the ROM lays it out)
bool DecodeModelTexture(const uint8_t* model, int index, Image& out) {
    int16_t listOff = *(const int16_t*)(model + 8);
    if (listOff <= 0) return false;
    const uint8_t* list = model + listOff;
    int count = *(const int16_t*)(list + 4);
    if (index < 0 || index >= count || count > 256) return false;
    const uint8_t* info = list + 8 + index * 16;
    int off = *(const int32_t*)info, type = *(const int16_t*)(info + 4);
    int w = info[8], h = info[9];
    int fmt = type == 1 ? FMT_CI4 : type == 2 ? FMT_CI8 : type == 4 ? FMT_RGBA16 : type == 8 ? FMT_RGBA32 : type == 16 ? FMT_I8 : 0;
    if (!fmt || w <= 0 || h <= 0) return false;
    const uint8_t* pix = list + 8 + count * 16 + off;
    const uint8_t* pal = nullptr;
    if (fmt == FMT_CI4) { pal = pix; pix += 0x20; }
    else if (fmt == FMT_CI8) { pal = pix; pix += 0x200; }
    Image tmp;
    tmp.w = w;
    tmp.h = h;
    tmp.px.resize((size_t)w * h);
    DecodePixels(pix, pal, fmt, w, h, tmp.px.data());
    out.w = w;
    out.h = h;
    out.px.resize(tmp.px.size());
    for (int y = 0; y < h; y++) // the model's textures are stored upside down
        for (int x = 0; x < w; x++) out.px[(size_t)(h - 1 - y) * w + x] = tmp.px[(size_t)y * w + x];
    return true;
}

pthread_mutex_t g_bgLock = PTHREAD_MUTEX_INITIALIZER;
Image g_backdropImg;
bool g_backdropDirty = false;
int g_backdropKey = -1;

void LoadBackdrop(int map) {
    int asset = 0x14AA, tex = 0; // Mumbo's Mountain where a map has none
    for (const auto& a : kAreas)
        if (a[0] == map) { asset = a[1]; tex = a[2]; break; }
    if (asset * 256 + tex == g_backdropKey) return;
    g_backdropKey = asset * 256 + tex;
    void* m = assetcache_get(asset);
    Image img;
    bool ok = m && DecodeModelTexture((const uint8_t*)m, tex, img);
    if (m) assetcache_release(m);
    if (!ok) { fprintf(stderr, "BananaPanel: backdrop 0x%x[%d] for map 0x%x failed\n", asset, tex, map); return; }
    pthread_mutex_lock(&g_bgLock);
    g_backdropImg = std::move(img);
    g_backdropDirty = true;
    pthread_mutex_unlock(&g_bgLock);
}

// ---- state snapshot, written by the game thread each frame --------------------------------------------------------
struct Snap {
    volatile int mode = 0, level = 0, map = 0, health = 0, maxHealth = 5, lives = 0, notes = 0, eggs = 0, red = 0, gold = 0,
                 jiggies = 0, tokens = 0, jinjos = 0, completion = 0, lair = 0;
    char name[64] = "";
};
Snap g_snap;

// ---- drawing ------------------------------------------------------------------------------------------------------
float g_s = 1.0f, g_ox = 0.0f;
inline float FX(float x) { return g_ox + x * g_s; }
inline float FY(float y) { return y * g_s; }

void DrawImage(const Image& im, float x0, float y0, float x1, float y1, int alpha) {
    if (im.px.empty() || x1 <= x0 || y1 <= y0) return;
    int ix0 = std::max(0, (int)floorf(x0)), ix1 = std::min(g_buf.w, (int)ceilf(x1));
    int iy0 = std::max(0, (int)floorf(y0)), iy1 = std::min(g_buf.h, (int)ceilf(y1));
    float sx = im.w / (x1 - x0), sy = im.h / (y1 - y0);
    for (int y = iy0; y < iy1; y++) {
        float v = (y + 0.5f - y0) * sy - 0.5f;
        int v0 = (int)floorf(v);
        float fv = v - v0;
        int va = std::max(0, std::min(im.h - 1, v0)), vb = std::max(0, std::min(im.h - 1, v0 + 1));
        uint32_t* dst = g_buf.px + (size_t)y * g_buf.pitch;
        for (int x = ix0; x < ix1; x++) {
            float u = (x + 0.5f - x0) * sx - 0.5f;
            int u0 = (int)floorf(u);
            float fu = u - u0;
            int ua = std::max(0, std::min(im.w - 1, u0)), ub = std::max(0, std::min(im.w - 1, u0 + 1));
            uint32_t t[4] = { im.px[(size_t)va * im.w + ua], im.px[(size_t)va * im.w + ub], im.px[(size_t)vb * im.w + ua],
                              im.px[(size_t)vb * im.w + ub] };
            float w[4] = { (1 - fu) * (1 - fv), fu * (1 - fv), (1 - fu) * fv, fu * fv };
            float a = 0, r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) {
                float ta = (t[k] >> 24) * w[k];
                a += ta;
                r += ((t[k] >> 16) & 255) * ta;
                g += ((t[k] >> 8) & 255) * ta;
                b += (t[k] & 255) * ta;
            }
            if (a <= 0.5f) continue;
            r /= a; g /= a; b /= a;
            float al = a / 255.0f * alpha / 255.0f;
            uint32_t d = dst[x];
            int dr = (int)(((d >> 16) & 255) * (1 - al) + r * al), dg = (int)(((d >> 8) & 255) * (1 - al) + g * al),
                db = (int)((d & 255) * (1 - al) + b * al);
            dst[x] = 0xFF000000u | (uint32_t)(dr << 16) | (uint32_t)(dg << 8) | (uint32_t)db;
        }
    }
}

const Image* SpriteFrame(int sprite, long ms, int phase) {
    auto& f = g_sprite[sprite];
    if (f.empty()) return nullptr;
    if (f.size() == 1) return &f[0];
    return &f[(size_t)(((ms + phase * 130L) / 130) % (long)f.size())];
}

// the picture drawn into a square, as the Android view does
void DrawIcon(int sprite, float cx, float cy, float size, int alpha, long ms, int phase) {
    const Image* im = SpriteFrame(sprite, ms, phase);
    if (!im) return;
    float h = size / 2;
    DrawImage(*im, FX(cx - h), FY(cy - h), FX(cx + h), FY(cy + h), alpha);
}

// the sprite font; false when a character has no glyph. Heights and positions are in design coordinates.
float GlyphWidth(const Glyph& g, float height) { return height * g.img.w / (float)g.img.h; }
float GlyphAdvance(const Glyph& g, float height, float w) {
    if (g.tight) {
        float overlap = w < height * 0.46f ? height * 0.22f : height * 0.13f;
        return std::max(height * 0.08f, w - overlap);
    }
    return std::max(w - height * 0.11f, (height * g.advance - height * 0.18f) * 0.82f);
}

float MeasureText(const char* s, float height) {
    float w = 0;
    for (; *s; s++) {
        if (*s == ' ') { w += height * 0.35f; continue; }
        auto it = g_glyphs.find(*s);
        if (it == g_glyphs.end()) return -1;
        float gw = GlyphWidth(it->second, height);
        w += GlyphAdvance(it->second, height, gw);
    }
    return w;
}

bool DrawText(const char* s, float x, float baseline, float height) {
    if (MeasureText(s, height) < 0) return false;
    for (; *s; s++) {
        if (*s == ' ') { x += height * 0.35f; continue; }
        const Glyph& g = g_glyphs[*s];
        float gw = GlyphWidth(g, height);
        float top = baseline - height;
        DrawImage(g.img, FX(x), FY(top), FX(x + gw), FY(top + height), 255);
        x += GlyphAdvance(g, height, gw);
    }
    return true;
}

void DrawTextCentered(const char* s, float cx, float baseline, float height, float maxWidth) {
    float w = MeasureText(s, height);
    if (w <= 0) return;
    if (w > maxWidth) { height *= maxWidth / w; w = maxWidth; }
    DrawText(s, cx - w / 2, baseline, height);
}

void DrawNumberAt(int v, float x, float baseline, float height) {
    char s[16];
    snprintf(s, sizeof s, "%d", v);
    DrawText(s, x, baseline, height);
}

uint32_t LevelColor(int level) {
    static const uint32_t c[] = { 0x00202020, 0x5A3A22, 0x1E4A7A, 0x2A3A40, 0x2A5A22, 0x4A6A88, 0x3A2A4A,
                                  0x7A6A3A, 0x2A6A3A, 0x3A4A5A, 0x4A2A58, 0x4A2A6A, 0x501818 };
    return 0xFF000000 | c[level >= 0 && level <= 12 ? level : 0];
}

uint32_t* g_bg = nullptr;
bool g_bgHas = false;

// the world's own picture, covering the screen and darkened, kept in a buffer of its own
void RebuildBackdrop() {
    pthread_mutex_lock(&g_bgLock);
    if (g_backdropDirty && !g_backdropImg.px.empty()) {
        if (!g_bg) g_bg = (uint32_t*)calloc(1, g_front_bytes);
        uint32_t* saved = g_buf.px;
        g_buf.px = g_bg;
        Fill(0, 0, g_buf.w, g_buf.h, 0xFF000000);
        float sc = std::max(g_buf.w / (float)g_backdropImg.w, g_buf.h / (float)g_backdropImg.h);
        float dw = g_backdropImg.w * sc, dh = g_backdropImg.h * sc;
        DrawImage(g_backdropImg, (g_buf.w - dw) / 2, (g_buf.h - dh) / 2, (g_buf.w + dw) / 2, (g_buf.h + dh) / 2, 255);
        for (int y = 0; y < g_buf.h; y++) {
            uint32_t* row = g_buf.px + (size_t)y * g_buf.pitch;
            for (int x = 0; x < g_buf.w; x++) {
                uint32_t p = row[x];
                row[x] = 0xFF000000u | ((((p >> 16) & 255) * 110 / 255) << 16) | ((((p >> 8) & 255) * 110 / 255) << 8) | ((p & 255) * 110 / 255);
            }
        }
        g_buf.px = saved;
        g_bgHas = true;
        g_backdropDirty = false;
    }
    pthread_mutex_unlock(&g_bgLock);
}

void Backdrop(int level) {
    RebuildBackdrop();
    if (g_bgHas) {
        memcpy(g_buf.px, g_bg, g_front_bytes);
        return;
    }
    uint32_t base = LevelColor(level);
    for (int y = 0; y < g_buf.h; y++) {
        int k = 70 - y * 35 / g_buf.h;
        uint32_t c = 0xFF000000 | ((((base >> 16) & 255) * k / 100) << 16) | ((((base >> 8) & 255) * k / 100) << 8) | ((base & 255) * k / 100);
        Fill(0, y, g_buf.w, 1, c);
    }
}

void DrawStats(long ms) {
    int mode = g_snap.mode, level = g_snap.level;
    // black outside of play: cutscenes, the title screen, the pause menu (the stats are on the game screen then)
    if (mode != 3 || level < 1 || level > 12) {
        Fill(0, 0, g_buf.w, g_buf.h, 0xFF000000);
        return;
    }
    g_s = std::min(g_buf.w / 1240.0f, g_buf.h / 1080.0f);
    g_ox = (g_buf.w - 1240 * g_s) / 2;
    Backdrop(level);

    // health row
    DrawIcon(S_BANJO, 118, 112, 154, 255, ms, 0);
    DrawNumberAt(g_snap.lives, 220, 112 + 26, 76);
    int max = std::max(1, std::min((int)g_snap.maxHealth, 12));
    int health = std::max(0, std::min((int)g_snap.health, max));
    float spacing = max > 8 ? 68 : 78, size = max > 8 ? 74 : 82;
    for (int i = 0; i < max; i++) DrawIcon(S_HEALTH, 420 + i * spacing, 112, size, i < health ? 255 : 70, ms, i);

    // Spiral Mountain shows nothing but Banjo and the honeycombs until Gruntilda's Lair has been reached
    if (level == 11 && !g_snap.lair) {
        DrawTextCentered(g_snap.name[0] ? g_snap.name : "BANJO KAZOOIE", 620, 990, 68, 1120);
        return;
    }
    struct Stat { int sprite; int value; float x, nx, y, size; int phase; };
    const Stat stats[] = {
        { S_NOTE, g_snap.notes, 280, 380, 268, 112, 1 },     { S_EGG, g_snap.eggs, 760, 860, 268, 112, 2 },
        { S_RED, g_snap.red, 280, 380, 418, 116, 3 },        { S_GOLD, g_snap.gold, 760, 860, 418, 112, 4 },
        { S_JIGGY, g_snap.jiggies, 280, 380, 568, 118, 5 },  { S_MUMBO, g_snap.tokens, 760, 860, 568, 116, 6 },
    };
    for (const Stat& s : stats) {
        DrawIcon(s.sprite, s.x, s.y, s.size, 255, ms, s.phase);
        DrawNumberAt(s.value, s.nx, s.y + 27, 78);
    }

    if (level == 6 || level == 11) {
        // Gruntilda's Lair and Spiral Mountain: the total completion in place of this level's Jinjos
        char n[16];
        snprintf(n, sizeof n, "%d", (int)g_snap.completion);
        float ts = 70, base = 742 + 30;
        float dw = MeasureText(n, ts), sp = ts * 0.35f, pw = ts * 0.72f, sw = MeasureText(" COMPLETE", ts);
        if (dw > 0 && sw > 0) {
            float x = (1240 - (dw + sp + pw + sw)) / 2;
            DrawText(n, x, base, ts);
            float px = x + dw + sp, small = ts * 0.42f, slash = ts * 0.68f;
            DrawText("0", px, base - ts * 0.34f, small);
            DrawText("/", px + small * 0.28f, base - ts * 0.02f, slash);
            DrawText("0", px + small * 0.80f, base + ts * 0.12f, small);
            DrawText(" COMPLETE", px + pw, base, ts);
        }
    } else {
        // the order the game numbers its Jinjos: bit i of the item
        static const int order[5] = { S_JB, S_JG, S_JO, S_JP, S_JY };
        for (int i = 0; i < 5; i++) {
            bool got = (g_snap.jinjos >> i) & 1;
            DrawIcon(order[i], 245 + i * 185, 742, 138, got ? 255 : 92, got ? ms : 0, i + 1);
        }
    }
    DrawTextCentered(g_snap.name[0] ? g_snap.name : "BANJO KAZOOIE", 620, 990, 68, 1120);
}

void* DrawThread(void*) {
    for (;;) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long ms = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
        if (g_loaded) DrawStats(ms);
        else Fill(0, 0, g_buf.w, g_buf.h, 0xFF000000);
        memcpy(g_front, g_buf.px, g_front_bytes);
        usleep(60000);
    }
    return nullptr;
}

} // namespace

// called by the game thread once a frame (gameloop.c, game_update)
extern "C" void BananaPanel_GameTick(void) {
    if (!g_started) return;
    if (!g_loaded) {
        LoadAssets();
        g_loaded = true;
    }
    g_snap.mode = GetCurrentGameMode();
    g_snap.level = level_get();
    int map = gsworld_getMap();
    if (map != g_snap.map) {
        g_snap.map = map;
        const char* n = port_getLevelName(map);
        snprintf((char*)g_snap.name, sizeof g_snap.name, "%s", n ? n : "");
        LoadBackdrop(map);
    }
    g_snap.health = item_getCount(ITEM_HEALTH);
    g_snap.maxHealth = item_getCount(ITEM_HEALTH_TOTAL);
    g_snap.lives = item_getCount(ITEM_LIFE);
    int lairLevel = g_snap.level == 6;
    g_snap.notes = lairLevel ? itemscore_noteScores_getTotal() : item_getCount(ITEM_NOTE);
    g_snap.eggs = item_getCount(ITEM_EGGS);
    g_snap.red = item_getCount(ITEM_RED_FEATHER);
    g_snap.gold = item_getCount(ITEM_GOLD_FEATHER);
    g_snap.jiggies = lairLevel ? jiggyscore_total() : item_getCount(ITEM_JIGGY_TOTAL);
    g_snap.tokens = item_getCount(ITEM_MUMBO_TOKEN_TOTAL);
    g_snap.lair = fileProgressFlag_get(0xBD) ? 1 : 0; // FILEPROG_BD_ENTER_LAIR_CUTSCENE
    g_snap.jinjos = item_getCount(ITEM_JINJOS);
    int total = jiggyscore_total() + itemscore_noteScores_getTotal() + honeycombscore_get_total();
    g_snap.completion = std::max(0, std::min(100, (total * 100 + 512) / 1024));
}

extern "C" void BananaPanel_Start(SDL_Window* win) {
    if (!getenv("BANANA_PANEL")) return;
    static Panel panel;
    if (!SetupPanel(win, panel)) { fprintf(stderr, "BananaPanel: setup failed\n"); return; }
    pthread_t t;
    pthread_create(&t, nullptr, DrawThread, nullptr);
    g_started = true;
    fprintf(stderr, "BananaPanel: running\n");
}
