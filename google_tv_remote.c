// SPDX-License-Identifier: GPL-3.0-only
// Google TV remote for Flipper Zero.
// Structure adapted from flipper-apple-tv-remote by Hanns Kronenberg (GPL-3.0-only):
// https://github.com/KronenbergBN/flipper-apple-tv-remote

#include <furi.h>
#include <furi_hal_bt.h>
#include <furi_hal_usb_hid.h>
#include <bt/bt_service/bt.h>
#include <extra_profiles/hid_profile.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <infrared_transmit.h>
#include <input/input.h>
#include <storage/storage.h>

#define TAG "GoogleTV"

/* Same HID identity and bond store as the official Bluetooth Remote app, so an
 * existing pairing is reused. No keys are exported, erased or printed. */
#define REMOTE_BOND_DIR  EXT_PATH("apps_data/hid_ble")
#define REMOTE_BOND_PATH REMOTE_BOND_DIR "/.bt_hid.keys"

/* Epson projector power, NECext address 83 55 / command 90 6F, taken from the
 * official firmware's projector.ir library. Epson asks for a second press to
 * confirm power off, exactly like its own remote. */
#define EPSON_IR_ADDRESS 0x5583
#define EPSON_IR_COMMAND 0x6F90

#define PULSE_MS       60
#define POWER_PULSE_MS 600
#define HIGHLIGHT_MS   160
#define MENU_ROWS      4

typedef enum {
    ActionHome,
    ActionVolumeUp,
    ActionVolumeDown,
    ActionMute,
    ActionPlayPause,
    ActionSearch,
    ActionProjectorPower,
    ActionTvPower,
    ActionClose,
    ActionCount,
} Action;

static const char* const action_labels[ActionCount] = {
    "Home",
    "Volume +",
    "Volume -",
    "Mute",
    "Play / Pause",
    "Search / Assistant",
    "Projector power (IR)",
    "Google TV power",
    "Back to remote",
};

typedef struct {
    bool connected;
    bool failed;
    bool highlight;
    bool menu;
    uint8_t action;
    InputKey last_key;
    const char* toast;
} RemoteScreen;

typedef struct {
    Gui* gui;
    Bt* bt;
    ViewPort* view_port;
    FuriMessageQueue* input;
    FuriMutex* lock;
    FuriHalBleProfileBase* profile;
    RemoteScreen screen;
    uint32_t highlight_until;
    uint32_t toast_until;
} Remote;

static void remote_arrow(Canvas* canvas, int cx, int cy, InputKey key) {
    int dx = 0, dy = 0;
    switch(key) {
    case InputKeyUp:
        dy = -1;
        break;
    case InputKeyDown:
        dy = 1;
        break;
    case InputKeyLeft:
        dx = -1;
        break;
    case InputKeyRight:
        dx = 1;
        break;
    default:
        canvas_draw_str_aligned(canvas, cx, cy, AlignCenter, AlignCenter, "OK");
        return;
    }
    /* Shaft from tail to tip, then two barbs back from the tip. */
    const int tx = cx + 3 * dx, ty = cy + 3 * dy;
    canvas_draw_line(canvas, cx - 3 * dx, cy - 3 * dy, tx, ty);
    canvas_draw_line(canvas, tx, ty, tx - 3 * dx - 3 * dy, ty - 3 * dy - 3 * dx);
    canvas_draw_line(canvas, tx, ty, tx - 3 * dx + 3 * dy, ty - 3 * dy + 3 * dx);
}

static void remote_button(Canvas* canvas, int x, int y, InputKey key, bool selected) {
    if(selected) {
        canvas_draw_rbox(canvas, x, y, 20, 12, 2);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_draw_rframe(canvas, x, y, 20, 12, 2);
    }
    remote_arrow(canvas, x + 10, y + 6, key);
    canvas_set_color(canvas, ColorBlack);
}

static void remote_draw(Canvas* canvas, void* context) {
    Remote* app = context;
    furi_mutex_acquire(app->lock, FuriWaitForever);
    RemoteScreen screen = app->screen;
    furi_mutex_release(app->lock);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Google TV");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas, 126, 10, AlignRight, AlignBottom, screen.connected ? "Connected" : "Waiting...");
    canvas_draw_line(canvas, 0, 13, 127, 13);

    if(screen.menu) {
        uint8_t first = 0;
        if(screen.action >= MENU_ROWS) first = screen.action - MENU_ROWS + 1;
        for(uint8_t row = 0; row < MENU_ROWS; row++) {
            const uint8_t i = first + row;
            const int y = 16 + row * 10;
            if(screen.action == i) {
                canvas_draw_box(canvas, 1, y - 1, 122, 10);
                canvas_set_color(canvas, ColorWhite);
            }
            canvas_draw_str(canvas, 5, y + 7, action_labels[i]);
            canvas_set_color(canvas, ColorBlack);
        }
        /* Scroll bar */
        const int bar = 40 * MENU_ROWS / ActionCount;
        const int top = 15 + (40 - bar) * first / (ActionCount - MENU_ROWS);
        canvas_draw_line(canvas, 126, 15, 126, 54);
        canvas_draw_box(canvas, 125, top, 3, bar);
    } else if(screen.failed) {
        canvas_draw_str(canvas, 2, 28, "Bluetooth unavailable");
        canvas_draw_str(canvas, 2, 40, "Please restart app");
    } else if(!screen.connected) {
        canvas_draw_str(canvas, 2, 25, "Google TV > Settings >");
        canvas_draw_str(canvas, 2, 35, "Remotes & Accessories >");
        canvas_draw_str(canvas, 2, 45, "Pair; pick Control+name");
        canvas_draw_str(canvas, 2, 55, "Hold OK: projector menu");
    } else {
        const bool hl = screen.highlight;
        remote_button(canvas, 23, 16, InputKeyUp, hl && screen.last_key == InputKeyUp);
        remote_button(canvas, 2, 29, InputKeyLeft, hl && screen.last_key == InputKeyLeft);
        remote_button(canvas, 23, 29, InputKeyOk, hl && screen.last_key == InputKeyOk);
        remote_button(canvas, 44, 29, InputKeyRight, hl && screen.last_key == InputKeyRight);
        remote_button(canvas, 23, 42, InputKeyDown, hl && screen.last_key == InputKeyDown);
        canvas_draw_str(canvas, 69, 24, "OK: Select");
        canvas_draw_str(canvas, 69, 34, "Back: Back");
        canvas_draw_str(canvas, 69, 45, "Hold OK:");
        canvas_draw_str(canvas, 69, 54, "Home, Vol...");
    }

    const char* footer = screen.toast ? screen.toast :
                         screen.menu  ? "</>: Vol  Back: close" :
                                        "Hold Back: Exit app";
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, footer);
}

static void remote_input(InputEvent* event, void* context) {
    Remote* app = context;
    /* Every outgoing command is a full press/release pulse, so dropping an
     * event when the queue is full can never leave a key held on the TV. */
    if(event->type != InputTypeRelease) furi_message_queue_put(app->input, event, 0);
}

static void remote_bt_status(BtStatus status, void* context) {
    Remote* app = context;
    furi_mutex_acquire(app->lock, FuriWaitForever);
    app->screen.connected = status == BtStatusConnected;
    app->screen.highlight = false;
    furi_mutex_release(app->lock);
    view_port_update(app->view_port);
}

static void remote_toast(Remote* app, const char* text, uint32_t ms) {
    furi_mutex_acquire(app->lock, FuriWaitForever);
    app->screen.toast = text;
    app->toast_until = furi_get_tick() + furi_ms_to_ticks(ms);
    furi_mutex_release(app->lock);
    view_port_update(app->view_port);
}

static bool remote_connected(Remote* app) {
    furi_mutex_acquire(app->lock, FuriWaitForever);
    bool connected = app->screen.connected;
    furi_mutex_release(app->lock);
    return connected && app->profile;
}

static bool remote_key(Remote* app, uint16_t code) {
    if(!remote_connected(app)) return false;
    bool sent = ble_profile_hid_kb_press(app->profile, code);
    furi_delay_ms(PULSE_MS);
    ble_profile_hid_kb_release_all(app->profile);
    return sent;
}

static bool remote_consumer(Remote* app, uint16_t code) {
    if(!remote_connected(app)) return false;
    bool sent = ble_profile_hid_consumer_key_press(app->profile, code);
    furi_delay_ms(code == HID_CONSUMER_POWER ? POWER_PULSE_MS : PULSE_MS);
    ble_profile_hid_consumer_key_release_all(app->profile);
    return sent;
}

static void remote_projector_power(Remote* app) {
    const InfraredMessage message = {
        .protocol = InfraredProtocolNECext,
        .address = EPSON_IR_ADDRESS,
        .command = EPSON_IR_COMMAND,
        .repeat = false,
    };
    /* One frame plus a repeat, like a short press on the Epson remote. */
    infrared_send(&message, 2);
    remote_toast(app, "IR sent. Again = confirm off", 2500);
}

static void remote_run_action(Remote* app, Action action) {
    bool sent = true;
    switch(action) {
    case ActionHome:
        sent = remote_consumer(app, HID_CONSUMER_AC_HOME);
        break;
    case ActionVolumeUp:
        sent = remote_consumer(app, HID_CONSUMER_VOLUME_INCREMENT);
        break;
    case ActionVolumeDown:
        sent = remote_consumer(app, HID_CONSUMER_VOLUME_DECREMENT);
        break;
    case ActionMute:
        sent = remote_consumer(app, HID_CONSUMER_MUTE);
        break;
    case ActionPlayPause:
        sent = remote_consumer(app, HID_CONSUMER_PLAY_PAUSE);
        break;
    case ActionSearch:
        sent = remote_consumer(app, HID_CONSUMER_AC_SEARCH);
        break;
    case ActionTvPower:
        sent = remote_consumer(app, HID_CONSUMER_POWER);
        break;
    case ActionProjectorPower:
        remote_projector_power(app);
        break;
    default:
        break;
    }
    if(!sent) remote_toast(app, "Google TV not connected", 1500);
}

/* Volume and mute keep the menu open so you can tap repeatedly. */
static bool action_keeps_menu(Action action) {
    return action == ActionVolumeUp || action == ActionVolumeDown || action == ActionMute ||
           action == ActionProjectorPower;
}

static void remote_set_menu(Remote* app, bool open) {
    furi_mutex_acquire(app->lock, FuriWaitForever);
    app->screen.menu = open;
    if(open) app->screen.action = 0;
    furi_mutex_release(app->lock);
    view_port_update(app->view_port);
}

static bool remote_handle_menu(Remote* app, const InputEvent* event, uint8_t action) {
    const bool pressish = event->type == InputTypePress || event->type == InputTypeRepeat;
    if((event->key == InputKeyUp || event->key == InputKeyDown) && pressish) {
        furi_mutex_acquire(app->lock, FuriWaitForever);
        app->screen.action =
            (action + (event->key == InputKeyDown ? 1 : ActionCount - 1)) % ActionCount;
        furi_mutex_release(app->lock);
        view_port_update(app->view_port);
    } else if((event->key == InputKeyLeft || event->key == InputKeyRight) && pressish) {
        remote_run_action(app, event->key == InputKeyRight ? ActionVolumeUp : ActionVolumeDown);
    } else if(event->key == InputKeyBack && event->type == InputTypeShort) {
        remote_set_menu(app, false);
    } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
        if(action == ActionClose) {
            remote_set_menu(app, false);
        } else {
            if(!action_keeps_menu((Action)action)) remote_set_menu(app, false);
            remote_run_action(app, (Action)action);
        }
    }
    return true;
}

static bool remote_handle_input(Remote* app, const InputEvent* event) {
    if(event->key == InputKeyBack && event->type == InputTypeLong) return false;

    furi_mutex_acquire(app->lock, FuriWaitForever);
    const bool menu = app->screen.menu;
    const uint8_t action = app->screen.action;
    furi_mutex_release(app->lock);

    if(menu) return remote_handle_menu(app, event, action);

    /* The menu opens even when disconnected: projector power is infrared. */
    if(event->key == InputKeyOk && event->type == InputTypeLong) {
        remote_set_menu(app, true);
        return true;
    }

    uint16_t code = 0;
    bool consumer = false;
    const bool pressish = event->type == InputTypePress || event->type == InputTypeRepeat;
    if(pressish) {
        switch(event->key) {
        case InputKeyUp:
            code = HID_KEYBOARD_UP_ARROW;
            break;
        case InputKeyDown:
            code = HID_KEYBOARD_DOWN_ARROW;
            break;
        case InputKeyLeft:
            code = HID_KEYBOARD_LEFT_ARROW;
            break;
        case InputKeyRight:
            code = HID_KEYBOARD_RIGHT_ARROW;
            break;
        default:
            break;
        }
    } else if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk) {
            code = HID_KEYBOARD_RETURN;
        } else if(event->key == InputKeyBack) {
            code = HID_CONSUMER_AC_BACK;
            consumer = true;
        }
    }
    if(!code) return true;

    const bool sent = consumer ? remote_consumer(app, code) : remote_key(app, code);
    if(sent) {
        furi_mutex_acquire(app->lock, FuriWaitForever);
        app->screen.highlight = true;
        app->screen.last_key = event->key;
        app->highlight_until = furi_get_tick() + furi_ms_to_ticks(HIGHLIGHT_MS);
        furi_mutex_release(app->lock);
        view_port_update(app->view_port);
    }
    return true;
}

static void remote_expire(Remote* app) {
    const uint32_t now = furi_get_tick();
    bool changed = false;
    furi_mutex_acquire(app->lock, FuriWaitForever);
    if(app->screen.highlight && (int32_t)(now - app->highlight_until) >= 0) {
        app->screen.highlight = false;
        changed = true;
    }
    if(app->screen.toast && (int32_t)(now - app->toast_until) >= 0) {
        app->screen.toast = NULL;
        changed = true;
    }
    furi_mutex_release(app->lock);
    if(changed) view_port_update(app->view_port);
}

int32_t google_tv_remote_app(void* context) {
    UNUSED(context);
    Remote* app = malloc(sizeof(Remote));
    memset(app, 0, sizeof(Remote));
    app->input = furi_message_queue_alloc(32, sizeof(InputEvent));
    app->lock = furi_mutex_alloc(FuriMutexTypeNormal);
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, remote_draw, app);
    view_port_input_callback_set(app->view_port, remote_input, app);
    app->gui = furi_record_open(RECORD_GUI);
    app->bt = furi_record_open(RECORD_BT);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    bt_disconnect(app->bt);
    furi_delay_ms(200);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, REMOTE_BOND_DIR);
    furi_record_close(RECORD_STORAGE);
    bt_keys_storage_set_storage_path(app->bt, REMOTE_BOND_PATH);
    app->profile = bt_profile_start(app->bt, ble_profile_hid, NULL);
    if(app->profile) {
        bt_set_status_changed_callback(app->bt, remote_bt_status, app);
        furi_hal_bt_start_advertising();
    } else {
        furi_mutex_acquire(app->lock, FuriWaitForever);
        app->screen.failed = true;
        furi_mutex_release(app->lock);
        view_port_update(app->view_port);
    }

    InputEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->input, &event, furi_ms_to_ticks(50)) == FuriStatusOk) {
            running = remote_handle_input(app, &event);
        }
        remote_expire(app);
    }

    bt_set_status_changed_callback(app->bt, NULL, NULL);
    if(app->profile) {
        ble_profile_hid_kb_release_all(app->profile);
        ble_profile_hid_consumer_key_release_all(app->profile);
    }
    bt_disconnect(app->bt);
    furi_delay_ms(200);
    bt_keys_storage_set_default_path(app->bt);
    if(!bt_profile_restore_default(app->bt)) {
        FURI_LOG_E(TAG, "Could not restore default Bluetooth profile");
    }

    view_port_enabled_set(app->view_port, false);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_BT);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->input);
    furi_mutex_free(app->lock);
    free(app);
    return 0;
}
