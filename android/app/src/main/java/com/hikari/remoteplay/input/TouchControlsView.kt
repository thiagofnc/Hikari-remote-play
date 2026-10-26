package com.hikari.remoteplay.input

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Typeface
import android.os.SystemClock
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import com.hikari.remoteplay.proto.Btn
import com.hikari.remoteplay.proto.PadState
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.hypot
import kotlin.math.min
import kotlin.math.roundToInt

// Transparent on-screen Switch controller drawn over the video.
//
// Every touch is owned by the control it landed on until it lifts, so several
// thumbs work at once. Sticks float: a touch anywhere on an empty half of the
// screen becomes that side's stick, centred where the thumb landed. Face
// buttons and the D-pad let a thumb slide from one button to the next.
// Double-tap and hold a stick to click it (L3 / R3).
class TouchControlsView(context: Context) : View(context) {

    var onPadChanged: ((PadState) -> Unit)? = null
    var onMenu: (() -> Unit)? = null

    var opacity = 0.6f
        set(v) { field = v.coerceIn(0.15f, 1f); invalidate() }
    var scale = 1f
        set(v) { field = v.coerceIn(0.7f, 1.4f); layoutControls(width, height) }
    var haptics = true

    private enum class Shape { Round, Rect, Pill }
    private enum class Icon { None, Home, Capture, Menu }

    private class Button(val bit: Int, val label: String, val shape: Shape, val icon: Icon = Icon.None) {
        var cx = 0f; var cy = 0f; var rw = 0f; var rh = 0f
        var slack = 1.3f
        fun hit(x: Float, y: Float): Boolean = when (shape) {
            Shape.Round -> hypot(x - cx, y - cy) <= rw * slack
            else -> abs(x - cx) <= rw * slack && abs(y - cy) <= rh * slack
        }
    }

    private class Stick(val clickBit: Int) {
        var homeX = 0f; var homeY = 0f; var radius = 0f
        var pointer = -1
        var originX = 0f; var originY = 0f
        var knobX = 0f; var knobY = 0f
        var vx = 0f; var vy = 0f
        var clicking = false
        var lastUp = 0L
    }

    private val left = Stick(Btn.LSTICK)
    private val right = Stick(Btn.RSTICK)
    private val face = listOf(
        Button(Btn.A, "A", Shape.Round), Button(Btn.B, "B", Shape.Round),
        Button(Btn.X, "X", Shape.Round), Button(Btn.Y, "Y", Shape.Round),
    )
    private val shoulders = listOf(
        Button(Btn.ZL, "ZL", Shape.Pill), Button(Btn.L, "L", Shape.Pill),
        Button(Btn.ZR, "ZR", Shape.Pill), Button(Btn.R, "R", Shape.Pill),
    )
    private val system = listOf(
        Button(Btn.MINUS, "−", Shape.Round), Button(Btn.PLUS, "+", Shape.Round),
        Button(Btn.CAPTURE, "", Shape.Round, Icon.Capture), Button(Btn.HOME, "", Shape.Round, Icon.Home),
    )
    private val menu = Button(0, "", Shape.Pill, Icon.Menu)
    private val buttons = face + shoulders + system

    private var dpadX = 0f; private var dpadY = 0f; private var dpadSize = 0f

    private enum class Owner { LeftStick, RightStick, Dpad, Buttons, Menu }
    private class Touch(val owner: Owner, var x: Float, var y: Float)
    private val touches = HashMap<Int, Touch>()

    private var current = PadState.NEUTRAL

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
    }
    private val rect = RectF()
    private val path = Path()

    init {
        isClickable = true
        isHapticFeedbackEnabled = true
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) = layoutControls(w, h)

    private fun layoutControls(w: Int, h: Int) {
        if (w == 0 || h == 0) return
        val u = min(w, h).toFloat()
        val s = scale
        val edge = 0.05f * u

        // Right hand: ABXY above, right stick below-left (Switch layout).
        val d = 0.125f * u * s
        val r = 0.064f * u * s
        val fx = w - edge - d - r
        val fy = h * 0.47f
        place(face[0], fx + d, fy, r)      // A east
        place(face[1], fx, fy + d, r)      // B south
        place(face[2], fx, fy - d, r)      // X north
        place(face[3], fx - d, fy, r)      // Y west
        face.forEach { it.slack = 1.35f }

        val sr = 0.135f * u * s
        right.radius = sr
        right.homeX = fx - 0.34f * u * s
        right.homeY = h - edge - sr

        // Left hand mirrors it: stick above, D-pad below-right.
        left.radius = sr
        left.homeX = w - fx
        left.homeY = fy
        dpadSize = 0.33f * u * s
        dpadX = w - right.homeX
        dpadY = right.homeY

        // Shoulders stacked in the top corners, ZL/ZR outermost.
        val pw = 0.115f * u * s
        val ph = 0.048f * u * s
        val top = edge * 0.6f + ph
        placeRect(shoulders[0], edge + pw, top, pw, ph)
        placeRect(shoulders[1], edge + pw, top + 2.9f * ph, pw, ph)
        placeRect(shoulders[2], w - edge - pw, top, pw, ph)
        placeRect(shoulders[3], w - edge - pw, top + 2.9f * ph, pw, ph)
        shoulders.forEach { it.slack = 1.25f }

        val sys = 0.042f * u * s
        place(system[0], w / 2f - 0.17f * u, top, sys)
        place(system[1], w / 2f + 0.17f * u, top, sys)
        place(system[2], w / 2f - 0.17f * u, h - top, sys)
        place(system[3], w / 2f + 0.17f * u, h - top, sys)
        system.forEach { it.slack = 1.4f }
        placeRect(menu, w / 2f, top, 0.055f * u, 0.03f * u)
        menu.slack = 1.6f

        for (st in listOf(left, right)) if (st.pointer < 0) resetStick(st)
        invalidate()
    }

    private fun place(b: Button, x: Float, y: Float, r: Float) { b.cx = x; b.cy = y; b.rw = r; b.rh = r }
    private fun placeRect(b: Button, x: Float, y: Float, hw: Float, hh: Float) { b.cx = x; b.cy = y; b.rw = hw; b.rh = hh }

    private fun resetStick(st: Stick) {
        st.originX = st.homeX; st.originY = st.homeY
        st.knobX = st.homeX; st.knobY = st.homeY
        st.vx = 0f; st.vy = 0f
        st.clicking = false
    }

    fun releaseAll() {
        touches.clear()
        left.pointer = -1; right.pointer = -1
        resetStick(left); resetStick(right)
        publish()
    }

    // ---- touch -------------------------------------------------------------------

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = e.actionIndex
                down(e.getPointerId(i), e.getX(i), e.getY(i))
            }
            MotionEvent.ACTION_MOVE -> for (i in 0 until e.pointerCount) {
                touches[e.getPointerId(i)]?.let { t ->
                    t.x = e.getX(i); t.y = e.getY(i)
                    if (t.owner == Owner.LeftStick) moveStick(left, t.x, t.y)
                    if (t.owner == Owner.RightStick) moveStick(right, t.x, t.y)
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                val i = e.actionIndex
                up(e.getPointerId(i), e.getX(i), e.getY(i))
            }
            MotionEvent.ACTION_CANCEL -> releaseAll()
        }
        publish()
        return true
    }

    private fun down(id: Int, x: Float, y: Float) {
        val owner = when {
            menu.hit(x, y) -> Owner.Menu
            buttons.any { it.hit(x, y) } -> Owner.Buttons
            abs(x - dpadX) <= dpadSize * 0.65f && abs(y - dpadY) <= dpadSize * 0.65f -> Owner.Dpad
            x < width / 2f && left.pointer < 0 -> Owner.LeftStick
            x >= width / 2f && right.pointer < 0 -> Owner.RightStick
            else -> return
        }
        touches[id] = Touch(owner, x, y)
        when (owner) {
            Owner.LeftStick -> grabStick(left, id, x, y)
            Owner.RightStick -> grabStick(right, id, x, y)
            else -> {}
        }
    }

    private fun up(id: Int, x: Float, y: Float) {
        val t = touches.remove(id) ?: return
        when (t.owner) {
            Owner.Menu -> if (menu.hit(x, y)) onMenu?.invoke()
            Owner.LeftStick -> releaseStick(left)
            Owner.RightStick -> releaseStick(right)
            else -> {}
        }
    }

    private fun grabStick(st: Stick, id: Int, x: Float, y: Float) {
        st.pointer = id
        // Touching the drawn stick keeps its centre; anywhere else re-centres on the thumb.
        if (hypot(x - st.homeX, y - st.homeY) <= st.radius) {
            st.originX = st.homeX; st.originY = st.homeY
        } else {
            st.originX = x; st.originY = y
        }
        st.clicking = SystemClock.uptimeMillis() - st.lastUp < DOUBLE_TAP_MS
        moveStick(st, x, y)
    }

    private fun releaseStick(st: Stick) {
        st.pointer = -1
        st.lastUp = if (st.clicking) 0 else SystemClock.uptimeMillis()
        resetStick(st)
    }

    private fun moveStick(st: Stick, x: Float, y: Float) {
        var dx = x - st.originX
        var dy = y - st.originY
        val dist = hypot(dx, dy)
        if (dist > st.radius) {
            dx *= st.radius / dist
            dy *= st.radius / dist
        }
        st.knobX = st.originX + dx
        st.knobY = st.originY + dy
        val m = hypot(dx, dy) / st.radius
        if (m < STICK_DEADZONE) {
            st.vx = 0f; st.vy = 0f
        } else {
            // Rescale past the deadzone so small deflections still reach the edge smoothly.
            val k = (m - STICK_DEADZONE) / (1 - STICK_DEADZONE) / m
            st.vx = dx / st.radius * k
            st.vy = dy / st.radius * k
        }
    }

    private fun dpadBits(x: Float, y: Float): Int {
        val dx = (x - dpadX) / (dpadSize / 2)
        val dy = (y - dpadY) / (dpadSize / 2)
        if (hypot(dx, dy) < 0.22f) return 0
        // 8 sectors of 45 degrees, starting at east.
        val a = Math.toDegrees(atan2(-dy, dx).toDouble()).let { if (it < 0) it + 360 else it }
        return when (((a + 22.5) / 45).toInt() % 8) {
            0 -> Btn.DPAD_RIGHT
            1 -> Btn.DPAD_RIGHT or Btn.DPAD_UP
            2 -> Btn.DPAD_UP
            3 -> Btn.DPAD_UP or Btn.DPAD_LEFT
            4 -> Btn.DPAD_LEFT
            5 -> Btn.DPAD_LEFT or Btn.DPAD_DOWN
            6 -> Btn.DPAD_DOWN
            else -> Btn.DPAD_DOWN or Btn.DPAD_RIGHT
        }
    }

    private fun axis(v: Float) = (v.coerceIn(-1f, 1f) * 32767f).roundToInt()

    private fun publish() {
        var bits = 0
        for (t in touches.values) {
            when (t.owner) {
                Owner.Buttons -> for (b in buttons) if (b.hit(t.x, t.y)) bits = bits or b.bit
                Owner.Dpad -> bits = bits or dpadBits(t.x, t.y)
                else -> {}
            }
        }
        if (left.clicking && left.pointer >= 0) bits = bits or Btn.LSTICK
        if (right.clicking && right.pointer >= 0) bits = bits or Btn.RSTICK
        val pad = PadState(
            buttons = bits,
            lx = axis(left.vx), ly = axis(-left.vy),   // screen +Y is down; wire +Y is up
            rx = axis(right.vx), ry = axis(-right.vy),
            lt = if (bits and Btn.ZL != 0) 255 else 0,
            rt = if (bits and Btn.ZR != 0) 255 else 0,
        )
        if (pad != current) {
            if (haptics && (bits and current.buttons.inv()) != 0) performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
            current = pad
            onPadChanged?.invoke(pad)
            invalidate()
        }
    }

    // ---- drawing -------------------------------------------------------------------

    private fun idle(a: Float) = Color.argb((a * opacity * 255).roundToInt(), 255, 255, 255)
    private fun lit(a: Float) = Color.argb((a * opacity * 255).roundToInt(), 0xFB, 0xBB, 0x04)

    override fun onDraw(c: Canvas) {
        val u = min(width, height).toFloat()
        stroke.strokeWidth = 0.006f * u
        val pressed = current.buttons

        for (st in listOf(left, right)) {
            val active = st.pointer >= 0
            fill.color = idle(0.10f)
            c.drawCircle(st.originX, st.originY, st.radius, fill)
            stroke.color = idle(if (active) 0.55f else 0.35f)
            c.drawCircle(st.originX, st.originY, st.radius, stroke)
            val clicked = pressed and st.clickBit != 0
            fill.color = if (clicked) lit(0.65f) else idle(if (active) 0.45f else 0.28f)
            c.drawCircle(st.knobX, st.knobY, st.radius * 0.48f, fill)
            stroke.color = if (clicked) lit(0.9f) else idle(0.6f)
            c.drawCircle(st.knobX, st.knobY, st.radius * 0.48f, stroke)
        }

        drawDpad(c, pressed)

        text.textSize = 0.05f * u * scale
        for (b in buttons) drawButton(c, b, pressed and b.bit != 0)
        drawButton(c, menu, touches.values.any { it.owner == Owner.Menu })
    }

    private fun drawButton(c: Canvas, b: Button, on: Boolean) {
        fill.color = if (on) lit(0.6f) else idle(0.12f)
        stroke.color = if (on) lit(0.95f) else idle(0.5f)
        when (b.shape) {
            Shape.Round -> {
                c.drawCircle(b.cx, b.cy, b.rw, fill)
                c.drawCircle(b.cx, b.cy, b.rw, stroke)
            }
            Shape.Rect, Shape.Pill -> {
                rect.set(b.cx - b.rw, b.cy - b.rh, b.cx + b.rw, b.cy + b.rh)
                val rr = if (b.shape == Shape.Pill) b.rh else b.rh * 0.3f
                c.drawRoundRect(rect, rr, rr, fill)
                c.drawRoundRect(rect, rr, rr, stroke)
            }
        }
        text.color = if (on) Color.argb((opacity * 255).roundToInt(), 0x1A, 0x14, 0x00) else idle(0.9f)
        when (b.icon) {
            Icon.None -> {
                val size = text.textSize
                if (b.shape == Shape.Round && b.rw < 0.05f * min(width, height)) text.textSize = b.rw * 1.3f
                c.drawText(b.label, b.cx, b.cy - (text.ascent() + text.descent()) / 2, text)
                text.textSize = size
            }
            Icon.Home -> {
                val k = b.rw * 0.5f
                path.reset()
                path.moveTo(b.cx, b.cy - k)
                path.lineTo(b.cx + k, b.cy)
                path.lineTo(b.cx + k * 0.65f, b.cy)
                path.lineTo(b.cx + k * 0.65f, b.cy + k * 0.8f)
                path.lineTo(b.cx - k * 0.65f, b.cy + k * 0.8f)
                path.lineTo(b.cx - k * 0.65f, b.cy)
                path.lineTo(b.cx - k, b.cy)
                path.close()
                fill.color = text.color
                c.drawPath(path, fill)
            }
            Icon.Capture -> {
                val k = b.rw * 0.5f
                rect.set(b.cx - k, b.cy - k, b.cx + k, b.cy + k)
                stroke.color = text.color
                c.drawRoundRect(rect, k * 0.3f, k * 0.3f, stroke)
                fill.color = text.color
                c.drawCircle(b.cx, b.cy, k * 0.4f, fill)
            }
            Icon.Menu -> {
                stroke.color = text.color
                val k = b.rw * 0.45f
                for (i in -1..1) c.drawLine(b.cx - k, b.cy + i * b.rh * 0.35f, b.cx + k, b.cy + i * b.rh * 0.35f, stroke)
            }
        }
    }

    private fun drawDpad(c: Canvas, pressed: Int) {
        val arm = dpadSize / 3
        val half = dpadSize / 2
        // Cross outline.
        path.reset()
        path.addRoundRect(RectF(dpadX - arm / 2, dpadY - half, dpadX + arm / 2, dpadY + half), arm * 0.2f, arm * 0.2f, Path.Direction.CW)
        path.addRoundRect(RectF(dpadX - half, dpadY - arm / 2, dpadX + half, dpadY + arm / 2), arm * 0.2f, arm * 0.2f, Path.Direction.CW)
        fill.color = idle(0.12f)
        c.drawPath(path, fill)
        stroke.color = idle(0.45f)
        drawArmOutline(c, arm, half)
        // Lit arms.
        fill.color = lit(0.65f)
        if (pressed and Btn.DPAD_UP != 0) c.drawRect(dpadX - arm / 2, dpadY - half, dpadX + arm / 2, dpadY - arm / 2, fill)
        if (pressed and Btn.DPAD_DOWN != 0) c.drawRect(dpadX - arm / 2, dpadY + arm / 2, dpadX + arm / 2, dpadY + half, fill)
        if (pressed and Btn.DPAD_LEFT != 0) c.drawRect(dpadX - half, dpadY - arm / 2, dpadX - arm / 2, dpadY + arm / 2, fill)
        if (pressed and Btn.DPAD_RIGHT != 0) c.drawRect(dpadX + arm / 2, dpadY - arm / 2, dpadX + half, dpadY + arm / 2, fill)
        // Arrow hints.
        fill.color = idle(0.7f)
        val t = arm * 0.22f
        val o = half - arm * 0.45f
        triangle(c, dpadX, dpadY - o, 0f, -t)
        triangle(c, dpadX, dpadY + o, 0f, t)
        triangle(c, dpadX - o, dpadY, -t, 0f)
        triangle(c, dpadX + o, dpadY, t, 0f)
    }

    private fun drawArmOutline(c: Canvas, arm: Float, half: Float) {
        val a = arm / 2
        path.reset()
        path.moveTo(dpadX - a, dpadY - half)
        path.lineTo(dpadX + a, dpadY - half)
        path.lineTo(dpadX + a, dpadY - a)
        path.lineTo(dpadX + half, dpadY - a)
        path.lineTo(dpadX + half, dpadY + a)
        path.lineTo(dpadX + a, dpadY + a)
        path.lineTo(dpadX + a, dpadY + half)
        path.lineTo(dpadX - a, dpadY + half)
        path.lineTo(dpadX - a, dpadY + a)
        path.lineTo(dpadX - half, dpadY + a)
        path.lineTo(dpadX - half, dpadY - a)
        path.lineTo(dpadX - a, dpadY - a)
        path.close()
        c.drawPath(path, stroke)
    }

    private fun triangle(c: Canvas, x: Float, y: Float, dx: Float, dy: Float) {
        path.reset()
        path.moveTo(x + dx, y + dy)
        path.lineTo(x - dx + dy * 0.9f, y - dy + dx * 0.9f)
        path.lineTo(x - dx - dy * 0.9f, y - dy - dx * 0.9f)
        path.close()
        c.drawPath(path, fill)
    }

    companion object {
        private const val STICK_DEADZONE = 0.08f
        private const val DOUBLE_TAP_MS = 280L
    }
}
