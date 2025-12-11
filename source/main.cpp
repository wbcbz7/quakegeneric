#pragma GCC optimize("Ofast")

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <time.h>

#include <pico/multicore.h>
#include <pico/stdlib.h>
#include <hardware/pio.h>
#include <hardware/i2c.h>
#include <hardware/vreg.h>
#include <hardware/sync.h>
#include <hardware/flash.h>
#include <hardware/clocks.h>
#include <hardware/structs/systick.h>

#ifdef PICO_RP2350
#include <hardware/regs/qmi.h>
#include <hardware/structs/qmi.h>
#endif

#include "graphics.h"
#include "mixer.h"

#include "audio.h"
#include "ff.h"
#include "psram_spi.h"
#ifdef KBDUSB
    #include "ps2kbd_mrmltr.h"
#else
    #include "ps2.h"
#endif

#if USE_NESPAD
#include "nespad.h"
#endif

#if DVI_HSTX
#include "dvi_defs.h"
#include "dvi_modes.h"
#endif

#include "quakegeneric.h"
#include "quakedef.h"
#include "sys.h"

extern "C" qboolean CDAudio_GetSamples(int16_t* buf, size_t n);

#define HOME_DIR (char*)"/QUAKE"

bool rp2350a = true;
uint8_t rx[4] = { 0 };

static int cpu_mhz = CPU_MHZ;
static int new_cpu_mhz = CPU_MHZ;
static int vreg = VREG_VOLTAGE_1_60;
static int new_vreg = VREG_VOLTAGE_1_60;
int flash_mhz = 88;
int psram_mhz = MAX_PSRAM_FREQ_MHZ;
static uint new_flash_timings = 0;
static uint new_psram_timings = 0;
static uint8_t override_video = 0xFF;
static uint8_t override_audio = 0xFF;

extern "C" bool is_i2s_enabled;
extern "C" int testPins(uint32_t pin0, uint32_t pin1);

struct semaphore vga_start_semaphore;

static FATFS fs;

struct input_bits_t {
    bool a: true;
    bool b: true;
    bool select: true;
    bool start: true;
    bool right: true;
    bool left: true;
    bool up: true;
    bool down: true;
};

input_bits_t gamepad1_bits = { false, false, false, false, false, false, false, false };
static input_bits_t gamepad2_bits = { false, false, false, false, false, false, false, false };
static input_bits_t kbd_bits = { false, false, false, false, false, false, false, false };

typedef struct key_action_s {
    int key;
    int down;
} key_action_t;

static key_action_t key_actions[32] = { 0 };
volatile static size_t next_key_action = 0;

inline static void add_key(int key, int down) {
    if (next_key_action == 32) return;
    if (next_key_action > 0) { // reduce actions
        const key_action_t& pk = key_actions[next_key_action - 1];
        if (pk.key == key && pk.down == down) return;
    }
    // <-> protection
    if (kbd_bits.right && key == K_LEFTARROW) add_key(K_RIGHTARROW, 0);
    if (kbd_bits.left && key == K_RIGHTARROW) add_key(K_LEFTARROW, 0);
    if (kbd_bits.up && key == K_DOWNARROW) add_key(K_UPARROW, 0);
    if (kbd_bits.down && key == K_UPARROW) add_key(K_DOWNARROW, 0);

    if (key == K_RIGHTARROW) kbd_bits.right = down;
    if (key == K_LEFTARROW) kbd_bits.left = down;
    if (key == K_DOWNARROW) kbd_bits.down = down;
    if (key == K_UPARROW) kbd_bits.up = down;

    key_action_t& k = key_actions[next_key_action++];
    k.key = key;
    k.down = down;
}

#ifndef KBDUSB
// TODO:
extern "C" bool handleScancode(const uint32_t ps2scancode) {
    #if 0
    if (ps2scancode != 0x45 && ps2scancode != 0x1D && ps2scancode != 0xC5) {
        char tmp1[16];
        snprintf(tmp1, 16, "%08X", ps2scancode);
        OSD::osdCenteredMsg(tmp1, LEVEL_WARN, 500);
    }
    #endif
    static bool pause_detected = false;
    if (pause_detected) {
        pause_detected = false;
        if (ps2scancode == 0x1D) return true; // ignore next byte after 0x45, TODO: split with NumLock
    }
    if ( ((ps2scancode >> 8) & 0xFF) == 0xE0) { // E0 block
        uint8_t cd = ps2scancode & 0xFF;
        bool pressed = cd < 0x80;
        cd &= 0x7F;
        #if 0
        switch (cd) {
            case 0x5B: kbdPushData(fabgl::VirtualKey::VK_LCTRL, pressed); return true; /// L WIN
            case 0x1D: {
                if (Config::CursorAsJoy) joyPushData(fabgl::VirtualKey::VK_DPAD_ALTFIRE, pressed);
                kbdPushData(fabgl::VirtualKey::VK_RCTRL, pressed);
                return true;
            }
            case 0x38: kbdPushData(fabgl::VirtualKey::VK_RALT, pressed); return true;
            case 0x5C: {  /// R WIN
                if (Config::CursorAsJoy) joyPushData(fabgl::VirtualKey::VK_DPAD_ALTFIRE, pressed);
                kbdPushData(fabgl::VirtualKey::VK_RCTRL, pressed);
                return true;
            }
            case 0x5D: kbdPushData(fabgl::VirtualKey::VK_F1, pressed); return true; /// MENU
            case 0x37: kbdPushData(fabgl::VirtualKey::VK_PRINTSCREEN, pressed); return true;
            case 0x46: kbdPushData(fabgl::VirtualKey::VK_BREAK, pressed); return true;
            case 0x52: kbdPushData(fabgl::VirtualKey::VK_INSERT, pressed); return true;
            case 0x47: {
                joyPushData(fabgl::VirtualKey::VK_MENU_HOME, pressed);
                kbdPushData(fabgl::VirtualKey::VK_HOME, pressed);
                return true;
            }
            case 0x4F: kbdPushData(fabgl::VirtualKey::VK_END, pressed); return true;
            case 0x49: kbdPushData(fabgl::VirtualKey::VK_PAGEUP, pressed); return true;
            case 0x51: kbdPushData(fabgl::VirtualKey::VK_PAGEDOWN, pressed); return true;
            case 0x53: kbdPushData(fabgl::VirtualKey::VK_DELETE, pressed); return true;
            case 0x48: {
                if (Config::CursorAsJoy) joyPushData(fabgl::VirtualKey::VK_DPAD_UP, pressed);
                joyPushData(fabgl::VirtualKey::VK_MENU_UP, pressed);
                kbdPushData(fabgl::VirtualKey::VK_UP, pressed);
                return true;
            }
            case 0x50: {
                if (Config::CursorAsJoy) joyPushData(fabgl::VirtualKey::VK_DPAD_DOWN, pressed);
                joyPushData(fabgl::VirtualKey::VK_MENU_DOWN, pressed);
                kbdPushData(fabgl::VirtualKey::VK_DOWN, pressed);
                return true;
            }
            case 0x4B: {
                if (Config::CursorAsJoy) joyPushData(fabgl::VirtualKey::VK_DPAD_LEFT, pressed);
                joyPushData(fabgl::VirtualKey::VK_MENU_LEFT, pressed);
                kbdPushData(fabgl::VirtualKey::VK_LEFT, pressed);
                return true;
            }
            case 0x4D: {
                if (Config::CursorAsJoy) joyPushData(fabgl::VirtualKey::VK_DPAD_RIGHT, pressed);
                joyPushData(fabgl::VirtualKey::VK_MENU_RIGHT, pressed);
                kbdPushData(fabgl::VirtualKey::VK_RIGHT, pressed);
                return true;
            }
            case 0x35: kbdPushData(fabgl::VirtualKey::VK_SLASH, pressed); return true;
            case 0x1C: { // VK_KP_ENTER
                kbdPushData(Config::rightSpace ? fabgl::VirtualKey::VK_SPACE : fabgl::VirtualKey::VK_RETURN, pressed);
                return true;
            }
        }
        #endif
        return true;
    }
    uint8_t cd = ps2scancode & 0xFF;
    bool pressed = cd < 0x80;
    cd &= 0x7F;
    #if 0
    switch (cd) {
        case 0x1E: kbdPushData(fabgl::VirtualKey::VK_A, pressed); return true;
        case 0x30: kbdPushData(fabgl::VirtualKey::VK_B, pressed); return true;
        case 0x2E: kbdPushData(fabgl::VirtualKey::VK_C, pressed); return true;
        case 0x20: kbdPushData(fabgl::VirtualKey::VK_D, pressed); return true;
        case 0x12: kbdPushData(fabgl::VirtualKey::VK_E, pressed); return true;
        case 0x21: kbdPushData(fabgl::VirtualKey::VK_F, pressed); return true;
        case 0x22: kbdPushData(fabgl::VirtualKey::VK_G, pressed); return true;
        case 0x23: kbdPushData(fabgl::VirtualKey::VK_H, pressed); return true;
        case 0x17: kbdPushData(fabgl::VirtualKey::VK_I, pressed); return true;
        case 0x24: kbdPushData(fabgl::VirtualKey::VK_J, pressed); return true;
        case 0x25: kbdPushData(fabgl::VirtualKey::VK_K, pressed); return true;
        case 0x26: kbdPushData(fabgl::VirtualKey::VK_L, pressed); return true;
        case 0x32: kbdPushData(fabgl::VirtualKey::VK_M, pressed); return true;
        case 0x31: kbdPushData(fabgl::VirtualKey::VK_N, pressed); return true;
        case 0x18: kbdPushData(fabgl::VirtualKey::VK_O, pressed); return true;
        case 0x19: kbdPushData(fabgl::VirtualKey::VK_P, pressed); return true;
        case 0x10: kbdPushData(fabgl::VirtualKey::VK_Q, pressed); return true;
        case 0x13: kbdPushData(fabgl::VirtualKey::VK_R, pressed); return true;
        case 0x1F: kbdPushData(fabgl::VirtualKey::VK_S, pressed); return true;
        case 0x14: kbdPushData(fabgl::VirtualKey::VK_T, pressed); return true;
        case 0x16: kbdPushData(fabgl::VirtualKey::VK_U, pressed); return true;
        case 0x2F: kbdPushData(fabgl::VirtualKey::VK_V, pressed); return true;
        case 0x11: kbdPushData(fabgl::VirtualKey::VK_W, pressed); return true;
        case 0x2D: kbdPushData(fabgl::VirtualKey::VK_X, pressed); return true;
        case 0x15: kbdPushData(fabgl::VirtualKey::VK_Y, pressed); return true;
        case 0x2C: kbdPushData(fabgl::VirtualKey::VK_Z, pressed); return true;

        case 0x0B: kbdPushData(fabgl::VirtualKey::VK_0, pressed); return true;
        case 0x02: kbdPushData(fabgl::VirtualKey::VK_1, pressed); return true;
        case 0x03: kbdPushData(fabgl::VirtualKey::VK_2, pressed); return true;
        case 0x04: kbdPushData(fabgl::VirtualKey::VK_3, pressed); return true;
        case 0x05: kbdPushData(fabgl::VirtualKey::VK_4, pressed); return true;
        case 0x06: kbdPushData(fabgl::VirtualKey::VK_5, pressed); return true;
        case 0x07: kbdPushData(fabgl::VirtualKey::VK_6, pressed); return true;
        case 0x08: kbdPushData(fabgl::VirtualKey::VK_7, pressed); return true;
        case 0x09: kbdPushData(fabgl::VirtualKey::VK_8, pressed); return true;
        case 0x0A: kbdPushData(fabgl::VirtualKey::VK_9, pressed); return true;

        case 0x29: kbdPushData(fabgl::VirtualKey::VK_TILDE, pressed); return true;
        case 0x0C: kbdPushData(fabgl::VirtualKey::VK_MINUS, pressed); return true;
        case 0x0D: kbdPushData(fabgl::VirtualKey::VK_EQUALS, pressed); return true;
        case 0x2B: kbdPushData(fabgl::VirtualKey::VK_BACKSLASH, pressed); return true;
        case 0x1A: kbdPushData(fabgl::VirtualKey::VK_LEFTBRACKET, pressed); return true;
        case 0x1B: kbdPushData(fabgl::VirtualKey::VK_RIGHTBRACKET, pressed); return true;
        case 0x27: kbdPushData(fabgl::VirtualKey::VK_SEMICOLON, pressed); return true;
        case 0x28: kbdPushData(fabgl::VirtualKey::VK_QUOTE, pressed); return true;
        case 0x33: kbdPushData(fabgl::VirtualKey::VK_COMMA, pressed); return true;
        case 0x34: kbdPushData(fabgl::VirtualKey::VK_PERIOD, pressed); return true;
        case 0x35: kbdPushData(fabgl::VirtualKey::VK_SLASH, pressed); return true;

        case 0x0E: {
            joyPushData(fabgl::VirtualKey::VK_MENU_BS, pressed);
            kbdPushData(fabgl::VirtualKey::VK_BACKSPACE, pressed);
            return true;
        }
        case 0x39: {
            joyPushData(fabgl::VirtualKey::VK_MENU_ENTER, pressed);
            kbdPushData(fabgl::VirtualKey::VK_SPACE, pressed);
            return true;
        }
        case 0x0F: {
            if (Config::TABasfire1) JPAD(fabgl::VirtualKey::VK_DPAD_FIRE, pressed);
            kbdPushData(fabgl::VirtualKey::VK_TAB, pressed);
            return true;
        }
        case 0x3A: kbdPushData(fabgl::VirtualKey::VK_CAPSLOCK, pressed); return true; /// TODO: CapsLock
        case 0x2A: kbdPushData(fabgl::VirtualKey::VK_LSHIFT, pressed); return true;
        case 0x1D: kbdPushData(fabgl::VirtualKey::VK_LCTRL, pressed); return true;
        case 0x38: {
            if (Config::CursorAsJoy) JPAD(fabgl::VirtualKey::VK_DPAD_FIRE, pressed);
            kbdPushData(fabgl::VirtualKey::VK_LALT, pressed);
            return true;
        }
        case 0x36: kbdPushData(fabgl::VirtualKey::VK_RSHIFT, pressed); return true;
        case 0x1C: {
            joyPushData(fabgl::VirtualKey::VK_MENU_ENTER, pressed);
            kbdPushData(fabgl::VirtualKey::VK_RETURN, pressed);
            return true;
        }
        case 0x01: kbdPushData(fabgl::VirtualKey::VK_ESCAPE, pressed); return true;
        case 0x3B: kbdPushData(fabgl::VirtualKey::VK_F1, pressed); return true;
        case 0x3C: kbdPushData(fabgl::VirtualKey::VK_F2, pressed); return true;
        case 0x3D: kbdPushData(fabgl::VirtualKey::VK_F3, pressed); return true;
        case 0x3E: kbdPushData(fabgl::VirtualKey::VK_F4, pressed); return true;
        case 0x3F: kbdPushData(fabgl::VirtualKey::VK_F5, pressed); return true;
        case 0x40: kbdPushData(fabgl::VirtualKey::VK_F6, pressed); return true;
        case 0x41: kbdPushData(fabgl::VirtualKey::VK_F7, pressed); return true;
        case 0x42: kbdPushData(fabgl::VirtualKey::VK_F8, pressed); return true;
        case 0x43: kbdPushData(fabgl::VirtualKey::VK_F9, pressed); return true;
        case 0x44: kbdPushData(fabgl::VirtualKey::VK_F10, pressed); return true;
        case 0x57: kbdPushData(fabgl::VirtualKey::VK_F11, pressed); return true;
        case 0x58: kbdPushData(fabgl::VirtualKey::VK_F12, pressed); return true;

        case 0x46: kbdPushData(fabgl::VirtualKey::VK_SCROLLLOCK, pressed); return true; /// TODO:
        case 0x45: {
            kbdPushData(fabgl::VirtualKey::VK_PAUSE, pressed);
            pause_detected = pressed;
            return true;
        }
        case 0x37: {
            JPAD(fabgl::VirtualKey::VK_DPAD_START, pressed);
            kbdPushData(fabgl::VirtualKey::VK_KP_MULTIPLY, pressed);
            return true;
        }
        case 0x4A: {
            JPAD(fabgl::VirtualKey::VK_DPAD_SELECT, pressed);
            kbdPushData(fabgl::VirtualKey::VK_MINUS, pressed);
            return true;
        }
        case 0x4E: {
            JPAD(fabgl::VirtualKey::VK_DPAD_FIRE, pressed);
            kbdPushData(fabgl::VirtualKey::VK_PLUS, pressed);
            return true;
        }
        case 0x53: {
            JPAD(fabgl::VirtualKey::VK_DPAD_FIRE, pressed);
            kbdPushData(fabgl::VirtualKey::VK_KP_PERIOD, pressed);
            return true;
        }
        case 0x52: {
            JPAD(fabgl::VirtualKey::VK_DPAD_ALTFIRE, pressed);
            kbdPushData(fabgl::VirtualKey::VK_KP_0, pressed);
            return true;
        }
        case 0x4F: {
            JPAD(fabgl::VirtualKey::VK_DPAD_LEFT, pressed);
            JPAD(fabgl::VirtualKey::VK_DPAD_DOWN, pressed);
            kbdPushData(fabgl::VirtualKey::VK_KP_1, pressed);
            return true;
        }
        case 0x50: {
            JPAD(fabgl::VirtualKey::VK_DPAD_DOWN, pressed);
            kbdPushData(fabgl::VirtualKey::VK_KP_2, pressed);
            return true;
        }
        case 0x51: {
            JPAD(fabgl::VirtualKey::VK_DPAD_RIGHT, pressed);
            JPAD(fabgl::VirtualKey::VK_DPAD_DOWN, pressed);
            kbdPushData(fabgl::VirtualKey::VK_KP_3, pressed);
            return true;
        }
        case 0x4B: {
            JPAD(fabgl::VirtualKey::VK_DPAD_LEFT, pressed);
            kbdPushData(fabgl::VirtualKey::VK_KP_4, pressed);
            return true;
        }
        case 0x4C: {
            JPAD(fabgl::VirtualKey::VK_DPAD_DOWN, pressed);
            kbdPushData(fabgl::VirtualKey::VK_KP_5, pressed);
            return true;
        }
        case 0x4D: {
            JPAD(fabgl::VirtualKey::VK_DPAD_RIGHT, pressed);
            kbdPushData(fabgl::VirtualKey::VK_KP_6, pressed);
            return true;
        }
        case 0x47: {
            JPAD(fabgl::VirtualKey::VK_DPAD_LEFT, pressed);
            JPAD(fabgl::VirtualKey::VK_DPAD_UP, pressed);
            kbdPushData(fabgl::VirtualKey::VK_KP_7, pressed);
            return true;
        }
        case 0x48: {
            JPAD(fabgl::VirtualKey::VK_DPAD_UP, pressed);
            kbdPushData(fabgl::VirtualKey::VK_KP_8, pressed);
            return true;
        }
        case 0x49: {
            JPAD(fabgl::VirtualKey::VK_DPAD_RIGHT, pressed);
            JPAD(fabgl::VirtualKey::VK_DPAD_UP, pressed);
            kbdPushData(fabgl::VirtualKey::VK_KP_9, pressed);
            return true;
        }
    }
    #endif
    return true;
}
#endif

#if USE_NESPAD

static void nespad_tick(void) {
    nespad_read();
    if ((nespad_state & DPAD_A) && !gamepad1_bits.a) add_key(K_JOY1, 1);
    if (!(nespad_state & DPAD_A) && gamepad1_bits.a) add_key(K_JOY1, 0);
    gamepad1_bits.a = (nespad_state & DPAD_A) != 0;

    if ((nespad_state & DPAD_B) && !gamepad1_bits.b) add_key(K_JOY2, 1);
    if (!(nespad_state & DPAD_B) && gamepad1_bits.b) add_key(K_JOY2, 0);
    gamepad1_bits.b = (nespad_state & DPAD_B) != 0;

    if ((nespad_state & DPAD_START) && !gamepad1_bits.start) add_key(K_JOY3, 1);
    if (!(nespad_state & DPAD_START) && gamepad1_bits.start) add_key(K_JOY3, 0);
    gamepad1_bits.start = (nespad_state & DPAD_START) != 0;

    if ((nespad_state & DPAD_SELECT) && !gamepad1_bits.select) add_key(K_JOY4, 1);
    if (!(nespad_state & DPAD_SELECT) && gamepad1_bits.select) add_key(K_JOY4, 0);
    gamepad1_bits.select = (nespad_state & DPAD_SELECT) != 0;
// TODO:
    gamepad1_bits.up = (nespad_state & DPAD_UP) != 0;
    gamepad1_bits.down = (nespad_state & DPAD_DOWN) != 0;
    gamepad1_bits.left = (nespad_state & DPAD_LEFT) != 0;
    gamepad1_bits.right = (nespad_state & DPAD_RIGHT) != 0;
// TODO:
    gamepad2_bits.a = (nespad_state2 & DPAD_A) != 0;
    gamepad2_bits.b = (nespad_state2 & DPAD_B) != 0;
    gamepad2_bits.select = (nespad_state2 & DPAD_SELECT) != 0;
    gamepad2_bits.start = (nespad_state2 & DPAD_START) != 0;
    gamepad2_bits.up = (nespad_state2 & DPAD_UP) != 0;
    gamepad2_bits.down = (nespad_state2 & DPAD_DOWN) != 0;
    gamepad2_bits.left = (nespad_state2 & DPAD_LEFT) != 0;
    gamepad2_bits.right = (nespad_state2 & DPAD_RIGHT) != 0;
}

#endif

#ifdef KBDUSB
inline static bool isInReport(hid_keyboard_report_t const *report, const unsigned char keycode) {
    for (unsigned char i: report->keycode) {
        if (i == keycode) {
            return true;
        }
    }
    return false;
}

int map_kc(uint8_t kc) {
    if (kc >= HID_KEY_A && kc <= HID_KEY_Z) {
        return 'a' + (kc - HID_KEY_A);
    }
    if (kc >= HID_KEY_1 && kc <= HID_KEY_9) {  // HID_KEY_0 handled in a switch
        return '1' + (kc - HID_KEY_1);
    }
    
    switch(kc) {
        case HID_KEY_0              : return '0';
        case HID_KEY_MINUS          : return '-';
        case HID_KEY_EQUAL          : return '=';
        case HID_KEY_BRACKET_LEFT   : return '[';
        case HID_KEY_BRACKET_RIGHT  : return ']';
        case HID_KEY_BACKSLASH      : return '\\';
        case HID_KEY_SEMICOLON      : return ';';
        case HID_KEY_APOSTROPHE     : return '\'';
        case HID_KEY_GRAVE          : return '`';
        case HID_KEY_COMMA          : return ',';
        case HID_KEY_PERIOD         : return '.';
        case HID_KEY_SLASH          : return '/';

        case HID_KEY_TAB: return K_TAB;
        case HID_KEY_ENTER:
        case HID_KEY_KEYPAD_ENTER:
            return K_ENTER;
        case HID_KEY_ESCAPE:        return K_ESCAPE;
        case HID_KEY_SPACE:         return K_SPACE;
        case HID_KEY_BACKSPACE:     return K_BACKSPACE;
        case HID_KEY_ARROW_UP:      return K_UPARROW;
        case HID_KEY_ARROW_DOWN:    return K_DOWNARROW;
        case HID_KEY_ARROW_LEFT:    return K_LEFTARROW;
        case HID_KEY_ARROW_RIGHT:   return K_RIGHTARROW;

        case HID_KEY_F1: return K_F1;
        case HID_KEY_F2: return K_F2;
        case HID_KEY_F3: return K_F3;
        case HID_KEY_F4: return K_F4;
        case HID_KEY_F5: return K_F5;
        case HID_KEY_F6: return K_F6;
        case HID_KEY_F7: return K_F7;
        case HID_KEY_F8: return K_F8;
        case HID_KEY_F9: return K_F9;
        case HID_KEY_F10: return K_F10;
        case HID_KEY_F11: return K_F11;
        case HID_KEY_F12: return K_F12;

        case HID_KEY_INSERT: return K_INS;
        case HID_KEY_DELETE: return K_DEL;
        case HID_KEY_PAGE_DOWN: return K_PGDN;
        case HID_KEY_PAGE_UP: return K_PGUP;
        case HID_KEY_HOME: return K_HOME;
        case HID_KEY_END: return K_END;

        case HID_KEY_PAUSE: return K_PAUSE;
    }
    return 0;
}

void process_kbd_report(
    hid_keyboard_report_t const *report,
    hid_keyboard_report_t const *prev_report
) {
    uint8_t up_mods = prev_report->modifier & ~(report->modifier);
    if (up_mods & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)) add_key(K_ALT, 0);
    if (up_mods & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) add_key(K_CTRL, 0);
    if (up_mods & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) add_key(K_SHIFT, 0);
    uint8_t new_mods = report->modifier & ~(prev_report->modifier);
    if (new_mods & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)) add_key(K_ALT, 1);
    if (new_mods & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) add_key(K_CTRL, 1);
    if (new_mods & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) add_key(K_SHIFT, 1);

    for (unsigned char kc: prev_report->keycode) {
        if ( !isInReport(report, kc) ) {
            int ikc = map_kc(kc);
            if (ikc) add_key(ikc, 0);
        }
    }
    for (unsigned char kc: report->keycode) {
        if ( !isInReport(prev_report, kc) ) {
            int ikc = map_kc(kc);
            if (ikc) add_key(ikc, 1);
        }
    }
}

static hid_mouse_report_t prev_report = { 0 };
static int cumulative_dx = 0, cumulative_dy = 0;
void __not_in_flash_func(process_mouse_report)(hid_mouse_report_t const * report) {
    uint8_t btns_a = prev_report.buttons & ~(report->buttons);
    if (btns_a & MOUSE_BUTTON_LEFT) add_key(K_MOUSE1, 0);
    if (btns_a & MOUSE_BUTTON_RIGHT) add_key(K_MOUSE2, 0);
    if (btns_a & MOUSE_BUTTON_MIDDLE) add_key(K_MOUSE3, 0);

    btns_a = ~prev_report.buttons & report->buttons;
    if (btns_a & MOUSE_BUTTON_LEFT) add_key(K_MOUSE1, 1);
    if (btns_a & MOUSE_BUTTON_RIGHT) add_key(K_MOUSE2, 1);
    if (btns_a & MOUSE_BUTTON_MIDDLE) add_key(K_MOUSE3, 1);

    prev_report = *report;
    cumulative_dx += report->x;
    cumulative_dy += report->y;
}

Ps2Kbd_Mrmltr ps2kbd(
        pio1,
        KBD_CLOCK_PIN,
        process_kbd_report
);
#endif

void repeat_me_for_input() {
    static uint32_t tickKbdRep1 = time_us_32();
    // 60 FPS loop
#define frame_tick (16666)
    static uint64_t tick = time_us_64();
    static uint64_t last_input_tick = tick;
        if (tick >= last_input_tick + frame_tick) {
#ifdef KBDUSB
            ps2kbd.tick();
#endif
#ifdef USE_NESPAD
            nespad_tick();
#endif
            last_input_tick = tick;
        }
        tick = time_us_64();
#ifdef KBDUSB
        tuh_task();
#endif
}

extern "C" bool SELECT_VGA;

//uint8_t* FRAME_BUF = (uint8_t*)0x20000000; // temp "trash" value
extern "C" uint8_t __aligned(4) FRAME_BUF[QUAKEGENERIC_RES_X * QUAKEGENERIC_RES_Y] = { 0 };

#if DVI_HSTX
enum {
    HSTX_OUT_PIN_LAYOUT_MURMULATOR2,
    HSTX_OUT_PIN_LAYOUT_PICODVISOCK,
};

static union dvi_hstx_pin_layout_t hstx_out_pin_layouts[] = {
    {   // Murmulator 2
    .clock_n = 0, .clock_p = 1,
    .lane0_n = 2, .lane0_p = 3,
    .lane1_n = 4, .lane1_p = 5,
    .lane2_n = 6, .lane2_p = 7,
    },
    {   // Pico-DVI-Sock
    .clock_n = 15-12, .clock_p = 14-12,
    .lane0_n = 13-12, .lane0_p = 12-12,
    .lane1_n = 19-12, .lane1_p = 18-12,
    .lane2_n = 17-12, .lane2_p = 16-12,
    }
};

// palette used by the DVI/VGA HSTX driver
static uint32_t linebuf_pal[256] = { 0 };

struct linebuf_cb_info_8bpp_t {
    const uint8_t  *pic;
    const uint32_t *pal;
};
static linebuf_cb_info_8bpp_t cb_index8_priv = {.pic = FRAME_BUF, .pal = linebuf_pal};

// line buffer callback used for converting the picture
extern "C" void linebuf_cb_index8_a(const struct dvi_linebuf_task_t *task, void *priv);

void __noinline hstx_init() {
    // init hstx driver here!
    bool is_vga    = SELECT_VGA;
    union dvi_hstx_pin_layout_t pin_cfg = hstx_out_pin_layouts[HSTX_OUT_PIN_LAYOUT_MURMULATOR2];
    int mode       = DVI_MODE_320x240;
    int hstx_div   = is_vga ? 1 : clock_get_hz(clk_sys)/(dvi_modes[mode].timings.pixelclock*5);
    int phase_rept = (clock_get_hz(clk_sys) * dvi_modes[mode].pixel_rep) / dvi_modes[mode].timings.pixelclock;
    if (is_vga) while (phase_rept >= 32) {
        hstx_div <<= 1; phase_rept >>= 1;
    }

    // configure HSTX clock divider
    clock_configure_int_divider(
        clk_hstx,
        CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
        0,
        clock_get_hz(clk_sys),
        hstx_div
    );

    // configure timings
    struct dvi_timings_t dvi_timings;
    memcpy(&dvi_timings, &dvi_modes[mode].timings, sizeof(struct dvi_timings_t));

    // adjust timings
    dvi_adjust_timings(&dvi_timings, DVI_HSTX_MODE_XRGB8888, dvi_modes[mode].pixel_rep, is_vga ? HSTX_TIMINGS_VGA_FIXUP : 0);

    // enable pins
    dvi_configure_hstx_command_expander(DVI_HSTX_MODE_XRGB8888, dvi_modes[mode].pixel_rep);
    if (is_vga) {
        vga_configure_hstx_output(pin_cfg, GPIO_SLEW_RATE_FAST, GPIO_DRIVE_STRENGTH_4MA, phase_rept);
    } else {
        dvi_configure_hstx_output(pin_cfg, GPIO_SLEW_RATE_FAST, GPIO_DRIVE_STRENGTH_4MA);
    }

    // allocate memory for the linebuf
    uint32_t linebuf_memsize;
    dvi_linebuf_get_memsize(&dvi_timings, &linebuf_memsize, is_vga ? 1 : dvi_modes[mode].pixel_rep);
    uint32_t *linebuf = (uint32_t*)malloc(linebuf_memsize);

    // allocate DMA channels
    struct dvi_resources_t dvires;
    for (int i = 0; i < 3; i++) dvires.dma_channels[i] = dma_claim_unused_channel(true);
    dvires.irq_dma = DMA_IRQ_0;
    dvires.irq_linebuf_callback = user_irq_claim_unused(true);

    // set timings
    dvi_linebuf_set_timings(&dvi_timings, is_vga ? 1 : dvi_modes[mode].pixel_rep);
    dvi_linebuf_set_line_rep(dvi_modes[mode].line_rep);

    // set resources
    dvi_linebuf_set_resources(&dvires, linebuf);

    // fill HSTX command list
    if (is_vga) {
        vga_linebuf_fill_hstx_cmdlist();
    } else {
        dvi_linebuf_fill_hstx_cmdlist();
    }

    // initialize DMA channels and IRQ handlers
    dvi_linebuf_init_dma();

    // set callback
    dvi_linebuf_set_cb(linebuf_cb_index8_a, &cb_index8_priv);

    // and start display output
    dvi_linebuf_start();
}

#endif

void __scratch_x("render") render_core() {
    // init graphics driver
    multicore_lockout_victim_init();
#if DVI_HSTX
    hstx_init();
    if (SELECT_VGA) {
        linebuf_pal[0x00] = 0x000000;
        linebuf_pal[0x0F] = 0xEBEBEB;
    } else {
        linebuf_pal[0x00] = vga_pwm_xlat_color32(0x000000);
        linebuf_pal[0x0F] = vga_pwm_xlat_color32(0xEBEBEB);
    }
#else
    multicore_lockout_victim_init();
    graphics_init();
    graphics_set_buffer(FRAME_BUF, QUAKEGENERIC_RES_X, QUAKEGENERIC_RES_Y);
    graphics_set_bgcolor(0x000000);
    graphics_set_palette(0,    0x000000);
    graphics_set_palette(0x0F, 0xEBEBEB);   // white in quake palette
#endif
    sem_acquire_blocking(&vga_start_semaphore);
    
    mixer_init();
    uint64_t tick = time_us_64();
    uint64_t last_cd_tick = 0;
    int16_t samples[2];
    while (true) {
        // Sound Blaster sampling
        if (tick > last_cd_tick + (1000000 / 44100)) {
            last_cd_tick = tick;
            if (!CDAudio_GetSamples(samples, 1)) {
                samples[0] = 0;
                samples[1] = 0;
            }
            mixer_samples(samples, 1);
        }
#if TFT
        refresh_lcd();
#endif
        tick = time_us_64();
    }
    __unreachable();
}

#if SOFTTV
typedef struct tv_out_mode_t {
    // double color_freq;
    float color_index;
    COLOR_FREQ_t c_freq;
    enum graphics_mode_t mode_bpp;
    g_out_TV_t tv_system;
    NUM_TV_LINES_t N_lines;
    bool cb_sync_PI_shift_lines;
    bool cb_sync_PI_shift_half_frame;
} tv_out_mode_t;
extern tv_out_mode_t tv_out_mode;

bool color_mode=true;
bool toggle_color() {
    color_mode=!color_mode;
    if(color_mode) {
        tv_out_mode.color_index= 1.0f;
    } else {
        tv_out_mode.color_index= 0.0f;
    }

    return true;
}
#endif

uint8_t psram_pin;
#if PICO_RP2350
#include <hardware/exception.h>
#include <hardware/structs/qmi.h>
#include <hardware/structs/xip.h>
#include <hardware/regs/sysinfo.h>

#ifdef BUTTER_PSRAM_GPIO
#define MB16 (16ul << 20)
#define MB8 (8ul << 20)
#define MB4 (4ul << 20)
#define MB1 (1ul << 20)
uint8_t* PSRAM_DATA = (uint8_t*)0x11000000;
static int BUTTER_PSRAM_SIZE = -1;
uint32_t __not_in_flash_func(butter_psram_size)() {
    if (BUTTER_PSRAM_SIZE != -1) return BUTTER_PSRAM_SIZE;
    for(register int i = MB8; i < MB16; i += 4096)
        PSRAM_DATA[i] = 16;
    for(register int i = MB4; i < MB8; i += 4096)
        PSRAM_DATA[i] = 8;
    for(register int i = MB1; i < MB4; i += 4096)
        PSRAM_DATA[i] = 4;
    for(register int i = 0; i < MB1; i += 4096)
        PSRAM_DATA[i] = 1;
    register uint32_t res = PSRAM_DATA[MB16 - 4096];
    for (register int i = MB16 - MB1; i < MB16; i += 4096) {
        if (res != PSRAM_DATA[i])
            return 0;
    }
    BUTTER_PSRAM_SIZE = res << 20;
    return BUTTER_PSRAM_SIZE;
}
void __no_inline_not_in_flash_func(psram_init)(uint cs_pin) {
    gpio_set_function(cs_pin, GPIO_FUNC_XIP_CS1);

    // Enable direct mode, PSRAM CS, clkdiv of 10
    qmi_hw->direct_csr = 10 << QMI_DIRECT_CSR_CLKDIV_LSB |
                         QMI_DIRECT_CSR_EN_BITS |
                         QMI_DIRECT_CSR_AUTO_CS1N_BITS;

    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
        tight_loop_contents();
    }

    // Enable QPI mode on the PSRAM
    const uint CMD_QPI_EN = 0x35;
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | CMD_QPI_EN;

    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
        tight_loop_contents();
    }

    // Set PSRAM timing
    const int max_psram_freq = MAX_PSRAM_FREQ_MHZ * MHZ;
    const int clock_hz = clock_get_hz(clk_sys);
    int divisor = (clock_hz + max_psram_freq - 1) / max_psram_freq;

    if (divisor == 1 && clock_hz > 100000000) {
        divisor = 2;
    }

    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000) {
        rxdelay += 1;
    }

    // Calculate timing parameters
    const int clock_period_fs = 1000000000000000ll / clock_hz;
    const int max_select = (125 * 1000000) / clock_period_fs;  // 125 = 8000ns / 64
    const int min_deselect = (18 * 1000000 + (clock_period_fs - 1)) / clock_period_fs - (divisor + 1) / 2;

    qmi_hw->m[1].timing = 1 << QMI_M1_TIMING_COOLDOWN_LSB |
                          QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
                          max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
                          min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
                          rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
                          divisor << QMI_M1_TIMING_CLKDIV_LSB;

    // Set PSRAM read format
    qmi_hw->m[1].rfmt = QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB |
                        QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB |
                        QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB |
                        QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB |
                        QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB |
                        QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB |
                        6 << QMI_M0_RFMT_DUMMY_LEN_LSB;

    qmi_hw->m[1].rcmd = 0xEB;

    // Set PSRAM write format
    qmi_hw->m[1].wfmt = QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |
                        QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB |
                        QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |
                        QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB |
                        QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB |
                        QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB;

    qmi_hw->m[1].wcmd = 0x38;

    // Disable direct mode
    qmi_hw->direct_csr = 0;

    // Enable writes to PSRAM
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
    // detect a chip size
    butter_psram_size();
}
#else
uint8_t* PSRAM_DATA = (uint8_t*)0;
uint32_t __not_in_flash_func(butter_psram_size)() { return 0; }
#endif
#else
uint8_t* PSRAM_DATA = (uint8_t*)0;
uint32_t __not_in_flash_func(butter_psram_size)() { return 0; }
#endif

#define STACK_CORE0 0x11800000

void sigbus(void) {
    quietlog = 0;
    Sys_Printf("SIGBUS exception caught... SP %ph (%d)\n", get_sp(), STACK_CORE0 - get_sp());
    #if PICO_DEFAULT_LED_PIN
    while(1) {
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }
    #endif
    /// TODO: reset_usb_boot(0, 0);
}
void __attribute__((naked, noreturn)) __printflike(1, 0) dummy_panic(__unused const char *fmt, ...) {
    quietlog = 0;
    Sys_Printf("*** PANIC ***\n");
    Sys_Printf("SP %ph (%s)\n", get_sp(), STACK_CORE0 - get_sp());
    if (fmt)
        Sys_Printf((char*)fmt);
    #if PICO_DEFAULT_LED_PIN
    while(1) {
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }
    #endif
}

static void __not_in_flash_func(flash_info)() {
    if (rx[0] == 0) {
        uint8_t tx[4] = {0x9f};
        flash_do_cmd(tx, rx, 4);
    }
}

extern "C" void __time_critical_func() vsync_handler() {

}

extern "C" uint8_t* get_line_buffer(int line) {
    return FRAME_BUF + QUAKEGENERIC_RES_X * line;
}

extern "C" int __time_critical_func() get_video_mode() {
    return 0;
}

extern "C" void QG_Init(void) {

}

extern "C" int QG_GetKey(int *down, int *key) {
#ifdef KBDUSB
    repeat_me_for_input();
#endif
    if (!next_key_action) return 0;
    const key_action_t& k = key_actions[--next_key_action];
    *key = k.key;
    *down = k.down;
    return 1;
}

extern "C" void QG_GetMouseMove(int *x, int *y) {
#ifdef KBDUSB
    repeat_me_for_input();
#endif
    *x = cumulative_dx;
    *y = cumulative_dy;
    cumulative_dx = 0;
    cumulative_dy = 0;
}

extern "C" void QG_Quit(void) {
	Sys_Printf ("QG_Quit\n");
}

// -----------------------------------------
// QG_DrawFrame()/QG_SetPalette() interfaces

extern "C" void QG_DrawFrame(void *pixels) {
#ifdef KBDUSB
    repeat_me_for_input();
#endif
    memcpy(FRAME_BUF, pixels, QUAKEGENERIC_RES_X * QUAKEGENERIC_RES_Y);
}

#if DVI_HSTX

extern "C" void QG_SetPalette(unsigned char palette[768]) {
    uint8_t *p = palette;
    if (SELECT_VGA) {
        for (int i = 0; i < 256; i++) {
            linebuf_pal[i] = vga_pwm_xlat_color(p[0], p[1], p[2]); p += 3;
        }
    } else {
        for (int i = 0; i < 256; i++) {
            linebuf_pal[i] = (p[2] << 0) | (p[1] << 8) | (p[0] << 16); p += 3;
        }
    }
}

#else

extern "C" void QG_SetPalette(unsigned char palette[768]) {
	for (int i = 0; i < 256; i++) {
        int i3 = i * 3;
        uint32_t pal888 = 
		    ((uint32_t)palette[i3] << 16) | // R
		    ((uint32_t)palette[i3 + 1] << 8) | // G
		    palette[i3 + 2]; // B
        graphics_set_palette(i, pal888);
	}
}

#endif
// -----------------------

extern "C" void QG_GetJoyAxes(float *axes)
{
    *axes = 0;
}

static int argc = 1;
static char** argv = nullptr;
static char arg0[] = "quake";
static char buf[256];     // Статический, чтобы указатели в argv были валидны

static void create_argv() {
    FIL* f = new FIL();
    if (f_open(f, "/quake/argv.conf", FA_READ) == FR_OK) {
        UINT br = 0;
        if (f_read(f, buf, sizeof(buf) - 1, &br) == FR_OK && br > 0) {
            // Гарантировать 0-терминацию
            buf[br] = 0;
            // === Подсчёт токенов ===
            int count = 1; // Включая argv[0]
            bool in_token = false;
            for (UINT i = 0; i < br; ++i) {
                char c = buf[i];
                bool sep = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
                if (!sep) {
                    if (!in_token)
                        ++count;
                    in_token = true;
                } else {
                    buf[i] = 0;   // нормализуем
                    in_token = false;
                }
            }
            // === Выделение argv ===
            argv = new char*[count + 1];
            int idx = 0;
            // argv[0] = "quake"
            argv[idx++] = arg0;
            // === Заполнение argv ===
            char* p = buf;
            while (idx < count && p < buf + br) {
                if (*p != 0) {
                    argv[idx++] = p;
                    // перейти к концу токена
                    while (*p) ++p;
                }
                ++p; // пропуск нулевого разделителя
            }
            // NULL-терминатор
            argv[idx] = nullptr;
            argc = count;
        } else {
            f_close(f);
            goto hw;
        }
        f_close(f);
    } else {
        hw:
        // файл отсутствует → использовать только arg0
        argv = new char*[2];
        argv[0] = arg0;
        argv[1] = nullptr;
        argc = 1;
    }
    delete f;
}

__attribute__((noreturn))
static void finish_him(void) {
    uint32_t sp_after;
    __asm volatile("mov %0, sp" : "=r"(sp_after));

    f_unlink("quake.log");

    uint8_t link_i2s_code = testPins(I2S_DATA_PIO, I2S_BCK_PIO);
    is_i2s_enabled = override_audio != 0xFF ? override_audio : link_i2s_code != 0;

    uint8_t linkVGA01;
    linkVGA01 = testPins(VGA_BASE_PIN, VGA_BASE_PIN + 1);
    if (override_video != 0xFF) {
        SELECT_VGA = override_video;
    } else {
    #if defined(ZERO) || defined(ZERO2) || defined(PICO_DV)
        SELECT_VGA = linkVGA01 == 0x1F;
    #else
        SELECT_VGA = (linkVGA01 == 0) || (linkVGA01 == 0x1F);
    #endif
    }

    Sys_Printf(" Hardware info\n");
    Sys_Printf(" --------------------------------------\n");
    uint32_t cpu_hz = clock_get_hz(clk_sys);
    Sys_Printf(" Chip model     : RP2350%c %d MHz\n", (rp2350a ? 'A' : 'B'), cpu_hz / 1000000);
    Sys_Printf(" VREG           : %d\n", vreg);
    Sys_Printf(" Flash size     : %d MB\n", (1 << rx[3]) >> 20);
    Sys_Printf(" Flash JEDEC ID : %02X-%02X-%02X-%02X\n", rx[0], rx[1], rx[2], rx[3]);
    if (new_flash_timings == qmi_hw->m[0].timing) {
        Sys_Printf(" Flash timings  : %p\n", new_flash_timings);
    } else {
        Sys_Printf(" Flash max freq.: %d MHz [T%p]\n", flash_mhz, qmi_hw->m[0].timing);
    }
    Sys_Printf(" PSRAM on GP%02d  : %d MB QSPI\n", psram_pin, butter_psram_size() >> 20);
    if (new_psram_timings == qmi_hw->m[1].timing) {
        Sys_Printf(" PSRAM timings  :[T%p]\n", new_psram_timings);
    } else {
        Sys_Printf(" PSRAM max freq.: %d MHz [T%p]\n", psram_mhz, qmi_hw->m[1].timing);
    }
    Sys_Printf(" Sound          : %s [%02x] %s\n", is_i2s_enabled ? "i2s" : "PWM", link_i2s_code, override_audio == 0xFF ? "" : "overriden");
    Sys_Printf(" Video          : %s [%02x] %s\n", SELECT_VGA ? "VGA" : "HDMI", linkVGA01, override_video == 0xFF ? "" : "overriden");
    Sys_Printf(" SP after switch: 0x%08X\n", sp_after);
    Sys_Printf(" .psram_data size:  0x%08X\n", (&__psram_data_end__ - &__psram_data_start__));
    Sys_Printf(" .psram_bss  size:  0x%08X\n", (&__psram_bss_end__ - &__psram_bss_start__));
    Sys_Printf(" .psram_heap start: 0x%08X\n", (&__psram_heap_start__));
    Sys_Printf(" --------------------------------------\n");

    if (new_cpu_mhz != cpu_mhz) {
        Sys_Printf("WARN: Failed to overclock to %d MHz (from conf)\n", new_cpu_mhz);
    }
    if (new_vreg < VREG_VOLTAGE_0_55 || new_vreg > VREG_VOLTAGE_3_30) {
        Sys_Printf("WARN: Unexpected VREG value: %d (from cong)\n", new_vreg);
    }

#if USE_NESPAD
    nespad_begin(clock_get_hz(clk_sys) / 1000, NES_GPIO_CLK, NES_GPIO_DATA, NES_GPIO_LAT);
#endif

/*
#if PICO_RP2350
    if (psram_pin != PSRAM_PIN_SCK)
#endif
    #ifndef MURM2
        init_psram();
    #endif
*/
    // send kbd reset only after initial process passed
#ifndef KBDUSB
    keyboard_send(0xFF);
#endif
    sem_init(&vga_start_semaphore, 0, 1);
    multicore_launch_core1(render_core);
    sem_release(&vga_start_semaphore);

    xipstream_init();

    create_argv();
	QG_Create(argc, argv);
	Sys_Printf ("QG_Create done\n");

    // main loop
    uint64_t t, old_t = time_us_64();
    while (true) {
        t = time_us_64();
#if FIXED_TIME_STEP
        QG_Tick(1.0 / 60.0);                // simulate 60fps tick rate
#else
        QG_Tick((t - old_t) / 1000000.0);
#endif
        old_t = t;
    }
    __unreachable();
}

__attribute__((naked))
void switch_stack(uint32_t new_sp, void (*entry)(void))
{
    __asm volatile(
        "msr msp, r0      \n"   // установить новый MSP = new_sp
        "bx r1            \n"   // перейти в entry(), уже на новом стеке
    );
}

static char* open_config(UINT* pbr) {
    FILINFO fileinfo;
    size_t file_size = 0;
    const char cfn[] = "/quake/quake.conf";
    if (f_stat(cfn, &fileinfo) != FR_OK || (fileinfo.fattrib & AM_DIR)) {
        return 0;
    } else {
        file_size = (size_t)fileinfo.fsize & 0xFFFFFFFF;
    }

    FIL f;
    if(f_open(&f, cfn, FA_READ) != FR_OK) {
        return 0;
    }
    char* buff = (char*)malloc(file_size + 1);
    if (f_read(&f, buff, file_size, pbr) != FR_OK) {
        ///printf("Failed to read /quake/quake.conf\n");
        free(buff);
        buff = 0;
    }
    f_close(&f);
    return buff;
}

inline static void tokenizeCfg(char* s, size_t sz) {
    size_t i = 0;
    for (; i < sz; ++i) {
        if (s[i] == '=' || s[i] == '\n' || s[i] == '\r') {
            s[i] = 0;
        }
    }
    s[i] = 0;
}

static char* next_token(char* t) {
    char *t1 = t + strlen(t);
    while(!*t1++);
    return t1 - 1;
}

void __not_in_flash() flash_timings() {
    if (!new_flash_timings) {
        const int max_flash_freq = flash_mhz * MHZ;
        const int clock_hz = cpu_mhz * MHZ;
        int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
        if (divisor == 1 && clock_hz >= 166000000) {
            divisor = 2;
        }
        int rxdelay = divisor;
        if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) {
            rxdelay += 1;
        }
        qmi_hw->m[0].timing = 0x60007000 |
                            rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                            divisor << QMI_M0_TIMING_CLKDIV_LSB;
    } else {
        qmi_hw->m[0].timing = new_flash_timings;
    }
}

void __not_in_flash() psram_timings() {
    if (!new_psram_timings) {
        const int max_psram_freq = psram_mhz * MHZ;
        const int clock_hz = cpu_mhz * MHZ;
        int divisor = (clock_hz + max_psram_freq - (max_psram_freq >> 4) - 1) / max_psram_freq;
        if (divisor == 1 && clock_hz >= 166000000) {
            divisor = 2;
        }
        int rxdelay = divisor;
        if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) {
            rxdelay += 1;
        }
        qmi_hw->m[1].timing = (qmi_hw->m[1].timing & ~0x000000FFF) |
                            rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                            divisor << QMI_M0_TIMING_CLKDIV_LSB;
    } else {
        qmi_hw->m[1].timing = new_psram_timings;
    }
}

static void load_config() {
    UINT br;
    char* buff = open_config(&br);
    if (buff) {
        tokenizeCfg(buff, br);
        char *t = buff;
        while (t - buff < br) {
            if (strcmp(t, "CPU") == 0) {
                t = next_token(t);
                new_cpu_mhz = atoi(t);
                if (clock_get_hz(clk_sys) != new_cpu_mhz * MHZ) {
                    if (set_sys_clock_hz(new_cpu_mhz * MHZ, 0) ) {
                        cpu_mhz = new_cpu_mhz;
                    }
                }
            } else if (strcmp(t, "VIDEO") == 0) {
                t = next_token(t);
                if (strcmp("HDMI", t) == 0) {
                    override_video = 0;
                } else if (strcmp("VGA", t) == 0) {
                    override_video = 1;
                }
            } else if (strcmp(t, "AUDIO") == 0) {
                t = next_token(t);
                if (strcmp("i2s", t) == 0) {
                    override_audio = 1;
                } else if (strcmp("PWM", t) == 0) {
                    override_audio = 0;
                }
            } else if (strcmp(t, "VREG") == 0) {
                t = next_token(t);
                new_vreg = atoi(t);
                if (new_vreg != vreg && new_vreg >= VREG_VOLTAGE_0_55 && new_vreg <= VREG_VOLTAGE_3_30) {
                    vreg = new_vreg;
                    vreg_set_voltage((vreg_voltage)vreg);
                }
            } else if (!new_flash_timings && strcmp(t, "FLASH") == 0) {
                t = next_token(t);
                int new_flash_mhz = atoi(t);
                if (flash_mhz != new_flash_mhz) {
                    flash_mhz = new_flash_mhz;
                    flash_timings();
                }
            } else if (strcmp(t, "FLASH_T") == 0) {
                t = next_token(t);
                char *endptr;
                new_flash_timings = (uint)strtol(t, &endptr, 16);
                if (*endptr == 0 && qmi_hw->m[0].timing != new_flash_timings) {
                    flash_timings();
                }
            } else if (!new_psram_timings && strcmp(t, "PSRAM") == 0) {
                t = next_token(t);
                int new_psram_mhz = atoi(t);
                if (psram_mhz != new_psram_mhz) {
                    psram_mhz = new_psram_mhz;
                    psram_timings();
                }
            } else if (strcmp(t, "PSRAM_T") == 0) {
                t = next_token(t);
                char *endptr;
                new_psram_timings = (uint)strtol(t, &endptr, 16);
                if (*endptr == 0 && qmi_hw->m[1].timing != new_psram_timings) {
                    psram_timings();
                }
            } else { // unknown token
                t = next_token(t);
            }
            t = next_token(t);
        }
        free(buff);
    }
}

int main() {
    flash_info();
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_60);
    flash_timings();
    sleep_ms(100);

    if (!set_sys_clock_khz(CPU_MHZ * KHZ, 0)) {
        #undef CPU_MHZ
        #define CPU_MHZ 252
        set_sys_clock_khz(CPU_MHZ * KHZ, 1); // fallback to failsafe clocks
    }

#ifdef KBDUSB
    tuh_init(BOARD_TUH_RHPORT);
    ps2kbd.init_gpio();
#else
    keyboard_init();
#endif

#if PICO_RP2350
    rp2350a = (*((io_ro_32*)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET)) & 1);
    #ifdef BUTTER_PSRAM_GPIO
        psram_pin = rp2350a ? BUTTER_PSRAM_GPIO : 47;
        psram_init(psram_pin);
        if(!butter_psram_size()) {
            Sys_Error("No QSPI PSRAM detected\n");
        }
    #endif
    exception_set_exclusive_handler(HARDFAULT_EXCEPTION, sigbus);
#endif

    psram_sections_init();      // init psram_data/psram_bss sections 

    #ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    for (int i = 0; i < 6; i++) {
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }
    #endif

    f_mount(&fs, "", 1);
    load_config();

    switch_stack(STACK_CORE0, finish_him);
    __unreachable();
}
