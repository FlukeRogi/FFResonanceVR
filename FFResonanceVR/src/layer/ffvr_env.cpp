// FFResonanceVR environment backdrop.
//
// Each scene is a cube map captured in the game itself (tools\capture_env.lua: the game's own renderer, its own map,
// SceneCapture2D, six 90-degree faces) with a scene-depth capture per face. Here every face becomes a grid mesh pushed
// out to the captured depth, so the deck under your feet, the railings and the rigging sit at their real distance with
// true stereo and head-motion parallax, while sky and far scenery end up far away. The mesh is drawn for each eye from
// the runtime's real eye poses into a projection layer of our own, submitted behind the screen.
//
// Files (assets\env\<folder>\): f r b l u d .jpg (or .png) colour faces - forward (towards the screen), right, back,
// left, up, down - captured with the UE rotators (0,yaw) (0,yaw+90) (0,yaw+180) (0,yaw-90) (90,yaw) (-90,yaw);
// f.f32 ... d.f32: square float32 planar scene depth in cm (tools\exr_depth.ps1); env.ini: [Env] Brightness=.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "ffvr_menu.h"
#include "stb/stb_image.h"

extern HMODULE FfModule();

const EnvInfo kEnvs[] = { { nullptr, "None" }, { "deck", "Airship Deck" }, { "castle", "Castle Courtyard" } };
const int kEnvCount = (int)(sizeof(kEnvs) / sizeof(kEnvs[0]));

namespace {

const float kMaxDepthCm = 60000;  // anything further is put here (sky)
// Depth differences that matter are judged as stereo disparity (1/distance), which is what the eyes see: 1 cm near
// you matters, 10 m at the castle walls barely. Per metre of 1/distance, 64 mm eyes see 0.064 rad = 220 arcmin.
const float kDispEdge = 0.018f;   // 1/m: an outline (about 4 arcmin between the two sides)
const float kDispSep = 0.009f;    // 1/m: a pixel this far behind its foreground triangle may go to the background (2 arcmin)
inline bool ApartM(float nearM, float farM, float t = kDispEdge) { return 1 / nearM - 1 / farM > t; }
inline bool ApartCm(float nearCm, float farCm, float t = kDispEdge) { return 100 / nearCm - 100 / farCm > t; }
const int kEyeW = 1760, kEyeH = 1760;

// Test switch FFVR_ENV_MODE: 0 = pictures as captured, 1 = edge colour clean-up and the per-layer pictures (default).
int EnvMode()
{
    wchar_t b[8] = L"";
    if (GetEnvironmentVariableW(L"FFVR_ENV_MODE", b, 8) && b[0] >= L'0' && b[0] <= L'1') return b[0] - L'0';
    return 1;
}

struct Vtx { float x, y, z, u, v, f; };   // f: 1 = foreground mesh, 0 = background mesh

// CPU side, produced by the loader thread
struct EnvData {
    int faceSize = 0, mips = 0;
    std::vector<std::vector<uint8_t>> level[6];   // [face][mip] RGBA8: the foreground mesh's picture
    std::vector<std::vector<uint8_t>> bgLevel[6]; // [face][mip] the background mesh's picture (half resolution)
    int bgSize = 0, bgMips = 0;
    std::vector<Vtx> verts;
    std::vector<uint32_t> idx;
    float brightness = 0.85f;
    float extent = 1.0f;   // face images cover -extent..+extent (overlap with the neighbours for cross-fading)
    float eyeHeight = 1.7f; // m: capture point above the scene's floor (depth straight down)
};

std::wstring EnvDir(const char* folder)
{
    wchar_t p[MAX_PATH]; GetModuleFileNameW(FfModule(), p, MAX_PATH);
    std::wstring d = p; d = d.substr(0, d.find_last_of(L"\\/"));
    d = d.substr(0, d.find_last_of(L"\\/"));
    std::wstring f; for (const char* c = folder; *c; ++c) f += (wchar_t)*c;
    return d + L"\\assets\\env\\" + f + L"\\";
}

bool ReadFile(const std::wstring& path, std::vector<uint8_t>& out)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    bool ok = n > 0 && fread(out.data(), 1, n, f) == (size_t)n;
    fclose(f);
    return ok;
}

// UE (X forward, Y right, Z up; cm) -> OpenXR (x right, y up, -z forward; m)
V3 UeToXr(float X, float Y, float Z) { return { Y * 0.01f, Z * 0.01f, -X * 0.01f }; }

void FaceBasis(int face, float fwd[3], float right[3], float up[3])
{
    static const float pitchYaw[6][2] = { { 0, 0 }, { 0, 90 }, { 0, 180 }, { 0, -90 }, { 90, 0 }, { -90, 0 } };
    float p = pitchYaw[face][0] * kDeg, y = pitchYaw[face][1] * kDeg;
    float cp = std::cos(p), sp = std::sin(p), cy = std::cos(y), sy = std::sin(y);
    fwd[0] = cp * cy; fwd[1] = cp * sy; fwd[2] = sp;
    right[0] = -sy; right[1] = cy; right[2] = 0;
    up[0] = -sp * cy; up[1] = -sp * sy; up[2] = cp;
}

// The six depth faces, looked up by direction.
struct DepthCube {
    std::vector<uint8_t> raw[6];
    int D[6] = {};
    float fw[6][3], rt[6][3], up[6][3];
    // Distance (cm) from the capture point along unit direction dir: the min (or max) over a (2*win+1)^2 pixel
    // window around it, over every face that has pixels there (edges and corners are shared by 2-3 faces).
    float Radial(const float dir[3], int win, bool wantMin) const
    {
        float best = wantMin ? 1e30f : -1.f;
        bool any = false;
        for (int k = 0; k < 6; ++k) {
            float f = dir[0] * fw[k][0] + dir[1] * fw[k][1] + dir[2] * fw[k][2];
            if (f <= 0.3f) continue;
            float u = (dir[0] * rt[k][0] + dir[1] * rt[k][1] + dir[2] * rt[k][2]) / f;
            float v = (dir[0] * up[k][0] + dir[1] * up[k][1] + dir[2] * up[k][2]) / f;
            if (!D[k]) {   // no depth for this face: a 50 m sphere
                if (std::fabs(u) <= 1.0001f && std::fabs(v) <= 1.0001f) { best = wantMin ? std::min(best, 5000.f) : std::max(best, 5000.f); any = true; }
                continue;
            }
            const int n = D[k];
            float fx = (u + 1) * 0.5f * n - 0.5f, fy = (1 - v) * 0.5f * n - 0.5f;   // pixel-centre coordinates
            int cx = (int)std::floor(fx + 0.5f), cy = (int)std::floor(fy + 0.5f);
            if (cx < -win - 1 || cy < -win - 1 || cx > n + win || cy > n + win) continue;
            const float* z = (const float*)raw[k].data();
            for (int y = std::max(0, cy - win); y <= std::min(n - 1, cy + win); ++y)
                for (int x = std::max(0, cx - win); x <= std::min(n - 1, cx + win); ++x) {
                    float d = z[(size_t)y * n + x];
                    if (!(d > 1)) continue;
                    float pu = (x + 0.5f) * 2.f / n - 1, pv = 1 - (y + 0.5f) * 2.f / n;
                    float r = std::min(d, kMaxDepthCm) * std::sqrt(1 + pu * pu + pv * pv);
                    best = wantMin ? std::min(best, r) : std::max(best, r);
                    any = true;
                }
        }
        if (!any) return kMaxDepthCm;
        return std::min(best, kMaxDepthCm * 1.8f);
    }
    // The nearest distance (cm) within the window around dir that is clearly behind 'nearCm' (ApartCm), or -1.
    float Beyond(const float dir[3], int win, float nearCm) const
    {
        float best = 1e30f;
        for (int k = 0; k < 6; ++k) {
            if (!D[k]) continue;
            float f = dir[0] * fw[k][0] + dir[1] * fw[k][1] + dir[2] * fw[k][2];
            if (f <= 0.3f) continue;
            float u = (dir[0] * rt[k][0] + dir[1] * rt[k][1] + dir[2] * rt[k][2]) / f;
            float v = (dir[0] * up[k][0] + dir[1] * up[k][1] + dir[2] * up[k][2]) / f;
            const int n = D[k];
            float fx = (u + 1) * 0.5f * n - 0.5f, fy = (1 - v) * 0.5f * n - 0.5f;
            int cx = (int)std::floor(fx + 0.5f), cy = (int)std::floor(fy + 0.5f);
            if (cx < -win - 1 || cy < -win - 1 || cx > n + win || cy > n + win) continue;
            const float* z = (const float*)raw[k].data();
            for (int y = std::max(0, cy - win); y <= std::min(n - 1, cy + win); ++y)
                for (int x = std::max(0, cx - win); x <= std::min(n - 1, cx + win); ++x) {
                    float d = z[(size_t)y * n + x];
                    if (!(d > 1)) continue;
                    float pu = (x + 0.5f) * 2.f / n - 1, pv = 1 - (y + 0.5f) * 2.f / n;
                    float r = std::min(d, kMaxDepthCm) * std::sqrt(1 + pu * pu + pv * pv);
                    if (ApartCm(nearCm, r)) best = std::min(best, r);
                }
        }
        return best < 1e29f ? std::min(best, kMaxDepthCm * 1.8f) : -1.f;
    }
    // A copy holding only the pixels on the near side of a depth jump (a 4-neighbour clearly further away, ApartCm);
    // the rest are 0 (ignored by the lookups).
    DepthCube NearEdges() const
    {
        DepthCube o = *this;
        for (int k = 0; k < 6; ++k) {
            const int n = D[k];
            if (!n) continue;
            const float* z = (const float*)raw[k].data();
            float* w = (float*)o.raw[k].data();
            for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
                float d = z[(size_t)y * n + x];
                bool edge = false;
                if (d > 1) {
                    const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                    for (auto& q : nb) {
                        int xx = x + q[0], yy = y + q[1];
                        if (xx < 0 || yy < 0 || xx >= n || yy >= n) continue;
                        if (ApartCm(d, z[(size_t)yy * n + xx])) { edge = true; break; }
                    }
                }
                w[(size_t)y * n + x] = edge ? d : 0.f;
            }
        }
        return o;
    }
};

// Cleans up the colour along depth edges of face k. The game's anti-aliasing blends each outline pixel with what is
// behind it (a lamp post's edge pixels are part sky), and the mesh puts such a pixel at one of the two depths. In a
// band of 2 pixels around every depth edge, a pixel that is a blend of the two sides (by colour, between the nearest
// pixel outside the band on the near side and on the far side) gets the colour of the side it is closer to. The image
// is changed once at load; nothing depends on the view. Returns the number of re-coloured pixels.
// Distance (m) from the capture point for every colour pixel of face k (pixel centres; the image covers +-extent).
std::vector<float> FaceDistances(const EnvData& e, const DepthCube& dc, int k)
{
    const int N = e.faceSize;
    const float E = e.extent;
    std::vector<float> R((size_t)N * N);
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
        float u = ((x + 0.5f) * 2.f / N - 1) * E, v = (1 - (y + 0.5f) * 2.f / N) * E;
        float d[3]; for (int a = 0; a < 3; ++a) d[a] = dc.fw[k][a] + u * dc.rt[k][a] + v * dc.up[k][a];
        float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (float& c : d) c /= l;
        R[(size_t)y * N + x] = dc.Radial(d, 0, true) * 0.01f;
    }
    return R;
}

size_t CleanFaceEdges(EnvData& e, const std::vector<float>& R, int k)
{
    const int N = e.faceSize, B = 2, W = 6;
    auto& col = e.level[k][0];
    std::vector<uint8_t> band((size_t)N * N, 0);
    auto mark = [&](int cx, int cy) {
        for (int y = std::max(0, cy - B); y <= std::min(N - 1, cy + B); ++y)
            for (int x = std::max(0, cx - B); x <= std::min(N - 1, cx + B); ++x) band[(size_t)y * N + x] = 1;
    };
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
        float r = R[(size_t)y * N + x];
        if (x + 1 < N) { float q = R[(size_t)y * N + x + 1]; if (ApartM(std::min(r, q), std::max(r, q))) mark(x, y); }
        if (y + 1 < N) { float q = R[(size_t)(y + 1) * N + x]; if (ApartM(std::min(r, q), std::max(r, q))) mark(x, y); }
    }
    size_t changed = 0;
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
        size_t i = (size_t)y * N + x;
        if (!band[i]) continue;
        float lo = 1e30f, hi = 0;
        for (int yy = std::max(0, y - W); yy <= std::min(N - 1, y + W); ++yy)
            for (int xx = std::max(0, x - W); xx <= std::min(N - 1, x + W); ++xx) { float r = R[(size_t)yy * N + xx]; lo = std::min(lo, r); hi = std::max(hi, r); }
        if (!ApartM(lo, hi)) continue;
        const float mid = std::sqrt(lo * hi);
        // the nearest pixel outside the band on each side (a real pixel: an average would mix e.g. tower and sky)
        size_t jn = SIZE_MAX, jf = SIZE_MAX; int dn = INT_MAX, df = INT_MAX;
        for (int yy = std::max(0, y - W); yy <= std::min(N - 1, y + W); ++yy)
            for (int xx = std::max(0, x - W); xx <= std::min(N - 1, x + W); ++xx) {
                size_t j = (size_t)yy * N + xx;
                if (band[j]) continue;
                int d2 = (xx - x) * (xx - x) + (yy - y) * (yy - y);
                if (R[j] < mid) { if (d2 < dn) { dn = d2; jn = j; } }
                else if (d2 < df) { df = d2; jf = j; }
            }
        if (jn == SIZE_MAX || jf == SIZE_MAX) continue;
        float nc[3], fc[3];
        for (int a = 0; a < 3; ++a) { nc[a] = col[jn * 4 + a]; fc[a] = col[jf * 4 + a]; }
        float dv[3] = { nc[0] - fc[0], nc[1] - fc[1], nc[2] - fc[2] };
        float dd = dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2];
        if (dd < 300) continue;
        uint8_t* c = &col[i * 4];
        float t = ((c[0] - fc[0]) * dv[0] + (c[1] - fc[1]) * dv[1] + (c[2] - fc[2]) * dv[2]) / dd;
        bool nearSide = t > 0.5f;
        if (t > 0.12f && t < 0.88f) {
            const float* s = nearSide ? nc : fc;
            for (int a = 0; a < 3; ++a) c[a] = (uint8_t)std::lround(std::max(0.f, std::min(255.f, s[a])));
            ++changed;
        }
    }
    return changed;
}

// A face's two meshes: distances (m) at the (G+1)^2 grid vertices, and which foreground triangles were left out.
struct FaceGrid { std::vector<float> fg, bg; std::vector<uint8_t> dropped; };   // dropped: 2 triangles per cell

// The meshes at a direction of face k (u, v within -1..1): the distance each mesh is drawn at there, and whether the
// foreground triangle there exists. Same triangles and interpolation as the GPU (to within the radial approximation).
bool GridAt(const FaceGrid& g, int G, float u, float v, float& rf, float& rb, bool& kept)
{
    if (std::fabs(u) > 1 || std::fabs(v) > 1) return false;
    float gx = (u + 1) * 0.5f * G, gy = (1 - v) * 0.5f * G;
    int i = std::min(G - 1, (int)gx), j = std::min(G - 1, (int)gy);
    float s = gx - i, t = gy - j;
    int a = j * (G + 1) + i, b = a + 1, c = a + G + 1, d = c + 1;
    bool lower = s + t <= 1;
    auto at = [&](const std::vector<float>& r) {
        return lower ? r[a] + s * (r[b] - r[a]) + t * (r[c] - r[a]) : r[d] + (1 - s) * (r[c] - r[d]) + (1 - t) * (r[b] - r[d]);
    };
    rf = at(g.fg); rb = at(g.bg);
    kept = !g.dropped[((size_t)j * G + i) * 2 + (lower ? 0 : 1)];
    return true;
}

// Each layer gets its own picture, fixed once at load (nothing depends on the view, so nothing can shimmer):
//  * foreground: where a foreground triangle is drawn at a near object's distance but the pixel shows something
//    further away (the sky sliver along a lamp post that the mesh grid cannot follow), the pixel is made transparent
//    (alpha 0), so the background mesh behind shows it at its real distance and the outline follows the picture,
//    not the 8-pixel mesh grid; its colour is grown in from the object, so filtering does not bleed sky into the edge;
//  * background: where the background mesh (only seen in the gaps that open behind outlines when the head moves)
//    would show a near object that the foreground already draws, it gets the colour of what is further away - so
//    the gaps show sky or wall instead of a ghost of the object.
// The colour grows in from neighbouring pixels of the right distance, one pixel per pass (fg up to 12, bg up to 40
// pixels); pixels it does not reach keep their colour. Pixels beyond the face edge (the cross-fade overlap) are left.
// bg receives the background picture. Returns the numbers of changed pixels.
std::pair<size_t, size_t> FixFaceLayers(EnvData& e, const std::vector<float>& R, const FaceGrid& g, int G, int k,
                                        std::vector<uint8_t>& bg)
{
    const int N = e.faceSize;
    const float E = e.extent;
    auto& fgc = e.level[k][0];
    bg = fgc;
    struct Need { int32_t i; float limit; };
    std::vector<Need> needF, needB;
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
        float u = ((x + 0.5f) * 2.f / N - 1) * E, v = (1 - (y + 0.5f) * 2.f / N) * E;
        float rf, rb; bool kept;
        if (!GridAt(g, G, u, v, rf, rb, kept) || !kept) continue;
        size_t i = (size_t)y * N + x;
        // a far pixel on the foreground goes to the background (transparent here) when the background mesh is nearer
        // its true distance (compared as stereo disparity, 1/distance: what the eyes see)
        if (ApartM(rf, R[i], kDispSep) && std::fabs(1 / R[i] - 1 / rb) < std::fabs(1 / R[i] - 1 / rf)) {
            needF.push_back({ (int32_t)i, std::sqrt(rf * R[i]) }); fgc[i * 4 + 3] = 0;
        }
        else if (ApartM(R[i], rb)) needB.push_back({ (int32_t)i, 2 / (1 / R[i] + 1 / rb) });   // near pixel on the background
    }
    // grow colour into the pixels in 'need' from neighbours whose (carried) distance passes the limit
    auto grow = [&](std::vector<uint8_t>& col, std::vector<Need>& need, bool wantNear, int passes) {
        std::vector<int16_t> pass((size_t)N * N, 0);      // 0: has its own colour; -1: to fill; >0: filled in that pass
        std::vector<float> dist(R);
        for (auto& n : need) pass[n.i] = -1;
        const std::vector<Need> needAll = need;
        size_t filled = 0;
        for (int p = 1; p <= passes && !need.empty(); ++p) {
            std::vector<Need> rest;
            std::vector<std::pair<int32_t, std::array<float, 4>>> done;   // applied after the pass (no order bias)
            for (auto& n : need) {
                int x = n.i % N, y = n.i / N;
                float acc[3] = {}, dsum = 0; int cnt = 0;
                const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                for (auto& o : nb) {
                    int xx = x + o[0], yy = y + o[1];
                    if (xx < 0 || yy < 0 || xx >= N || yy >= N) continue;
                    size_t j = (size_t)yy * N + xx;
                    if (pass[j] < 0 || pass[j] >= p) continue;
                    if (wantNear ? dist[j] > n.limit : dist[j] < n.limit) continue;
                    for (int a = 0; a < 3; ++a) acc[a] += col[j * 4 + a];
                    dsum += dist[j]; ++cnt;
                }
                if (cnt) done.push_back({ n.i, { acc[0] / cnt, acc[1] / cnt, acc[2] / cnt, dsum / cnt } });
                else rest.push_back(n);
            }
            for (auto& d : done) {
                for (int a = 0; a < 3; ++a) col[(size_t)d.first * 4 + a] = (uint8_t)std::lround(d.second[a]);
                dist[d.first] = d.second[3]; pass[d.first] = (int16_t)p;
            }
            filled += done.size();
            need.swap(rest);
        }
        if (!wantNear) {
            // the fill spreads in straight lines (streaks, steps); smooth it among filled pixels and far source pixels
            std::vector<float> lim((size_t)N * N, 0);
            std::vector<int32_t> list;
            for (size_t i = 0; i < pass.size(); ++i) if (pass[i] > 0) list.push_back((int32_t)i);
            for (auto& n : needAll) lim[n.i] = n.limit;
            for (int it = 0; it < 6; ++it) {
                std::vector<std::array<uint8_t, 3>> out(list.size());
                for (size_t q = 0; q < list.size(); ++q) {
                    int i = list[q], x = i % N, y = i / N;
                    float acc[3] = {}; int cnt = 0;
                    for (int yy = std::max(0, y - 2); yy <= std::min(N - 1, y + 2); ++yy)
                        for (int xx = std::max(0, x - 2); xx <= std::min(N - 1, x + 2); ++xx) {
                            size_t j = (size_t)yy * N + xx;
                            if (!(pass[j] > 0 || (pass[j] == 0 && R[j] >= lim[i]))) continue;
                            for (int a = 0; a < 3; ++a) acc[a] += col[j * 4 + a];
                            ++cnt;
                        }
                    for (int a = 0; a < 3; ++a) out[q][a] = cnt ? (uint8_t)std::lround(acc[a] / cnt) : col[(size_t)i * 4 + a];
                }
                for (size_t q = 0; q < list.size(); ++q) for (int a = 0; a < 3; ++a) col[(size_t)list[q] * 4 + a] = out[q][a];
            }
        }
        return filled;
    };
    size_t nf = grow(fgc, needF, true, 12);
    size_t nb = grow(bg, needB, false, 40);
    return { nf, nb };
}

// Full-resolution depth (<face>.zd, tools\pack_depth.ps1): 16-bit log2(cm) * 4096, row deltas, deflate.
bool LoadZd(const std::wstring& path, std::vector<uint8_t>& out, int& D)
{
    std::vector<uint8_t> f;
    if (!ReadFile(path, f) || f.size() < 8 || memcmp(f.data(), "FFZD", 4)) return false;
    int n = 0; memcpy(&n, f.data() + 4, 4);
    int len = 0;
    char* p = stbi_zlib_decode_noheader_malloc((const char*)f.data() + 8, (int)f.size() - 8, &len);
    if (!p || n <= 0 || len != n * n * 2) { free(p); return false; }
    out.resize((size_t)n * n * 4);
    float* z = (float*)out.data();
    const uint16_t* q = (const uint16_t*)p;
    for (int y = 0; y < n; ++y) {
        uint32_t prev = 0;
        for (int x = 0; x < n; ++x) { prev = (prev + q[(size_t)y * n + x]) & 0xFFFF; z[(size_t)y * n + x] = std::exp2(prev / 4096.f); }
    }
    free(p);
    D = n;
    return true;
}

std::shared_ptr<EnvData> LoadEnv(const char* folder)
{
    static const char* names[6] = { "f", "r", "b", "l", "u", "d" };
    auto e = std::make_shared<EnvData>();
    std::wstring dir = EnvDir(folder);
    e->brightness = (float)GetPrivateProfileIntW(L"Env", L"BrightnessPercent", 85, (dir + L"env.ini").c_str()) / 100.f;
    { wchar_t b[32]; GetPrivateProfileStringW(L"Env", L"Extent", L"1.0", b, 32, (dir + L"env.ini").c_str()); e->extent = std::max(1.0f, std::min(1.5f, (float)_wtof(b))); }
    for (int k = 0; k < 6; ++k) {
        std::vector<uint8_t> file;
        std::wstring base = dir; for (const char* c = names[k]; *c; ++c) base += (wchar_t)*c;
        if (!ReadFile(base + L".jpg", file) && !ReadFile(base + L".png", file)) { FfLog("env %s: missing face %s", folder, names[k]); return nullptr; }
        int w, h, c;
        unsigned char* px = stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &c, 4);
        if (!px || w != h || (k > 0 && w != e->faceSize)) { if (px) stbi_image_free(px); FfLog("env %s: bad face %s", folder, names[k]); return nullptr; }
        e->faceSize = w;
        e->level[k].emplace_back(px, px + (size_t)w * h * 4);
        stbi_image_free(px);
    }
    // depth maps (planar depth in cm per face)
    DepthCube dc;
    for (int k = 0; k < 6; ++k) {
        std::wstring base = dir; base += (wchar_t)names[k][0];
        auto& raw = dc.raw[k];
        if (ReadFile(base + L".f32", raw)) { int D = (int)std::lround(std::sqrt(raw.size() / 4.0)); if ((size_t)D * D * 4 == raw.size()) dc.D[k] = D; }
        if (!dc.D[k]) FfLog("env %s: no depth for face %s, using a 50 m sphere", folder, names[k]);
        FaceBasis(k, dc.fw[k], dc.rt[k], dc.up[k]);
    }
    // full-resolution depth for the picture fixes (<face>.zd); without it the mesh depth is used
    DepthCube fine;
    int fineFaces = 0;
    for (int k = 0; k < 6; ++k) {
        std::wstring base = dir; base += (wchar_t)names[k][0];
        if (LoadZd(base + L".zd", fine.raw[k], fine.D[k])) ++fineFaces;
        else { fine.raw[k] = dc.raw[k]; fine.D[k] = dc.D[k]; }
        FaceBasis(k, fine.fw[k], fine.rt[k], fine.up[k]);
    }
    const int mode = EnvMode();
    const int kGrid = 256;   // mesh cells per face edge (2 mesh-depth pixels per cell)
    FaceGrid grids[6];
    const DepthCube edges = dc.NearEdges();
    { const float down[3] = { 0, 0, -1 }; float r = dc.Radial(down, 2, true); if (r > 50 && r < 400) e->eyeHeight = r * 0.01f; }
    // Two meshes per face over the same directions:
    //  * foreground: the nearest surface around each vertex; triangles that span a depth jump (an object's outline
    //    against the distance - seen edge-on from the capture point) are left out, so nothing is stretched between
    //    near and far;
    //  * background, slightly further out: the furthest surface around each vertex, all triangles. It shows only in
    //    the small gaps the foreground leaves behind outlines when the head moves away from the capture point.
    // Depth is looked up by direction across all faces, so vertices on a cube edge get the same position on both
    // faces (no cracks along the edges).
    size_t dropped = 0;
    // (The background mesh is drawn first: the foreground is blended over it where its picture is transparent.)
    for (int pass = 0; pass < 2; ++pass) {
        const bool fg = pass == 1;
        for (int k = 0; k < 6; ++k) {
            uint32_t first = (uint32_t)e->verts.size();
            std::vector<float> rad((kGrid + 1) * (kGrid + 1));
            for (int j = 0; j <= kGrid; ++j) for (int i = 0; i <= kGrid; ++i) {
                float u = -1 + 2.f * i / kGrid, v = 1 - 2.f * j / kGrid;
                float dir[3]; for (int a = 0; a < 3; ++a) dir[a] = dc.fw[k][a] + u * dc.rt[k][a] + v * dc.up[k][a];
                float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                for (float& c : dir) c /= len;
                // foreground: the nearest surface around the vertex; at an outline (a pixel on the near side of a depth
                // jump within 3 mesh pixels) it reaches out to the object, so every mesh cell holding any of the near
                // object's pixels has all its corners at the object's distance: such a cell is drawn (its far pixels
                // transparent) rather than left out or stretched - left out, its object pixels would be drawn by the
                // background mesh far away (stair-stepped fragments in the sky). Smooth surfaces (a receding floor)
                // have no marked jumps, so they are not pulled nearer.
                float rf = dc.Radial(dir, 1, true);
                { float rE = edges.Radial(dir, 3, true); if (ApartCm(rE, rf)) rf = rE; }
                float r = rf;
                if (!fg) {
                    // background: the next surface behind the foreground (a wall behind a tree, not the sky behind
                    // the wall); where there is none (no outline nearby), the furthest surface around the vertex
                    r = dc.Beyond(dir, 4, rf);
                    if (r < 0) r = dc.Radial(dir, 4, false);
                    r *= 1.02f;
                }
                rad[j * (kGrid + 1) + i] = r;
                V3 p = UeToXr(dir[0] * r, dir[1] * r, dir[2] * r);
                e->verts.push_back({ p.x, p.y, p.z, (float)i / kGrid, (float)j / kGrid, fg ? 1.f : 0.f });
            }
            auto& gr = fg ? grids[k].fg : grids[k].bg;
            gr.resize(rad.size());
            for (size_t q = 0; q < rad.size(); ++q) gr[q] = rad[q] * 0.01f;
            if (fg) grids[k].dropped.assign((size_t)kGrid * kGrid * 2, 0);
            auto edgeOn = [&](uint32_t a, uint32_t b, uint32_t c) {
                // |cos| between the triangle's normal and the view ray from the capture point
                const Vtx &A = e->verts[a], &B = e->verts[b], &C = e->verts[c];
                float ux = B.x - A.x, uy = B.y - A.y, uz = B.z - A.z, wx = C.x - A.x, wy = C.y - A.y, wz = C.z - A.z;
                float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
                float mx = (A.x + B.x + C.x) / 3, my = (A.y + B.y + C.y) / 3, mz = (A.z + B.z + C.z) / 3;
                float nl = std::sqrt(nx * nx + ny * ny + nz * nz), ml = std::sqrt(mx * mx + my * my + mz * mz);
                return nl < 1e-12f || std::fabs(nx * mx + ny * my + nz * mz) < 0.015f * nl * ml;
            };
            for (int j = 0; j < kGrid; ++j) for (int i = 0; i < kGrid; ++i) {
                uint32_t a = first + j * (kGrid + 1) + i, b = a + 1, c = a + kGrid + 1, d = c + 1;
                int tri = 0;
                for (auto t : { std::array<uint32_t, 3>{ a, b, c }, std::array<uint32_t, 3>{ b, d, c } }) {
                    if (fg && edgeOn(t[0], t[1], t[2])) { ++dropped; grids[k].dropped[((size_t)j * kGrid + i) * 2 + tri] = 1; ++tri; continue; }
                    e->idx.insert(e->idx.end(), { t[0], t[1], t[2] });
                    ++tri;
                }
            }
        }
    }
    // the pictures: edge colour clean-up, then each layer's own fixed picture (FixFaceLayers)
    {
        DWORD t0 = GetTickCount();
        std::vector<std::thread> th;
        size_t cleaned[6] = {}, fixedF[6] = {}, fixedB[6] = {};
        std::vector<uint8_t> bgFull[6];
        for (int k = 0; k < 6; ++k) th.emplace_back([&, k] {
            if (mode >= 1) {
                std::vector<float> R = FaceDistances(*e, fine, k);
                cleaned[k] = CleanFaceEdges(*e, R, k);
                if (fineFaces == 6) {   // (needs the full-resolution depth: from the mesh depth the edges would be 4-pixel steps)
                    auto n = FixFaceLayers(*e, R, grids[k], kGrid, k, bgFull[k]);
                    fixedF[k] = n.first; fixedB[k] = n.second;
                } else {
                    bgFull[k] = e->level[k][0];
                }
            } else {
                bgFull[k] = e->level[k][0];
            }
            // debug: FFVR_ENV_DUMP=<folder> writes each face's two pictures and a mask (raw RGBA, N x N):
            // mask R = foreground triangle left out, G = foreground transparent, B = background picture changed
            wchar_t dd[MAX_PATH];
            if (GetEnvironmentVariableW(L"FFVR_ENV_DUMP", dd, MAX_PATH)) {
                const int N = e->faceSize;
                std::vector<uint8_t> m((size_t)N * N * 4, 0);
                for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
                    size_t i = (size_t)y * N + x;
                    float u = ((x + 0.5f) * 2.f / N - 1) * e->extent, v = (1 - (y + 0.5f) * 2.f / N) * e->extent;
                    float rf, rb; bool kept;
                    if (GridAt(grids[k], kGrid, u, v, rf, rb, kept) && !kept) m[i * 4] = 255;
                    if (e->level[k][0][i * 4 + 3] == 0) m[i * 4 + 1] = 255;
                    if (bgFull[k].size() == m.size() && memcmp(&bgFull[k][i * 4], &e->level[k][0][i * 4], 3) && m[i * 4 + 1] == 0) m[i * 4 + 2] = 255;
                    m[i * 4 + 3] = 255;
                }
                // err: the distance each pixel is drawn at from the capture point (foreground where drawn and opaque,
                // else background) vs its true distance: red = drawn too near, blue = too far (by over 2 arcmin of
                // stereo disparity)
                std::vector<uint8_t> er((size_t)N * N * 4, 0);
                {
                    std::vector<float> R = FaceDistances(*e, fine, k);
                    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
                        size_t i = (size_t)y * N + x;
                        const uint8_t* c = &e->level[k][0][i * 4];
                        uint8_t gy = (uint8_t)((c[0] + c[1] + c[2]) / 9);
                        er[i * 4] = er[i * 4 + 1] = er[i * 4 + 2] = gy; er[i * 4 + 3] = 255;
                        float u = ((x + 0.5f) * 2.f / N - 1) * e->extent, v = (1 - (y + 0.5f) * 2.f / N) * e->extent;
                        float rf, rb; bool kept;
                        if (!GridAt(grids[k], kGrid, u, v, rf, rb, kept)) continue;
                        float drawn = (kept && c[3] > 127) ? rf : rb;
                        // as stereo disparity error (64 mm eyes): over 2 arcmin
                        float dd = 0.064f * (1 / drawn - 1 / R[i]);
                        if (dd > 0.00058f) er[i * 4] = 255;
                        else if (dd < -0.00058f) er[i * 4 + 2] = 255;
                    }
                }
                const char* nm[4] = { "fg", "bg", "mask", "err" };
                const std::vector<uint8_t>* src[4] = { &e->level[k][0], &bgFull[k], &m, &er };
                for (int q = 0; q < 4; ++q) {
                    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%s\\%d_%S.rgba", dd, k, nm[q]);
                    if (FILE* f = _wfopen(p, L"wb")) { fwrite(src[q]->data(), 1, src[q]->size(), f); fclose(f); }
                }
            }
        });
        for (auto& t : th) t.join();
        size_t c = 0, f = 0, b = 0; for (int k = 0; k < 6; ++k) { c += cleaned[k]; f += fixedF[k]; b += fixedB[k]; }
        FfLog("env %s: %d full-resolution depth faces; edge colours cleaned %zu px, foreground fixed %zu px, background fixed %zu px (%lu ms)",
              folder, fineFaces, c, f, b, GetTickCount() - t0);
        // mip chains (box filter on the sRGB values: close enough for a backdrop); the background picture is kept at
        // half resolution (it is only seen in small gaps)
        auto down = [](const std::vector<uint8_t>& src, int s) {
            int d = s / 2;
            std::vector<uint8_t> dst((size_t)d * d * 4);
            for (int y = 0; y < d; ++y) for (int x = 0; x < d; ++x) for (int ch = 0; ch < 4; ++ch) {
                int sum = src[((size_t)(2 * y) * s + 2 * x) * 4 + ch] + src[((size_t)(2 * y) * s + 2 * x + 1) * 4 + ch] +
                          src[((size_t)(2 * y + 1) * s + 2 * x) * 4 + ch] + src[((size_t)(2 * y + 1) * s + 2 * x + 1) * 4 + ch];
                dst[((size_t)y * d + x) * 4 + ch] = (uint8_t)((sum + 2) / 4);
            }
            return dst;
        };
        for (int k = 0; k < 6; ++k) {
            auto& lv = e->level[k];
            for (int s = e->faceSize; s > 1; s /= 2) lv.push_back(down(lv.back(), s));
            auto& bl = e->bgLevel[k];
            bl.push_back(down(bgFull[k], e->faceSize));
            for (int s = e->faceSize / 2; s > 1; s /= 2) bl.push_back(down(bl.back(), s));
        }
        e->mips = (int)e->level[0].size();
        e->bgSize = e->faceSize / 2;
        e->bgMips = (int)e->bgLevel[0].size();
    }
    FfLog("env %s loaded: faces %d px, %d mips, %zu vertices, %zu edge-on triangles left out", folder, e->faceSize, e->mips, e->verts.size(), dropped);
    return e;
}

// The pixel shader looks the colour up by direction from the capture point. Where faces meet, it cross-fades
// between them over a band of +-blend (tangent units) around the edge, using the overlap each face image has beyond
// its edge (extent > 1): the game shades a view looking straight up or down a little differently, and the fade turns
// that step into a soft gradient.
// (Per-pixel outlines - discarding foreground pixels by a colour-resolution distance map - were tried on 2026-11-22 and
// rejected in the headset: the per-pixel decisions shimmer with head motion, foliage worst. The edge fixes are now in
// the pictures themselves, made once at load: FixFaceLayers.)
const char* kShader = R"(
cbuffer C : register(b0) { row_major float4x4 mvp; float4 tint; float4 face; };   // face.x extent, face.y blend
Texture2DArray tex : register(t0);     // the foreground mesh's picture
Texture2DArray texBg : register(t1);   // the background mesh's picture (see FixFaceLayers)
SamplerState sm : register(s0);
struct VO { float4 p : SV_Position; float3 pos : TEXCOORD0; nointerpolation float fg : TEXCOORD1; };
VO vs(float3 p : POSITION, float3 uv : TEXCOORD) { VO o; o.p = mul(mvp, float4(p, 1)); o.pos = p; o.fg = uv.z; return o; }
static const float3 FW[6] = { float3(1,0,0), float3(0,1,0), float3(-1,0,0), float3(0,-1,0), float3(0,0,1), float3(0,0,-1) };
static const float3 RT[6] = { float3(0,1,0), float3(-1,0,0), float3(0,-1,0), float3(1,0,0), float3(0,1,0), float3(0,1,0) };
static const float3 UP[6] = { float3(0,0,1), float3(0,0,1), float3(0,0,1), float3(0,0,1), float3(-1,0,0), float3(1,0,0) };
float4 ps(VO i) : SV_Target
{
    float3 d = float3(-i.pos.z, i.pos.x, i.pos.y);   // OpenXR -> UE axes
    float E = face.x, B = face.y;
    float3 sum = 0; float wsum = 0, asum = 0;
    [unroll] for (int k = 0; k < 6; ++k) {
        float f = max(dot(d, FW[k]), 1e-3 * length(d));
        float u = dot(d, RT[k]) / f, v = dot(d, UP[k]) / f;
        float w;
        if (B > 0) w = saturate((1 + B - abs(u)) / (2 * B)) * saturate((1 + B - abs(v)) / (2 * B));
        else w = (abs(u) <= 1 && abs(v) <= 1) ? 1 : 0;
        w *= dot(d, FW[k]) > 0 ? 1 : 0;
        w = w * w * (3 - 2 * w);
        float2 uv = float2((clamp(u, -E, E) / E + 1) * 0.5, (1 - clamp(v, -E, E) / E) * 0.5);
        float2 gx = ddx(uv), gy = ddy(uv);   // (outside the branch: every pixel of the quad computes them)
        if (w > 0) {
            float4 c = i.fg > 0.5 ? tex.SampleGrad(sm, float3(uv, k), gx, gy) : float4(texBg.SampleGrad(sm, float3(uv, k), gx, gy).rgb, 1);
            sum += w * c.rgb; asum += w * c.a; wsum += w;
        }
    }
    // alpha: the foreground picture's transparency (blended over the background mesh, drawn first)
    return float4(sum / max(wsum, 1e-4) * tint.rgb, asum / max(wsum, 1e-4));
}
)";

// ---- GPU state ----------------------------------------------------------------------------------------------
XrSession g_ses = XR_NULL_HANDLE;
ID3D12Device* g_dev = nullptr;
ID3D12CommandQueue* g_q = nullptr;
XrSwapchain g_sc = XR_NULL_HANDLE;
std::vector<ID3D12Resource*> g_images;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
ID3D12DescriptorHeap *g_rtvHeap = nullptr, *g_dsvHeap = nullptr, *g_srvHeap = nullptr;
ID3D12Resource *g_depth = nullptr, *g_tex = nullptr, *g_texBg = nullptr, *g_vb = nullptr, *g_ib = nullptr, *g_upload = nullptr;
ID3D12RootSignature* g_rs = nullptr;
ID3D12PipelineState* g_pso = nullptr;
const int kFrames = 3;
ID3D12CommandAllocator* g_alloc[kFrames] = {};
UINT64 g_allocFence[kFrames] = {};
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
UINT64 g_fenceVal = 0;
int g_frameSlot = 0;
bool g_pipelineFailed = false;
UINT g_indexCount = 0;
float g_brightness = 0.85f;
float g_extent = 1.0f;
float g_eyeHeight = 1.7f;
int g_gpuEnv = 0;                         // scene whose data is on the GPU
UINT64 g_uploadFence = 0;                 // the upload buffer can go once the GPU has passed this
XrCompositionLayerProjection g_layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
XrCompositionLayerProjectionView g_views[2];

// loader
std::mutex g_loadMutex;
std::shared_ptr<EnvData> g_loaded;        // finished, waiting for upload
int g_loadingEnv = 0, g_loadedEnv = 0;
std::atomic<bool> g_loading{ false };

template <class T> void Rel(T*& p) { if (p) p->Release(); p = nullptr; }

void WaitIdle()
{
    if (!g_fence || g_fence->GetCompletedValue() >= g_fenceVal) return;
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_fence->SetEventOnCompletion(g_fenceVal, e); WaitForSingleObject(e, 2000); CloseHandle(e);
}

void ReleaseScene() { WaitIdle(); Rel(g_tex); Rel(g_texBg); Rel(g_vb); Rel(g_ib); Rel(g_upload); g_gpuEnv = 0; g_indexCount = 0; }

bool CreatePipeline()
{
    if (g_pso) return true;
    if (g_pipelineFailed || !g_dev || !g_q) return false;
    g_pipelineFailed = true;
    // swapchain (both eyes side by side)
    uint32_t n = 0;
    Nx_EnumerateSwapchainFormats()(g_ses, 0, &n, nullptr);
    std::vector<int64_t> fmts(n);
    Nx_EnumerateSwapchainFormats()(g_ses, n, &n, fmts.data());
    for (int64_t want : { (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB })
        if (!g_fmt) for (auto f : fmts) if (f == want) { g_fmt = (DXGI_FORMAT)f; break; }
    if (!g_fmt) { FfLog("env: no sRGB swapchain format"); return false; }
    XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = g_fmt; ci.sampleCount = 1; ci.width = kEyeW * 2; ci.height = kEyeH; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    if (Nx_CreateSwapchain()(g_ses, &ci, &g_sc) != XR_SUCCESS) { FfLog("env: xrCreateSwapchain failed"); g_sc = XR_NULL_HANDLE; return false; }
    Nx_EnumerateSwapchainImages()(g_sc, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
    Nx_EnumerateSwapchainImages()(g_sc, n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
    for (auto& i : imgs) g_images.push_back(i.texture);

    D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, n };
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_rtvHeap))) return false;
    hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_dsvHeap))) return false;
    hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_srvHeap))) return false;
    UINT rtvStep = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (uint32_t i = 0; i < n; ++i) {
        D3D12_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = g_fmt; rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * rtvStep;
        g_dev->CreateRenderTargetView(g_images[i], &rv, h);
    }
    D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC dd{}; dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; dd.Width = kEyeW * 2; dd.Height = kEyeH;
    dd.DepthOrArraySize = 1; dd.MipLevels = 1; dd.Format = DXGI_FORMAT_D32_FLOAT; dd.SampleDesc.Count = 1; dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv{}; cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 0;
    if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, __uuidof(ID3D12Resource), (void**)&g_depth))) return false;
    g_dev->CreateDepthStencilView(g_depth, nullptr, g_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    // root signature: 24 root constants (b0), one SRV table (t0 foreground, t1 background picture), static sampler (s0)
    D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants = { 0, 0, 24 }; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable = { 1, &range }; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC ss{}; ss.Filter = D3D12_FILTER_ANISOTROPIC; ss.MaxAnisotropy = 8;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; ss.MaxLOD = D3D12_FLOAT32_MAX; ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd{ 2, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3DBlob *sig = nullptr, *err = nullptr, *vs = nullptr, *ps = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { FfLog("env: root signature failed"); return false; }
    HRESULT hr = g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&g_rs);
    sig->Release();
    if (FAILED(hr)) return false;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "env", nullptr, nullptr, "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &err)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), "env", nullptr, nullptr, "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &err))) {
        FfLog("env: shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        Rel(err); Rel(vs); return false;
    }
    D3D12_INPUT_ELEMENT_DESC il[2] = { { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                                       { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_rs;
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    {   // colour blended by the picture's alpha; the layer's own alpha stays 1 (opaque for the compositor)
        auto& bs = pd.BlendState.RenderTarget[0];
        bs.BlendEnable = TRUE;
        bs.SrcBlend = D3D12_BLEND_SRC_ALPHA; bs.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; bs.BlendOp = D3D12_BLEND_OP_ADD;
        bs.SrcBlendAlpha = D3D12_BLEND_ZERO; bs.DestBlendAlpha = D3D12_BLEND_ONE; bs.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    pd.SampleMask = 0xffffffff;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = TRUE; pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;   // reversed Z
    pd.InputLayout = { il, 2 };
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = g_fmt; pd.DSVFormat = DXGI_FORMAT_D32_FLOAT; pd.SampleDesc.Count = 1;
    hr = g_dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&g_pso);
    vs->Release(); ps->Release();
    if (FAILED(hr)) { FfLog("env: pipeline state failed 0x%08x", (unsigned)hr); return false; }
    for (auto& a : g_alloc) if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&a))) return false;
    if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&g_list)) ||
        FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fence))) return false;
    g_list->Close();
    g_pipelineFailed = false;
    FfLog("env pipeline ready: swapchain %dx%d, %u images", kEyeW * 2, kEyeH, n);
    return true;
}

ID3D12Resource* Buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp{ type };
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = size; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, __uuidof(ID3D12Resource), (void**)&r))) return nullptr;
    return r;
}

// Records the upload of a loaded scene into the (open) command list.
bool RecordUpload(const EnvData& e)
{
    ReleaseScene();
    D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
    // two texture arrays: the foreground picture and the background picture (each 6 faces with mips)
    struct Tex { ID3D12Resource** res; const std::vector<std::vector<uint8_t>>* levels; int size, mips; D3D12_RESOURCE_DESC desc;
                 std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp; std::vector<UINT> rows; std::vector<UINT64> rowBytes; UINT64 bytes = 0, offset = 0; };
    Tex tx[2] = { { &g_tex, e.level, e.faceSize, e.mips }, { &g_texBg, e.bgLevel, e.bgSize, e.bgMips } };
    UINT64 off = 0;
    for (auto& t : tx) {
        D3D12_RESOURCE_DESC& td = t.desc; td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = t.size; td.Height = t.size;
        td.DepthOrArraySize = 6; td.MipLevels = (UINT16)t.mips; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; td.SampleDesc.Count = 1;
        if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)t.res))) return false;
        UINT subs = 6 * t.mips;
        t.fp.resize(subs); t.rows.resize(subs); t.rowBytes.resize(subs);
        t.offset = (off + 511) & ~511ull;
        g_dev->GetCopyableFootprints(&td, 0, subs, t.offset, t.fp.data(), t.rows.data(), t.rowBytes.data(), &t.bytes);
        off = t.offset + t.bytes;
    }
    UINT64 vbBytes = e.verts.size() * sizeof(Vtx), ibBytes = e.idx.size() * 4;
    UINT64 vbOff = (off + 255) & ~255ull, ibOff = (vbOff + vbBytes + 255) & ~255ull;
    g_upload = Buffer(D3D12_HEAP_TYPE_UPLOAD, ibOff + ibBytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    g_vb = Buffer(D3D12_HEAP_TYPE_DEFAULT, vbBytes, D3D12_RESOURCE_STATE_COPY_DEST);
    g_ib = Buffer(D3D12_HEAP_TYPE_DEFAULT, ibBytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!g_upload || !g_vb || !g_ib) return false;
    uint8_t* map = nullptr; D3D12_RANGE none{ 0, 0 };
    if (FAILED(g_upload->Map(0, &none, (void**)&map))) return false;
    for (auto& t : tx)
        for (int f = 0; f < 6; ++f) for (int m = 0; m < t.mips; ++m) {
            UINT s = f * t.mips + m; const auto& src = t.levels[f][m];
            for (UINT y = 0; y < t.rows[s]; ++y)
                memcpy(map + t.fp[s].Offset + (size_t)y * t.fp[s].Footprint.RowPitch, src.data() + (size_t)y * t.rowBytes[s], (size_t)t.rowBytes[s]);
        }
    memcpy(map + vbOff, e.verts.data(), vbBytes);
    memcpy(map + ibOff, e.idx.data(), ibBytes);
    g_upload->Unmap(0, nullptr);
    for (auto& t : tx)
        for (UINT s = 0; s < (UINT)t.fp.size(); ++s) {
            D3D12_TEXTURE_COPY_LOCATION d{ *t.res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; d.SubresourceIndex = s;
            D3D12_TEXTURE_COPY_LOCATION sl{ g_upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; sl.PlacedFootprint = t.fp[s];
            g_list->CopyTextureRegion(&d, 0, 0, 0, &sl, nullptr);
        }
    g_list->CopyBufferRegion(g_vb, 0, g_upload, vbOff, vbBytes);
    g_list->CopyBufferRegion(g_ib, 0, g_upload, ibOff, ibBytes);
    D3D12_RESOURCE_BARRIER b[4]{};
    ID3D12Resource* res[4] = { g_tex, g_texBg, g_vb, g_ib };
    D3D12_RESOURCE_STATES after[4] = { D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                       D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_INDEX_BUFFER };
    for (int i = 0; i < 4; ++i) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[i].Transition.pResource = res[i];
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b[i].Transition.StateAfter = after[i];
    }
    g_list->ResourceBarrier(4, b);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    for (auto& t : tx) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = t.desc.Format; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2DArray.MipLevels = t.mips; sv.Texture2DArray.ArraySize = 6;
        g_dev->CreateShaderResourceView(*t.res, &sv, h);
        h.ptr += g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    g_indexCount = (UINT)e.idx.size();
    g_brightness = e.brightness;
    g_extent = e.extent;
    g_eyeHeight = e.eyeHeight;
    return true;
}

// row-major 4x4, column vectors (p' = M p)
struct M4 { float m[16]; };
M4 Mul(const M4& a, const M4& b)
{
    M4 r{};
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { float s = 0; for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j]; r.m[i * 4 + j] = s; }
    return r;
}
M4 FromPose(const P& p)
{
    V3 x = Rot(p.q, { 1, 0, 0 }), y = Rot(p.q, { 0, 1, 0 }), z = Rot(p.q, { 0, 0, 1 });
    return { { x.x, y.x, z.x, p.p.x, x.y, y.y, z.y, p.p.y, x.z, y.z, z.z, p.p.z, 0, 0, 0, 1 } };
}
// OpenXR FOV -> D3D clip, reversed Z with an infinite far plane
M4 Projection(const XrFovf& f, float nearZ)
{
    float l = std::tan(f.angleLeft), r = std::tan(f.angleRight), u = std::tan(f.angleUp), d = std::tan(f.angleDown);
    return { { 2 / (r - l), 0, (r + l) / (r - l), 0,
               0, 2 / (u - d), (u + d) / (u - d), 0,
               0, 0, 0, nearZ,
               0, 0, -1, 0 } };
}

void StartLoad(int env)
{
    if (g_loading) return;
    g_loading = true; g_loadingEnv = env;
    std::thread([env] {
        auto e = LoadEnv(kEnvs[env].folder);
        { std::lock_guard<std::mutex> lk(g_loadMutex); g_loaded = e; g_loadedEnv = e ? env : -env; }
        g_loading = false;
    }).detach();
}

} // namespace

void EnvInit(XrSession s, ID3D12Device* dev, ID3D12CommandQueue* q)
{
    g_ses = s; g_dev = dev; g_q = q; g_pipelineFailed = false;
}

void EnvShutdown()
{
    WaitIdle();
    ReleaseScene();
    if (g_sc) Nx_DestroySwapchain()(g_sc);
    g_sc = XR_NULL_HANDLE; g_images.clear(); g_fmt = DXGI_FORMAT_UNKNOWN;
    Rel(g_pso); Rel(g_rs); Rel(g_depth); Rel(g_rtvHeap); Rel(g_dsvHeap); Rel(g_srvHeap); Rel(g_list); Rel(g_fence);
    for (auto& a : g_alloc) Rel(a);
    for (auto& v : g_allocFence) v = 0;
    g_fenceVal = 0;
    g_ses = XR_NULL_HANDLE; g_dev = nullptr; g_q = nullptr;
}

void EnvAddLayer(XrTime t, const P& S, int env, std::vector<const XrCompositionLayerBaseHeader*>& out)
{
    if (env <= 0 || env >= kEnvCount || !g_dev) return;
    // scene data: load in the background, upload when ready
    std::shared_ptr<EnvData> ready;
    {
        std::lock_guard<std::mutex> lk(g_loadMutex);
        if (g_loaded && g_loadedEnv == env) { ready = g_loaded; }
        if (g_loadedEnv != env && g_loadedEnv != -env && g_gpuEnv != env) { g_loaded.reset(); if (!g_loading) StartLoad(env); }
    }
    if (g_gpuEnv != env && !ready) return;
    if (!CreatePipeline()) return;

    XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO };
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; li.displayTime = t; li.space = LayerLocalSpace();
    XrViewState vs{ XR_TYPE_VIEW_STATE };
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t nv = 0;
    if (Nx_LocateViews()(g_ses, &li, &vs, 2, &nv, views) != XR_SUCCESS || nv < 2 ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return;

    int slot = g_frameSlot; g_frameSlot = (g_frameSlot + 1) % kFrames;
    if (g_fence->GetCompletedValue() < g_allocFence[slot]) {
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_fence->SetEventOnCompletion(g_allocFence[slot], e); WaitForSingleObject(e, 100); CloseHandle(e);
        if (g_fence->GetCompletedValue() < g_allocFence[slot]) return;
    }
    uint32_t idx = 0;
    if (Nx_AcquireSwapchainImage()(g_sc, nullptr, &idx) != XR_SUCCESS) return;
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = 100000000;
    if (Nx_WaitSwapchainImage()(g_sc, &wi) != XR_SUCCESS) { Nx_ReleaseSwapchainImage()(g_sc, nullptr); return; }
    g_alloc[slot]->Reset();
    g_list->Reset(g_alloc[slot], nullptr);
    if (ready) {
        bool ok = RecordUpload(*ready);
        { std::lock_guard<std::mutex> lk(g_loadMutex); g_loaded.reset(); }
        g_gpuEnv = ok ? env : 0;
        if (!ok) FfLog("env: upload failed");
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)idx * g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    const float black[4] = { 0, 0, 0, 1 };
    g_list->ClearRenderTargetView(rtv, black, 0, nullptr);
    g_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    // Floor: the scene was captured from standing eye height. With "real floor", the scene is moved up or down so that
    // its floor is where the real floor is for the head height at the last recentre (so it is right sitting too).
    float floorShift = 0;
    { float hh = RecentreHeadHeight(); if (CurrentSettings().envFloor && hh > 0) floorShift = std::max(-0.6f, std::min(1.4f, g_eyeHeight - hh)); }
    static float lastShift = 1e9f;
    if (std::fabs(floorShift - lastShift) > 0.005f && g_gpuEnv == env) { lastShift = floorShift; FfLog("env floor: scene eye height %.2f m, head %.2f m above the real floor -> scene moved %+.2f m", g_eyeHeight, RecentreHeadHeight(), floorShift); }
    if (g_gpuEnv == env && g_indexCount) {
        g_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        g_list->SetGraphicsRootSignature(g_rs);
        g_list->SetPipelineState(g_pso);
        ID3D12DescriptorHeap* heaps[] = { g_srvHeap };
        g_list->SetDescriptorHeaps(1, heaps);
        g_list->SetGraphicsRootDescriptorTable(1, g_srvHeap->GetGPUDescriptorHandleForHeapStart());
        g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VERTEX_BUFFER_VIEW vbv{ g_vb->GetGPUVirtualAddress(), (UINT)(g_vb->GetDesc().Width), sizeof(Vtx) };
        D3D12_INDEX_BUFFER_VIEW ibv{ g_ib->GetGPUVirtualAddress(), g_indexCount * 4, DXGI_FORMAT_R32_UINT };
        g_list->IASetVertexBuffers(0, 1, &vbv);
        g_list->IASetIndexBuffer(&ibv);
        for (int eye = 0; eye < 2; ++eye) {
            D3D12_VIEWPORT vp{ (float)(eye * kEyeW), 0, (float)kEyeW, (float)kEyeH, 0, 1 };
            D3D12_RECT sr{ eye * kEyeW, 0, (eye + 1) * kEyeW, kEyeH };
            g_list->RSSetViewports(1, &vp); g_list->RSSetScissorRects(1, &sr);
            // scene frame = S (viewer at the capture point, facing the screen) -> eye
            P eyeP = FromXr(views[eye].pose);
            M4 mvp = Mul(Projection(views[eye].fov, 0.05f), FromPose(Compose(Inv(eyeP), Compose(S, P{ Q{}, { 0, floorShift, 0 } }))));
            float c[24]; memcpy(c, mvp.m, 64); c[16] = c[17] = c[18] = g_brightness; c[19] = 1;
            c[20] = g_extent; c[21] = std::min(0.08f, std::max(0.f, g_extent - 1.02f)); c[22] = c[23] = 0;
            g_list->SetGraphicsRoot32BitConstants(0, 24, c, 0);
            g_list->DrawIndexedInstanced(g_indexCount, 1, 0, 0, 0);
        }
    }
    g_list->Close();
    ID3D12CommandList* lists[] = { g_list };
    g_q->ExecuteCommandLists(1, lists);
    g_q->Signal(g_fence, ++g_fenceVal);
    g_allocFence[slot] = g_fenceVal;
    if (ready) g_uploadFence = g_fenceVal;
    else if (g_upload && g_fence->GetCompletedValue() >= g_uploadFence) Rel(g_upload);
    Nx_ReleaseSwapchainImage()(g_sc, nullptr);
    if (g_gpuEnv != env) return;

    for (int eye = 0; eye < 2; ++eye) {
        g_views[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
        g_views[eye].pose = views[eye].pose;
        g_views[eye].fov = views[eye].fov;
        g_views[eye].subImage.swapchain = g_sc;
        g_views[eye].subImage.imageRect = { { eye * kEyeW, 0 }, { kEyeW, kEyeH } };
    }
    g_layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    g_layer.space = LayerLocalSpace();
    g_layer.viewCount = 2;
    g_layer.views = g_views;
    out.push_back((const XrCompositionLayerBaseHeader*)&g_layer);
}
