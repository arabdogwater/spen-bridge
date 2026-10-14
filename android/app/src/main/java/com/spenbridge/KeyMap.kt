package com.spenbridge

import android.view.KeyEvent

/** Android keycode -> Windows virtual-key code, for keys the S Pen Air Actions can emit. */
object KeyMap {
    fun toWindowsVk(keyCode: Int): Int = when (keyCode) {
        in KeyEvent.KEYCODE_A..KeyEvent.KEYCODE_Z -> 0x41 + (keyCode - KeyEvent.KEYCODE_A)
        in KeyEvent.KEYCODE_0..KeyEvent.KEYCODE_9 -> 0x30 + (keyCode - KeyEvent.KEYCODE_0)
        in KeyEvent.KEYCODE_F1..KeyEvent.KEYCODE_F12 -> 0x70 + (keyCode - KeyEvent.KEYCODE_F1)
        KeyEvent.KEYCODE_LEFT_BRACKET -> 0xDB
        KeyEvent.KEYCODE_RIGHT_BRACKET -> 0xDD
        KeyEvent.KEYCODE_MINUS -> 0xBD
        KeyEvent.KEYCODE_EQUALS, KeyEvent.KEYCODE_PLUS -> 0xBB
        KeyEvent.KEYCODE_COMMA -> 0xBC
        KeyEvent.KEYCODE_PERIOD -> 0xBE
        KeyEvent.KEYCODE_SLASH -> 0xBF
        KeyEvent.KEYCODE_SEMICOLON -> 0xBA
        KeyEvent.KEYCODE_APOSTROPHE -> 0xDE
        KeyEvent.KEYCODE_BACKSLASH -> 0xDC
        KeyEvent.KEYCODE_GRAVE -> 0xC0
        KeyEvent.KEYCODE_SPACE -> 0x20
        KeyEvent.KEYCODE_TAB -> 0x09
        KeyEvent.KEYCODE_ENTER -> 0x0D
        KeyEvent.KEYCODE_ESCAPE -> 0x1B
        KeyEvent.KEYCODE_DEL -> 0x08
        KeyEvent.KEYCODE_FORWARD_DEL -> 0x2E
        KeyEvent.KEYCODE_PAGE_UP -> 0x21
        KeyEvent.KEYCODE_PAGE_DOWN -> 0x22
        KeyEvent.KEYCODE_MOVE_HOME -> 0x24
        KeyEvent.KEYCODE_MOVE_END -> 0x23
        KeyEvent.KEYCODE_DPAD_LEFT -> 0x25
        KeyEvent.KEYCODE_DPAD_UP -> 0x26
        KeyEvent.KEYCODE_DPAD_RIGHT -> 0x27
        KeyEvent.KEYCODE_DPAD_DOWN -> 0x28
        else -> 0
    }
}
