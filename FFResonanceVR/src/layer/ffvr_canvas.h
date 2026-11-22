#pragma once
// Minimal CPU 2D renderer for the VR menu: premultiplied-alpha RGBA float canvas, bilinear image blits (with tint and
// 9-slice), gradients and TrueType text (stb_truetype). Colours are sRGB-encoded values, blended in that space like
// the game's UI.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct Col { float r = 1, g = 1, b = 1, a = 1; };
inline Col Rgb(int r, int g, int b, float a = 1) { return { r / 255.f, g / 255.f, b / 255.f, a }; }

struct Image {
    int w = 0, h = 0;
    std::vector<float> px;   // premultiplied RGBA, 0..1
    bool Load(const std::wstring& path);
    bool Valid() const { return w > 0 && h > 0; }
};

struct Font {
    std::vector<unsigned char> data;
    void* info = nullptr;    // stbtt_fontinfo
    bool Load(const std::wstring& path);
    ~Font();
    struct Glyph { int w = 0, h = 0, x0 = 0, y0 = 0; float advance = 0; std::vector<unsigned char> a; };
    std::map<std::pair<int, int>, Glyph> cache;   // (codepoint, pixel size)
    const Glyph& Get(int cp, int size);
    float Measure(const std::string& utf8, int size);
    float Ascent(int size);
};

struct Canvas {
    int w = 0, h = 0;
    std::vector<float> px;
    void Resize(int W, int H) { w = W; h = H; px.assign((size_t)W * H * 4, 0.f); }
    void Clear() { std::fill(px.begin(), px.end(), 0.f); }
    void Blend(int x, int y, float r, float g, float b, float a);   // premultiplied "over"
    void FillRect(float x, float y, float w, float h, Col c);
    void VGradient(float x, float y, float w, float h, Col top, Col bottom);
    void Outline(float x, float y, float w, float h, float t, Col c);
    // image sub-rect (sx,sy,sw,sh) -> dest rect, tinted; mirrorX flips horizontally
    void Blit(const Image& im, float dx, float dy, float dw, float dh, Col tint = {}, float sx = 0, float sy = 0, float sw = -1, float sh = -1, bool mirrorX = false);
    // 9-slice: src border (pixels of the image) drawn at dst border size
    void Blit9(const Image& im, float dx, float dy, float dw, float dh, int srcBorder, float dstBorder, Col tint = {});
    // text; returns the advance. x is the left edge, y the baseline
    float Text(Font& f, int size, float x, float y, const std::string& utf8, Col c);
    // 8-bit output, RGBA or BGRA, premultiplied
    void ToRGBA8(std::vector<uint8_t>& out, bool bgra) const;
};
