package com.spenbridge

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.Process
import android.os.SystemClock
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CopyOnWriteArraySet
import kotlin.random.Random

data class PcInfo(val name: String, val address: InetAddress, val monW: Int, val monH: Int) {
    val ip: String get() = address.hostAddress ?: "?"
}

data class LinkStatus(
    val connected: Boolean,
    val connecting: Boolean,
    val pc: PcInfo?,
    val rttMs: Int,
)

/**
 * UDP link to the PC. All socket work and link state live on one high-priority
 * thread ("spen-net"); the UI only posts into it and receives callbacks on main.
 */
object PenLink {
    interface Listener {
        fun onPcsChanged(pcs: List<PcInfo>) {}
        fun onStatus(status: LinkStatus) {}
    }

    private val thread = HandlerThread("spen-net", Process.THREAD_PRIORITY_URGENT_DISPLAY).also { it.start() }
    private val net = Handler(thread.looper)
    private val main = Handler(Looper.getMainLooper())
    private val listeners = CopyOnWriteArraySet<Listener>()

    private lateinit var app: Context
    private var started = false
    private var socket: DatagramSocket? = null

    // ---- net-thread state ----
    private val found = LinkedHashMap<String, Pair<PcInfo, Long>>()
    private var target: InetAddress? = null
    private var targetName = ""
    private var session = 0
    private var connected = false
    private var monW = 1920
    private var monH = 1080
    private var rttMs = -1
    private var lastPongAt = 0L
    private var lastHelloAt = 0L
    private var lastPingAt = 0L
    private var lastDiscoverAt = 0L
    private var seq = 0
    private val lastSample = ByteArray(Proto.SAMPLE_SIZE)
    private var lastSampleActive = false
    private var lastPenSendAt = 0L

    @Volatile var status = LinkStatus(false, false, null, -1)
        private set
    @Volatile var pcs: List<PcInfo> = emptyList()
        private set

    fun start(context: Context) {
        if (started) return
        started = true
        app = context.applicationContext
        net.post { openSocket() }
        net.post(tick)
    }

    fun addListener(l: Listener) {
        listeners.add(l)
        val s = status
        val p = pcs
        main.post { l.onStatus(s); l.onPcsChanged(p) }
    }

    fun removeListener(l: Listener) {
        listeners.remove(l)
    }

    fun connect(address: InetAddress, name: String) = net.post {
        val old = target
        if (old != null && old != address) send(header(Proto.T_BYE).array(), old)
        target = address
        targetName = name
        session = Random.nextInt()
        connected = false
        rttMs = -1
        lastHelloAt = 0
        publishStatus()
    }

    fun disconnect() = net.post {
        target?.let { send(header(Proto.T_BYE).array(), it) }
        target = null
        connected = false
        lastSampleActive = false
        publishStatus()
    }

    /**
     * Sends [count] packed samples. [important] packets (touch down / lift /
     * leave range) are sent three times; the PC drops duplicate sequence numbers.
     */
    fun sendPen(samples: ByteArray, count: Int, important: Boolean) = net.post {
        val t = target ?: return@post
        if (!connected || count <= 0) return@post
        System.arraycopy(samples, (count - 1) * Proto.SAMPLE_SIZE, lastSample, 0, Proto.SAMPLE_SIZE)
        lastSampleActive = (lastSample[0].toInt() and (Proto.F_IN_RANGE or Proto.F_CONTACT)) != 0
        val pkt = penPacket(samples, count)
        send(pkt, t)
        if (important) {
            send(pkt, t)
            send(pkt, t)
        }
        lastPenSendAt = SystemClock.elapsedRealtime()
    }

    fun sendKey(mods: Int, vk: Int) = net.post {
        val t = target ?: return@post
        if (!connected) return@post
        val b = header(Proto.T_KEY, 2)
        b.put(mods.toByte()).put(vk.toByte())
        send(b.array(), t)
    }

    // ------------------------------------------------------------------ internals

    private val tick = object : Runnable {
        override fun run() {
            try {
                step(SystemClock.elapsedRealtime())
            } catch (_: Exception) {
            }
            net.postDelayed(this, 20)
        }
    }

    private fun step(now: Long) {
        val t = target
        if ((t == null || !connected) && now - lastDiscoverAt >= 1000) {
            lastDiscoverAt = now
            discover()
        }
        if (t != null) {
            if (!connected) {
                if (now - lastHelloAt >= 400) {
                    lastHelloAt = now
                    sendHello(t)
                }
            } else {
                if (now - lastPingAt >= 500) {
                    lastPingAt = now
                    val b = header(Proto.T_PING, 8)
                    b.putLong(System.nanoTime())
                    send(b.array(), t)
                }
                if (now - lastPongAt > 3000) {
                    connected = false
                    rttMs = -1
                    publishStatus()
                }
                // Keep the PC's pen state alive while the pen sits still.
                if (lastSampleActive && now - lastPenSendAt >= 50) {
                    send(penPacket(lastSample, 1), t)
                    lastPenSendAt = now
                }
            }
        }
        val before = found.size
        found.values.removeAll { now - it.second > 4500 }
        if (found.size != before) publishPcs()
    }

    private fun openSocket() {
        try {
            val s = DatagramSocket(null)
            s.reuseAddress = true
            s.broadcast = true
            // DSCP EF -> Wi-Fi WMM voice queue: lowest-latency air time.
            s.trafficClass = 0xB8
            s.bind(InetSocketAddress(0))
            // If Wi-Fi has no internet, Android may route by default over mobile
            // data. Pin the socket to Wi-Fi so local packets always go out there.
            wifiNetwork()?.let { runCatching { it.bindSocket(s) } }
            socket = s
            Thread({ receiveLoop(s) }, "spen-rx").apply {
                isDaemon = true
                priority = Thread.MAX_PRIORITY
                start()
            }
        } catch (_: Exception) {
            net.postDelayed({ openSocket() }, 1000)
        }
    }

    private fun receiveLoop(s: DatagramSocket) {
        val buf = ByteArray(1500)
        val pkt = DatagramPacket(buf, buf.size)
        while (!s.isClosed) {
            try {
                pkt.length = buf.size
                s.receive(pkt)
                val data = buf.copyOf(pkt.length)
                val from = pkt.address
                net.post { handle(data, from) }
            } catch (_: Exception) {
                if (s.isClosed) break
            }
        }
    }

    private fun handle(d: ByteArray, from: InetAddress) {
        if (d.size < Proto.HEADER_SIZE || d[0] != 'S'.code.toByte() || d[1] != 'P'.code.toByte() ||
            d[2] != Proto.VERSION
        ) return
        val b = ByteBuffer.wrap(d).order(ByteOrder.LITTLE_ENDIAN)
        b.position(Proto.HEADER_SIZE)
        val now = SystemClock.elapsedRealtime()
        try {
            when (d[3]) {
                Proto.T_OFFER -> {
                    val w = b.short.toInt() and 0xFFFF
                    val h = b.short.toInt() and 0xFFFF
                    val name = readName(b)
                    val ip = from.hostAddress ?: return
                    val isNew = !found.containsKey(ip)
                    found[ip] = PcInfo(name, from, w, h) to now
                    if (isNew) publishPcs()
                }
                Proto.T_WELCOME -> {
                    if (from != target) return
                    if (b.int != session) return
                    monW = b.short.toInt() and 0xFFFF
                    monH = b.short.toInt() and 0xFFFF
                    targetName = readName(b)
                    connected = true
                    lastPongAt = now
                    publishStatus()
                }
                Proto.T_PONG -> {
                    if (from != target) return
                    val sent = b.long
                    val rtt = ((System.nanoTime() - sent) / 1_000_000L).toInt()
                    lastPongAt = now
                    if (rtt != rttMs) {
                        rttMs = rtt
                        publishStatus()
                    }
                }
            }
        } catch (_: Exception) {
            // Malformed packet: ignore.
        }
    }

    private fun discover() {
        val pkt = header(Proto.T_DISCOVER).array()
        for (addr in broadcastTargets()) send(pkt, addr)
        // Also poke the last known PC directly (works across subnets / when broadcast is filtered).
        target?.let { send(pkt, it) }
    }

    private fun sendHello(t: InetAddress) {
        val name = (Build.MODEL ?: "Android").toByteArray(Charsets.UTF_8).take(64).toByteArray()
        val b = header(Proto.T_HELLO, 4 + 1 + name.size)
        b.putInt(session).put(name.size.toByte()).put(name)
        send(b.array(), t)
    }

    private fun penPacket(samples: ByteArray, count: Int): ByteArray {
        val b = header(Proto.T_PEN, 4 + 1 + count * Proto.SAMPLE_SIZE)
        b.putInt(seq++)
        b.put(count.toByte())
        b.put(samples, 0, count * Proto.SAMPLE_SIZE)
        return b.array()
    }

    private fun header(type: Byte, payload: Int = 0): ByteBuffer {
        val b = ByteBuffer.allocate(Proto.HEADER_SIZE + payload).order(ByteOrder.LITTLE_ENDIAN)
        b.put('S'.code.toByte()).put('P'.code.toByte()).put(Proto.VERSION).put(type)
        return b
    }

    private fun readName(b: ByteBuffer): String {
        if (!b.hasRemaining()) return ""
        val n = b.get().toInt() and 0xFF
        val bytes = ByteArray(minOf(n, b.remaining()))
        b.get(bytes)
        return String(bytes, Charsets.UTF_8)
    }

    private fun send(data: ByteArray, to: InetAddress) {
        val s = socket ?: return
        try {
            s.send(DatagramPacket(data, data.size, to, Proto.PORT))
        } catch (_: Exception) {
        }
    }

    private fun wifiNetwork(): Network? {
        val cm = app.getSystemService(ConnectivityManager::class.java) ?: return null
        @Suppress("DEPRECATION")
        return cm.allNetworks.firstOrNull {
            cm.getNetworkCapabilities(it)?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
        }
    }

    private fun broadcastTargets(): List<InetAddress> {
        val out = ArrayList<InetAddress>()
        try {
            out += InetAddress.getByAddress(byteArrayOf(-1, -1, -1, -1))
            val cm = app.getSystemService(ConnectivityManager::class.java)
            val net = wifiNetwork() ?: cm?.activeNetwork
            val lp = if (net != null) cm?.getLinkProperties(net) else null
            lp?.linkAddresses?.forEach { la ->
                val a = la.address
                if (a is Inet4Address && la.prefixLength in 1..31) {
                    val ip = ByteBuffer.wrap(a.address).int
                    val mask = -1 shl (32 - la.prefixLength)
                    val bc = ip or mask.inv()
                    out += InetAddress.getByAddress(ByteBuffer.allocate(4).putInt(bc).array())
                }
            }
        } catch (_: Exception) {
        }
        return out
    }

    private fun publishStatus() {
        val t = target
        val st = LinkStatus(
            connected = connected,
            connecting = t != null && !connected,
            pc = t?.let { PcInfo(targetName, it, monW, monH) },
            rttMs = rttMs,
        )
        status = st
        main.post { listeners.forEach { it.onStatus(st) } }
    }

    private fun publishPcs() {
        val list = found.values.map { it.first }
        pcs = list
        main.post { listeners.forEach { it.onPcsChanged(list) } }
    }
}
