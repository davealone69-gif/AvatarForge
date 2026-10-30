/*
 * AvatarForge :: KernelAccess.kt
 *
 * Per-engine fences between code that mutates a kernel mesh and code that reads it. A render frame
 * and its attached engine share one fence; an isolated generation engine uses another, so an
 * emergency stop can leave the currently displayed character animating while the build unwinds.
 */
package com.avatarforge.sdk.internal

import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock

internal object KernelAccess {
    private class FenceEntry {
        val lock = ReentrantLock()
        var users = 0
    }

    private val registryLock = Any()
    private val locks = mutableMapOf<Long, FenceEntry>()

    /** Acquires a registry reference before waiting, so an entry cannot be removed under a waiter. */
    private fun lease(handle: Long): FenceEntry = synchronized(registryLock) {
        val entry = locks.getOrPut(handle) { FenceEntry() }
        entry.users++
        entry
    }

    private fun release(handle: Long, entry: FenceEntry, unlock: Boolean) {
        if (unlock) entry.lock.unlock()
        synchronized(registryLock) {
            entry.users--
            if (entry.users == 0 && locks[handle] === entry) locks.remove(handle)
        }
    }

    /** Compatibility fence for operations that do not yet have an engine handle. */
    fun <T> withKernel(block: () -> T): T = withKernels(0L, block = block)

    /** Runs [block] while holding the fence for [engineHandle]. */
    fun <T> withKernel(engineHandle: Long, block: () -> T): T =
        withKernels(engineHandle, block = block)

    /**
     * Takes multiple engine fences in stable handle order while a renderer swaps its backing mesh.
     * Distinct non-zero handles are used; zero is the compatibility fence for handle-free calls.
     */
    fun <T> withKernels(vararg engineHandles: Long, block: () -> T): T {
        val handles = engineHandles.filter { it != 0L }.distinct().sorted().ifEmpty { listOf(0L) }
        val leased = handles.map { it to lease(it) }
        leased.forEach { (_, entry) -> entry.lock.lock() }
        try {
            return block()
        } finally {
            leased.asReversed().forEach { (handle, entry) -> release(handle, entry, unlock = true) }
        }
    }

    /** Bounded variant for user-visible ray picking; null means the fence was not obtained in time. */
    fun <T> tryWithKernel(engineHandle: Long, timeoutMs: Long, block: () -> T): T? {
        val entry = lease(engineHandle)
        val acquired = try {
            entry.lock.tryLock(timeoutMs, TimeUnit.MILLISECONDS)
        } catch (interrupted: InterruptedException) {
            Thread.currentThread().interrupt()
            false
        }
        if (!acquired) {
            release(engineHandle, entry, unlock = false)
            return null
        }
        try {
            return block()
        } finally {
            release(engineHandle, entry, unlock = true)
        }
    }

    /** Removes a fence after its native handle has been destroyed and all users have detached. */
    fun forget(engineHandle: Long) {
        if (engineHandle == 0L) return
        synchronized(registryLock) {
            val entry = locks[engineHandle] ?: return
            if (entry.users == 0 && !entry.lock.isLocked) locks.remove(engineHandle)
        }
    }
}
