package com.hikari.remoteplay

import android.app.Application

// The session outlives the activity: a rotation or other recreation must not
// stop the stream or reconnect.
class HikariApp : Application() {
    val session: Session by lazy { Session(this) }
}
