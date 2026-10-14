package com.spenbridge

import android.app.Activity
import android.app.ActivityManager
import android.app.admin.DevicePolicyManager
import android.content.ComponentName
import android.content.pm.ActivityInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.KeyEvent
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.window.OnBackInvokedCallback
import android.window.OnBackInvokedDispatcher

/**
 * The tablet surface. Fullscreen, screen kept on, pinned (lock task) so home /
 * recents / notifications are blocked. The only way out is Volume Up x3.
 */
class TabletActivity : Activity(), PenLink.Listener {

    private lateinit var prefs: Prefs
    private lateinit var pen: PenView
    private var wifiLock: WifiManager.WifiLock? = null
    private val volUpTimes = LongArray(EXIT_PRESSES)
    private var exiting = false
    private var backCallback: OnBackInvokedCallback? = null
    private var lastConnected = true

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = Prefs(this)
        PenLink.start(this)

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.attributes = window.attributes.apply {
            layoutInDisplayCutoutMode = if (Build.VERSION.SDK_INT >= 30)
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
            else WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }

        pen = PenView(this, prefs)
        setContentView(pen)
        pen.requestFocus()

        PenLink.status.pc?.let { applyMonitor(it.monW, it.monH) }

        if (Build.VERSION.SDK_INT >= 33) {
            // Swallow Back entirely (gesture and button) while in tablet mode.
            val cb = OnBackInvokedCallback { pen.showBanner("Press Volume Up 3× to exit", 1500) }
            onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_OVERLAY, cb)
            backCallback = cb
        }
        hideSystemUi()
    }

    override fun onResume() {
        super.onResume()
        PenLink.addListener(this)
        hideSystemUi()
        val wm = applicationContext.getSystemService(WifiManager::class.java)
        // LOW_LATENCY disables Wi-Fi power save while we're foreground with the screen on.
        wifiLock = wm?.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "spenbridge:lowlatency")?.apply {
            setReferenceCounted(false)
            acquire()
        }
    }

    override fun onPause() {
        pen.releasePen()
        PenLink.removeListener(this)
        wifiLock?.let { if (it.isHeld) it.release() }
        wifiLock = null
        super.onPause()
    }

    override fun onDestroy() {
        if (Build.VERSION.SDK_INT >= 33) backCallback?.let { onBackInvokedDispatcher.unregisterOnBackInvokedCallback(it) }
        super.onDestroy()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            hideSystemUi()
            if (prefs.lockScreen && !exiting) enterLock()
        } else {
            pen.releasePen()
        }
    }

    // ------------------------------------------------------------------ lock

    private fun enterLock() {
        val am = getSystemService(ActivityManager::class.java) ?: return
        if (am.lockTaskModeState != ActivityManager.LOCK_TASK_MODE_NONE) return
        val dpm = getSystemService(DevicePolicyManager::class.java)
        if (dpm != null && dpm.isDeviceOwnerApp(packageName)) {
            // Hard kiosk: no unpin gesture, no home, no recents, no status bar.
            val admin = ComponentName(this, AdminReceiver::class.java)
            dpm.setLockTaskPackages(admin, arrayOf(packageName))
            dpm.setLockTaskFeatures(admin, DevicePolicyManager.LOCK_TASK_FEATURE_NONE)
        }
        // Without device owner this is Android screen pinning (Settings > Security > Pin app).
        try {
            startLockTask()
        } catch (_: Exception) {
        }
    }

    private fun exitTablet() {
        exiting = true
        pen.releasePen()
        try {
            stopLockTask()
        } catch (_: Exception) {
        }
        finish()
    }

    // ------------------------------------------------------------------ keys

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        when (event.keyCode) {
            KeyEvent.KEYCODE_VOLUME_UP -> {
                if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) onVolumeUp()
                return true
            }
            KeyEvent.KEYCODE_VOLUME_DOWN -> {
                if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0 && prefs.volumeDownUndo) {
                    PenLink.sendKey(Proto.MOD_CTRL, Proto.VK_Z)
                    pen.showBanner("Undo", 500)
                }
                return true
            }
            KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_APP_SWITCH, KeyEvent.KEYCODE_MENU, KeyEvent.KEYCODE_SEARCH -> {
                return true
            }
        }
        return super.dispatchKeyEvent(event)
    }

    /** S Pen Air Actions (see res/xml/spen_remote_actions.xml) and any other keys -> PC. */
    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (event.repeatCount > 0 && keyCode != KeyEvent.KEYCODE_LEFT_BRACKET &&
            keyCode != KeyEvent.KEYCODE_RIGHT_BRACKET
        ) return true
        val vk = KeyMap.toWindowsVk(keyCode)
        if (vk != 0) {
            var mods = 0
            if (event.isCtrlPressed) mods = mods or Proto.MOD_CTRL
            if (event.isShiftPressed) mods = mods or Proto.MOD_SHIFT
            if (event.isAltPressed) mods = mods or Proto.MOD_ALT
            if (event.isMetaPressed) mods = mods or Proto.MOD_WIN
            PenLink.sendKey(mods, vk)
            return true
        }
        return true
    }

    @Deprecated("Back is swallowed in tablet mode")
    override fun onBackPressed() {
        pen.showBanner("Press Volume Up 3× to exit", 1500)
    }

    private fun onVolumeUp() {
        val now = SystemClock.uptimeMillis()
        System.arraycopy(volUpTimes, 1, volUpTimes, 0, EXIT_PRESSES - 1)
        volUpTimes[EXIT_PRESSES - 1] = now
        val first = volUpTimes[0]
        if (first != 0L && now - first <= EXIT_WINDOW_MS) {
            exitTablet()
            return
        }
        val recent = volUpTimes.count { it != 0L && now - it <= EXIT_WINDOW_MS }
        val left = EXIT_PRESSES - recent
        pen.showBanner(if (left == 1) "Press once more to exit" else "Press $left more times to exit", 1200)
    }

    // ------------------------------------------------------------------ link

    override fun onStatus(status: LinkStatus) {
        val pc = status.pc
        if (pc != null) applyMonitor(pc.monW, pc.monH)
        pen.statusLine = when {
            status.connected -> "${pc?.name ?: "PC"} · ${pc?.monW}×${pc?.monH} · ${if (status.rttMs >= 0) "${status.rttMs} ms" else "…"}"
            status.connecting -> "Connecting to ${pc?.name?.ifEmpty { pc.ip } ?: "PC"}…"
            else -> "Not connected"
        }
        if (!status.connected && lastConnected) pen.showBanner("PC connection lost — reconnecting…", 2500)
        if (status.connected && !lastConnected) pen.showBanner("Connected", 800)
        lastConnected = status.connected
    }

    private fun applyMonitor(w: Int, h: Int) {
        if (w <= 0 || h <= 0) return
        if (pen.monitorW != w) pen.monitorW = w
        if (pen.monitorH != h) pen.monitorH = h
        val want = if (w >= h) ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        else ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
        if (requestedOrientation != want) requestedOrientation = want
    }

    private fun hideSystemUi() {
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false)
            window.insetsController?.let {
                it.hide(WindowInsets.Type.systemBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                or View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_LAYOUT_STABLE or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN)
        }
    }

    private companion object {
        const val EXIT_PRESSES = 3
        const val EXIT_WINDOW_MS = 2000L
    }
}
