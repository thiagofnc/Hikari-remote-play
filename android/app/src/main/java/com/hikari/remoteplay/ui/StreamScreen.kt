package com.hikari.remoteplay.ui

import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.requiredSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.hikari.remoteplay.PictureMode
import com.hikari.remoteplay.Session
import com.hikari.remoteplay.input.TouchControlsView
import com.hikari.remoteplay.proto.Preset
import com.hikari.remoteplay.proto.VideoCodec
import kotlinx.coroutines.delay
import java.util.Locale

@Composable
fun StreamScreen(session: Session) {
    val config by session.config.collectAsState()
    val menuOpen by session.menuOpen.collectAsState()
    val gamepadActive by session.gamepadActive.collectAsState()
    val toast by session.toast.collectAsState()
    var showControls by remember { mutableStateOf(session.prefs.showControls) }
    var showStats by remember { mutableStateOf(session.prefs.showStats) }
    var opacity by remember { mutableFloatStateOf(session.prefs.controlsOpacity) }
    var scale by remember { mutableFloatStateOf(session.prefs.controlsScale) }
    var haptics by remember { mutableStateOf(session.prefs.haptics) }
    var pictureMode by remember { mutableStateOf(session.prefs.pictureMode) }
    var editingLayout by remember { mutableStateOf(false) }
    var editSelection by remember { mutableStateOf<TouchControlsView.Group?>(null) }
    var editSize by remember { mutableFloatStateOf(1f) }
    var controlsView by remember { mutableStateOf<TouchControlsView?>(null) }
    var shown by remember { mutableLongStateOf(0L) }

    LaunchedEffect(Unit) {
        while (true) {
            shown = session.decoder.framesShown
            delay(250)
        }
    }

    val controlsVisible = (showControls && !gamepadActive && !menuOpen) || editingLayout

    BackHandler(enabled = editingLayout) { editingLayout = false }

    BoxWithConstraints(
        Modifier
            .fillMaxSize()
            .background(Color.Black)
            // With the touch controls hidden, a tap brings them (or the menu) back.
            .pointerInput(showControls, gamepadActive) {
                detectTapGestures {
                    if (gamepadActive && showControls) session.showTouchControls() else if (!showControls) session.openMenu()
                }
            },
        contentAlignment = Alignment.Center,
    ) {
        val aspect = if (config.height > 0) config.width.toFloat() / config.height else 16f / 9f
        AndroidView(
            factory = { ctx ->
                SurfaceView(ctx).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) = session.decoder.setSurface(holder.surface)
                        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
                        override fun surfaceDestroyed(holder: SurfaceHolder) = session.decoder.setSurface(null)
                    })
                }
            },
            modifier = when (pictureMode) {
                PictureMode.Fit -> Modifier.aspectRatio(aspect)
                PictureMode.Stretch -> Modifier.fillMaxSize()
                // Cover the screen keeping the picture's shape; the overflow is off screen.
                PictureMode.Fill -> if (maxWidth / maxHeight > aspect) Modifier.requiredSize(maxWidth, maxWidth / aspect)
                else Modifier.requiredSize(maxHeight * aspect, maxHeight)
            },
        )

        if (shown == 0L) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                CircularProgressIndicator(color = Hk.amber, strokeWidth = 3.dp, modifier = Modifier.size(36.dp))
                Spacer(Modifier.height(14.dp))
                Text("Waiting for the first picture", color = Hk.textSecondary, fontSize = 14.sp)
            }
        }

        if (controlsVisible) {
            AndroidView(
                factory = { ctx ->
                    TouchControlsView(ctx).apply {
                        onPadChanged = session::onTouchPad
                        onMenu = session::openMenu
                        onSelectionChanged = { g ->
                            editSelection = g
                            if (g != null) editSize = groupSize(g)
                        }
                        onLayoutChanged = { session.prefs.controlsLayout = it }
                        setLayout(session.prefs.controlsLayout)
                        controlsView = this
                    }
                },
                update = {
                    it.opacity = opacity
                    it.scale = scale
                    it.haptics = haptics
                    it.editing = editingLayout
                },
                onRelease = {
                    it.releaseAll()
                    if (controlsView === it) controlsView = null
                },
                modifier = Modifier.fillMaxSize(),
            )
        } else if (!menuOpen) {
            // Release anything the touch layer was holding when it went away.
            LaunchedEffect(Unit) { session.onTouchPad(com.hikari.remoteplay.proto.PadState.NEUTRAL) }
        }

        if (editingLayout) {
            LayoutEditor(
                selection = editSelection,
                size = editSize,
                onSize = { v -> editSelection?.let { controlsView?.setGroupSize(it, v) }; editSize = v },
                onReset = { controlsView?.resetLayout(); editSize = 1f },
                onDone = { editingLayout = false },
                modifier = Modifier.align(Alignment.Center),
            )
        }

        if (showStats) StatsOverlay(session, Modifier.align(Alignment.TopStart).padding(10.dp))

        toast?.let { Toast(it, Modifier.align(Alignment.TopCenter).padding(top = 48.dp)) }

        if (menuOpen) {
            StreamMenu(
                session,
                showControls = showControls,
                onShowControls = { showControls = it; session.prefs.showControls = it },
                showStats = showStats,
                onShowStats = { showStats = it; session.prefs.showStats = it },
                opacity = opacity,
                onOpacity = { opacity = it; session.prefs.controlsOpacity = it },
                scale = scale,
                onScale = { scale = it; session.prefs.controlsScale = it },
                haptics = haptics,
                onHaptics = { haptics = it; session.prefs.haptics = it },
                pictureMode = pictureMode,
                onPictureMode = { pictureMode = it; session.prefs.pictureMode = it },
                onEditLayout = {
                    showControls = true
                    session.prefs.showControls = true
                    session.showTouchControls()
                    editingLayout = true
                    session.closeMenu()
                },
            )
        }
    }
}

@Composable
private fun StreamMenu(
    session: Session,
    showControls: Boolean, onShowControls: (Boolean) -> Unit,
    showStats: Boolean, onShowStats: (Boolean) -> Unit,
    opacity: Float, onOpacity: (Float) -> Unit,
    scale: Float, onScale: (Float) -> Unit,
    haptics: Boolean, onHaptics: (Boolean) -> Unit,
    pictureMode: PictureMode, onPictureMode: (PictureMode) -> Unit,
    onEditLayout: () -> Unit,
) {
    val preset by session.preset.collectAsState()
    Box(
        Modifier
            .fillMaxSize()
            .background(Color(0xB30E0F11))
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) { session.closeMenu() },
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier
                .widthIn(max = 640.dp)
                .fillMaxWidth(0.9f)
                .clip(RoundedCornerShape(14.dp))
                .background(Hk.panel)
                .border(1.dp, Hk.border, RoundedCornerShape(14.dp))
                .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {}
                .verticalScroll(rememberScrollState())
                .padding(20.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Paused", fontSize = 18.sp, fontWeight = FontWeight.SemiBold, color = Hk.text, modifier = Modifier.weight(1f))
                Text("Input is released while this menu is open", fontSize = 12.sp, color = Hk.textMuted)
            }
            Spacer(Modifier.height(16.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                PrimaryButton("Resume", onClick = session::closeMenu, modifier = Modifier.weight(1f))
                SecondaryButton("Stop streaming", onClick = session::stopStream, modifier = Modifier.weight(1f).height(52.dp))
            }
            Spacer(Modifier.height(18.dp))
            SectionLabel("Stream")
            Segmented(
                Preset.entries, preset,
                label = { if (it == Preset.MinimumLatency) "Latency" else it.label },
                onSelect = session::setPreset,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(10.dp))
            SectionLabel("Picture")
            Segmented(PictureMode.entries, pictureMode, label = { it.label }, onSelect = onPictureMode, modifier = Modifier.fillMaxWidth())
            Text(
                when (pictureMode) {
                    PictureMode.Fit -> "The whole picture, sharp and undistorted. Wide phones show bars at the sides."
                    PictureMode.Fill -> "Fills the screen by trimming a little off the top and bottom."
                    PictureMode.Stretch -> "Fills the screen by widening the picture."
                },
                fontSize = 12.sp, color = Hk.textMuted, modifier = Modifier.padding(top = 6.dp, bottom = 4.dp),
            )
            ToggleRow("Performance overlay", showStats, onShowStats)
            Spacer(Modifier.height(10.dp))
            SectionLabel("Touch controls")
            ToggleRow("Show touch controls", showControls, onShowControls)
            ToggleRow("Vibrate on press", haptics, onHaptics)
            SliderRow("Opacity", opacity, 0.15f..1f, onOpacity)
            SliderRow("Size", scale, 0.7f..1.4f, onScale)
            SecondaryButton("Move and resize controls", onClick = onEditLayout, modifier = Modifier.fillMaxWidth().padding(top = 4.dp))
            Text(
                "Touch anywhere on an empty side to use that stick. Double-tap and hold a stick to click it. The D-pad presses one direction at a time; use the stick for diagonals. A connected controller hides the touch controls; tap the screen to bring them back.",
                fontSize = 12.sp, color = Hk.textMuted, modifier = Modifier.padding(top = 6.dp),
            )
        }
    }
}

@Composable
private fun LayoutEditor(
    selection: TouchControlsView.Group?,
    size: Float, onSize: (Float) -> Unit,
    onReset: () -> Unit, onDone: () -> Unit,
    modifier: Modifier,
) {
    Column(
        modifier
            .widthIn(max = 360.dp)
            .fillMaxWidth(0.45f)
            .clip(RoundedCornerShape(14.dp))
            .background(Color(0xE6181A1D))
            .border(1.dp, Hk.border, RoundedCornerShape(14.dp))
            .padding(14.dp),
    ) {
        Text(
            selection?.label ?: "Drag a control to move it",
            fontSize = 15.sp, fontWeight = FontWeight.SemiBold, color = Hk.text,
        )
        if (selection != null) {
            SliderRow("Size", size, TouchControlsView.MIN_GROUP_SIZE..TouchControlsView.MAX_GROUP_SIZE, onSize)
        } else {
            Text("Tap a control to resize it. Changes are saved as you go.", fontSize = 12.sp, color = Hk.textMuted, modifier = Modifier.padding(top = 4.dp))
        }
        Spacer(Modifier.height(10.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            SecondaryButton("Reset", onClick = onReset, modifier = Modifier.weight(1f))
            PrimaryButton("Done", onClick = onDone, modifier = Modifier.weight(1f).height(46.dp))
        }
    }
}

@Composable
private fun ToggleRow(label: String, on: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(label, color = Hk.text, fontSize = 14.sp, modifier = Modifier.weight(1f))
        Switch(
            checked = on, onCheckedChange = onChange,
            colors = SwitchDefaults.colors(
                checkedTrackColor = Hk.amber, checkedThumbColor = Hk.onAmber,
                uncheckedTrackColor = Hk.control, uncheckedThumbColor = Hk.textSecondary, uncheckedBorderColor = Hk.border,
            ),
        )
    }
}

@Composable
private fun SliderRow(label: String, value: Float, range: ClosedFloatingPointRange<Float>, onChange: (Float) -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(label, color = Hk.text, fontSize = 14.sp, modifier = Modifier.width(80.dp))
        Slider(
            value = value, onValueChange = onChange, valueRange = range, modifier = Modifier.weight(1f),
            colors = SliderDefaults.colors(thumbColor = Hk.amber, activeTrackColor = Hk.amber, inactiveTrackColor = Hk.border),
        )
    }
}

@Composable
private fun StatsOverlay(session: Session, modifier: Modifier) {
    var lines by remember { mutableStateOf(listOf<String>()) }
    LaunchedEffect(Unit) {
        while (true) {
            val n = session.client.stats
            val d = session.decoder.stats
            val c = session.config.value
            val h = session.hostStats.value
            fun f(v: Double) = String.format(Locale.US, "%.1f", v)
            lines = buildList {
                add("${c.width}×${c.height} ${if (c.codec == VideoCodec.HEVC) "HEVC" else "H.264"} · ${f(d.presentFps)} fps · ${f(n.mbps)} Mb/s")
                add("RTT ${f(n.rttMs)} ms · jitter ${f(n.jitterMs)} ms · loss ${f(n.lossPct)}% · queue ${f(n.queueDelayMs)} ms")
                add("Decode p50 ${f(d.decodeP50Ms)} / p95 ${f(d.decodeP95Ms)} ms" + if (n.clockSynced && d.endToEndMs > 0) " · capture→display ${f(d.endToEndMs)} ms" else "")
                add("Frames ${n.framesComplete} · dropped ${n.framesDropped} · FEC ${n.framesFecRecovered} · keyframe asks ${n.keyframeRequests}")
                if (h != null) {
                    add("Host: ${h.encoderName} · encode p50 ${f(h.encodeP50 / 1000.0)} ms · input ${f(h.inputOwdP50 / 1000.0)} ms one way")
                    add("Bridge→Switch p50 ${f(h.espRxToConsoleP50 / 1000.0)} ms · input ${session.input.rate}/s")
                }
                add("Audio buffer ${session.audio.bufferedMs} ms · ${d.decoderName}")
            }
            delay(500)
        }
    }
    Column(
        modifier.clip(RoundedCornerShape(8.dp)).background(Color(0x990E0F11)).padding(horizontal = 10.dp, vertical = 8.dp)
    ) {
        for (l in lines) Text(l, color = Hk.text, fontSize = 11.sp, fontFamily = FontFamily.Monospace, lineHeight = 15.sp)
    }
}
