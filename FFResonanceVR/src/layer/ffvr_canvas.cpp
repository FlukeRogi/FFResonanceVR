#include "ffvr_canvas.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG   // environment faces (ffvr_env.cpp)
#define STBI_NO_STDIO
#include "stb/stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

static bool ReadAll(const std::wstring& path, std::vector<unsigned char>& out)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    size_t got = n > 0 ? fread(out.data(), 1, n, f) : 0;
    fclose(f);
    return got == (size_t)n && n > 0;
}

bool Image::Load(const std::wstring& path)
{
    std::vector<unsigned char> file;
    if (!ReadAll(path, file)) return false;
    int c = 0;
    unsigned char* d = stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &c, 4);
    if (!d) { w = h = 0; return false; }
    px.resize((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        float a = d[i * 4 + 3] / 255.f;
        px[i * 4 + 0] = d[i * 4 + 0] / 255.f * a; px[i * 4 + 1] = d[i * 4 + 1] / 255.f * a;
        px[i * 4 + 2] = d[i * 4 + 2] / 255.f * a; px[i * 4 + 3] = a;
    }
    stbi_image_free(d);
    return true;
}

bool Font::Load(const std::wstring& path)
{
    if (!ReadAll(path, data)) return false;
    auto* fi = new stbtt_fontinfo();
    if (!stbtt_InitFont(fi, data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0))) { delete fi; return false; }
    info = fi;
    return true;
}
Font::~Font() { delete (stbtt_fontinfo*)info; }

const Font::Glyph& Font::Get(int cp, int size)
{
    auto key = std::make_pair(cp, size);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Glyph g;
    if (info) {
        auto* fi = (stbtt_fontinfo*)info;
        float s = stbtt_ScaleForPixelHeight(fi, (float)size);
        int adv, lsb; stbtt_GetCodepointHMetrics(fi, cp, &adv, &lsb);
        g.advance = adv * s;
        int x0, y0, x1, y1; stbtt_GetCodepointBitmapBox(fi, cp, s, s, &x0, &y0, &x1, &y1);
        g.w = x1 - x0; g.h = y1 - y0; g.x0 = x0; g.y0 = y0;
        if (g.w > 0 && g.h > 0) { g.a.resize((size_t)g.w * g.h); stbtt_MakeCodepointBitmap(fi, g.a.data(), g.w, g.h, g.w, s, s, cp); }
    }
    return cache.emplace(key, std::move(g)).first->second;
}

static std::vector<int> Utf8ToCps(const std::string& s)
{
    std::vector<int> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        int cp = c, n = 1;
        if (c >= 0xF0) { cp = c & 7; n = 4; } else if (c >= 0xE0) { cp = c & 15; n = 3; } else if (c >= 0xC0) { cp = c & 31; n = 2; }
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (s[i + k] & 63);
        out.push_back(cp); i += n;
    }
    return out;
}

float Font::Measure(const std::string& utf8, int size)
{
    float x = 0;
    auto cps = Utf8ToCps(utf8);
    for (size_t i = 0; i < cps.size(); ++i) {
        x += Get(cps[i], size).advance;
        if (info && i + 1 < cps.size()) x += stbtt_GetCodepointKernAdvance((stbtt_fontinfo*)info, cps[i], cps[i + 1]) * stbtt_ScaleForPixelHeight((stbtt_fontinfo*)info, (float)size);
    }
    return x;
}
float Font::Ascent(int size)
{
    if (!info) return (float)size;
    auto* fi = (stbtt_fontinfo*)info;
    int a, d, l; stbtt_GetFontVMetrics(fi, &a, &d, &l);
    return a * stbtt_ScaleForPixelHeight(fi, (float)size);
}

void Canvas::Blend(int x, int y, float r, float g, float b, float a)
{
    if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
    float* p = &px[((size_t)y * w + x) * 4];
    float k = 1 - a;
    p[0] = r + p[0] * k; p[1] = g + p[1] * k; p[2] = b + p[2] * k; p[3] = a + p[3] * k;
}

// coverage of pixel column/row [i, i+1) by [a, b)
static float Cover(int i, float a, float b) { return std::max(0.f, std::min((float)i + 1, b) - std::max((float)i, a)); }

void Canvas::FillRect(float x, float y, float rw, float rh, Col c) { VGradient(x, y, rw, rh, c, c); }

void Canvas::VGradient(float x, float y, float rw, float rh, Col t, Col bcol)
{
    int x0 = std::max(0, (int)std::floor(x)), x1 = std::min(w, (int)std::ceil(x + rw));
    int y0 = std::max(0, (int)std::floor(y)), y1 = std::min(h, (int)std::ceil(y + rh));
    for (int yy = y0; yy < y1; ++yy) {
        float cy = Cover(yy, y, y + rh);
        float u = rh > 0 ? std::min(1.f, std::max(0.f, (yy + 0.5f - y) / rh)) : 0;
        Col c{ t.r + (bcol.r - t.r) * u, t.g + (bcol.g - t.g) * u, t.b + (bcol.b - t.b) * u, t.a + (bcol.a - t.a) * u };
        for (int xx = x0; xx < x1; ++xx) {
            float a = c.a * cy * Cover(xx, x, x + rw);
            Blend(xx, yy, c.r * a, c.g * a, c.b * a, a);
        }
    }
}

void Canvas::Outline(float x, float y, float rw, float rh, float t, Col c)
{
    FillRect(x, y, rw, t, c); FillRect(x, y + rh - t, rw, t, c);
    FillRect(x, y + t, t, rh - 2 * t, c); FillRect(x + rw - t, y + t, t, rh - 2 * t, c);
}

static void Sample(const Image& im, float u, float v, float out[4])
{
    // bilinear, clamped; u,v in source pixels (centres at +0.5)
    float fx = u - 0.5f, fy = v - 0.5f;
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    float ax = fx - x0, ay = fy - y0;
    out[0] = out[1] = out[2] = out[3] = 0;
    for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) {
        int xx = std::min(im.w - 1, std::max(0, x0 + i)), yy = std::min(im.h - 1, std::max(0, y0 + j));
        float wgt = (i ? ax : 1 - ax) * (j ? ay : 1 - ay);
        const float* p = &im.px[((size_t)yy * im.w + xx) * 4];
        for (int c = 0; c < 4; ++c) out[c] += p[c] * wgt;
    }
}

void Canvas::Blit(const Image& im, float dx, float dy, float dw, float dh, Col tint, float sx, float sy, float sw, float sh, bool mirrorX)
{
    if (!im.Valid() || dw <= 0 || dh <= 0) return;
    if (sw < 0) sw = (float)im.w;
    if (sh < 0) sh = (float)im.h;
    int x0 = std::max(0, (int)std::floor(dx)), x1 = std::min(w, (int)std::ceil(dx + dw));
    int y0 = std::max(0, (int)std::floor(dy)), y1 = std::min(h, (int)std::ceil(dy + dh));
    // when shrinking a lot, supersample to avoid aliasing
    int ss = std::max(1, std::min(4, (int)std::ceil(std::max(sw / dw, sh / dh))));
    for (int yy = y0; yy < y1; ++yy) for (int xx = x0; xx < x1; ++xx) {
        float acc[4] = {}; int n = 0;
        for (int j = 0; j < ss; ++j) for (int i = 0; i < ss; ++i) {
            float px_ = xx + (i + 0.5f) / ss, py_ = yy + (j + 0.5f) / ss;
            if (px_ < dx || px_ >= dx + dw || py_ < dy || py_ >= dy + dh) continue;
            float u = (px_ - dx) / dw, v = (py_ - dy) / dh;
            if (mirrorX) u = 1 - u;
            float s[4]; Sample(im, sx + u * sw, sy + v * sh, s);
            for (int c = 0; c < 4; ++c) acc[c] += s[c];
            ++n;
        }
        if (!n) continue;
        float k = 1.f / (ss * ss);
        float a = acc[3] * k * tint.a;
        Blend(xx, yy, acc[0] * k * tint.r * tint.a, acc[1] * k * tint.g * tint.a, acc[2] * k * tint.b * tint.a, a);
    }
}

void Canvas::Blit9(const Image& im, float dx, float dy, float dw, float dh, int sb, float db, Col tint)
{
    if (!im.Valid()) return;
    float sxs[4] = { 0, (float)sb, (float)(im.w - sb), (float)im.w }, sys[4] = { 0, (float)sb, (float)(im.h - sb), (float)im.h };
    float dxs[4] = { dx, dx + db, dx + dw - db, dx + dw }, dys[4] = { dy, dy + db, dy + dh - db, dy + dh };
    for (int j = 0; j < 3; ++j) for (int i = 0; i < 3; ++i)
        Blit(im, dxs[i], dys[j], dxs[i + 1] - dxs[i], dys[j + 1] - dys[j], tint, sxs[i], sys[j], sxs[i + 1] - sxs[i], sys[j + 1] - sys[j]);
}

float Canvas::Text(Font& f, int size, float x, float y, const std::string& utf8, Col c)
{
    auto cps = Utf8ToCps(utf8);
    float start = x;
    for (size_t i = 0; i < cps.size(); ++i) {
        const auto& g = f.Get(cps[i], size);
        int ox = (int)std::lround(x) + g.x0, oy = (int)std::lround(y) + g.y0;
        for (int yy = 0; yy < g.h; ++yy) for (int xx = 0; xx < g.w; ++xx) {
            float a = g.a[(size_t)yy * g.w + xx] / 255.f * c.a;
            if (a > 0) Blend(ox + xx, oy + yy, c.r * a, c.g * a, c.b * a, a);
        }
        x += g.advance;
        if (f.info && i + 1 < cps.size()) x += stbtt_GetCodepointKernAdvance((stbtt_fontinfo*)f.info, cps[i], cps[i + 1]) * stbtt_ScaleForPixelHeight((stbtt_fontinfo*)f.info, (float)size);
    }
    return x - start;
}

void Canvas::ToRGBA8(std::vector<uint8_t>& out, bool bgra) const
{
    out.resize((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        auto q = [](float v) { return (uint8_t)std::lround(std::min(1.f, std::max(0.f, v)) * 255.f); };
        uint8_t r = q(px[i * 4]), g = q(px[i * 4 + 1]), b = q(px[i * 4 + 2]), a = q(px[i * 4 + 3]);
        out[i * 4 + 0] = bgra ? b : r; out[i * 4 + 1] = g; out[i * 4 + 2] = bgra ? r : b; out[i * 4 + 3] = a;
    }
}
