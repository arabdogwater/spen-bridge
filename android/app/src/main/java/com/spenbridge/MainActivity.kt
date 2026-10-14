package com.spenbridge

import android.app.Activity
import android.app.admin.DevicePolicyManager
import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import android.view.View
import android.view.inputmethod.EditorInfo
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import java.net.InetAddress

class MainActivity : Activity(), PenLink.Listener {

    private lateinit var prefs: Prefs
    private lateinit var status: TextView
    private lateinit var pcList: LinearLayout
    private lateinit var emptyHint: TextView
    private lateinit var ipInput: EditText
    private var autoConnectTried = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        prefs = Prefs(this)
        PenLink.start(this)

        status = findViewById(R.id.status)
        pcList = findViewById(R.id.pcList)
        emptyHint = findViewById(R.id.emptyHint)
        ipInput = findViewById(R.id.ipInput)
        ipInput.setText(prefs.lastPcIp)

        findViewById<Button>(R.id.connectIp).setOnClickListener { connectManual() }
        ipInput.setOnEditorActionListener { _, id, _ ->
            if (id == EditorInfo.IME_ACTION_GO) { connectManual(); true } else false
        }

        bindSwitch(R.id.swAspect, prefs.keepAspect) { prefs.keepAspect = it }
        bindSwitch(R.id.swLock, prefs.lockScreen) { prefs.lockScreen = it }
        bindSwitch(R.id.swVolUndo, prefs.volumeDownUndo) { prefs.volumeDownUndo = it }
        bindSwitch(R.id.swTrail, prefs.inkTrail) { prefs.inkTrail = it }

        findViewById<Button>(R.id.start).setOnClickListener {
            if (PenLink.status.pc == null) {
                Toast.makeText(this, "Pick your PC first", Toast.LENGTH_SHORT).show()
            } else {
                startActivity(Intent(this, TabletActivity::class.java))
            }
        }

        val dpm = getSystemService(DevicePolicyManager::class.java)
        val release = findViewById<Button>(R.id.releaseOwner)
        if (dpm?.isDeviceOwnerApp(packageName) == true) {
            release.visibility = View.VISIBLE
            release.setOnClickListener {
                @Suppress("DEPRECATION")
                dpm.clearDeviceOwnerApp(packageName)
                release.visibility = View.GONE
                Toast.makeText(this, "Device-owner kiosk mode removed", Toast.LENGTH_SHORT).show()
            }
        }
    }

    override fun onResume() {
        super.onResume()
        PenLink.addListener(this)
    }

    override fun onPause() {
        PenLink.removeListener(this)
        super.onPause()
    }

    private fun bindSwitch(id: Int, value: Boolean, set: (Boolean) -> Unit) {
        val sw = findViewById<Switch>(id)
        sw.isChecked = value
        sw.setOnCheckedChangeListener { _, v -> set(v) }
    }

    private fun connectManual() {
        val ip = ipInput.text.toString().trim()
        if (ip.isEmpty()) return
        Thread {
            try {
                val addr = InetAddress.getByName(ip)
                runOnUiThread { connectTo(addr, ip) }
            } catch (_: Exception) {
                runOnUiThread { Toast.makeText(this, "Bad address: $ip", Toast.LENGTH_SHORT).show() }
            }
        }.start()
    }

    private fun connectTo(addr: InetAddress, name: String) {
        prefs.lastPcIp = addr.hostAddress ?: ""
        PenLink.connect(addr, name)
    }

    override fun onPcsChanged(pcs: List<PcInfo>) {
        pcList.removeAllViews()
        emptyHint.visibility = if (pcs.isEmpty()) View.VISIBLE else View.GONE
        val current = PenLink.status.pc?.ip
        for (pc in pcs) {
            val b = Button(this)
            b.isAllCaps = false
            b.text = "${pc.name}   ·   ${pc.ip}   ·   ${pc.monW}×${pc.monH}" + if (pc.ip == current) "   ✓" else ""
            b.setOnClickListener { connectTo(pc.address, pc.name) }
            pcList.addView(b, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        }
        // Auto-connect: the PC used last time, or the only PC on the network.
        if (!autoConnectTried && PenLink.status.pc == null) {
            val pick = pcs.firstOrNull { it.ip == prefs.lastPcIp } ?: pcs.singleOrNull()
            if (pick != null) {
                autoConnectTried = true
                connectTo(pick.address, pick.name)
            }
        }
    }

    override fun onStatus(status: LinkStatus) {
        val pc = status.pc
        this.status.text = when {
            status.connected -> "● Connected to ${pc?.name}  (${pc?.ip})\nMonitor ${pc?.monW}×${pc?.monH}" +
                if (status.rttMs >= 0) "  ·  ${status.rttMs} ms round trip" else ""
            status.connecting -> "◌ Connecting to ${pc?.name?.ifEmpty { pc.ip }}…\nMake sure SpenBridge.exe is running and allowed through the firewall."
            else -> "○ Not connected"
        }
        this.status.setTextColor(if (status.connected) Color.rgb(134, 239, 172) else Color.rgb(232, 238, 248))
        onPcsChanged(PenLink.pcs)
    }
}
