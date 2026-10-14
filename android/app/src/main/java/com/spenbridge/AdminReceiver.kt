package com.spenbridge

import android.app.admin.DeviceAdminReceiver

/**
 * Only active if the user explicitly makes the app device owner via adb.
 * Device owner turns screen pinning into a hard kiosk lock (no unpin gesture).
 */
class AdminReceiver : DeviceAdminReceiver()
