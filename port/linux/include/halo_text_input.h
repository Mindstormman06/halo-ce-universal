/*
HALO_TEXT_INPUT.H

Typing on the desktop builds. While the menus' on-screen keyboard
(source/interface/virtual_keyboard.c) is up, the real keyboard types into it
(port/linux/src/sdl_platform.c) instead of driving the controller
(port/linux/src/xinput_sdl.c): characters go in at the cursor, backspace,
delete, the arrows, home and end edit, enter finishes and escape cancels.
*/

#ifndef HALO_TEXT_INPUT_H
#define HALO_TEXT_INPUT_H

enum halo_text_input_kind
{
	_halo_text_input_character,
	_halo_text_input_backspace,
	_halo_text_input_delete,
	_halo_text_input_left,
	_halo_text_input_right,
	_halo_text_input_home,
	_halo_text_input_end,
	_halo_text_input_done,
	_halo_text_input_cancel,
};

struct halo_text_input
{
	unsigned char kind;
	/* for _halo_text_input_character: UTF-16, the basic multilingual plane
	only */
	unsigned short character;
};

/* takes the next character typed or key pressed while typing; 0 when there
is none. Always 0 on Android. */
int halo_text_input_next(struct halo_text_input *input);

#endif
