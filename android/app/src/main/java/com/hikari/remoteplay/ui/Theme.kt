package com.hikari.remoteplay.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp

// Tokens from DESIGN.md: near-black canvas, raised hairline cards, lit amber actions.
object Hk {
    val bg = Color(0xFF0E0F11)
    val panel = Color(0xFF16171A)
    val control = Color(0xFF1E1F23)
    val controlHover = Color(0xFF26282D)
    val border = Color(0xFF2B2D33)
    val borderStrong = Color(0xFF3A3D44)
    val text = Color(0xFFF3F4F6)
    val textSecondary = Color(0xFFA8ACB4)
    val textMuted = Color(0xFF80858E)
    val amber = Color(0xFFFBBB04)
    val amberPressed = Color(0xFFE2A600)
    val onAmber = Color(0xFF1A1400)
    val good = Color(0xFF3DD68C)
    val warning = Color(0xFFF5A524)
    val error = Color(0xFFFF5C5C)
    val joyconLeft = Color(0xFF00B8D9)
    val joyconRight = Color(0xFFFF5F52)
}

private val scheme = darkColorScheme(
    primary = Hk.amber,
    onPrimary = Hk.onAmber,
    secondary = Hk.control,
    onSecondary = Hk.text,
    background = Hk.bg,
    onBackground = Hk.text,
    surface = Hk.panel,
    onSurface = Hk.text,
    surfaceVariant = Hk.control,
    onSurfaceVariant = Hk.textSecondary,
    outline = Hk.border,
    outlineVariant = Hk.border,
    error = Hk.error,
)

private val typography = Typography(
    headlineSmall = TextStyle(fontSize = 22.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 0.sp),
    titleMedium = TextStyle(fontSize = 16.sp, fontWeight = FontWeight.SemiBold),
    bodyMedium = TextStyle(fontSize = 14.sp, lineHeight = 20.sp),
    bodySmall = TextStyle(fontSize = 12.5.sp, lineHeight = 17.sp),
    labelLarge = TextStyle(fontSize = 15.sp, fontWeight = FontWeight.SemiBold),
    labelMedium = TextStyle(fontSize = 12.sp, fontWeight = FontWeight.Medium),
)

@Composable
fun HikariTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = scheme, typography = typography, content = content)
}
