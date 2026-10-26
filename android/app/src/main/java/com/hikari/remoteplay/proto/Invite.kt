package com.hikari.remoteplay.proto

// Invites: everything a client needs to pair with a host (shared/common/invite.cpp).
//
//   hikari://living-room.tail1234.ts.net/K7QM-2XPA-9TRB?ip=100.101.102.103
//   hikari://100.101.102.103:47801/K7QM-2XPA-9TRB
data class Invite(val host: String, val fallback: String, val code: String) {

    fun toUri(): String = "hikari://$host/${HikariCrypto.formatCode(code)}" + if (fallback.isNotEmpty()) "?ip=$fallback" else ""

    companion object {
        private fun trim(s: String): String {
            var b = 0
            var e = s.length
            while (b < e && (s[b].isWhitespace() || s[b] in "\"'<")) ++b
            while (e > b && (s[e - 1].isWhitespace() || s[e - 1] in "\"'>.")) --e
            return s.substring(b, e)
        }

        private fun validHost(h: String): Boolean =
            h.isNotEmpty() && h.length <= 253 && h.all { it.isLetterOrDigit() && it.code < 128 || it in ".-:[]_" }

        // Accepts surrounding whitespace/quotes and a missing "hikari://" prefix.
        fun parse(text: String): Invite? {
            var s = trim(text)
            val scheme = "hikari://"
            if (s.length >= scheme.length && s.substring(0, scheme.length).lowercase() == scheme) s = s.substring(scheme.length)
            val slash = s.indexOf('/')
            if (slash < 0) return null
            val host = s.substring(0, slash)
            var rest = s.substring(slash + 1)
            var fallback = ""
            val q = rest.indexOf('?')
            if (q >= 0) {
                val query = rest.substring(q + 1)
                rest = rest.substring(0, q)
                if (query.startsWith("ip=")) fallback = query.substring(3).substringBefore('&')
            }
            val code = HikariCrypto.normalizeCode(rest)
            if (!validHost(host) || code.length != 12) return null
            if (fallback.isNotEmpty() && !validHost(fallback)) fallback = ""
            return Invite(host, fallback, code)
        }
    }
}

// "name", "ip", "ip:port", "[v6]:port" -> (host, port).
fun splitHostPort(s: String, defaultPort: Int = Proto.DEFAULT_PORT): Pair<String, Int> {
    if (s.startsWith("[")) {
        val close = s.indexOf(']')
        if (close > 0) {
            val host = s.substring(1, close)
            val port = s.substring(close + 1).removePrefix(":").toIntOrNull() ?: defaultPort
            return host to port
        }
    }
    val colon = s.lastIndexOf(':')
    if (colon > 0 && s.indexOf(':') == colon) {
        val port = s.substring(colon + 1).toIntOrNull()
        if (port != null) return s.substring(0, colon) to port
    }
    return s to defaultPort
}
