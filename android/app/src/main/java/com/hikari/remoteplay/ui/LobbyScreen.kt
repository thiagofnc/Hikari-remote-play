package com.hikari.remoteplay.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.hikari.remoteplay.Session
import com.hikari.remoteplay.net.ConnState
import com.hikari.remoteplay.proto.ConsoleState
import com.hikari.remoteplay.proto.HostIssue
import com.hikari.remoteplay.proto.Preset
import com.hikari.remoteplay.proto.SessionPhase

@Composable
fun LobbyScreen(session: Session) {
    val (conn, _) = session.conn.collectAsState().value
    val st by session.status.collectAsState()
    val hostName by session.hostName.collectAsState()
    val preset by session.preset.collectAsState()
    val endedByUser by session.endedByUser.collectAsState()
    val toast by session.toast.collectAsState()

    val connected = conn == ConnState.Connected
    val progress = st != null && st!!.phase != SessionPhase.Lobby && st!!.phase != SessionPhase.Failed
    val failed = st?.phase == SessionPhase.Failed
    val replaced = conn == ConnState.Replaced
    val rejected = conn == ConnState.Rejected
    val unreachable = conn == ConnState.Unreachable
    val resume = endedByUser && st?.console == ConsoleState.Ready && st?.phase == SessionPhase.Lobby

    Box(Modifier.fillMaxSize().background(Hk.bg)) {
        Column(
            Modifier
                .fillMaxSize()
                .safeDrawingPadding()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 20.dp, vertical = 16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Column(Modifier.widthIn(max = 560.dp).fillMaxWidth()) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Brand(Modifier.weight(1f))
                    ConnPill(conn)
                }
                Spacer(Modifier.height(24.dp))

                // Console card: what the console is doing and the one action that matters.
                HkCard(Modifier.fillMaxWidth()) {
                    Text(
                        if (hostName.isEmpty()) "Your Switch 2" else "$hostName's Switch 2",
                        style = MaterialTheme.typography.headlineSmall,
                    )
                    Spacer(Modifier.height(4.dp))
                    val state = when {
                        st == null -> if (connected) "Checking your console" else "Waiting for the host"
                        else -> when (st!!.console) {
                            ConsoleState.Ready -> if (resume) "Stream ended. Your console is still on." else "Ready to play"
                            ConsoleState.Asleep -> "Asleep. Play will wake your console."
                            ConsoleState.Off -> "Off or undocked"
                            ConsoleState.Waking -> "Waking your console"
                            ConsoleState.AwakeNoVideo -> "Waiting for an HDMI signal"
                            ConsoleState.Unknown -> "Checking your console"
                        }
                    }
                    Text(state, color = Hk.textSecondary, fontSize = 15.sp)

                    if (progress) {
                        val s = st!!
                        Spacer(Modifier.height(18.dp))
                        val current = when (s.phase) {
                            SessionPhase.Waking -> 0
                            SessionPhase.WaitingForVideo -> 1
                            SessionPhase.WaitingForController -> 2
                            else -> 3
                        }
                        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            listOf("Wake", "Picture", "Controller", "Stream").forEachIndexed { i, label ->
                                Column(Modifier.weight(1f)) {
                                    Box(
                                        Modifier.fillMaxWidth().height(4.dp).clip(RoundedCornerShape(2.dp))
                                            .background(if (i < current) Hk.good else if (i == current) Hk.amber else Hk.border)
                                    )
                                    Text(label, fontSize = 12.sp, color = if (i <= current) Hk.text else Hk.textMuted, modifier = Modifier.padding(top = 6.dp))
                                }
                            }
                        }
                        Spacer(Modifier.height(12.dp))
                        Text(
                            s.message.ifEmpty { if (s.phase == SessionPhase.Streaming) "Waiting for the first picture" else "Starting…" },
                            color = Hk.textSecondary, fontSize = 13.sp,
                        )
                        if (s.phaseProgress > 0) {
                            Spacer(Modifier.height(8.dp))
                            LinearProgressIndicator(
                                progress = { s.phaseProgress / 1000f },
                                modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(2.dp)),
                                color = Hk.amber, trackColor = Hk.border,
                            )
                        }
                        Spacer(Modifier.height(16.dp))
                        SecondaryButton("Cancel", onClick = { session.stopStream() })
                    } else {
                        if (failed) {
                            Spacer(Modifier.height(10.dp))
                            Text(st!!.message.ifEmpty { issueText(st!!.issue) }, color = Hk.error, fontSize = 13.sp)
                        }
                        Spacer(Modifier.height(18.dp))
                        val label = when {
                            replaced -> "Take over"
                            rejected -> "Set up connection"
                            failed -> "Try again"
                            resume -> "Resume"
                            else -> "Play"
                        }
                        PrimaryButton(
                            label,
                            onClick = {
                                when {
                                    replaced -> session.takeOver()
                                    rejected -> session.openSetup()
                                    else -> session.play()
                                }
                            },
                            enabled = connected || replaced || rejected,
                            modifier = Modifier.fillMaxWidth(),
                        )
                        Spacer(Modifier.height(10.dp))
                        val hint = when {
                            replaced -> "Another device is connected to your host. Take over to play here; the other device will be disconnected."
                            rejected -> "The host did not accept the pairing code. Paste a fresh invite from the host."
                            unreachable -> "No answer from the host. Check that Hikari is running there and, away from home, that Tailscale is on on this phone."
                            failed -> "Check the host, then retry."
                            else -> "Keep the host app running on your PC at home."
                        }
                        Text(hint, color = Hk.textMuted, fontSize = 12.5.sp)
                    }
                }

                Spacer(Modifier.height(16.dp))
                HkCard(Modifier.fillMaxWidth()) {
                    Text("Stream preference", style = MaterialTheme.typography.titleMedium)
                    Spacer(Modifier.height(12.dp))
                    Segmented(
                        Preset.entries, preset,
                        label = { if (it == Preset.MinimumLatency) "Latency" else it.label },
                        onSelect = session::setPreset,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Spacer(Modifier.height(8.dp))
                    Text(
                        when (preset) {
                            Preset.MinimumLatency -> "Prioritize response time."
                            Preset.Balanced -> "Balance responsiveness and picture quality."
                            Preset.Quality -> "Prioritize picture quality."
                        },
                        color = Hk.textMuted, fontSize = 12.5.sp,
                    )
                }

                Spacer(Modifier.height(16.dp))
                HkCard(Modifier.fillMaxWidth()) {
                    Text("Host", style = MaterialTheme.typography.titleMedium)
                    Spacer(Modifier.height(4.dp))
                    Text(session.prefs.invite?.host ?: "No host configured", color = Hk.textSecondary, fontSize = 13.sp)
                    Spacer(Modifier.height(14.dp))
                    val s = st
                    Row { CheckItem("Bridge", s?.bridgeOk); CheckItem("Capture", s?.captureOk) }
                    Spacer(Modifier.height(10.dp))
                    Row { CheckItem("HDMI signal", s?.signalOk); CheckItem("Encoder", s?.encoderOk) }
                    Spacer(Modifier.height(16.dp))
                    Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        SecondaryButton("Wake console", onClick = session::wake, enabled = connected, modifier = Modifier.weight(1f))
                        SecondaryButton("Sleep", onClick = session::sleep, enabled = connected && s?.console == ConsoleState.Ready, modifier = Modifier.weight(1f))
                    }
                }

                Spacer(Modifier.height(12.dp))
                GhostButton("Change host", onClick = session::openSetup)
                Text(
                    "Touch controls appear over the stream. A Bluetooth controller works too; hold − and + to open the menu.",
                    color = Hk.textMuted, fontSize = 12.sp, modifier = Modifier.padding(horizontal = 12.dp),
                )
            }
        }
        toast?.let { Toast(it, Modifier.align(Alignment.BottomCenter).safeDrawingPadding().padding(bottom = 20.dp)) }
    }
}

@Composable
private fun ConnPill(conn: ConnState) {
    val ok = conn == ConnState.Connected
    val color = when (conn) {
        ConnState.Connected -> Hk.good
        ConnState.Rejected, ConnState.Unreachable -> Hk.error
        else -> Hk.warning
    }
    Row(
        Modifier.clip(RoundedCornerShape(12.dp)).background(Hk.control).padding(horizontal = 10.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Dot(color)
        Text(if (ok) "Connected" else conn.label, fontSize = 12.sp, color = Hk.text, fontWeight = FontWeight.Medium,
            modifier = Modifier.padding(start = 7.dp))
    }
}

@Composable
fun Toast(text: String, modifier: Modifier = Modifier) {
    Box(
        modifier.clip(RoundedCornerShape(12.dp)).background(Hk.controlHover).padding(horizontal = 16.dp, vertical = 10.dp)
    ) { Text(text, color = Hk.text, fontSize = 13.sp) }
}

private fun issueText(i: HostIssue) = when (i) {
    HostIssue.None -> "Could not start the session"
    HostIssue.NoBridge -> "The controller bridge isn't connected to the host."
    HostIssue.NoCapture -> "The capture card isn't connected to the host."
    HostIssue.NoSignal -> "No HDMI signal. Is the console docked and on?"
    HostIssue.NoEncoder -> "The host couldn't start a video encoder."
    HostIssue.WakeFailed -> "The console didn't wake up."
    HostIssue.ControllerNotAccepted -> "The console didn't accept the controller bridge."
}
