package com.hikari.remoteplay.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.hikari.remoteplay.R

@Composable
fun HkCard(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    Column(
        modifier
            .clip(RoundedCornerShape(12.dp))
            .background(Hk.panel)
            .border(1.dp, Hk.border, RoundedCornerShape(12.dp))
            .padding(18.dp),
        content = content,
    )
}

@Composable
fun PrimaryButton(text: String, onClick: () -> Unit, modifier: Modifier = Modifier, enabled: Boolean = true) {
    Button(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier.height(52.dp),
        shape = RoundedCornerShape(8.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = Hk.amber, contentColor = Hk.onAmber,
            disabledContainerColor = Hk.control, disabledContentColor = Hk.textMuted,
        ),
    ) { Text(text, style = androidx.compose.material3.MaterialTheme.typography.labelLarge) }
}

@Composable
fun SecondaryButton(text: String, onClick: () -> Unit, modifier: Modifier = Modifier, enabled: Boolean = true) {
    OutlinedButton(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier.height(46.dp),
        shape = RoundedCornerShape(8.dp),
        border = BorderStroke(1.dp, Hk.border),
        colors = ButtonDefaults.outlinedButtonColors(containerColor = Hk.control, contentColor = Hk.text),
        contentPadding = PaddingValues(horizontal = 16.dp),
    ) { Text(text, fontWeight = FontWeight.SemiBold, fontSize = 14.sp) }
}

@Composable
fun GhostButton(text: String, onClick: () -> Unit, modifier: Modifier = Modifier) {
    TextButton(onClick = onClick, modifier = modifier) {
        Text(text, color = Hk.textSecondary, fontWeight = FontWeight.Medium, fontSize = 14.sp)
    }
}

@Composable
fun Dot(color: Color, size: Dp = 8.dp) {
    Box(Modifier.size(size).clip(CircleShape).background(color))
}

@Composable
fun Brand(modifier: Modifier = Modifier, subtitle: String? = null) {
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        Image(painterResource(R.drawable.hikari_mark), contentDescription = null, modifier = Modifier.size(36.dp))
        Column(Modifier.padding(start = 12.dp)) {
            Text("Hikari", fontSize = 20.sp, fontWeight = FontWeight.SemiBold, color = Hk.text)
            if (subtitle != null) Text(subtitle, fontSize = 13.sp, color = Hk.textSecondary)
        }
    }
}

// Segmented choice in the style of the desktop preset picker.
@Composable
fun <T> Segmented(options: List<T>, selected: T, label: (T) -> String, onSelect: (T) -> Unit, modifier: Modifier = Modifier) {
    Row(
        modifier
            .clip(RoundedCornerShape(8.dp))
            .background(Hk.control)
            .border(1.dp, Hk.border, RoundedCornerShape(8.dp))
            .padding(3.dp),
        horizontalArrangement = Arrangement.spacedBy(3.dp),
    ) {
        for (o in options) {
            val on = o == selected
            TextButton(
                onClick = { onSelect(o) },
                modifier = Modifier.weight(1f).height(38.dp),
                shape = RoundedCornerShape(6.dp),
                colors = ButtonDefaults.textButtonColors(
                    containerColor = if (on) Hk.controlHover else Color.Transparent,
                    contentColor = if (on) Hk.text else Hk.textSecondary,
                ),
                contentPadding = PaddingValues(horizontal = 4.dp),
            ) { Text(label(o), fontSize = 13.sp, fontWeight = if (on) FontWeight.SemiBold else FontWeight.Medium, maxLines = 1) }
        }
    }
}

@Composable
fun RowScope.CheckItem(label: String, ok: Boolean?) {
    Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically) {
        Dot(
            when (ok) {
                true -> Hk.good
                false -> Hk.error
                null -> Hk.textMuted
            }
        )
        Text(label, Modifier.padding(start = 8.dp), fontSize = 13.sp, color = Hk.textSecondary, maxLines = 1)
    }
}

@Composable
fun SectionLabel(text: String) {
    Text(text.uppercase(), fontSize = 11.sp, letterSpacing = 1.sp, fontWeight = FontWeight.SemiBold, color = Hk.textMuted,
        modifier = Modifier.fillMaxWidth().padding(bottom = 8.dp))
}
