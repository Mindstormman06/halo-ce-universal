/*
INPUT_BINDINGS.H

The keys and mouse buttons that drive the controller from the keyboard
(xinput_sdl.c), and that the settings overlay's KEYBINDS tab changes
(overlay.c). Each action has one binding, kept in config.toml as "input.key_*"
in the name SDL gives the key ("Space", "Left Ctrl") or the mouse button
("Left Mouse", "Mouse 4"); empty for none. The arrows, enter, escape, F1,
backspace and the mouse wheel work as before whatever is bound.
*/

#ifndef __HALO_LINUX_INPUT_BINDINGS_H
#define __HALO_LINUX_INPUT_BINDINGS_H

#include <stddef.h>

/* in the order of the overlay's rows */
enum input_action
{
	_action_move_forward,
	_action_move_back,
	_action_move_left,
	_action_move_right,
	_action_jump,
	_action_melee,
	_action_action,
	_action_switch_weapon,
	_action_flashlight,
	_action_switch_grenade,
	_action_throw_grenade,
	_action_fire,
	_action_crouch,
	_action_zoom,
	NUMBER_OF_INPUT_ACTIONS
};

/* a binding is 0 for none, an SDL scancode, or INPUT_BINDING_MOUSE plus an
SDL_BUTTON_* */
#define INPUT_BINDING_MOUSE 1000

const char *input_action_label(enum input_action action);
const char *input_action_help(enum input_action action);
/* the name in config.toml */
const char *input_action_setting(enum input_action action);

int input_binding(enum input_action action);
int input_binding_default(enum input_action action);
/* how a binding reads: "W", "Left Mouse", "None" */
void input_binding_name(int binding, char *name, size_t size);
/* whether the binding can be given to an action (the overlay's own keys
cannot) */
int input_binding_allowed(int binding);
/* binds the action, sets it in the settings (not written to config.toml),
and if another action had the binding gives that one the action's old
binding; returns that other action, or -1 */
int input_binding_set(enum input_action action, int binding);

#endif
