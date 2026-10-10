package org.sdrpp.sdrpp

// Platform-independent session ownership. The same lock protects lookup,
// descriptor duplication and detach/close, so native code can acquire a lease.
class DeviceManager<D, C>(
    private val devices: () -> List<D>,
    private val key: (D) -> String,
    private val hasPermission: (D) -> Boolean,
    private val requestPermission: (D) -> Unit,
    private val open: (D) -> C?,
    private val close: (C) -> Unit,
    private val valid: (C) -> Boolean
) {
    private val lock = Any()
    private val connections = mutableMapOf<String, C>()
    private val pending = mutableSetOf<String>()
    private val denied = mutableSetOf<String>()
    private var closed = false
    private var status = "No compatible USB device"

    private fun discard(name: String) {
        connections.remove(name)?.let { close(it) }
        pending.remove(name)
        denied.remove(name)
    }

    fun <R> withDevice(matches: (D) -> Boolean, use: (D, C) -> R): R? = synchronized(lock) {
        if (closed) return@synchronized null
        val attached = devices()
        val names = attached.map(key).toSet()
        connections.keys.toList().filter { it !in names }.forEach { discard(it) }
        pending.retainAll(names)
        denied.retainAll(names)
        val candidates = attached.filter(matches).sortedBy(key)
        if (candidates.isEmpty()) status = "No compatible USB device"
        for (device in candidates) {
            val name = key(device)
            if (!hasPermission(device)) {
                connections.remove(name)?.let { close(it) }
                if (name in denied) status = "USB permission denied; press Refresh to retry"
                else {
                    status = "Waiting for USB permission"
                    if (pending.add(name)) requestPermission(device)
                }
                continue
            }
            pending.remove(name)
            denied.remove(name)
            var connection = connections[name]
            if (connection != null && !valid(connection)) {
                discard(name)
                connection = null
            }
            if (connection == null) {
                connection = open(device)
                if (connection == null || !valid(connection)) {
                    connection?.let { close(it) }
                    status = "USB permission granted, but openDevice failed; press Refresh"
                    continue
                }
                connections[name] = connection
            }
            status = "USB connected"
            return@synchronized use(device, connection)
        }
        null
    }

    fun permissionResult(device: D, granted: Boolean) = synchronized(lock) {
        val name = key(device)
        pending.remove(name)
        // Ignore a late reply for a detached device; also recheck real permission.
        if (closed || devices().none { key(it) == name }) return@synchronized
        if (granted && hasPermission(device)) {
            denied.remove(name)
            status = "USB permission granted; opening device"
        } else {
            denied.add(name)
            status = "USB permission denied; press Refresh to retry"
        }
    }

    fun detached(device: D) = synchronized(lock) {
        discard(key(device))
        status = "USB disconnected"
    }
    fun refresh() = synchronized(lock) { denied.clear() }
    fun getStatus(): String = synchronized(lock) { status }
    fun closeAll() = synchronized(lock) {
        closed = true
        connections.keys.toList().forEach { discard(it) }
        pending.clear()
        denied.clear()
    }
}
