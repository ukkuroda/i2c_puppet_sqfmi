#include "app_config.h"
#include "fifo.h"
#include "keyboard.h"
#include "reg.h"
#include "pi.h"

#include <pico/stdlib.h>

#define LIST_SIZE	10 // size of the list keeping track of all the pressed keys

struct entry
{
	char chr;
	char alt;
};

struct list_item
{
	const struct entry *p_entry;
	uint32_t hold_start_time;
	enum key_state state;
	char effective_key;
};

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

/*

In https://github.com/wallComputer/bbqX0kbd_driver/blob/main/source/mod_src/bbq20kbd_pmod_codes.h
'f' is apped to KEY_ESC

*/
#if 0
static const struct entry kbd_entries[][NUM_OF_COLS] =
{
	{ { KEY_COMPOSE },  { 'W', '1' },              { 'G', '/' },              { 'S', '4' },              { 'L', '"'  },  { 'H' , ':' } },
	{ { },                 { 'Q', '#' },              { 'R', '3' },              { 'E', '2' },              { 'O', '+'  },  { 'U', '_'  } },
	{ { KEY_OPEN },   { '~', '0' },              { 'F', '6' },              { KEY_LEFTSHIFT }, { 'K', '\''  }, { 'J', ';'  } },
	{ { },                 { ' ', '\t' },             { 'C', '9' },              { 'Z', '7' },              { 'M', '.'  },  { 'N', ','  } },
	{ { KEY_PROPS },   KEY_LEFTMETA, { 'T', '(' },              { 'D', '5' },              { 'I', '-'  },  { 'Y', ')'  } },
	{ { KEY_ESC },  KEY_LEFTALT, { 'V', '?' },              { 'X', '8' },              { '$', '`'  },  { 'B', '!'  } },
	{ { },                 { 'A', '*' },              KEY_RIGHTSHIFT, { 'P', '@' },              { '\b', 'f' },       { '\n', '|' } },
};
#else
static const struct entry kbd_entries[NUM_OF_ROWS][NUM_OF_COLS] = {
{ {KEY_COMPOSE}, {KEY_W}, {KEY_G}, {KEY_S}, {KEY_L}, {KEY_H} },
{            {}, {KEY_Q}, {KEY_R}, {KEY_E}, {KEY_O}, {KEY_U} },
{    {KEY_OPEN}, {KEY_0}, {KEY_F}, {KEY_LEFTSHIFT}, {KEY_K}, {KEY_J} },
{            {}, {KEY_SPACE}, {KEY_C}, {KEY_Z}, {KEY_M}, {KEY_N} },
{   {KEY_PROPS}, {KEY_LEFTMETA}, {KEY_T}, {KEY_D}, {KEY_I}, {KEY_Y} },
{     {KEY_ESC}, {KEY_LEFTALT}, {KEY_V}, {KEY_X}, {KEY_MUTE}, {KEY_B} },
{            {}, {KEY_A}, {KEY_RIGHTSHIFT}, {KEY_P}, {KEY_BACKSPACE}, {KEY_ENTER} },
};
#endif

#if NUM_OF_BTNS > 0
static const struct entry btn_entries[NUM_OF_BTNS] =
{
	BTN_KEYS
};

static const uint8_t btn_pins[NUM_OF_BTNS] =
{
	PINS_BTNS
};
#endif

#pragma GCC diagnostic pop

static struct
{
	struct key_callback *key_callbacks;

	struct list_item list[LIST_SIZE];
} self;

static int64_t rk(alarm_id_t id, void *user_data)
{
	(void)id;

	const int data = (int)user_data;

	keyboard_inject_event((char)data, KEY_STATE_RELEASED);

	return 0;
}

static void transition_to(struct list_item * const p_item, const enum key_state next_state)
{
	const struct entry * const p_entry = p_item->p_entry;

	p_item->state = next_state;

	if (!p_entry)
		return;

	if (p_item->effective_key == '\0') {
		char key = p_entry->chr;

		p_item->effective_key = key;
	}

	if (p_item->effective_key == '\0')
		return;

	keyboard_inject_event(p_item->effective_key, next_state);
}

static void next_item_state(struct list_item * const p_item, const bool pressed)
{
	switch (p_item->state) {
		case KEY_STATE_IDLE:
			if (pressed) {
				transition_to(p_item, KEY_STATE_PRESSED);

				p_item->hold_start_time = to_ms_since_boot(get_absolute_time());
			}
			break;

		case KEY_STATE_PRESSED:
			if ((to_ms_since_boot(get_absolute_time()) - p_item->hold_start_time) > (reg_get_value(REG_ID_HLD) * 10)) {
				transition_to(p_item, KEY_STATE_HOLD);
			 } else if(!pressed) {
				transition_to(p_item, KEY_STATE_RELEASED);
			}
			break;

		case KEY_STATE_HOLD:
			if (!pressed) {
				transition_to(p_item, KEY_STATE_RELEASED);
			} else if ((to_ms_since_boot(get_absolute_time()) - p_item->hold_start_time) > LONG_HOLD_MS) {
				if(p_item->effective_key == KEY_STOP){
					//inject power key
					char key = KEY_STOP;
					keyboard_inject_event(key, KEY_STATE_PRESSED);
					//delay release key
					add_alarm_in_ms(10, rk, (void*)(int)key, true);
				}
				transition_to(p_item, KEY_STATE_LONG_HOLD);
			}
			break;

		case KEY_STATE_LONG_HOLD:
			if (!pressed)
				transition_to(p_item, KEY_STATE_RELEASED);
			break;			

		case KEY_STATE_RELEASED:
		{
			p_item->p_entry = NULL;
			p_item->effective_key = '\0';
			transition_to(p_item, KEY_STATE_IDLE);
			break;
		}
	}
}

static int64_t timer_task(alarm_id_t id, void *user_data)
{
	(void)id;
	(void)user_data;

	for (uint32_t c = 0; c < NUM_OF_COLS; ++c) {
		gpio_pull_up(col_pins[c]);
		gpio_put(col_pins[c], 0);
		gpio_set_dir(col_pins[c], GPIO_OUT);

		for (uint32_t r = 0; r < NUM_OF_ROWS; ++r) {
			const bool pressed = (gpio_get(row_pins[r]) == 0);
			const int32_t key_idx = (int32_t)((r * NUM_OF_COLS) + c);

			int32_t list_idx = -1;
			for (int32_t i = 0; i < LIST_SIZE; ++i) {
				if (self.list[i].p_entry != &((const struct entry*)kbd_entries)[key_idx])
					continue;

				list_idx = i;
				break;
			}

			if (list_idx > -1) {
				next_item_state(&self.list[list_idx], pressed);
				continue;
			}

			if (!pressed)
				continue;

			for (uint32_t i = 0 ; i < LIST_SIZE; ++i) {
				if (self.list[i].p_entry != NULL)
					continue;

				self.list[i].p_entry = &((const struct entry*)kbd_entries)[key_idx];
				self.list[i].effective_key = '\0';
				self.list[i].state = KEY_STATE_IDLE;
				next_item_state(&self.list[i], pressed);

				break;
			}
		}

		gpio_put(col_pins[c], 1);
		gpio_disable_pulls(col_pins[c]);
		gpio_set_dir(col_pins[c], GPIO_IN);
	}

#if NUM_OF_BTNS > 0
	for (uint32_t b = 0; b < NUM_OF_BTNS; ++b) {
		const bool pressed = (gpio_get(btn_pins[b]) == 0);

		int32_t list_idx = -1;
		for (int32_t i = 0; i < LIST_SIZE; ++i) {
			if (self.list[i].p_entry != &((const struct entry*)btn_entries)[b])
				continue;

			list_idx = i;
			break;
		}

		if (list_idx > -1) {
			next_item_state(&self.list[list_idx], pressed);
			continue;
		}

		if (!pressed)
			continue;

		for (uint32_t i = 0 ; i < LIST_SIZE; ++i) {
			if (self.list[i].p_entry != NULL)
				continue;

			self.list[i].p_entry = &((const struct entry*)btn_entries)[b];
			self.list[i].effective_key = '\0';
			self.list[i].state = KEY_STATE_IDLE;
			next_item_state(&self.list[i], pressed);

			break;
		}
	}
#endif

	// negative value means interval since last alarm time
	return -(reg_get_value(REG_ID_FRQ) * 1000);
}

void keyboard_inject_event(char key, enum key_state state)
{
	const struct fifo_item item = { key, state };
	if (!fifo_enqueue(item)) {
		if (reg_is_bit_set(REG_ID_CFG, CFG_OVERFLOW_INT))
			reg_set_bit(REG_ID_INT, INT_OVERFLOW);

		if (reg_is_bit_set(REG_ID_CFG, CFG_OVERFLOW_ON))
			fifo_enqueue_force(item);
	}

	struct key_callback *cb = self.key_callbacks;
	while (cb) {
		cb->func(key, state);

		cb = cb->next;
	}
}

bool keyboard_is_key_down(char key)
{
	for (int32_t i = 0; i < LIST_SIZE; ++i) {
		struct list_item *item = &self.list[i];

		if (item->p_entry == NULL)
			continue;

		if ((item->state != KEY_STATE_PRESSED) && (item->state != KEY_STATE_HOLD) && (item->state != KEY_STATE_LONG_HOLD))
			continue;

		if (item->effective_key != key)
			continue;

		return true;
	}

	return false;
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
	while (cb->next)
		cb = cb->next;

	cb->next = callback;
}

void keyboard_init(void)
{
	// rows
	for (uint32_t i = 0; i < NUM_OF_ROWS; ++i) {
		gpio_init(row_pins[i]);
		gpio_pull_up(row_pins[i]);
		gpio_set_dir(row_pins[i], GPIO_IN);
	}

	// cols
	for(uint32_t i = 0; i < NUM_OF_COLS; ++i) {
		gpio_init(col_pins[i]);
		gpio_set_dir(col_pins[i], GPIO_IN);
	}

	// btns
#if NUM_OF_BTNS > 0
	for(uint32_t i = 0; i < NUM_OF_BTNS; ++i) {
		gpio_init(btn_pins[i]);
		gpio_pull_up(btn_pins[i]);
		gpio_set_dir(btn_pins[i], GPIO_IN);
	}
#endif

	add_alarm_in_ms(reg_get_value(REG_ID_FRQ), timer_task, NULL, true);
}
