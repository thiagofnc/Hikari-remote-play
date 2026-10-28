package com.hikari.remoteplay

import android.content.Context
import com.hikari.remoteplay.proto.Invite
import com.hikari.remoteplay.proto.Preset

class Prefs(context: Context) {
    private val sp = context.getSharedPreferences("hikari", Context.MODE_PRIVATE)

    var invite: Invite?
        get() = sp.getString("invite", null)?.let { Invite.parse(it) }
        set(v) = sp.edit().putString("invite", v?.toUri()).apply()

    var hostName: String
        get() = sp.getString("host_name", "") ?: ""
        set(v) = sp.edit().putString("host_name", v).apply()

    var preset: Preset
        get() = Preset.entries.getOrElse(sp.getInt("preset", Preset.Balanced.ordinal)) { Preset.Balanced }
        set(v) = sp.edit().putInt("preset", v.ordinal).apply()

    var controlsOpacity: Float
        get() = sp.getFloat("controls_opacity", 0.6f)
        set(v) = sp.edit().putFloat("controls_opacity", v).apply()

    var controlsScale: Float
        get() = sp.getFloat("controls_scale", 1f)
        set(v) = sp.edit().putFloat("controls_scale", v).apply()

    var showControls: Boolean
        get() = sp.getBoolean("show_controls", true)
        set(v) = sp.edit().putBoolean("show_controls", v).apply()

    var haptics: Boolean
        get() = sp.getBoolean("haptics", true)
        set(v) = sp.edit().putBoolean("haptics", v).apply()

    // Where each touch control sits and how big it is (TouchControlsView.layoutString()).
    var controlsLayout: String
        get() = sp.getString("controls_layout", "") ?: ""
        set(v) = sp.edit().putString("controls_layout", v).apply()

    var pictureMode: PictureMode
        get() = PictureMode.entries.getOrElse(sp.getInt("picture_mode", PictureMode.Fit.ordinal)) { PictureMode.Fit }
        set(v) = sp.edit().putInt("picture_mode", v.ordinal).apply()

    var showStats: Boolean
        get() = sp.getBoolean("show_stats", false)
        set(v) = sp.edit().putBoolean("show_stats", v).apply()
}

// How the 16:9 picture uses a phone screen that is usually wider than that.
enum class PictureMode(val label: String) {
    Fit("Fit"),          // as large as possible without cropping or distortion
    Fill("Zoom"),        // fills the screen, trimming the top and bottom edges
    Stretch("Stretch"),  // fills the screen, widening the picture
}
