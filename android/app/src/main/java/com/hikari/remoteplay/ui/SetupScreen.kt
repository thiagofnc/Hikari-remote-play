package com.hikari.remoteplay.ui

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.hikari.remoteplay.Session
import com.hikari.remoteplay.net.DiscoveredHost
import com.hikari.remoteplay.net.ProbeResult
import com.hikari.remoteplay.proto.HikariCrypto
import com.hikari.remoteplay.proto.Invite
import kotlinx.coroutines.delay

@Composable
fun SetupScreen(session: Session, prefill: String?, consumedPrefill: () -> Unit) {
    val clipboard = LocalClipboardManager.current
    val context = LocalContext.current
    var invite by remember { mutableStateOf(session.prefs.invite?.toUri() ?: "") }
    var host by remember { mutableStateOf("") }
    var code by remember { mutableStateOf("") }
    var manual by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var found by remember { mutableStateOf(emptyList<DiscoveredHost>()) }

    LaunchedEffect(prefill) {
        if (prefill != null) {
            invite = prefill
            manual = false
            consumedPrefill()
        }
    }
    LaunchedEffect(Unit) {
        while (true) {
            found = session.discovered().sortedBy { it.name }
            delay(1000)
        }
    }

    fun target(): Invite? = if (manual) {
        val c = HikariCrypto.normalizeCode(code)
        if (host.isBlank() || c.length != 12) null else Invite(host.trim(), "", c)
    } else Invite.parse(invite)

    fun submit() {
        val t = target()
        if (t == null) {
            error = if (manual) "Enter the host address and the 12-character pairing code shown on the host."
            else "That doesn't look like a Hikari invite. On the host, click Copy invite and paste it here."
            return
        }
        error = null
        busy = true
        session.probe(t) { r ->
            busy = false
            error = when (r) {
                is ProbeResult.Ok -> null
                ProbeResult.BadCode -> "The host didn't accept that pairing code. Check the code shown on the host."
                ProbeResult.BadVersion -> "The host runs a different version of Hikari. Update both to the same version."
                ProbeResult.NoAnswer -> "No answer from ${t.host}. Make sure Hikari is running on the host. Away from home, turn on Tailscale on this phone."
            }
        }
    }

    val fieldColors = OutlinedTextFieldDefaults.colors(
        focusedBorderColor = Hk.amber, unfocusedBorderColor = Hk.border,
        focusedContainerColor = Hk.control, unfocusedContainerColor = Hk.control,
        cursorColor = Hk.amber, focusedLabelColor = Hk.amber, unfocusedLabelColor = Hk.textMuted,
    )

    Column(
        Modifier
            .fillMaxSize()
            .background(Hk.bg)
            .safeDrawingPadding()
            .imePadding()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 20.dp, vertical = 16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Column(Modifier.widthIn(max = 560.dp).fillMaxWidth()) {
            Brand(subtitle = "Your Switch 2, anywhere.")
            Spacer(Modifier.height(28.dp))
            Text("Connect to your host", style = androidx.compose.material3.MaterialTheme.typography.headlineSmall)
            Spacer(Modifier.height(6.dp))
            Text(
                "On the PC at home, open Hikari, choose Host, and click Copy invite. Then paste the invite here.",
                color = Hk.textSecondary, fontSize = 14.sp,
            )
            Spacer(Modifier.height(20.dp))

            HkCard(Modifier.fillMaxWidth()) {
                if (!manual) {
                    OutlinedTextField(
                        value = invite,
                        onValueChange = { invite = it; error = null },
                        label = { Text("Invite") },
                        placeholder = { Text("hikari://living-room/K7QM-2XPA-9TRB", color = Hk.textMuted) },
                        singleLine = false,
                        maxLines = 3,
                        colors = fieldColors,
                        modifier = Modifier.fillMaxWidth(),
                        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                    )
                    Spacer(Modifier.height(10.dp))
                    Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        SecondaryButton("Paste", onClick = {
                            clipboard.getText()?.text?.let { invite = it.trim(); error = null }
                        })
                        GhostButton("Enter address and code", onClick = { manual = true; error = null })
                    }
                } else {
                    OutlinedTextField(
                        value = host, onValueChange = { host = it; error = null },
                        label = { Text("Host address") },
                        placeholder = { Text("100.101.102.103 or living-room", color = Hk.textMuted) },
                        singleLine = true, colors = fieldColors, modifier = Modifier.fillMaxWidth(),
                    )
                    Spacer(Modifier.height(10.dp))
                    OutlinedTextField(
                        value = code, onValueChange = { code = it.uppercase(); error = null },
                        label = { Text("Pairing code") },
                        placeholder = { Text("K7QM-2XPA-9TRB", color = Hk.textMuted) },
                        singleLine = true, colors = fieldColors, modifier = Modifier.fillMaxWidth(),
                        keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Characters, imeAction = ImeAction.Done),
                    )
                    Spacer(Modifier.height(4.dp))
                    GhostButton("Paste an invite instead", onClick = { manual = false; error = null })
                }

                error?.let {
                    Spacer(Modifier.height(10.dp))
                    Text(it, color = Hk.error, fontSize = 13.sp)
                }
                Spacer(Modifier.height(14.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    PrimaryButton(if (busy) "Checking…" else "Save and connect", onClick = ::submit, enabled = !busy, modifier = Modifier.weight(1f))
                    if (busy) CircularProgressIndicator(Modifier.padding(start = 14.dp).size(22.dp), color = Hk.amber, strokeWidth = 2.dp)
                }
            }

            if (found.isNotEmpty()) {
                Spacer(Modifier.height(22.dp))
                SectionLabel("Hosts on this network")
                for (h in found) {
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .padding(bottom = 8.dp)
                            .clip(RoundedCornerShape(10.dp))
                            .background(Hk.panel)
                            .border(1.dp, Hk.border, RoundedCornerShape(10.dp))
                            .clickable {
                                manual = true
                                host = h.address
                                error = null
                            }
                            .padding(14.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Dot(Hk.good)
                        Column(Modifier.padding(start = 12.dp).weight(1f)) {
                            Text(h.name.ifEmpty { "Hikari host" }, fontSize = 15.sp, color = Hk.text)
                            Text(h.address + if (h.tailscale) "  ·  Tailscale" else "", fontSize = 12.sp, color = Hk.textMuted)
                        }
                        Text("Use", color = Hk.amber, fontSize = 14.sp)
                    }
                }
                Text("Picking a host fills in its address. You still need the pairing code shown on the host.", color = Hk.textMuted, fontSize = 12.sp)
            }

            Spacer(Modifier.height(22.dp))
            SectionLabel("Playing away from home")
            HkCard(Modifier.fillMaxWidth()) {
                Text(
                    "Install Tailscale on this phone and sign in with the same account as the host PC. You don't need port forwarding.",
                    color = Hk.textSecondary, fontSize = 13.sp,
                )
                Spacer(Modifier.height(12.dp))
                SecondaryButton("Open Tailscale", onClick = { openTailscale(context) })
            }
            if (session.prefs.invite != null) {
                Spacer(Modifier.height(12.dp))
                GhostButton("Cancel", onClick = { session.cancelSetup() })
            }
        }
    }
}

private fun openTailscale(context: Context) {
    val launch = context.packageManager.getLaunchIntentForPackage("com.tailscale.ipn")
    val intent = launch ?: Intent(Intent.ACTION_VIEW, Uri.parse("market://details?id=com.tailscale.ipn"))
    intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    try {
        context.startActivity(intent)
    } catch (e: ActivityNotFoundException) {
        context.startActivity(
            Intent(Intent.ACTION_VIEW, Uri.parse("https://play.google.com/store/apps/details?id=com.tailscale.ipn"))
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        )
    }
}
