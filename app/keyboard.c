#include "app_config.h"
#include "fifo.h"
#include "keyboard.h"
#include "reg.h"
#include "pi.h"

#include <pico/stdlib.h>

// Size of the list keeping track of all the pressed keys
#define MAX_TRACKED_KEYS 10

static struct
{
	struct key_callback *key_callbacks;
} self;

// Key and buttons definitions

static const uint8_t row_pins[NUM_OF_ROWS] =
{
	PINS_ROWS
};

static const uint8_t col_pins[NUM_OF_COLS] =
{
	PINS_COLS
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

static const uint8_t kbd_entries[NUM_OF_ROWS][NUM_OF_COLS] =
//  Touchpad center key
{ { KEY_COMPOSE, KEY_W, KEY_G, KEY_S, KEY_L, KEY_H }
, {         0x0, KEY_Q, KEY_R, KEY_E, KEY_O, KEY_U }
//     Call button
, {    KEY_OPEN, KEY_0, KEY_F, KEY_LEFTSHIFT, KEY_K, KEY_J }
, {         0x0, KEY_SPACE, KEY_C, KEY_Z, KEY_M, KEY_N }
//    Berry key  Symbol key
, {   KEY_PROPS, KEY_RIGHTALT, KEY_T, KEY_D, KEY_I, KEY_Y }
//      Back key Alt key
, {     KEY_ESC, KEY_LEFTALT, KEY_V, KEY_X, KEY_MUTE, KEY_B }
, {         0x0, KEY_A, KEY_RIGHTSHIFT, KEY_P, KEY_BACKSPACE, KEY_ENTER }
};
static bool kbd_state[256] = {};

#if NUM_OF_BTNS > 0

// Call end key mapped to GPIO 4
static const char btn_entries[NUM_OF_BTNS] = { KEY_POWER };
static const uint8_t btn_pins[NUM_OF_BTNS] = { 4 };
#endif

#pragma GCC diagnostic pop

uint power_key_hold_start_time;
enum key_state power_key_state;

// Bitfield for modifiers to send with any pressed alpha key
uint8_t active_modifiers;

struct sticky_modifier
{
	// Internal tracking bitfield to determine sticky state
	// All sticky modifiers need to have a unique bitfield
	uint8_t bit;

	// Bitfield add to USB scancode to produce modified effect
	// Active modifiers stored in `active_modifiers`
	// Only set for modifiers corresponding to a USB modifier
	// Virtual mappings internal to this driver will have this
	// set to 0 and use a `map_callback` instead.
	uint8_t scancode_modifier;

	// When sticky modifier system has determined that
	// modifier should be applied, run this callback
	// and report the returned keycode result to the input system
	void(*set_callback)(uint8_t keycode);
	void(*unset_callback)(uint8_t keycode);
	uint8_t(*map_callback)(uint8_t keycode);
};

static struct sticky_modifier sticky_ctrl;
static struct sticky_modifier sticky_shift;
static struct sticky_modifier sticky_phys_alt;
static struct sticky_modifier sticky_altgr;

// Modifier mode flags and lock status
uint8_t held_modifier_keys;
uint8_t pending_sticky_modifier_keys;
uint8_t sticky_modifier_keys;
uint8_t apply_phys_alt; // "Real" modifiers like
// Shift and Control are handled by simulating input key
// events. Since phys. alt is hardcoded, the state is here.
uint8_t current_phys_alt_keycode; // Store the last keycode
// sent in the phys. alt map to simulate a key up event
// when the key is released after phys. alt is released

static void enable_modifier(uint8_t modifier)
{
	active_modifiers |= modifier;
}

static void disable_modifier(uint8_t modifier)
{
	active_modifiers &= ~modifier;
}

static void enable_phys_alt(uint8_t keycode)
{
	apply_phys_alt = 1;
}

static void disable_phys_alt(uint8_t keycode)
{
	// Send key up event if there is a current phys. alt key being held
	if (current_phys_alt_keycode) {
		keyboard_inject_event(current_phys_alt_keycode, KEY_STATE_RELEASED);
		current_phys_alt_keycode = 0;
	}

	apply_phys_alt = 0;
}

// Map physical keys to their Alt+key combination as
// printed on the keyboard, properly mapped in the keymap file
static uint8_t map_alt_keycode(uint8_t keycode)
{
	if (!apply_phys_alt) {
		return 0;
	}

	switch (keycode) {
	case KEY_Q: return 135;
	case KEY_W: return 136;
	case KEY_E: return 137;
	case KEY_R: return 138;
	case KEY_T: return 139;
	case KEY_Y: return 140;
	case KEY_U: return 141;
	case KEY_I: return 142;
	case KEY_O: return 143;
	case KEY_P: return 144;
	case KEY_A: return 145;
	case KEY_S: return 146;
	case KEY_D: return 147;
	case KEY_F: return 148;
	case KEY_G: return 149;
	case KEY_H: return 150;
	case KEY_J: return 151;
	case KEY_K: return 152;
	case KEY_L: return 153;
	case KEY_BACKSPACE: return KEY_DELETE;
	case KEY_Z: return 154;
	case KEY_X: return 155;
	case KEY_C: return 156;
	case KEY_V: return 157;
	case KEY_B: return 158;
	case KEY_N: return 159;
	case KEY_M: return 160;
	case KEY_MUTE: return 161;
	case KEY_ENTER: return KEY_TAB;
	}

	return 0;
}

static uint8_t map_and_store_alt_keycode(uint8_t keycode)
{
	uint8_t mapped_keycode;

	mapped_keycode = map_alt_keycode(keycode);
	if (mapped_keycode) {
		current_phys_alt_keycode = mapped_keycode;
		return mapped_keycode;
	}

	return keycode;
}

// Sticky modifier keys follow BB Q10 convention
// Holding modifier while typing alpha keys will apply to all alpha keys
// until released.
// One press and release will enter sticky mode, apply modifier key to
// the next alpha key only. If the same modifier key is pressed and
// released again in sticky mode, it will be canceled.
static enum key_state transition_sticky_modifier(
		struct sticky_modifier const* sticky_modifier, bool pressed)
{
	if (pressed) {

		// Set "held" state and "pending sticky" state.
		held_modifier_keys |= sticky_modifier->bit;

		// If pressed again while sticky, clear sticky
		if (sticky_modifier_keys & sticky_modifier->bit) {
			sticky_modifier_keys &= ~sticky_modifier->bit;

		// Otherwise, set pending sticky to be applied on release
		} else {
			pending_sticky_modifier_keys |= sticky_modifier->bit;
		}

		// Report modifier to input system as held
		if (sticky_modifier->set_callback) {
			sticky_modifier->set_callback(sticky_modifier->scancode_modifier);
		}

	// Released
	} else {

		// Unset "held" state
		held_modifier_keys &= ~sticky_modifier->bit;

		// If any alpha key was typed during hold,
		// `apply_sticky_modifiers` will clear "pending sticky" state.
		// If still in "pending sticky", set "sticky" state.
		if (pending_sticky_modifier_keys & sticky_modifier->bit) {

			sticky_modifier_keys |= sticky_modifier->bit;
			pending_sticky_modifier_keys &= ~sticky_modifier->bit;
		}

		// Report modifier to input system as released
		if (sticky_modifier->unset_callback) {
			sticky_modifier->unset_callback(sticky_modifier->scancode_modifier);
		}
	}
}

// Called before sending an alpha key to apply any pending sticky modifiers
static void apply_sticky_modifier(struct sticky_modifier const* sticky_modifier)
{
	if (held_modifier_keys & sticky_modifier->bit) {
		pending_sticky_modifier_keys &= ~sticky_modifier->bit;

	} else if (sticky_modifier_keys & sticky_modifier->bit) {
		if (sticky_modifier->set_callback) {
			sticky_modifier->set_callback(sticky_modifier->scancode_modifier);
		}
	}
}

// Called after applying any pending sticky modifiers,
// before sending alpha key, to perform any hard-coded mapping
static uint8_t map_sticky_modifier(struct sticky_modifier const* sticky_modifier,
	uint8_t keycode)
{
	if (sticky_modifier->map_callback) {
		return sticky_modifier->map_callback(keycode);
	}

	return keycode;
}

// Called after sending the alpha key to reset
// any sticky modifiers
static void reset_sticky_modifier(struct sticky_modifier const* sticky_modifier)
{
	if (sticky_modifier_keys & sticky_modifier->bit) {
		sticky_modifier_keys &= ~sticky_modifier->bit;

		if (sticky_modifier->unset_callback) {
			sticky_modifier->unset_callback(sticky_modifier->scancode_modifier);
		}
	}
}

static void handle_key_event(uint8_t keycode, bool pressed)
{
	uint8_t mapped_keycode, modifiers;

	// Don't send power key over USB
	if ((keycode == 0) || (keycode == KEY_POWER)) {
		return;
	}

	// Set / get modifiers, report key event
	switch (keycode) {

	case KEY_LEFTSHIFT:
	case KEY_RIGHTSHIFT:
		transition_sticky_modifier(&sticky_shift, pressed);
		break;

	// Map call key to Control in keymap
	case KEY_OPEN:
		transition_sticky_modifier(&sticky_ctrl, pressed);
		break;

	// Map left alt (physical alt key) to hardcoded alt-map
	case KEY_LEFTALT:
		transition_sticky_modifier(&sticky_phys_alt, pressed);
		break;

	// Map Symbol key to AltGr
	case KEY_RIGHTALT:
		transition_sticky_modifier(&sticky_altgr, pressed);
		break;

	// Pressing touchpad will enable Meta mode (not sticky at this level)
	case KEY_COMPOSE:
		keyboard_inject_event(KEY_COMPOSE,
			(pressed) ? KEY_STATE_PRESSED : KEY_STATE_RELEASED);
		break;

	default:

		// Apply pending sticky modifiers
		apply_sticky_modifier(&sticky_shift);
		apply_sticky_modifier(&sticky_ctrl);
		apply_sticky_modifier(&sticky_phys_alt);
		apply_sticky_modifier(&sticky_altgr);

		// Run sticky modifier key remaps (only phys. alt needs virtual remapping)
		mapped_keycode = map_sticky_modifier(&sticky_phys_alt, keycode);

		// Report final key to input system
		if (mapped_keycode) {
			keyboard_inject_event(mapped_keycode,
				(pressed) ? KEY_STATE_PRESSED : KEY_STATE_RELEASED);
		}

		// Reset sticky modifiers
		reset_sticky_modifier(&sticky_shift);
		reset_sticky_modifier(&sticky_ctrl);
		reset_sticky_modifier(&sticky_phys_alt);
		reset_sticky_modifier(&sticky_altgr);
	}
}

static int64_t release_power_key_alarm_callback(alarm_id_t _, void* __)
{
	keyboard_inject_event(KEY_POWER, KEY_STATE_RELEASED);

	return 0;
}

static void transition_power_key_state(bool const pressed)
{
	uint power_key_held_for
		= to_ms_since_boot(get_absolute_time()) - power_key_hold_start_time;

	switch (power_key_state) {

		// Idle -> Pressed
		case KEY_STATE_IDLE:
			if (pressed) {
				power_key_state = KEY_STATE_PRESSED;

				// Track hold time for transitioning to Hold and Long Hold states
				power_key_hold_start_time = to_ms_since_boot(get_absolute_time());
			}
			break;

		// Pressed -> Hold | Released
		case KEY_STATE_PRESSED:
			if (power_key_held_for > (reg_get_value(REG_ID_HLD) * 10)) {
				power_key_state = KEY_STATE_HOLD;

			} else if (!pressed) {
				power_key_state = KEY_STATE_RELEASED;
			}
			break;

		// Hold -> Released | Long Hold
		case KEY_STATE_HOLD:
			if (!pressed) {
				power_key_state = KEY_STATE_RELEASED;

			} else if (power_key_held_for > LONG_HOLD_MS) {

				// Simulate press event and schedule release
				keyboard_inject_event(KEY_POWER, KEY_STATE_PRESSED);
				add_alarm_in_ms(10, release_power_key_alarm_callback, NULL, true);

				power_key_state = KEY_STATE_LONG_HOLD;
			}
			break;

		// Long Hold -> Released
		case KEY_STATE_LONG_HOLD:
			if (!pressed) {
				power_key_state = KEY_STATE_RELEASED;
			}
			break;

		// Released -> Idle
		case KEY_STATE_RELEASED:
			power_key_state = KEY_STATE_IDLE;
			break;
	}
}

static int64_t timer_task(alarm_id_t id, void *user_data)
{
	(void)id;
	(void)user_data;
	uint c, r, i, key_idx;
	bool pressed;

	for (c = 0; c < NUM_OF_COLS; c++) {
		gpio_pull_up(col_pins[c]);
		gpio_put(col_pins[c], 0);
		gpio_set_dir(col_pins[c], GPIO_OUT);

		for (r = 0; r < NUM_OF_ROWS; r++) {
			pressed = (gpio_get(row_pins[r]) == 0);
			key_idx = (uint)((r * NUM_OF_COLS) + c);
			if (kbd_state[key_idx] != pressed) {
				handle_key_event(kbd_entries[r][c], pressed);
				kbd_state[key_idx] = pressed;
			}
		}

		gpio_put(col_pins[c], 1);
		gpio_disable_pulls(col_pins[c]);
		gpio_set_dir(col_pins[c], GPIO_IN);
	}

#if NUM_OF_BTNS > 0
	for (i = 0; i < NUM_OF_BTNS; i++) {
		pressed = (gpio_get(btn_pins[i]) == 0);
		transition_power_key_state(pressed);
	}
#endif

	// negative value means interval since last alarm time
	return -(reg_get_value(REG_ID_FRQ) * 1000);
}

void keyboard_inject_event(uint8_t key, enum key_state state)
{
	struct fifo_item item;
	item.scancode = key;
	item.shift_modifier = (active_modifiers & sticky_shift.scancode_modifier) ? 1 : 0;
	item.ctrl_modifier  = (active_modifiers &  sticky_ctrl.scancode_modifier) ? 1 : 0;
	item.altgr_modifier = (active_modifiers & sticky_altgr.scancode_modifier) ? 1 : 0;
	item.state = state;

	if (!fifo_enqueue(item)) {
		if (reg_is_bit_set(REG_ID_CFG, CFG_OVERFLOW_INT)) {
			reg_set_bit(REG_ID_INT, INT_OVERFLOW);
		}

		if (reg_is_bit_set(REG_ID_CFG, CFG_OVERFLOW_ON)) {
			fifo_enqueue_force(item);
		}
	}

	struct key_callback *cb = self.key_callbacks;
	while (cb) {
		cb->func(key, active_modifiers, state);
		cb = cb->next;
	}
}

void keyboard_add_key_callback(struct key_callback *callback)
{
	// first callback
	if (!self.key_callbacks) {
		self.key_callbacks = callback;
		return;
	}

	// find last and insert after
	struct key_callback *cb = self.key_callbacks;
	while (cb->next) {
		cb = cb->next;
	}
	cb->next = callback;
}

void keyboard_init(void)
{
	uint i;

	// rows
	for (i = 0; i < NUM_OF_ROWS; ++i) {
		gpio_init(row_pins[i]);
		gpio_pull_up(row_pins[i]);
		gpio_set_dir(row_pins[i], GPIO_IN);
	}

	// cols
	for(i = 0; i < NUM_OF_COLS; ++i) {
		gpio_init(col_pins[i]);
		gpio_set_dir(col_pins[i], GPIO_IN);
	}

	// btns
#if NUM_OF_BTNS > 0
	for(i = 0; i < NUM_OF_BTNS; ++i) {
		gpio_init(btn_pins[i]);
		gpio_pull_up(btn_pins[i]);
		gpio_set_dir(btn_pins[i], GPIO_IN);
	}
#endif

	held_modifier_keys = 0;
	pending_sticky_modifier_keys = 0;
	sticky_modifier_keys = 0;
	apply_phys_alt = 0;
	current_phys_alt_keycode = 0;
	active_modifiers = 0;

	sticky_ctrl.bit = (1 << 0);
	sticky_ctrl.scancode_modifier = KEY_MOD_LCTRL;
	sticky_ctrl.set_callback = enable_modifier;
	sticky_ctrl.unset_callback = disable_modifier;
	sticky_ctrl.map_callback = NULL;

	sticky_shift.bit = (1 << 1);
	sticky_shift.scancode_modifier = KEY_MOD_LSHIFT;
	sticky_shift.set_callback = enable_modifier;
	sticky_shift.unset_callback = disable_modifier;
	sticky_shift.map_callback = NULL;

	sticky_phys_alt.bit = (1 << 2);
	sticky_phys_alt.scancode_modifier = 0;
	sticky_phys_alt.set_callback = enable_phys_alt;
	sticky_phys_alt.unset_callback = disable_phys_alt;
	sticky_phys_alt.map_callback = map_and_store_alt_keycode;

	sticky_altgr.bit = (1 << 3);
	sticky_altgr.scancode_modifier = KEY_MOD_RALT;
	sticky_altgr.set_callback = enable_modifier;
	sticky_altgr.unset_callback = disable_modifier;
	sticky_altgr.map_callback = NULL;

	add_alarm_in_ms(reg_get_value(REG_ID_FRQ), timer_task, NULL, true);
}
