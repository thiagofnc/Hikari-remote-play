package com.hikari.remoteplay

import android.annotation.SuppressLint
import android.content.Intent
import android.content.pm.ActivityInfo
import android.os.Build
import android.os.Bundle
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.hikari.remoteplay.ui.HikariTheme
import com.hikari.remoteplay.ui.LobbyScreen
import com.hikari.remoteplay.ui.SetupScreen
import com.hikari.remoteplay.ui.StreamScreen

class MainActivity : ComponentActivity() {
    private lateinit var session: Session
    // An invite opened from a hikari:// link, waiting to prefill Setup.
    private val pendingInvite = mutableStateOf<String?>(null)

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(statusBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT))
        super.onCreate(savedInstanceState)
        session = (application as HikariApp).session
        handleIntent(intent)

        setContent {
            HikariTheme {
                val screen by session.screen.collectAsState()
                val menuOpen by session.menuOpen.collectAsState()
                LaunchedEffect(screen) { applyScreenMode(screen) }
                BackHandler(enabled = screen == Screen.Stream) {
                    if (menuOpen) session.closeMenu() else session.openMenu()
                }
                BackHandler(enabled = screen == Screen.Setup && session.prefs.invite != null) { session.cancelSetup() }
                when (screen) {
                    Screen.Setup -> SetupScreen(session, pendingInvite.value) { pendingInvite.value = null }
                    Screen.Lobby -> LobbyScreen(session)
                    Screen.Stream -> StreamScreen(session)
                }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(intent: Intent?) {
        val uri = intent?.data ?: return
        if (uri.scheme == "hikari") {
            pendingInvite.value = uri.toString()
            session.openSetup()
        }
    }

    private fun applyScreenMode(screen: Screen) {
        val controller = WindowCompat.getInsetsController(window, window.decorView)
        if (screen == Screen.Stream) {
            requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            controller.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            controller.hide(WindowInsetsCompat.Type.systemBars())
            window.attributes = window.attributes.apply {
                // Use the area around cutouts too: the controls hug the screen edges.
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                    layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                }
                // Run the panel at its fastest refresh rate: a new frame reaches the glass sooner.
                preferredDisplayModeId = fastestDisplayMode()
            }
        } else {
            requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            controller.show(WindowInsetsCompat.Type.systemBars())
            window.attributes = window.attributes.apply { preferredDisplayModeId = 0 }
        }
    }

    // The highest refresh rate at the current resolution (0 = let the system choose).
    private fun fastestDisplayMode(): Int = runCatching {
        @Suppress("DEPRECATION")
        val display = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) display!! else windowManager.defaultDisplay
        val now = display.mode
        display.supportedModes
            .filter { it.physicalWidth == now.physicalWidth && it.physicalHeight == now.physicalHeight }
            .maxByOrNull { it.refreshRate }?.modeId ?: 0
    }.getOrDefault(0)

    override fun onStart() {
        super.onStart()
        session.onForeground()
    }

    override fun onStop() {
        // Recreated for a configuration change (e.g. rotating into the stream): not leaving.
        if (!isChangingConfigurations) session.onBackground()
        super.onStop()
    }

    // ---- physical controllers ----------------------------------------------------------

    // Overriding the platform method is fine; lint trips on androidx's restricted re-declaration.
    @SuppressLint("RestrictedApi")
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (session.screen.value == Screen.Stream && session.gamepad.isGamepad(event.source) && session.gamepad.onKey(event)) {
            session.onPhysicalPad()
            // With the menu up, the pad also navigates it.
            if (!session.menuOpen.value) return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (session.screen.value == Screen.Stream && session.gamepad.isGamepad(event.source) && session.gamepad.onMotion(event)) {
            session.onPhysicalPad()
            return true
        }
        return super.dispatchGenericMotionEvent(event)
    }
}
