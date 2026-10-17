/*
 * af_math.h -- header-only vector / matrix / quaternion / noise layer for the AvatarForge
 * C++ kernel. Mirrors the semantics of the Kotlin `engine.math` package one-for-one so the
 * JVM-side SDK can pre/post-process without semantic drift.
 *
 * Conventions
 *   - Right-handed, +Y up, -Z forward (camera & glTF convention).
 *   - Mat4 column-major float[16], identical to GLSL/glTF byte layout.
 *   - SI units, metres.
 */
#ifndef AF_MATH_H
#define AF_MATH_H

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <limits>

namespace af {

static constexpr float kPi = 3.14159265358979323846f;
static constexpr float kTau = 6.28318530717958647692f;
static constexpr float kEps = 1e-6f;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float saturate(float v) { return clampf(v, 0.0f, 1.0f); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float sqr(float v) { return v * v; }
inline float radians(float deg) { return deg * (kPi / 180.0f); }
inline float degrees(float rad) { return rad * (180.0f / kPi); }
inline float sign(float v) { return v < 0.0f ? -1.0f : 1.0f; }
inline int   mini(int a, int b) { return a < b ? a : b; }
inline int   maxi(int a, int b) { return a > b ? a : b; }
inline float minf(float a, float b) { return a < b ? a : b; }
inline float maxf(float a, float b) { return a > b ? a : b; }

inline float smoothstepf(float e0, float e1, float x) {
    const float t = saturate((x - e0) / (e1 - e0 + kEps));
    return t * t * (3.0f - 2.0f * t);
}

/** Polynomial smooth-min: the workhorse for fusing anatomical primitives into one surface. */
inline float smin(float a, float b, float k) {
    if (k <= kEps) return minf(a, b);
    const float h = saturate(0.5f + 0.5f * (b - a) / k);
    return lerpf(b, a, h) - k * h * (1.0f - h);
}

inline float smax(float a, float b, float k) {
    if (k <= kEps) return maxf(a, b);
    const float h = saturate(0.5f - 0.5f * (b - a) / k);
    return lerpf(b, a, h) + k * h * (1.0f - h);
}

/* --------------------------------------------------------------------------- Vec3 -------- */
struct Vec2 {
    float x = 0.0f, y = 0.0f;
    Vec2() = default;
    Vec2(float a, float b) : x(a), y(b) {}
    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2 minv(const Vec2& o) const { return {minf(x, o.x), minf(y, o.y)}; }
    Vec2 maxv(const Vec2& o) const { return {maxf(x, o.x), maxf(y, o.y)}; }
    float lengthSq() const { return x * x + y * y; }
    float length() const { return std::sqrt(lengthSq()); }
    bool isFinite() const { return std::isfinite(x) && std::isfinite(y); }
};

struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    explicit Vec3(float s) : x(s), y(s), z(s) {}

    float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3 operator*(const Vec3& o) const { return {x * o.x, y * o.y, z * o.z}; }   /* component-wise */
    Vec3 operator/(const Vec3& o) const { return {x / o.x, y / o.y, z / o.z}; }   /* component-wise */
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    Vec3& operator/=(float s) { x /= s; y /= s; z /= s; return *this; }
    
    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    float lengthSq() const { return x * x + y * y + z * z; }
    float length() const { return std::sqrt(lengthSq()); }
    float distanceTo(const Vec3& o) const { return (*this - o).length(); }
    Vec3 normalized() const {
        const float l = length();
        return l < kEps ? Vec3(0, 0, 0) : Vec3(x / l, y / l, z / l);
    }
    Vec3 normalizedOr(const Vec3& fallback) const {
        const float l = length();
        return l < kEps ? fallback : Vec3(x / l, y / l, z / l);
    }
    Vec3 lerp(const Vec3& o, float t) const { return {af::lerpf(x, o.x, t), af::lerpf(y, o.y, t), af::lerpf(z, o.z, t)}; }
    Vec3 abs() const { return {std::fabs(x), std::fabs(y), std::fabs(z)}; }
    Vec3 minv(const Vec3& o) const { return {minf(x, o.x), minf(y, o.y), minf(z, o.z)}; }
    Vec3 maxv(const Vec3& o) const { return {maxf(x, o.x), maxf(y, o.y), maxf(z, o.z)}; }
    Vec3 withY(float yy) const { return {x, yy, z}; }
    Vec3 withX(float xx) const { return {xx, y, z}; }
    Vec3 withZ(float zz) const { return {x, y, zz}; }
    float maxComponent() const { return maxf(x, maxf(y, z)); }
    float minComponent() const { return minf(x, minf(y, z)); }
    bool isFinite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
};
inline Vec3 operator*(float s, const Vec3& v) { return v * s; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 vmin(const Vec3& a, const Vec3& b) { return a.minv(b); }
inline Vec3 vmax(const Vec3& a, const Vec3& b) { return a.maxv(b); }

/* --------------------------------------------------------------------------- Vec4 -------- */
struct Vec4 {
    float x, y, z, w;
    Vec4() : x(0), y(0), z(0), w(0) {}
    Vec4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
    Vec4(const Vec3& v, float d) : x(v.x), y(v.y), z(v.z), w(d) {}
    Vec3 xyz() const { return {x, y, z}; }
    float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
    Vec4 operator+(const Vec4& o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    Vec4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
};

/* ------------------------------------------------------------------------- Quat ---------- */
struct Quat {
    float x, y, z, w;
    Quat() : x(0), y(0), z(0), w(1) {}
    Quat(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}

    static Quat identity() { return {0, 0, 0, 1}; }

    Quat operator*(const Quat& o) const {
        return {
            w * o.x + x * o.w + y * o.z - z * o.y,
            w * o.y - x * o.z + y * o.w + z * o.x,
            w * o.z + x * o.y - y * o.x + z * o.w,
            w * o.w - x * o.x - y * o.y - z * o.z
        };
    }
    Quat operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
    Quat operator+(const Quat& o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }

    Quat conjugate() const { return {-x, -y, -z, w}; }
    float dot(const Quat& o) const { return x * o.x + y * o.y + z * o.z + w * o.w; }
    Quat normalized() const {
        const float l = std::sqrt(x * x + y * y + z * z + w * w);
        return l < kEps ? identity() : Quat(x / l, y / l, z / l, w / l);
    }

    Vec3 rotate(const Vec3& v) const {
        const Vec3 qv(x, y, z);
        const Vec3 t = qv.cross(v) + v * w;
        return v + qv.cross(t) * 2.0f;
    }

    static Quat fromAxisAngle(const Vec3& axis, float angleRad) {
        const Vec3 a = axis.normalizedOr({0, 0, 1});
        const float h = angleRad * 0.5f;
        const float s = std::sin(h);
        return Quat(a.x * s, a.y * s, a.z * s, std::cos(h));
    }

    static Quat fromEulerXYZ(const Vec3& e) {
        return (fromAxisAngle({0, 0, 1}, e.z) * fromAxisAngle({0, 1, 0}, e.y) *
                fromAxisAngle({1, 0, 0}, e.x)).normalized();
    }

    static Quat fromTo(const Vec3& from, const Vec3& to) {
        const Vec3 f = from.normalizedOr({0, 1, 0});
        const Vec3 t = to.normalizedOr({0, 1, 0});
        const float d = clampf(f.dot(t), -1.0f, 1.0f);
        if (d > 0.99999f) return identity();
        if (d < -0.99999f) {
            Vec3 axis = Vec3(1, 0, 0).cross(f);
            if (axis.lengthSq() < 1e-8f) axis = Vec3(0, 1, 0).cross(f);
            return fromAxisAngle(axis.normalizedOr({1, 0, 0}), kPi);
        }
        const Vec3 c = f.cross(t);
        return Quat(c.x, c.y, c.z, 1.0f + d).normalized();
    }

    static Quat slerp(const Quat& a, const Quat& bIn, float t) {
        float cosHalf = a.dot(bIn);
        Quat b = bIn;
        if (cosHalf < 0.0f) { b = Quat(-b.x, -b.y, -b.z, -b.w); cosHalf = -cosHalf; }
        if (cosHalf > 0.9995f) {
            return Quat(lerpf(a.x, b.x, t), lerpf(a.y, b.y, t), lerpf(a.z, b.z, t), lerpf(a.w, b.w, t)).normalized();
        }
        const float theta = std::acos(clampf(cosHalf, -1.0f, 1.0f));
        const float s = std::sin(theta);
        const float wa = std::sin((1.0f - t) * theta) / s;
        const float wb = std::sin(t * theta) / s;
        return Quat(a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb).normalized();
    }
};

/* ------------------------------------------------------------------------- Mat4 ---------- */
struct Mat4 {
    float m[16];
    Mat4() { for (int i = 0; i < 16; ++i) m[i] = 0.0f; }
    static Mat4 identity() {
        Mat4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
    static Mat4 translation(const Vec3& t) {
        Mat4 r = identity();
        r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
        return r;
    }
    static Mat4 scale(const Vec3& s) {
        Mat4 r = identity();
        r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
        return r;
    }
    static Mat4 fromQuat(const Quat& qIn) {
        const Quat q = qIn.normalized();
        const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        Mat4 r;
        r.m[0] = 1 - 2 * (yy + zz); r.m[4] = 2 * (xy - wz);     r.m[8]  = 2 * (xz + wy);
        r.m[1] = 2 * (xy + wz);     r.m[5] = 1 - 2 * (xx + zz); r.m[9]  = 2 * (yz - wx);
        r.m[2] = 2 * (xz - wy);     r.m[6] = 2 * (yz + wx);     r.m[10] = 1 - 2 * (xx + yy);
        r.m[3] = 0; r.m[7] = 0; r.m[11] = 0;
        r.m[12] = 0; r.m[13] = 0; r.m[14] = 0; r.m[15] = 1;
        return r;
    }
    static Mat4 trs(const Vec3& t, const Quat& q, const Vec3& s) {
        Mat4 r = fromQuat(q);
        r.m[0] *= s.x; r.m[1] *= s.x; r.m[2] *= s.x;
        r.m[4] *= s.y; r.m[5] *= s.y; r.m[6] *= s.y;
        r.m[8] *= s.z; r.m[9] *= s.z; r.m[10] *= s.z;
        r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
        return r;
    }
    /** Frame with +Y along (to-from) and origin at `from`; used to seat bone spaces. */
    static Mat4 alignY(const Vec3& from, const Vec3& to, const Vec3& upHint = {0, 0, 1}) {
        const Vec3 yAxis = (to - from).normalizedOr({0, 1, 0});
        Vec3 zAxis = upHint - yAxis * upHint.dot(yAxis);
        if (zAxis.lengthSq() < 1e-8f) zAxis = Vec3(1, 0, 0) - yAxis * yAxis.x;
        zAxis = zAxis.normalizedOr({0, 0, 1});
        const Vec3 xAxis = yAxis.cross(zAxis).normalizedOr({1, 0, 0});
        Mat4 r = identity();
        r.m[0] = xAxis.x; r.m[1] = xAxis.y; r.m[2] = xAxis.z;
        r.m[4] = yAxis.x; r.m[5] = yAxis.y; r.m[6] = yAxis.z;
        r.m[8] = zAxis.x; r.m[9] = zAxis.y; r.m[10] = zAxis.z;
        r.m[12] = from.x; r.m[13] = from.y; r.m[14] = from.z;
        return r;
    }
    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int c = 0; c < 4; ++c) {
            const int c4 = c * 4;
            for (int row = 0; row < 4; ++row) {
                r.m[c4 + row] = m[row] * o.m[c4] + m[4 + row] * o.m[c4 + 1] +
                                m[8 + row] * o.m[c4 + 2] + m[12 + row] * o.m[c4 + 3];
            }
        }
        return r;
    }
    Vec3 transformPoint(const Vec3& p) const {
        const float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
        const float iw = std::fabs(w) < kEps ? 1.0f : 1.0f / w;
        return { (m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12]) * iw,
                 (m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13]) * iw,
                 (m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]) * iw };
    }
    Vec3 transformDirection(const Vec3& d) const {
        return { m[0] * d.x + m[4] * d.y + m[8] * d.z,
                 m[1] * d.x + m[5] * d.y + m[9] * d.z,
                 m[2] * d.x + m[6] * d.y + m[10] * d.z };
    }
    Mat4 inverse() const {
        const float* a = m;
        float inv[16];
        inv[0]  =  a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
        inv[4]  = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
        inv[8]  =  a[4]*a[9]*a[15]  - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
        inv[12] = -a[4]*a[9]*a[14]  + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
        inv[1]  = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
        inv[5]  =  a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
        inv[9]  = -a[0]*a[9]*a[15]  + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
        inv[13] =  a[0]*a[9]*a[14]  - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
        inv[2]  =  a[1]*a[6]*a[15]  - a[1]*a[7]*a[14]  - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7]  - a[13]*a[3]*a[6];
        inv[6]  = -a[0]*a[6]*a[15]  + a[0]*a[7]*a[14]  + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7]  + a[12]*a[3]*a[6];
        inv[10] =  a[0]*a[5]*a[15]  - a[0]*a[7]*a[13]  - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7]  - a[12]*a[3]*a[5];
        inv[14] = -a[0]*a[5]*a[14]  + a[0]*a[6]*a[13]  + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6]  + a[12]*a[2]*a[5];
        inv[3]  = -a[1]*a[6]*a[11]  + a[1]*a[7]*a[10]  + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7]   + a[9]*a[3]*a[6];
        inv[7]  =  a[0]*a[6]*a[11]  - a[0]*a[7]*a[10]  - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7]   - a[8]*a[3]*a[6];
        inv[11] = -a[0]*a[5]*a[11]  + a[0]*a[7]*a[9]   + a[4]*a[1]*a[11] - a[4]*a[3]*a[9]  - a[8]*a[1]*a[7]   + a[8]*a[3]*a[5];
        inv[15] =  a[0]*a[5]*a[10]  - a[0]*a[6]*a[9]   - a[4]*a[1]*a[10] + a[4]*a[2]*a[9]  + a[8]*a[1]*a[6]   - a[8]*a[2]*a[5];
        float det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
        Mat4 r;
        if (std::fabs(det) < 1e-12f) return identity();
        det = 1.0f / det;
        for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * det;
        return r;
    }
    Mat4 transposed() const {
        Mat4 r;
        for (int c = 0; c < 4; ++c) for (int row = 0; row < 4; ++row) r.m[c * 4 + row] = m[row * 4 + c];
        return r;
    }
    void toFloatArray(float* out16) const { for (int i = 0; i < 16; ++i) out16[i] = m[i]; }
};

/* ------------------------------------------------------------------- Noise / hashing ----- */
/**
 * Deterministic hash-based value noise. Reproducibility across CPU/GPU/ABI is a hard
 * requirement: identical (spec, seed) must yield byte-identical geometry on every device, so
 * every stochastic term in the pipeline is drawn from integer hashing - never from rand().
 */
class Noise {
public:
    explicit Noise(uint32_t seed = 1337u) : seed_(seed) {}

    uint32_t hash3(int x, int y, int z) const {
        uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u +
                     static_cast<uint32_t>(z) * 1274126177u + seed_ * 2654435761u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= (h >> 16);
        return h;
    }
    float hash3f(int x, int y, int z) const { return static_cast<float>(hash3(x, y, z) & 0x00FFFFFFu) / 16777216.0f; }
    float hash1f(int x) const { return hash3f(x, x * 7 + 13, x * 31 + 7); }

    float value(const Vec3& p) const {
        const int xi = static_cast<int>(std::floor(p.x));
        const int yi = static_cast<int>(std::floor(p.y));
        const int zi = static_cast<int>(std::floor(p.z));
        const float fx = p.x - static_cast<float>(xi);
        const float fy = p.y - static_cast<float>(yi);
        const float fz = p.z - static_cast<float>(zi);
        const float ux = fx * fx * (3.0f - 2.0f * fx);
        const float uy = fy * fy * (3.0f - 2.0f * fy);
        const float uz = fz * fz * (3.0f - 2.0f * fz);
        const float c000 = hash3f(xi, yi, zi),     c100 = hash3f(xi + 1, yi, zi);
        const float c010 = hash3f(xi, yi + 1, zi), c110 = hash3f(xi + 1, yi + 1, zi);
        const float c001 = hash3f(xi, yi, zi + 1), c101 = hash3f(xi + 1, yi, zi + 1);
        const float c011 = hash3f(xi, yi + 1, zi + 1), c111 = hash3f(xi + 1, yi + 1, zi + 1);
        const float x00 = lerpf(c000, c100, ux), x10 = lerpf(c010, c110, ux);
        const float x01 = lerpf(c001, c101, ux), x11 = lerpf(c011, c111, ux);
        return lerpf(lerpf(x00, x10, uy), lerpf(x01, x11, uy), uz) * 2.0f - 1.0f;
    }

    float fbm(const Vec3& p, int octaves = 4, float lacunarity = 2.03f, float gain = 0.5f) const {
        float amp = 1.0f, freq = 1.0f, sum = 0.0f, norm = 0.0f;
        for (int i = 0; i < octaves; ++i) {
            sum += amp * value(p * freq);
            norm += amp;
            amp *= gain;
            freq *= lacunarity;
        }
        return norm < kEps ? 0.0f : sum / norm;
    }

    /** Ridged multifractal - muscle striation, hair clumping, fabric slub. */
    float ridged(const Vec3& p, int octaves = 3, float lacunarity = 2.1f, float gain = 0.55f) const {
        float amp = 1.0f, freq = 1.0f, sum = 0.0f, norm = 0.0f;
        for (int i = 0; i < octaves; ++i) {
            const float n = 1.0f - std::fabs(value(p * freq));
            sum += amp * n * n;
            norm += amp;
            amp *= gain;
            freq *= lacunarity;
        }
        return norm < kEps ? 0.0f : sum / norm;
    }

    /** Worley F1 - pores, leather grain, brushed metal speckle. */
    float cell(const Vec3& p, float scale = 1.0f) const {
        const Vec3 sp = p * scale;
        const int xi = static_cast<int>(std::floor(sp.x));
        const int yi = static_cast<int>(std::floor(sp.y));
        const int zi = static_cast<int>(std::floor(sp.z));
        float best = std::numeric_limits<float>::max();
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int cx = xi + dx, cy = yi + dy, cz = zi + dz;
                    const Vec3 cellPos(static_cast<float>(cx) + hash3f(cx, cy, cz),
                                       static_cast<float>(cy) + hash3f(cx + 71, cy - 13, cz + 5),
                                       static_cast<float>(cz) + hash3f(cx - 23, cy + 41, cz - 7));
                    const float d = (cellPos - sp).lengthSq();
                    if (d < best) best = d;
                }
        return minf(1.0f, std::sqrt(maxf(0.0f, best)));
    }

private:
    uint32_t seed_;
};

/* ------------------------------------------------------------------ SDF primitives ------- */
/** Exact distance to an ellipsoid (Nvidia's closed-form approximation, accurate to ~1%). */
inline float sdEllipsoid(const Vec3& p, const Vec3& r) {
    const Vec3 rr = vmax(r, Vec3(1e-5f));
    const float k0 = (p / rr).length();
    const float k1 = (p / (rr * rr)).length();
    return k0 < kEps ? -rr.minComponent() : k0 * (k0 - 1.0f) / maxf(k1, kEps);
}

inline float sdCapsule(const Vec3& p, const Vec3& a, const Vec3& b, float radius) {
    const Vec3 pa = p - a, ba = b - a;
    const float denom = ba.dot(ba);
    const float h = denom < kEps ? 0.0f : saturate(pa.dot(ba) / denom);
    return (pa - ba * h).length() - radius;
}

/** Tapered capsule: radius interpolates linearly from ra at `a` to rb at `b`. */
inline float sdTaperedCapsule(const Vec3& p, const Vec3& a, const Vec3& b, float ra, float rb, float* outT = nullptr) {
    const Vec3 pa = p - a, ba = b - a;
    const float denom = ba.dot(ba);
    const float h = denom < kEps ? 0.0f : saturate(pa.dot(ba) / denom);
    if (outT) *outT = h;
    const float r = lerpf(ra, rb, h);
    return (pa - ba * h).length() - r;
}

inline float sdRoundBox(const Vec3& p, const Vec3& b, float r) {
    const Vec3 q = p.abs() - b;
    return q.maxv(Vec3(0.0f)).length() + minf(maxf(q.x, maxf(q.y, q.z)), 0.0f) - r;
}

/** Elliptical cylinder about the Y axis: cross-section radii (rx, rz), half height hy. */
inline float sdElliCylinderY(const Vec3& p, float rx, float rz, float hy) {
    const float nx = std::fabs(p.x) / maxf(rx, 1e-5f);
    const float nz = std::fabs(p.z) / maxf(rz, 1e-5f);
    const float k = maxf(nx, nz);
    const float radial = (k - 1.0f) * minf(rx, rz);
    const float vertical = std::fabs(p.y) - hy;
    return minf(maxf(radial, vertical), 0.0f) +
           Vec3(maxf(radial, 0.0f), maxf(vertical, 0.0f), 0.0f).length();
}

/** Super-ellipsoid |x/a|^e + |z/b|^e = 1 cross-section, used for chest/waist/pelvis shaping. */
inline float sdSuperEllipseCylinderY(const Vec3& p, float rx, float rz, float hy, float e) {
    const float nx = std::fabs(p.x) / maxf(rx, 1e-5f);
    const float nz = std::fabs(p.z) / maxf(rz, 1e-5f);
    const float k = std::pow(std::pow(nx, e) + std::pow(nz, e), 1.0f / e);
    const float radial = (k - 1.0f) * minf(rx, rz);
    const float vertical = std::fabs(p.y) - hy;
    return minf(maxf(radial, vertical), 0.0f) +
           Vec3(maxf(radial, 0.0f), maxf(vertical, 0.0f), 0.0f).length();
}

/** Capsule with smooth tapering along an arbitrary axis - limbs, fingers, toes. */
inline float sdLimb(const Vec3& p, const Vec3& a, const Vec3& b, float ra, float rb) {
    return sdTaperedCapsule(p, a, b, ra, rb);
}

/* ----------------------------------------------------------- anthropometric helpers ----- */
/**
 * Ramanujan ellipse perimeter approximation, relative error < 1e-5 for aspect ratios < 3.
 * This is what links a real tape measurement ("waist 78 cm") to mesh geometry.
 */
inline float ellipsePerimeter(float rx, float rz) {
    const float a = maxf(rx, 1e-5f), b = maxf(rz, 1e-5f);
    const float h = sqr(a - b) / sqr(a + b);
    const float p = 1.0f + 3.0f * h / (10.0f + std::sqrt(4.0f - 3.0f * h));
    return kPi * (a + b) * p;
}

/**
 * Inverts the perimeter relation: given a target circumference and a depth:width ratio,
 * returns the (rx, rz) semi-axes whose elliptical perimeter matches the measurement.
 * Bisection is used because the relation is monotonic and cheap; 40 iterations gives
 * sub-micrometre agreement, and the tests assert perimeter round-trip error < 0.05%.
 */
inline void ellipseFromCircumference(float circumference, float depthOverWidth, float* outRx, float* outRz) {
    float lo = 1e-4f, hi = maxf(circumference, 1e-3f);
    for (int i = 0; i < 40; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (ellipsePerimeter(mid, mid * depthOverWidth) < circumference) lo = mid; else hi = mid;
    }
    *outRx = 0.5f * (lo + hi);
    *outRz = (*outRx) * depthOverWidth;
}

/* ------------------------------------------------------------------ colour helpers ------- */
struct Lab { float l, a, b; };

inline float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline float linearToSrgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

inline Vec3 srgbToLinear(const Vec3& c) {
    return {srgbToLinear(c.x), srgbToLinear(c.y), srgbToLinear(c.z)};
}
inline Vec3 linearToSrgb(const Vec3& c) {
    return {linearToSrgb(c.x), linearToSrgb(c.y), linearToSrgb(c.z)};
}

inline Lab rgbToLab(const Vec3& linearRgb) {
    // D65, sRGB primaries.
    const float X = 0.4124564f * linearRgb.x + 0.3575761f * linearRgb.y + 0.1804375f * linearRgb.z;
    const float Y = 0.2126729f * linearRgb.x + 0.7151522f * linearRgb.y + 0.0721750f * linearRgb.z;
    const float Z = 0.0193339f * linearRgb.x + 0.1191920f * linearRgb.y + 0.9503041f * linearRgb.z;
    auto f = [](float t) {
        return t > 0.008856f ? std::cbrt(t) : (7.787f * t + 16.0f / 116.0f);
    };
    const float fx = f(X / 0.95047f), fy = f(Y), fz = f(Z / 1.08883f);
    return {116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz)};
}

inline Vec3 labToRgb(const Lab& lab) {
    const float fy = (lab.l + 16.0f) / 116.0f;
    const float fx = fy + lab.a / 500.0f;
    const float fz = fy - lab.b / 200.0f;
    auto finv = [](float t) {
        const float t3 = t * t * t;
        return t3 > 0.008856f ? t3 : (t - 16.0f / 116.0f) / 7.787f;
    };
    const float X = 0.95047f * finv(fx);
    const float Y = finv(fy);
    const float Z = 1.08883f * finv(fz);
    return {
         3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z,
        -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z,
         0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z
    };
}

/** Fitzpatrick I..VI melanin mapping used to drive skin albedo and subsurface colour. */
inline Vec3 skinAlbedoFromMelanin(float melanin, const Vec3& undertone) {
    const float m = saturate(melanin);
    // Linear-sRGB endpoints measured from the Fitzpatrick scale, interpolated in L*a*b*
    // (perceptual interpolation avoids the muddy midtones you get from naive RGB lerping).
    const Lab light{ 87.0f, 12.0f, 17.0f };
    const Lab dark { 33.0f, 12.0f, 18.0f };
    Lab out{ lerpf(light.l, dark.l, m), lerpf(light.a, dark.a, m), lerpf(light.b, dark.b, m) };
    Vec3 rgb = labToRgb(out);
    rgb.x = saturate(rgb.x + undertone.x * 0.06f);
    rgb.y = saturate(rgb.y + undertone.y * 0.04f);
    rgb.z = saturate(rgb.z + undertone.z * 0.03f);
    return rgb;
}

} // namespace af

#endif /* AF_MATH_H */
