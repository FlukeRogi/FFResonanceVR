#pragma once
// Pose math (OpenXR convention: right-handed, +Y up, -Z forward)
#include <cmath>
#include <openxr/openxr.h>

struct V3 { float x = 0, y = 0, z = 0; };
struct Q { float x = 0, y = 0, z = 0, w = 1; };
struct P { Q q; V3 p; };
inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 operator-(V3 a) { return { -a.x, -a.y, -a.z }; }
inline V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Len(V3 a) { return std::sqrt(Dot(a, a)); }
inline Q Mul(Q a, Q b)
{
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
inline Q Conj(Q a) { return { -a.x, -a.y, -a.z, a.w }; }
inline V3 Rot(Q q, V3 v) { Q r = Mul(Mul(q, { v.x, v.y, v.z, 0 }), Conj(q)); return { r.x, r.y, r.z }; }
inline Q Norm(Q q) { float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w); return l > 1e-8f ? Q{ q.x / l, q.y / l, q.z / l, q.w / l } : Q{}; }
inline Q AxisAngle(V3 axis, float rad) { float s = std::sin(rad * 0.5f); return { axis.x * s, axis.y * s, axis.z * s, std::cos(rad * 0.5f) }; }
static const float kDeg = 3.14159265358979f / 180.0f;
inline P Compose(const P& a, const P& b) { return { Mul(a.q, b.q), a.p + Rot(a.q, b.p) }; }
inline P Inv(const P& a) { Q c = Conj(a.q); return { c, Rot(c, -a.p) }; }
inline float AngleDeg(Q a, Q b)
{
    Q d = Mul(Conj(Norm(a)), Norm(b));
    double v = std::sqrt((double)d.x * d.x + (double)d.y * d.y + (double)d.z * d.z);
    return (float)(2.0 * std::atan2(v, std::fabs((double)d.w)) / kDeg);
}
inline P FromXr(const XrPosef& p) { return { Norm({ p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w }), { p.position.x, p.position.y, p.position.z } }; }
inline XrPosef ToXr(const P& p) { XrPosef r; r.orientation = { p.q.x, p.q.y, p.q.z, p.q.w }; r.position = { p.p.x, p.p.y, p.p.z }; return r; }
