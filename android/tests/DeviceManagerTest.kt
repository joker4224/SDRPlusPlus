package org.sdrpp.sdrpp

// Run with kotlinc ../app/src/main/java/DeviceManager.kt DeviceManagerTest.kt
// -include-runtime -d usb-tests.jar && java -jar usb-tests.jar
private data class Device(val name: String, val rtl: Boolean)
private data class Connection(val id: Int, var alive: Boolean = true)

fun main() {
    var attached = listOf(Device("rtl", true), Device("unrelated", false))
    val permitted = mutableSetOf<String>()
    var requests = 0
    var opens = 0
    var closes = 0
    var failOpen = false
    val manager = DeviceManager(
        devices = { attached }, key = { it.name },
        hasPermission = { it.name in permitted },
        requestPermission = { requests++ },
        open = { _: Device -> if (failOpen) null else Connection(++opens) },
        close = { connection -> check(connection.alive); connection.alive = false; closes++ },
        valid = { it.alive }
    )
    fun acquire(): Int? = manager.withDevice({ it.rtl }) { _, connection -> connection.id }
    check(acquire() == null && requests == 1)
    repeat(10) { check(acquire() == null) }
    check(requests == 1) // No permission-dialog storm; never asks for unrelated USB.
    permitted.add("rtl")
    manager.permissionResult(attached[0], true)
    check(acquire() == 1 && opens == 1)
    check(acquire() == 1 && opens == 1)

    val first = attached[0]
    attached = attached.drop(1)
    permitted.remove("rtl")
    manager.detached(first)
    check(closes == 1 && acquire() == null)
    manager.permissionResult(first, true) // Late permission reply after detach.
    check(acquire() == null && opens == 1)
    attached = listOf(first)
    check(acquire() == null && requests == 2)
    manager.permissionResult(first, false)
    repeat(3) { check(acquire() == null) }
    check(requests == 2)
    manager.refresh()
    check(acquire() == null && requests == 3)
    permitted.add("rtl")
    manager.permissionResult(first, true)
    failOpen = true
    check(acquire() == null && manager.getStatus().contains("openDevice failed"))
    failOpen = false
    check(acquire() == 2)

    // A missed detach broadcast is recovered by reconciling the real device list.
    attached = emptyList()
    check(acquire() == null && closes == 2)
    attached = listOf(first)
    check(acquire() == 3)
    // Device/connection access and detach cannot interleave within a native lease.
    val acquired = java.util.concurrent.CountDownLatch(1)
    val release = java.util.concurrent.CountDownLatch(1)
    val finished = java.util.concurrent.CountDownLatch(1)
    val threadError = java.util.concurrent.atomic.AtomicReference<Throwable>()
    val worker = Thread {
        try {
        manager.withDevice({ it.rtl }) { _, connection ->
            acquired.countDown()
            check(release.await(3, java.util.concurrent.TimeUnit.SECONDS))
            check(connection.alive)
        }
        } catch (error: Throwable) { threadError.set(error); release.countDown() }
    }
    worker.start()
    check(acquired.await(3, java.util.concurrent.TimeUnit.SECONDS))
    val detach = Thread { manager.detached(first); finished.countDown() }
    detach.start()
    check(!finished.await(50, java.util.concurrent.TimeUnit.MILLISECONDS))
    release.countDown()
    worker.join(3000)
    detach.join(3000)
    check(!worker.isAlive && !detach.isAlive && closes == 3)
    check(threadError.get() == null)

    check(acquire() == 4)
    manager.closeAll()
    manager.closeAll()
    check(closes == 4 && acquire() == null)
    manager.permissionResult(first, true)
    check(acquire() == null)
    println("PASS: USB permission, denial/retry, hotplug, stale callbacks, null-open, session reuse/close and concurrent lease scenarios")
}
