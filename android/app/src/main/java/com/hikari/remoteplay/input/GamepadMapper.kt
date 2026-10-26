package com.hikari.remoteplay.input

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import com.hikari.remoteplay.proto.Btn
import com.hikari.remoteplay.proto.PadState
import kotlin.math.abs
import kotlin.math.roundToInt

// Bluetooth/USB gamepads. Android reports Xbox-style positional buttons
// (BUTTON_A = south); like the desktop default, map by position so the button
// under your thumb matches the Switch's (south -> B, east -> A, ...).
class GamepadMapper {
    private var buttons = 0
    private var lx = 0f; private var ly = 0f; private var rx = 0f; private var ry = 0f
    private var lt = 0f; private var rt = 0f
    private var hatX = 0f; private var hatY = 0f

    var deadzone = 0.08f

    fun isGamepad(source: Int): Boolean =
        source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
            source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK ||
            source and InputDevice.SOURCE_DPAD == InputDevice.SOURCE_DPAD

    // Returns true if the key was a controller button we consumed.
    fun onKey(e: KeyEvent): Boolean {
        val bit = when (e.keyCode) {
            KeyEvent.KEYCODE_BUTTON_A -> Btn.B
            KeyEvent.KEYCODE_BUTTON_B -> Btn.A
            KeyEvent.KEYCODE_BUTTON_X -> Btn.Y
            KeyEvent.KEYCODE_BUTTON_Y -> Btn.X
            KeyEvent.KEYCODE_BUTTON_L1 -> Btn.L
            KeyEvent.KEYCODE_BUTTON_R1 -> Btn.R
            KeyEvent.KEYCODE_BUTTON_L2 -> Btn.ZL
            KeyEvent.KEYCODE_BUTTON_R2 -> Btn.ZR
            KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BACK -> Btn.MINUS
            KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_MENU -> Btn.PLUS
            KeyEvent.KEYCODE_BUTTON_THUMBL -> Btn.LSTICK
            KeyEvent.KEYCODE_BUTTON_THUMBR -> Btn.RSTICK
            KeyEvent.KEYCODE_BUTTON_MODE -> Btn.HOME
            KeyEvent.KEYCODE_MEDIA_RECORD, KeyEvent.KEYCODE_BUTTON_1 -> Btn.CAPTURE
            KeyEvent.KEYCODE_DPAD_UP -> Btn.DPAD_UP
            KeyEvent.KEYCODE_DPAD_DOWN -> Btn.DPAD_DOWN
            KeyEvent.KEYCODE_DPAD_LEFT -> Btn.DPAD_LEFT
            KeyEvent.KEYCODE_DPAD_RIGHT -> Btn.DPAD_RIGHT
            else -> return false
        }
        buttons = when (e.action) {
            KeyEvent.ACTION_DOWN -> buttons or bit
            KeyEvent.ACTION_UP -> buttons and bit.inv()
            else -> buttons
        }
        return true
    }

    fun onMotion(e: MotionEvent): Boolean {
        if (e.action != MotionEvent.ACTION_MOVE) return false
        lx = e.getAxisValue(MotionEvent.AXIS_X)
        ly = e.getAxisValue(MotionEvent.AXIS_Y)
        rx = e.getAxisValue(MotionEvent.AXIS_Z)
        ry = e.getAxisValue(MotionEvent.AXIS_RZ)
        lt = maxOf(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE))
        rt = maxOf(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS))
        hatX = e.getAxisValue(MotionEvent.AXIS_HAT_X)
        hatY = e.getAxisValue(MotionEvent.AXIS_HAT_Y)
        return true
    }

    private fun axis(v: Float, invert: Boolean): Int {
        val a = if (abs(v) < deadzone) 0f else v
        val s = (if (invert) -a else a).coerceIn(-1f, 1f)
        return (s * 32767f).roundToInt()
    }

    fun state(): PadState {
        var b = buttons
        if (hatX < -0.5f) b = b or Btn.DPAD_LEFT
        if (hatX > 0.5f) b = b or Btn.DPAD_RIGHT
        if (hatY < -0.5f) b = b or Btn.DPAD_UP
        if (hatY > 0.5f) b = b or Btn.DPAD_DOWN
        val ltv = (lt.coerceIn(0f, 1f) * 255).roundToInt()
        val rtv = (rt.coerceIn(0f, 1f) * 255).roundToInt()
        // Analog triggers also press ZL/ZR: the console's wired pad is digital.
        if (ltv > 64) b = b or Btn.ZL
        if (rtv > 64) b = b or Btn.ZR
        // Android +Y is down; the wire format is +Y up.
        return PadState(b, axis(lx, false), axis(ly, true), axis(rx, false), axis(ry, true), ltv, rtv)
    }

    fun reset() {
        buttons = 0
        lx = 0f; ly = 0f; rx = 0f; ry = 0f; lt = 0f; rt = 0f; hatX = 0f; hatY = 0f
    }
}
