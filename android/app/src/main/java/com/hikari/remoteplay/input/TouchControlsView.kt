package com.hikari.remoteplay.input

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
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
import kotlin.math.hypot
import kotlin.math.min
import kotlin.math.roundToInt

// Transparent on-screen Switch controller drawn over the video.
//
// Every touch is owned by the control it landed on until it lifts, so several
// thumbs work at once. Sticks float: a touch anywhere on an empty half of the
// screen becomes that side's stick, centred where the thumb landed. Face
// buttons and the D-pad let a thumb slide from one button to the next; the
// D-pad presses one direction at a time (the stick is there for diagonals).
// Double-tap and hold a stick to click it (L3 / R3).
//
// In editing mode nothing is sent: dragging a control moves it, and the
// selected control can be resized. The layout is stored as offsets relative to
// the default position (fractions of the view size), so it survives a
// different screen.
class TouchControlsView(context: Context) : View(context) {

    var onPadChanged: ((PadState) -> Unit)? = null
    var onMenu: (() -> Unit)? = null
    // Editing: the control picked up (null when none), and the layout after each change.
    var onSelectionChanged: ((Group?) -> Unit)? = null
    var onLayoutChanged: ((String) -> Unit)? = null

    enum class Group(val key: String, val label: String) {
        LeftStick("ls", "Left stick"), RightStick("rs", "Right stick"), Dpad("dpad", "D-pad"),
        Face("abxy", "A B X Y"), LeftShoulders("lsh", "ZL and L"), RightShoulders("rsh", "ZR and R"),
        Minus("minus", "Minus"), Plus("plus", "Plus"), Capture("capture", "Capture"), Home("home", "Home"),
    }

    private class Placement(var dx: Float = 0f, var dy: Float = 0f, var size: Float = 1f)
    private val placements = Group.entries.associateWith { Placement() }

    var editing = false
        set(v) {
            if (field == v) return
            field = v
            releaseAll()
            select(null)
            invalidate()
        }
    var selected: Group? = null
        private set
    private var dragPointer = -1
    private var dragX = 0f; private var dragY = 0f

    var opacity = 0.6f
        set(v) { field = v.coerceIn(0.15f, 1f); invalidate() }
    var scale = 1f
        set(v) { field = v.coerceIn(0.7f, 1.4f); layoutControls(width, height) }
    var haptics = true

    private enum class Shape { Round, Rect, Pill }
    private enum class Icon { None, Home, Capture, Menu }

    private class Button(val bit: Int, val label: String, val shape: Shape, val group: Group?, val icon: Icon = Icon.None) {
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
        Button(Btn.A, "A", Shape.Round, Group.Face), Button(Btn.B, "B", Shape.Round, Group.Face),
        Button(Btn.X, "X", Shape.Round, Group.Face), Button(Btn.Y, "Y", Shape.Round, Group.Face),
    )
    private val shoulders = listOf(
        Button(Btn.ZL, "ZL", Shape.Pill, Group.LeftShoulders), Button(Btn.L, "L", Shape.Pill, Group.LeftShoulders),
        Button(Btn.ZR, "ZR", Shape.Pill, Group.RightShoulders), Button(Btn.R, "R", Shape.Pill, Group.RightShoulders),
    )
    private val system = listOf(
        Button(Btn.MINUS, "−", Shape.Round, Group.Minus), Button(Btn.PLUS, "+", Shape.Round, Group.Plus),
        Button(Btn.CAPTURE, "", Shape.Round, Group.Capture, Icon.Capture), Button(Btn.HOME, "", Shape.Round, Group.Home, Icon.Home),
    )
    // The menu button stays put so the menu is always reachable.
    private val menu = Button(0, "", Shape.Pill, null, Icon.Menu)
    private val buttons = face + shoulders + system

    private var dpadX = 0f; private var dpadY = 0f; private var dpadSize = 0f
    private var faceX = 0f; private var faceY = 0f; private var faceR = 0f

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
    private val dashed = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
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
        fun k(g: Group) = s * placements.getValue(g).size
        // Default centre moved by the saved offset, kept on screen.
        fun px(g: Group, x: Float) = (x + placements.getValue(g).dx * w).coerceIn(0f, w.toFloat())
        fun py(g: Group, y: Float) = (y + placements.getValue(g).dy * h).coerceIn(0f, h.toFloat())

        // Default anchors follow the global size only, so resizing one control never moves another.
        // Right hand: ABXY above, right stick below-left (Switch layout).
        val d0 = 0.125f * u * s
        val fx0 = w - edge - d0 - 0.064f * u * s
        val fy0 = h * 0.47f
        val rsx0 = fx0 - 0.34f * u * s
        val rsy0 = h - edge - 0.135f * u * s

        faceX = px(Group.Face, fx0); faceY = py(Group.Face, fy0)
        val d = 0.125f * u * k(Group.Face)
        val r = 0.064f * u * k(Group.Face)
        faceR = d + r
        place(face[0], faceX + d, faceY, r)      // A east
        place(face[1], faceX, faceY + d, r)      // B south
        place(face[2], faceX, faceY - d, r)      // X north
        place(face[3], faceX - d, faceY, r)      // Y west
        face.forEach { it.slack = 1.35f }

        right.radius = 0.135f * u * k(Group.RightStick)
        right.homeX = px(Group.RightStick, rsx0)
        right.homeY = py(Group.RightStick, rsy0)

        // Left hand mirrors it: stick above, D-pad below-right.
        left.radius = 0.135f * u * k(Group.LeftStick)
        left.homeX = px(Group.LeftStick, w - fx0)
        left.homeY = py(Group.LeftStick, fy0)
        dpadSize = 0.33f * u * k(Group.Dpad)
        dpadX = px(Group.Dpad, w - rsx0)
        dpadY = py(Group.Dpad, rsy0)

        // Shoulders stacked in the top corners, ZL/ZR outermost.
        val pw0 = 0.115f * u * s
        val ph0 = 0.048f * u * s
        val top = edge * 0.6f + ph0
        for ((g, x0, zl, l) in listOf(
            ShoulderPair(Group.LeftShoulders, edge + pw0, shoulders[0], shoulders[1]),
            ShoulderPair(Group.RightShoulders, w - edge - pw0, shoulders[2], shoulders[3]),
        )) {
            val pw = 0.115f * u * k(g)
            val ph = 0.048f * u * k(g)
            val cx = px(g, x0)
            val cy = py(g, top + 1.45f * ph0)
            placeRect(zl, cx, cy - 1.45f * ph, pw, ph)
            placeRect(l, cx, cy + 1.45f * ph, pw, ph)
        }
        shoulders.forEach { it.slack = 1.25f }

        val sysX = listOf(w / 2f - 0.17f * u, w / 2f + 0.17f * u, w / 2f - 0.17f * u, w / 2f + 0.17f * u)
        val sysY = listOf(top, top, h - top, h - top)
        for ((i, b) in system.withIndex()) {
            val g = b.group!!
            place(b, px(g, sysX[i]), py(g, sysY[i]), 0.042f * u * k(g))
        }
        system.forEach { it.slack = 1.4f }
        placeRect(menu, w / 2f, top, 0.055f * u, 0.03f * u)
        menu.slack = 1.6f

        for (st in listOf(left, right)) if (st.pointer < 0) resetStick(st)
        invalidate()
    }

    private data class ShoulderPair(val g: Group, val x: Float, val zl: Button, val l: Button)

    // ---- layout editing -------------------------------------------------------------

    // "key:dx,dy,size;..." for the groups that differ from the default.
    fun layoutString(): String = placements.entries
        .filter { (_, p) -> p.dx != 0f || p.dy != 0f || p.size != 1f }
        .joinToString(";") { (g, p) -> "${g.key}:${p.dx},${p.dy},${p.size}" }

    fun setLayout(str: String) {
        placements.values.forEach { it.dx = 0f; it.dy = 0f; it.size = 1f }
        for (entry in str.split(';')) {
            val (key, values) = entry.split(':').takeIf { it.size == 2 } ?: continue
            val g = Group.entries.firstOrNull { it.key == key } ?: continue
            val v = values.split(',').map { it.toFloatOrNull() }
            if (v.size != 3 || v.any { it == null || it.isNaN() }) continue
            placements.getValue(g).apply {
                dx = v[0]!!.coerceIn(-1f, 1f)
                dy = v[1]!!.coerceIn(-1f, 1f)
                size = v[2]!!.coerceIn(MIN_GROUP_SIZE, MAX_GROUP_SIZE)
            }
        }
        layoutControls(width, height)
    }

    fun resetLayout() {
        setLayout("")
        onLayoutChanged?.invoke(layoutString())
    }

    fun groupSize(g: Group): Float = placements.getValue(g).size

    fun setGroupSize(g: Group, v: Float) {
        placements.getValue(g).size = v.coerceIn(MIN_GROUP_SIZE, MAX_GROUP_SIZE)
        layoutControls(width, height)
        onLayoutChanged?.invoke(layoutString())
    }

    private fun select(g: Group?) {
        if (selected == g) return
        selected = g
        onSelectionChanged?.invoke(g)
        invalidate()
    }

    private fun groupAt(x: Float, y: Float): Group? {
        (system + shoulders).firstOrNull { it.hit(x, y) }?.let { return it.group }
        return when {
            hypot(x - faceX, y - faceY) <= faceR * 1.1f -> Group.Face
            abs(x - dpadX) <= dpadSize * 0.6f && abs(y - dpadY) <= dpadSize * 0.6f -> Group.Dpad
            hypot(x - left.homeX, y - left.homeY) <= left.radius * 1.1f -> Group.LeftStick
            hypot(x - right.homeX, y - right.homeY) <= right.radius * 1.1f -> Group.RightStick
            else -> null
        }
    }

    private fun bounds(g: Group, out: RectF) {
        fun circle(x: Float, y: Float, r: Float) = out.set(x - r, y - r, x + r, y + r)
        fun union(a: Button, b: Button) = out.set(
            min(a.cx - a.rw, b.cx - b.rw), min(a.cy - a.rh, b.cy - b.rh),
            maxOf(a.cx + a.rw, b.cx + b.rw), maxOf(a.cy + a.rh, b.cy + b.rh),
        )
        when (g) {
            Group.LeftStick -> circle(left.homeX, left.homeY, left.radius)
            Group.RightStick -> circle(right.homeX, right.homeY, right.radius)
            Group.Dpad -> circle(dpadX, dpadY, dpadSize / 2)
            Group.Face -> circle(faceX, faceY, faceR)
            Group.LeftShoulders -> union(shoulders[0], shoulders[1])
            Group.RightShoulders -> union(shoulders[2], shoulders[3])
            else -> system.first { it.group == g }.let { circle(it.cx, it.cy, it.rw) }
        }
        val pad = 0.012f * min(width, height)
        out.inset(-pad, -pad)
    }

    private fun onEditTouch(e: MotionEvent) {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                if (dragPointer >= 0) return
                val i = e.actionIndex
                val g = groupAt(e.getX(i), e.getY(i))
                select(g)
                if (g != null) {
                    dragPointer = e.getPointerId(i)
                    dragX = e.getX(i); dragY = e.getY(i)
                }
            }
            MotionEvent.ACTION_MOVE -> {
                val i = e.findPointerIndex(dragPointer)
                val g = selected
                if (i < 0 || g == null || width == 0 || height == 0) return
                val p = placements.getValue(g)
                p.dx = (p.dx + (e.getX(i) - dragX) / width).coerceIn(-1f, 1f)
                p.dy = (p.dy + (e.getY(i) - dragY) / height).coerceIn(-1f, 1f)
                dragX = e.getX(i); dragY = e.getY(i)
                layoutControls(width, height)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> {
                if (e.actionMasked != MotionEvent.ACTION_CANCEL && e.getPointerId(e.actionIndex) != dragPointer) return
                if (dragPointer >= 0) onLayoutChanged?.invoke(layoutString())
                dragPointer = -1
            }
        }
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
        if (editing) {
            onEditTouch(e)
            return true
        }
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
            // One thumb owns the D-pad, so two touches can't press two directions.
            abs(x - dpadX) <= dpadSize * 0.65f && abs(y - dpadY) <= dpadSize * 0.65f ->
                if (touches.values.any { it.owner == Owner.Dpad }) return else Owner.Dpad
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
        // Four 90 degree sectors: only ever one direction, never a diagonal.
        return if (abs(dx) >= abs(dy)) {
            if (dx > 0) Btn.DPAD_RIGHT else Btn.DPAD_LEFT
        } else {
            if (dy > 0) Btn.DPAD_DOWN else Btn.DPAD_UP
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

        for (b in buttons) {
            text.textSize = 0.05f * u * scale * (b.group?.let { placements.getValue(it).size } ?: 1f)
            drawButton(c, b, pressed and b.bit != 0)
        }
        text.textSize = 0.05f * u * scale
        drawButton(c, menu, touches.values.any { it.owner == Owner.Menu })

        if (editing) {
            // Outline every movable control; the picked-up one in amber.
            dashed.strokeWidth = 0.005f * u
            dashed.pathEffect = DashPathEffect(floatArrayOf(0.02f * u, 0.012f * u), 0f)
            for (g in Group.entries) {
                bounds(g, rect)
                val on = g == selected
                dashed.color = if (on) Color.argb(255, 0xFB, 0xBB, 0x04) else Color.argb(150, 255, 255, 255)
                c.drawRoundRect(rect, 0.02f * u, 0.02f * u, dashed)
            }
        }
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
        const val MIN_GROUP_SIZE = 0.5f
        const val MAX_GROUP_SIZE = 1.8f
    }
}
