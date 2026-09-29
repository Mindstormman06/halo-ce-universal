/*
OVERLAY.H

The desktop ports' settings overlay (overlay.c).
*/

#ifndef __HALO_LINUX_OVERLAY_H
#define __HALO_LINUX_OVERLAY_H

#include <SDL3/SDL_events.h>

enum overlay_event_result
{
	/* the event is the game's */
	_overlay_event_ignored,
	/* the overlay took it */
	_overlay_event_consumed,
	/* the overlay took it, and opened or closed */
	_overlay_event_opened,
	_overlay_event_closed,
};

BOOL overlay_is_open(void);
/* an event from the event loop (sdl_platform.c), on the main thread */
enum overlay_event_result overlay_handle_event(const SDL_Event *event);
/* TRUE while a controller's state must not reach the game: while the
overlay is open, and until the buttons held when it closed are let go */
BOOL overlay_holds_gamepad(const XINPUT_GAMEPAD *pad);
/* draws over the presented picture, in the window's pixels, with the
window's framebuffer bound (D3DDevice_Present, d3d8_gl.c) */
void overlay_draw(int width, int height);

/* the game's side (port/linux/game/overlay_pause.c) */
void halo_overlay_pause(int paused);

#endif
