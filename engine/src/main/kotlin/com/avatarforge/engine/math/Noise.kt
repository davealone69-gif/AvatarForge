package com.avatarforge.engine.math

import kotlin.math.abs
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sqrt

/*
 * AvatarForge :: deterministic procedural noise + anthropometric helpers.
 *
 * All noise is hash based and therefore reproducible across devices and runs:
 * the same seed + the same AvatarSpec always yields a byte-identical mesh.
 * That property is what makes "Generate again" a real, verifiable operation.
 */

class ValueNoise3D(private val seed: Int = 1337) {

    private fun hash(x: Int, y: Int, z: Int): Float {
        var h = x * 374761393 + y * 668265263 + z * 2147483647 + seed * 1274126177
        h = (h xor (h shr 13)) * 1274126177
        h = h xor (h shr 16)
        return (h and 0x7fffffff) / 0x7fffffff.toFloat()
    }

    /** Trilinear value noise in [-1, 1]. */
    fun sample(p: Vec3): Float {
        val xi = floor(p.x).toInt(); val yi = floor(p.y).toInt(); val zi = floor(p.z).toInt()
        val fx = p.x - xi; val fy = p.y - yi; val fz = p.z - zi
        val ux = fx * fx * (3f - 2f * fx)
        val uy = fy * fy * (3f - 2f * fy)
        val uz = fz * fz * (3f - 2f * fz)
        val c000 = hash(xi, yi, zi); val c100 = hash(xi + 1, yi, zi)
        val c010 = hash(xi, yi + 1, zi); val c110 = hash(xi + 1, yi + 1, zi)
        val c001 = hash(xi, yi, zi + 1); val c101 = hash(xi + 1, yi, zi + 1)
        val c011 = hash(xi, yi + 1, zi + 1); val c111 = hash(xi + 1, yi + 1, zi + 1)
        val x00 = lerp(c000, c100, ux); val x10 = lerp(c010, c110, ux)
        val x01 = lerp(c001, c101, ux); val x11 = lerp(c011, c111, ux)
        val y0 = lerp(x00, x10, uy); val y1 = lerp(x01, x11, uy)
        return lerp(y0, y1, uz) * 2f - 1f
    }

    /** Fractal Brownian motion. */
    fun fbm(p: Vec3, octaves: Int = 4, lacunarity: Float = 2.03f, gain: Float = 0.5f): Float {
        var amp = 1f
        var freq = 1f
        var sum = 0f
        var norm = 0f
        for (i in 0 until octaves) {
            sum += amp * sample(p * freq)
            norm += amp
            amp *= gain
            freq *= lacunarity
        }
        return if (norm < EPS) 0f else sum / norm
    }

    /** Ridged multifractal - used for hair clumping, fabric weave breakup and muscle striation. */
    fun ridged(p: Vec3, octaves: Int = 3, lacunarity: Float = 2.1f, gain: Float = 0.55f): Float {
        var amp = 1f
        var freq = 1f
        var sum = 0f
        var norm = 0f
        for (i in 0 until octaves) {
            val n = 1f - abs(sample(p * freq))
            sum += amp * n * n
            norm += amp
            amp *= gain
            freq *= lacunarity
        }
        return if (norm < EPS) 0f else sum / norm
    }

    /** Cellular / Worley style noise (F1 distance), useful for pebbled leather & knuckles. */
    fun cell(p: Vec3, scale: Float = 1f): Float {
        val sp = p * scale
        val xi = floor(sp.x).toInt(); val yi = floor(sp.y).toInt(); val zi = floor(sp.z).toInt()
        var best = Float.MAX_VALUE
        for (dz in -1..1) for (dy in -1..1) for (dx in -1..1) {
            val cx = xi + dx; val cy = yi + dy; val cz = zi + dz
            val px = cx + hash(cx, cy, cz)
            val py = cy + hash(cx + 71, cy - 13, cz + 5)
            val pz = cz + hash(cx - 23, cy + 41, cz - 7)
            val d = (Vec3(px, py, pz) - sp).lengthSq()
            if (d < best) best = d
        }
        return min(1f, sqrt(max(0f, best)))
    }
}

/**
 * Ramanujan ellipse perimeter approximation (accurate to ~1e-5 relative for r1/r2 < 3).
 * Used to convert real-world tape measurements (waist circumference in cm) into
 * mesh radii, which is what makes the anthropometric fit physically meaningful.
 */
fun ellipsePerimeter(rx: Float, rz: Float): Float {
    val a = max(rx, 1e-5f)
    val b = max(rz, 1e-5f)
    val h = ((a - b) * (a - b)) / ((a + b) * (a + b))
    val p = 1f + 3f * h / (10f + sqrt(4f - 3f * h))
    return kotlin.math.PI.toFloat() * (a + b) * p
}

/**
 * Solves for the semi-axes (rx, rz) of an elliptical cross section that has perimeter
 * [circumference] and depth/width ratio [depthOverWidth] (front-to-back depth divided by
 * side-to-side width - ~0.68 for the human waist, ~0.60 for the chest, ~1.15 for the pelvis).
 */
fun ellipseFromCircumference(circumference: Float, depthOverWidth: Float): Pair<Float, Float> {
    var lo = 1e-4f
    var hi = circumference
    repeat(48) {
        val mid = (lo + hi) * 0.5f
        val p = ellipsePerimeter(mid, mid * depthOverWidth)
        if (p < circumference) lo = mid else hi = mid
    }
    val rx = (lo + hi) * 0.5f
    return Pair(rx, rx * depthOverWidth)
}

/** Catmull-Rom interpolation across an ordered list of scalar control points (animation curves). */
fun catmullRom(p0: Float, p1: Float, p2: Float, p3: Float, t: Float): Float {
    val t2 = t * t
    val t3 = t2 * t
    return 0.5f * (2f * p1 + (-p0 + p2) * t + (2f * p0 - 5f * p1 + 4f * p2 - p3) * t2 + (-p0 + 3f * p1 - 3f * p2 + p3) * t3)
}

/** Hermite (cubic) ease used by keyframe channels. */
fun hermite(v0: Float, v1: Float, m0: Float, m1: Float, t: Float): Float {
    val t2 = t * t
    val t3 = t2 * t
    return (2f * t3 - 3f * t2 + 1f) * v0 + (t3 - 2f * t2 + t) * m0 + (-2f * t3 + 3f * t2) * v1 + (t3 - t2) * m1
}
