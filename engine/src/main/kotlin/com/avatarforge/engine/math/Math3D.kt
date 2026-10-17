package com.avatarforge.engine.math

import kotlin.math.abs
import kotlin.math.acos
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt
import kotlin.math.tan

/*
 * AvatarForge :: engine.math
 *
 * Pure-Kotlin linear algebra for the on-device avatar pipeline.
 * Deliberately dependency free: this module compiles for both the JVM (unit tests /
 * desktop QA) and Android (ARM64) with no native code, no third party math library.
 *
 * Conventions
 *  - Right handed coordinate system, +Y up, -Z forward (camera/glTF convention).
 *  - Mat4 is stored COLUMN MAJOR in a FloatArray(16), exactly like OpenGL/glTF:
 *
 *        | m0  m4  m8  m12 |
 *        | m1  m5  m9  m13 |
 *        | m2  m6  m10 m14 |
 *        | m3  m7  m11 m15 |
 *
 *  - Units are SI metres. A 1.75 m avatar is 1.75f tall.
 */

const val EPS = 1e-6f

fun clamp(v: Float, lo: Float, hi: Float): Float = if (v < lo) lo else if (v > hi) hi else v

fun lerp(a: Float, b: Float, t: Float): Float = a + (b - a) * t

fun smoothStep(edge0: Float, edge1: Float, x: Float): Float {
    val t = clamp((x - edge0) / (edge1 - edge0 + EPS), 0f, 1f)
    return t * t * (3f - 2f * t)
}

fun smootherStep(edge0: Float, edge1: Float, x: Float): Float {
    val t = clamp((x - edge0) / (edge1 - edge0 + EPS), 0f, 1f)
    return t * t * t * (t * (t * 6f - 15f) + 10f)
}

fun remap(v: Float, a0: Float, a1: Float, b0: Float, b1: Float): Float = b0 + (b1 - b0) * clamp((v - a0) / (a1 - a0 + EPS), 0f, 1f)

/** Polynomial smooth minimum. Used to blend SDF primitives of the body into one continuous surface. */
fun smoothMin(a: Float, b: Float, k: Float): Float {
    if (k <= EPS) return min(a, b)
    val h = clamp(0.5f + 0.5f * (b - a) / k, 0f, 1f)
    return lerp(b, a, h) - k * h * (1f - h)
}

/** Smooth maximum (intersection with soft edges). */
fun smoothMax(a: Float, b: Float, k: Float): Float {
    if (k <= EPS) return max(a, b)
    val h = clamp(0.5f - 0.5f * (b - a) / k, 0f, 1f)
    return lerp(b, a, h) + k * h * (1f - h)
}

// ---------------------------------------------------------------------------------------------
// Vectors
// ---------------------------------------------------------------------------------------------

data class Vec2(val x: Float, val y: Float) {
    operator fun plus(o: Vec2) = Vec2(x + o.x, y + o.y)
    operator fun minus(o: Vec2) = Vec2(x - o.x, y - o.y)
    operator fun times(s: Float) = Vec2(x * s, y * s)
    operator fun div(s: Float) = Vec2(x / s, y / s)
    fun length() = sqrt(x * x + y * y)
    fun dot(o: Vec2) = x * o.x + y * o.y
    fun lerp(o: Vec2, t: Float) = Vec2(lerp(x, o.x, t), lerp(y, o.y, t))
}

data class Vec3(val x: Float, val y: Float, val z: Float) {
    operator fun plus(o: Vec3) = Vec3(x + o.x, y + o.y, z + o.z)
    operator fun minus(o: Vec3) = Vec3(x - o.x, y - o.y, z - o.z)
    operator fun times(s: Float) = Vec3(x * s, y * s, z * s)
    operator fun times(o: Vec3) = Vec3(x * o.x, y * o.y, z * o.z)
    operator fun div(s: Float) = Vec3(x / s, y / s, z / s)
    operator fun unaryMinus() = Vec3(-x, -y, -z)
    operator fun get(i: Int) = when (i) {
        0 -> x; 1 -> y; else -> z
    }

    fun dot(o: Vec3) = x * o.x + y * o.y + z * o.z
    fun cross(o: Vec3) = Vec3(y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x)
    fun length() = sqrt(x * x + y * y + z * z)
    fun lengthSq() = x * x + y * y + z * z
    fun distanceTo(o: Vec3) = (this - o).length()
    fun normalized(): Vec3 {
        val l = length()
        return if (l < EPS) Vec3(0f, 0f, 0f) else Vec3(x / l, y / l, z / l)
    }

    fun lerp(o: Vec3, t: Float) = Vec3(lerp(x, o.x, t), lerp(y, o.y, t), lerp(z, o.z, t))
    fun abs() = Vec3(abs(x), abs(y), abs(z))
    fun minComponent() = min(x, min(y, z))
    fun maxComponent() = max(x, max(y, z))
    fun isFinite() = x.isFinite() && y.isFinite() && z.isFinite()
    fun withY(yy: Float) = Vec3(x, yy, z)
    fun toVec4(w: Float = 1f) = Vec4(x, y, z, w)

    companion object {
        val ZERO = Vec3(0f, 0f, 0f)
        val ONE = Vec3(1f, 1f, 1f)
        val UP = Vec3(0f, 1f, 0f)
        val FORWARD = Vec3(0f, 0f, -1f)
        val RIGHT = Vec3(1f, 0f, 0f)
    }
}

data class Vec4(val x: Float, val y: Float, val z: Float, val w: Float) {
    operator fun plus(o: Vec4) = Vec4(x + o.x, y + o.y, z + o.z, w + o.w)
    operator fun times(s: Float) = Vec4(x * s, y * s, z * s, w * s)
    fun xyz() = Vec3(x, y, z)
    fun dot(o: Vec4) = x * o.x + y * o.y + z * o.z + w * o.w
    fun length() = sqrt(x * x + y * y + z * z + w * w)
}

// ---------------------------------------------------------------------------------------------
// Quaternion
// ---------------------------------------------------------------------------------------------

data class Quat(val x: Float, val y: Float, val z: Float, val w: Float) {
    operator fun times(o: Quat) = Quat(
        w * o.x + x * o.w + y * o.z - z * o.y,
        w * o.y - x * o.z + y * o.w + z * o.x,
        w * o.z + x * o.y - y * o.x + z * o.w,
        w * o.w - x * o.x - y * o.y - z * o.z
    )

    fun conjugate() = Quat(-x, -y, -z, w)

    fun normalized(): Quat {
        val l = sqrt(x * x + y * y + z * z + w * w)
        return if (l < EPS) IDENTITY else Quat(x / l, y / l, z / l, w / l)
    }

    fun rotate(v: Vec3): Vec3 {
        // v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
        val qv = Vec3(x, y, z)
        val t = qv.cross(v) + v * w
        return v + qv.cross(t) * 2f
    }

    fun toMat4(): Mat4 = Mat4.fromQuat(this)

    fun toEulerXYZ(): Vec3 {
        // Intrinsic X -> Y -> Z extraction; used for animation authoring/debug output.
        val sinr = 2f * (w * x + y * z)
        val cosr = 1f - 2f * (x * x + y * y)
        val rx = kotlin.math.atan2(sinr, cosr)
        val sinp = clamp(2f * (w * y - z * x), -1f, 1f)
        val ry = kotlin.math.asin(sinp)
        val siny = 2f * (w * z + x * y)
        val cosy = 1f - 2f * (y * y + z * z)
        val rz = kotlin.math.atan2(siny, cosy)
        return Vec3(rx, ry, rz)
    }

    companion object {
        val IDENTITY = Quat(0f, 0f, 0f, 1f)

        fun fromAxisAngle(axis: Vec3, angleRad: Float): Quat {
            val a = axis.normalized()
            val h = angleRad * 0.5f
            val s = sin(h)
            return Quat(a.x * s, a.y * s, a.z * s, cos(h))
        }

        fun fromEulerXYZ(e: Vec3): Quat {
            val qx = fromAxisAngle(Vec3(1f, 0f, 0f), e.x)
            val qy = fromAxisAngle(Vec3(0f, 1f, 0f), e.y)
            val qz = fromAxisAngle(Vec3(0f, 0f, 1f), e.z)
            return (qz * qy * qx).normalized()
        }

        /** Shortest arc rotation taking [from] onto [to]. Degenerate-safe. */
        fun fromTo(from: Vec3, to: Vec3): Quat {
            val f = from.normalized()
            val t = to.normalized()
            val d = clamp(f.dot(t), -1f, 1f)
            if (d > 0.99999f) return IDENTITY
            if (d < -0.99999f) {
                var axis = Vec3(1f, 0f, 0f).cross(f)
                if (axis.lengthSq() < 1e-8f) axis = Vec3(0f, 1f, 0f).cross(f)
                return fromAxisAngle(axis.normalized(), kotlin.math.PI.toFloat())
            }
            val c = f.cross(t)
            return Quat(c.x, c.y, c.z, 1f + d).normalized()
        }

        fun slerp(a: Quat, b: Quat, t: Float): Quat {
            var cosHalf = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w
            var bb = b
            if (cosHalf < 0f) {
                bb = Quat(-b.x, -b.y, -b.z, -b.w)
                cosHalf = -cosHalf
            }
            if (cosHalf > 0.9995f) {
                return Quat(
                    lerp(a.x, bb.x, t), lerp(a.y, bb.y, t),
                    lerp(a.z, bb.z, t), lerp(a.w, bb.w, t)
                ).normalized()
            }
            val theta = acos(cosHalf)
            val s = sin(theta)
            val wa = sin((1f - t) * theta) / s
            val wb = sin(t * theta) / s
            return Quat(
                a.x * wa + bb.x * wb, a.y * wa + bb.y * wb,
                a.z * wa + bb.z * wb, a.w * wa + bb.w * wb
            ).normalized()
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Matrices
// ---------------------------------------------------------------------------------------------

class Mat4(val m: FloatArray = FloatArray(16)) {

    init {
        require(m.size == 16) { "Mat4 requires 16 floats" }
    }

    companion object {
        fun identity(): Mat4 {
            val r = Mat4()
            r.m[0] = 1f; r.m[5] = 1f; r.m[10] = 1f; r.m[15] = 1f
            return r
        }

        fun translation(t: Vec3) = identity().also {
            it.m[12] = t.x; it.m[13] = t.y; it.m[14] = t.z
        }

        fun scale(s: Vec3) = identity().also {
            it.m[0] = s.x; it.m[5] = s.y; it.m[10] = s.z
        }

        fun rotation(axis: Vec3, angle: Float) = fromQuat(Quat.fromAxisAngle(axis, angle))

        fun fromQuat(q: Quat): Mat4 {
            val n = q.normalized()
            val x = n.x; val y = n.y; val z = n.z; val w = n.w
            val xx = x * x; val yy = y * y; val zz = z * z
            val xy = x * y; val xz = x * z; val yz = y * z
            val wx = w * x; val wy = w * y; val wz = w * z
            val r = Mat4()
            r.m[0] = 1f - 2f * (yy + zz); r.m[1] = 2f * (xy + wz); r.m[2] = 2f * (xz - wy); r.m[3] = 0f
            r.m[4] = 2f * (xy - wz); r.m[5] = 1f - 2f * (xx + zz); r.m[6] = 2f * (yz + wx); r.m[7] = 0f
            r.m[8] = 2f * (xz + wy); r.m[9] = 2f * (yz - wx); r.m[10] = 1f - 2f * (xx + yy); r.m[11] = 0f
            r.m[12] = 0f; r.m[13] = 0f; r.m[14] = 0f; r.m[15] = 1f
            return r
        }

        /** Translation * Rotation * Scale composed in TRS order. */
        fun trs(t: Vec3, q: Quat, s: Vec3): Mat4 {
            val r = fromQuat(q)
            r.m[0] *= s.x; r.m[1] *= s.x; r.m[2] *= s.x
            r.m[4] *= s.y; r.m[5] *= s.y; r.m[6] *= s.y
            r.m[8] *= s.z; r.m[9] *= s.z; r.m[10] *= s.z
            r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z
            return r
        }

        /** Builds a matrix whose +Y axis is [to] and origin is [from] (used to align limb segments). */
        fun alignY(from: Vec3, to: Vec3, upHint: Vec3 = Vec3(0f, 0f, 1f)): Mat4 {
            val yAxis = (to - from).normalized()
            var zAxis = upHint - yAxis * upHint.dot(yAxis)
            if (zAxis.lengthSq() < 1e-8f) {
                zAxis = Vec3(1f, 0f, 0f) - yAxis * yAxis.x
            }
            zAxis = zAxis.normalized()
            val xAxis = yAxis.cross(zAxis).normalized()
            val r = Mat4()
            r.m[0] = xAxis.x; r.m[1] = xAxis.y; r.m[2] = xAxis.z; r.m[3] = 0f
            r.m[4] = yAxis.x; r.m[5] = yAxis.y; r.m[6] = yAxis.z; r.m[7] = 0f
            r.m[8] = zAxis.x; r.m[9] = zAxis.y; r.m[10] = zAxis.z; r.m[11] = 0f
            r.m[12] = from.x; r.m[13] = from.y; r.m[14] = from.z; r.m[15] = 1f
            return r
        }

        fun perspective(fovYDeg: Float, aspect: Float, near: Float, far: Float): Mat4 {
            val f = 1f / tan(fovYDeg * 0.5f * (kotlin.math.PI.toFloat() / 180f))
            val r = Mat4()
            r.m[0] = f / aspect; r.m[5] = f
            r.m[10] = (far + near) / (near - far); r.m[11] = -1f
            r.m[14] = (2f * far * near) / (near - far)
            return r
        }

        fun ortho(l: Float, r0: Float, b: Float, t: Float, n: Float, f: Float): Mat4 {
            val r = Mat4()
            r.m[0] = 2f / (r0 - l); r.m[5] = 2f / (t - b); r.m[10] = -2f / (f - n)
            r.m[12] = -(r0 + l) / (r0 - l); r.m[13] = -(t + b) / (t - b); r.m[14] = -(f + n) / (f - n)
            r.m[15] = 1f
            return r
        }

        fun lookAt(eye: Vec3, center: Vec3, up: Vec3 = Vec3.UP): Mat4 {
            val f = (center - eye).normalized()
            var s = f.cross(up)
            if (s.lengthSq() < 1e-8f) s = f.cross(Vec3(0f, 0f, 1f))
            s = s.normalized()
            val u = s.cross(f)
            val r = Mat4()
            r.m[0] = s.x; r.m[1] = u.x; r.m[2] = -f.x; r.m[3] = 0f
            r.m[4] = s.y; r.m[5] = u.y; r.m[6] = -f.y; r.m[7] = 0f
            r.m[8] = s.z; r.m[9] = u.z; r.m[10] = -f.z; r.m[11] = 0f
            r.m[12] = -s.dot(eye); r.m[13] = -u.dot(eye); r.m[14] = f.dot(eye); r.m[15] = 1f
            return r
        }
    }

    operator fun times(o: Mat4): Mat4 {
        val r = Mat4()
        for (c in 0 until 4) {
            val c4 = c * 4
            for (row in 0 until 4) {
                r.m[c4 + row] = m[row] * o.m[c4] + m[4 + row] * o.m[c4 + 1] +
                    m[8 + row] * o.m[c4 + 2] + m[12 + row] * o.m[c4 + 3]
            }
        }
        return r
    }

    fun timesPoint(p: Vec3): Vec3 {
        val x = p.x; val y = p.y; val z = p.z
        val w = m[3] * x + m[7] * y + m[11] * z + m[15]
        val iw = if (abs(w) < EPS) 1f else 1f / w
        return Vec3(
            (m[0] * x + m[4] * y + m[8] * z + m[12]) * iw,
            (m[1] * x + m[5] * y + m[9] * z + m[13]) * iw,
            (m[2] * x + m[6] * y + m[10] * z + m[14]) * iw
        )
    }

    /** Ignores translation (w = 0). */
    fun timesDirection(d: Vec3): Vec3 = Vec3(
        m[0] * d.x + m[4] * d.y + m[8] * d.z,
        m[1] * d.x + m[5] * d.y + m[9] * d.z,
        m[2] * d.x + m[6] * d.y + m[10] * d.z
    )

    fun transpose(): Mat4 {
        val r = Mat4()
        for (c in 0 until 4) for (row in 0 until 4) r.m[c * 4 + row] = m[row * 4 + c]
        return r
    }

    fun toFloatArrayColumnMajor(): FloatArray = m.copyOf()

    /** General 4x4 inverse (cofactor expansion). Returns identity if singular. */
    fun inverse(): Mat4 {
        val a00 = m[0]; val a01 = m[1]; val a02 = m[2]; val a03 = m[3]
        val a10 = m[4]; val a11 = m[5]; val a12 = m[6]; val a13 = m[7]
        val a20 = m[8]; val a21 = m[9]; val a22 = m[10]; val a23 = m[11]
        val a30 = m[12]; val a31 = m[13]; val a32 = m[14]; val a33 = m[15]

        val b00 = a00 * a11 - a01 * a10
        val b01 = a00 * a12 - a02 * a10
        val b02 = a00 * a13 - a03 * a10
        val b03 = a01 * a12 - a02 * a11
        val b04 = a01 * a13 - a03 * a11
        val b05 = a02 * a13 - a03 * a12
        val b06 = a20 * a31 - a21 * a30
        val b07 = a20 * a32 - a22 * a30
        val b08 = a20 * a33 - a23 * a30
        val b09 = a21 * a32 - a22 * a31
        val b10 = a21 * a33 - a23 * a31
        val b11 = a22 * a33 - a23 * a32

        val det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06
        if (abs(det) < 1e-12f) return identity()
        val id = 1f / det
        val r = Mat4()
        r.m[0] = (a11 * b11 - a12 * b10 + a13 * b09) * id
        r.m[1] = (a02 * b10 - a01 * b11 - a03 * b09) * id
        r.m[2] = (a31 * b05 - a32 * b04 + a33 * b03) * id
        r.m[3] = (a22 * b04 - a21 * b05 - a23 * b03) * id
        r.m[4] = (a12 * b08 - a10 * b11 - a13 * b07) * id
        r.m[5] = (a00 * b11 - a02 * b08 + a03 * b07) * id
        r.m[6] = (a32 * b02 - a30 * b05 - a33 * b01) * id
        r.m[7] = (a20 * b05 - a22 * b02 + a23 * b01) * id
        r.m[8] = (a10 * b10 - a11 * b08 + a13 * b06) * id
        r.m[9] = (a01 * b08 - a00 * b10 - a03 * b06) * id
        r.m[10] = (a30 * b04 - a31 * b02 + a33 * b00) * id
        r.m[11] = (a21 * b02 - a20 * b04 - a23 * b00) * id
        r.m[12] = (a11 * b07 - a10 * b09 - a12 * b06) * id
        r.m[13] = (a00 * b09 - a01 * b07 + a02 * b06) * id
        r.m[14] = (a31 * b01 - a30 * b03 - a32 * b00) * id
        r.m[15] = (a20 * b03 - a21 * b01 + a22 * b00) * id
        return r
    }

    /** Upper-left 3x3 inverse-transpose, packed for a GLSL mat3 uniform (column major). */
    fun normalMatrix3(): FloatArray {
        val inv = inverse()
        return floatArrayOf(
            inv.m[0], inv.m[4], inv.m[8],
            inv.m[1], inv.m[5], inv.m[9],
            inv.m[2], inv.m[6], inv.m[10]
        )
    }

    fun copyOf(): Mat4 = Mat4(m.copyOf())
}
