package com.spenbridge

import android.content.Context
import android.content.SharedPreferences

class Prefs(context: Context) {
    private val sp: SharedPreferences =
        context.applicationContext.getSharedPreferences("spenbridge", Context.MODE_PRIVATE)

    var keepAspect: Boolean
        get() = sp.getBoolean("keepAspect", true)
        set(v) = sp.edit().putBoolean("keepAspect", v).apply()

    var lockScreen: Boolean
        get() = sp.getBoolean("lockScreen", true)
        set(v) = sp.edit().putBoolean("lockScreen", v).apply()

    var volumeDownUndo: Boolean
        get() = sp.getBoolean("volumeDownUndo", true)
        set(v) = sp.edit().putBoolean("volumeDownUndo", v).apply()

    var inkTrail: Boolean
        get() = sp.getBoolean("inkTrail", true)
        set(v) = sp.edit().putBoolean("inkTrail", v).apply()

    var lastPcIp: String
        get() = sp.getString("lastPcIp", "") ?: ""
        set(v) = sp.edit().putString("lastPcIp", v).apply()
}
