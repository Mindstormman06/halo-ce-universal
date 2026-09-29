/*
OVERLAY_PAUSE.C

The settings overlay (port/linux/src/overlay.c) stops the game's clock
while it is open, as the pause menu does, but only in a game on this
machine alone: a system link game goes on for everyone else, and the main
menu has nothing to pause. The clock is started again only if the overlay
stopped it.
*/

#include "cseries.h"
#include "game/game.h"
#include "interface/ui_widget.h"

static boolean overlay_paused = FALSE;

void halo_overlay_pause(int paused)
{
	if (paused)
	{
		if (!overlay_paused && game_in_progress() && !main_menu_is_active() &&
			game_connection() == _game_connection_local && !game_time_get_paused())
		{
			game_time_set_paused(TRUE);
			overlay_paused = TRUE;
		}
	}
	else if (overlay_paused)
	{
		overlay_paused = FALSE;
		if (game_in_progress())
			game_time_set_paused(FALSE);
	}
}
