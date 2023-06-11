#pragma once

#include "input-event-codes.h"

#include <stdbool.h>
#include <stdint.h>

enum key_state
{
	KEY_STATE_IDLE = 0,
	KEY_STATE_PRESSED,
	KEY_STATE_HOLD,
	KEY_STATE_RELEASED,
	KEY_STATE_LONG_HOLD,
};

#define LONG_HOLD_MS    3000

struct key_callback
{
	void (*func)(char, enum key_state);
	struct key_callback *next;
};

void keyboard_inject_event(char key, enum key_state state);

void keyboard_add_key_callback(struct key_callback *callback);

void keyboard_init(void);
