package com.spenbridge

/** Wire protocol shared with the PC receiver. See PROTOCOL.md at the repo root. */
object Proto {
    const val PORT = 47800
    const val VERSION: Byte = 1

    const val T_DISCOVER: Byte = 1
    const val T_OFFER: Byte = 2
    const val T_HELLO: Byte = 3
    const val T_WELCOME: Byte = 4
    const val T_PEN: Byte = 5
    const val T_PING: Byte = 6
    const val T_PONG: Byte = 7
    const val T_KEY: Byte = 8
    const val T_BYE: Byte = 9

    const val F_IN_RANGE = 1
    const val F_CONTACT = 2
    const val F_BARREL = 4
    const val F_ERASER = 8

    const val HEADER_SIZE = 4
    const val SAMPLE_SIZE = 16
    const val MAX_SAMPLES = 48

    const val MOD_CTRL = 1
    const val MOD_SHIFT = 2
    const val MOD_ALT = 4
    const val MOD_WIN = 8

    // Windows virtual-key codes used by the phone-side shortcuts.
    const val VK_Z = 0x5A
}
