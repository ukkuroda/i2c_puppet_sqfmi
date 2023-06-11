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

struct key_lock_callback
{
	void (*func)(bool, bool);
	struct key_lock_callback *next;
};

void keyboard_inject_event(char key, enum key_state state);

bool keyboard_is_key_down(char key);
bool keyboard_is_mod_on(enum key_mod mod);

void keyboard_add_key_callback(struct key_callback *callback);
void keyboard_add_lock_callback(struct key_lock_callback *callback);

bool keyboard_get_capslock(void);
bool keyboard_get_numlock(void);

void keyboard_init(void);
