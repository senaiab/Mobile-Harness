package com.jarves.mh.runtime

import android.os.ParcelFileDescriptor
import java.io.ByteArrayInputStream
import java.io.File
import java.io.FileInputStream
import java.io.InputStream
import java.io.OutputStream

internal class NativeSpawnProcess private constructor(
    private val pid: Int,
    internal val outputFile: File?,
    private val stdin: OutputStream,
    internal val ptyMasterFd: Int = -1,
) : Process() {
    @Volatile private var result: Int? = null

    override fun getOutputStream(): OutputStream = stdin
    override fun getInputStream(): InputStream = FileInputStream(outputFile!!)
    override fun getErrorStream(): InputStream = ByteArrayInputStream(ByteArray(0))

    override fun waitFor(): Int {
        result?.let { return it }
        return NativeSpawn.waitFor(pid, false).also { result = it }
    }

    override fun exitValue(): Int {
        result?.let { return it }
        val status = NativeSpawn.waitFor(pid, true)
        if (status == NativeSpawn.STILL_RUNNING) throw IllegalThreadStateException("Process is still running")
        return status.also { result = it }
    }

    override fun destroy() {
        NativeSpawn.kill(pid, 15)
    }

    /** Send the same interrupt signal produced by Ctrl+C in a real terminal. */
    internal fun interrupt() {
        NativeSpawn.kill(pid, 2)
    }

    override fun destroyForcibly(): Process {
        NativeSpawn.kill(pid, 9)
        return this
    }

    override fun isAlive(): Boolean = runCatching { exitValue(); false }.getOrDefault(true)

    companion object {
        fun start(argv: List<String>, environment: Map<String, String>, cwd: String, outputFile: File): NativeSpawnProcess {
            outputFile.parentFile?.mkdirs()
            val spawned = NativeSpawn.spawn(
                argv.toTypedArray(),
                environment.map { "${it.key}=${it.value}" }.toTypedArray(),
                cwd,
                outputFile.absolutePath,
            )
            check(spawned.size == 2 && spawned[0] > 0) { "Native runtime launch failed" }
            val input = ParcelFileDescriptor.AutoCloseOutputStream(ParcelFileDescriptor.adoptFd(spawned[1]))
            return NativeSpawnProcess(spawned[0], outputFile, input)
        }

        fun startPty(argv: List<String>, environment: Map<String, String>, cwd: String): NativeSpawnProcess {
            val spawned = NativeSpawn.spawnPty(
                argv.toTypedArray(),
                environment.map { "${it.key}=${it.value}" }.toTypedArray(),
                cwd,
            )
            check(spawned.size == 2 && spawned[0] > 0) { "PTY terminal launch failed" }
            val masterFd = spawned[1]
            val stdinStream = object : java.io.OutputStream() {
                override fun write(b: Int) = write(byteArrayOf(b.toByte()))
                override fun write(b: ByteArray, off: Int, len: Int) {
                    NativeSpawn.writePty(masterFd, b.copyOfRange(off, off + len))
                }
            }
            return NativeSpawnProcess(spawned[0], null, stdinStream, masterFd)
        }
    }
}

private object NativeSpawn {
    const val STILL_RUNNING = -2

    init {
        System.loadLibrary("pocketspawn")
    }

    external fun spawn(argv: Array<String>, environment: Array<String>, cwd: String, outputFile: String): IntArray
    external fun spawnPty(argv: Array<String>, environment: Array<String>, cwd: String): IntArray
    external fun writePty(masterFd: Int, data: ByteArray): Int
    external fun waitFor(pid: Int, noHang: Boolean): Int
    external fun kill(pid: Int, signal: Int): Int
}
