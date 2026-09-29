/*
SDL_PLATFORM.C

The SDL3 window, OpenGL context and event loop behind the Linux build.

The window is created with the Direct3D device (d3d8_gl.c) on the game's
main thread, which is also the only thread that pumps events. Keyboard and
mouse state gathered here feeds the controller emulation in xinput_sdl.c
and the debug keyboard that the game's console reads.
*/

#include "platform.h"
#include "sdl_platform.h"
#include "gl.h"
#include "port_config.h"
#include "p2p.h"
#include "xiso.h"
#include "halo_text_input.h"
#ifndef HALO_ANDROID
#include "overlay.h"
#endif

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window *platform_window;
static SDL_GLContext platform_gl_context;
static SDL_ThreadID platform_event_thread;
static BOOL platform_sdl_started = FALSE;

static struct platform_input_state input_state;
/* keys pressed since the last read, so a press and release between two
reads still counts as a press (input injected on Android, or a slow frame) */
static unsigned char keys_pressed[SDL_SCANCODE_COUNT];
#ifndef HALO_ANDROID
/* the menus' pointer (platform_ui_pointer_set_active), under input_lock */
static struct platform_ui_pointer ui_pointer;
static float ui_pointer_wheel;
#endif
static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;

/* debug keyboard queue */
#define KEYSTROKE_QUEUE_SIZE 64
static struct platform_keystroke keystroke_queue[KEYSTROKE_QUEUE_SIZE];
static unsigned long keystroke_head, keystroke_count;

/* typing into the on-screen keyboard (halo_text_input.h), under input_lock:
what was typed, whether the game wants typing and whether SDL's text input
is on */
#define TEXT_INPUT_QUEUE_SIZE 64
static struct halo_text_input text_input_queue[TEXT_INPUT_QUEUE_SIZE];
static unsigned long text_input_head, text_input_count;
static BOOL text_input_wanted, text_input_started;
/* keys held when typing ended: they stay up for the controller until
released, so the enter that finished does not also press A */
static unsigned char keys_held_over[SDL_SCANCODE_COUNT];

#ifndef HALO_ANDROID
/* updater.c's: the desktop self-updater */
void updater_start(void);
void updater_poll(SDL_Window *window);
static void platform_overlay_changed(BOOL open);
#endif

BOOL platform_sdl_initialize(void)
{
	if (platform_sdl_started)
		return TRUE;
	/* a copy of the game started to open an invite link hands it to the
	one already running, and goes */
	if (p2p_hand_off_invite())
		exit(EXIT_SUCCESS);
	SDL_SetHint(SDL_HINT_APP_NAME, "Halo");
#ifdef HALO_ANDROID
	/* landscape only; the back key arrives as a key event (xinput_sdl.c)
	instead of closing the activity */
	SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
	SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
	/* touching the screen must not aim or fire (the mouse drives the
	controller emulation in xinput_sdl.c) */
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS))
	{
		platform_log("SDL_Init failed: %s", SDL_GetError());
		return FALSE;
	}
	platform_sdl_started = TRUE;
#ifndef HALO_ANDROID
	/* found (or offered to the player, platform_offer_game_data) before the
	game's window opens */
	platform_data_root();
	/* (a new version looked for meanwhile, updater_poll asking about it) */
	updater_start();
#endif
	return TRUE;
}

#ifndef HALO_ANDROID
/* ---------- first start without game data (xbox_files.c) */

struct data_extraction
{
	pthread_mutex_t lock;
	char image[1024];
	char destination[1024];
	char file[256];
	unsigned long long done;
	unsigned long long total;
	BOOL finished;
	BOOL succeeded;
	char error[512];
};

static void data_extraction_progress(void *context, const char *file, unsigned long long done,
	unsigned long long total)
{
	struct data_extraction *extraction = context;

	pthread_mutex_lock(&extraction->lock);
	snprintf(extraction->file, sizeof(extraction->file), "%s", file);
	extraction->done = done;
	extraction->total = total;
	pthread_mutex_unlock(&extraction->lock);
}

static void *data_extraction_thread(void *context)
{
	struct data_extraction *extraction = context;
	BOOL succeeded = xiso_extract_maps(extraction->image, extraction->destination, data_extraction_progress,
		extraction, extraction->error, sizeof(extraction->error)) != 0;

	pthread_mutex_lock(&extraction->lock);
	extraction->succeeded = succeeded;
	extraction->finished = TRUE;
	pthread_mutex_unlock(&extraction->lock);
	return NULL;
}

/* copies the maps, showing how far it has got; closing the window quits */
static BOOL data_extract(const char *image, const char *destination, char *error, int error_size)
{
	static struct data_extraction extraction;
	SDL_Window *window;
	SDL_Renderer *renderer = NULL;
	pthread_t thread;
	BOOL finished = FALSE;

	memset(&extraction, 0, sizeof(extraction));
	pthread_mutex_init(&extraction.lock, NULL);
	snprintf(extraction.image, sizeof(extraction.image), "%s", image);
	snprintf(extraction.destination, sizeof(extraction.destination), "%s", destination);
	if (pthread_create(&thread, NULL, data_extraction_thread, &extraction) != 0)
	{
		snprintf(error, (size_t)error_size, "Could not start the extraction.");
		return FALSE;
	}
	/* (waited for through extraction.finished; the Windows port's threads
	cannot be joined) */
	pthread_detach(thread);
	window = SDL_CreateWindow("Halo", 640, 150, 0);
	if (window)
	{
		renderer = SDL_CreateRenderer(window, NULL);
		if (renderer)
			SDL_SetRenderVSync(renderer, 1);
	}
	while (!finished)
	{
		SDL_Event event;
		char file[256];
		unsigned long long done, total;

		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
			{
				platform_log("extraction cancelled");
				exit(EXIT_SUCCESS);
			}
		}
		pthread_mutex_lock(&extraction.lock);
		finished = extraction.finished;
		snprintf(file, sizeof(file), "%s", extraction.file);
		done = extraction.done;
		total = extraction.total;
		pthread_mutex_unlock(&extraction.lock);
		if (renderer)
		{
			char line[320];
			SDL_FRect bar = { 20.0f, 100.0f, 600.0f, 24.0f };
			float fraction = total ? (float)((double)done / (double)total) : 0.0f;

			SDL_SetRenderDrawColor(renderer, 12, 16, 20, 255);
			SDL_RenderClear(renderer);
			SDL_SetRenderDrawColor(renderer, 230, 230, 230, 255);
			SDL_SetRenderScale(renderer, 2.0f, 2.0f);
			SDL_RenderDebugText(renderer, 10.0f, 10.0f, "Extracting the maps folder...");
			SDL_SetRenderScale(renderer, 1.0f, 1.0f);
			snprintf(line, sizeof(line), "%s  (%llu of %llu MB)", file, done >> 20, total >> 20);
			SDL_RenderDebugText(renderer, 20.0f, 70.0f, line);
			SDL_SetRenderDrawColor(renderer, 60, 66, 72, 255);
			SDL_RenderFillRect(renderer, &bar);
			bar.w *= fraction;
			SDL_SetRenderDrawColor(renderer, 90, 160, 90, 255);
			SDL_RenderFillRect(renderer, &bar);
			SDL_RenderPresent(renderer);
		}
		SDL_Delay(16);
	}
	if (renderer)
		SDL_DestroyRenderer(renderer);
	if (window)
		SDL_DestroyWindow(window);
	if (!extraction.succeeded)
		snprintf(error, (size_t)error_size, "%s", extraction.error);
	return extraction.succeeded;
}

struct data_image_choice
{
	SDL_AtomicInt done;
	char path[1024];
};

static void SDLCALL data_image_chosen(void *userdata, const char * const *files, int filter)
{
	struct data_image_choice *choice = userdata;

	(void)filter;
	if (files && files[0])
		snprintf(choice->path, sizeof(choice->path), "%s", files[0]);
	SDL_SetAtomicInt(&choice->done, 1);
}

/* the disc image the player picks; FALSE if they pick none */
static BOOL data_choose_image(char *path, int size)
{
	static const SDL_DialogFileFilter filters[] =
	{
		{ "Xbox disc images", "iso;xiso" },
		{ "All files", "*" },
	};
	static struct data_image_choice choice;

	memset(&choice, 0, sizeof(choice));
	SDL_ShowOpenFileDialog(data_image_chosen, &choice, NULL, filters, 2, NULL, false);
	/* the dialog answers through events (and on some systems another
	thread) */
	while (!SDL_GetAtomicInt(&choice.done))
		SDL_WaitEventTimeout(NULL, 50);
	if (!choice.path[0])
		return FALSE;
	snprintf(path, (size_t)size, "%s", choice.path);
	return TRUE;
}

BOOL platform_offer_game_data(const char *destination)
{
	static const SDL_MessageBoxButtonData buttons[] =
	{
		{ SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes" },
		{ SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No" },
	};
	char message[1400];

	/* not for runs nobody is watching */
	if (config_boolean("debug.hidden_window") || config_real("debug.exit_after") > 0.0 ||
		!SDL_Init(SDL_INIT_VIDEO))
	{
		return FALSE;
	}
	snprintf(message, sizeof(message),
		"Halo's game data (its maps folder) was not found.\n\n"
		"Extract the maps folder from an Xbox disc image (.iso) of Halo: Combat Evolved? "
		"It is copied to %s/maps (about 2 GB).\n\n"
		"(Or put the maps folder there yourself, or set paths.data in config.toml.)",
		destination);
	for (;;)
	{
		SDL_MessageBoxData question = { SDL_MESSAGEBOX_INFORMATION, NULL, "Halo", message, 2, buttons, NULL };
		char image[1024];
		char error[512];
		int answer = 0;

		if (!SDL_ShowMessageBox(&question, &answer) || answer != 1)
		{
			platform_log("no game data: quitting");
			exit(EXIT_SUCCESS);
		}
		/* no image picked: ask again */
		if (!data_choose_image(image, sizeof(image)))
			continue;
		platform_log("extracting the maps folder from %s to %s", image, destination);
		if (data_extract(image, destination, error, sizeof(error)))
		{
			platform_log("extracted the maps folder");
			return TRUE;
		}
		platform_log("extraction failed: %s", error);
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Halo", error, NULL);
	}
}
#endif

int halo_interpolation_enabled(void)
{
	static int enabled = -1;

	if (enabled < 0)
		enabled = config_boolean("display.interpolation");
	return enabled;
}

int halo_errors_on_screen(void)
{
	static int enabled = -1;

	if (enabled < 0)
		enabled = config_boolean("debug.errors_on_screen");
	return enabled;
}

int halo_warnings_on_screen(void)
{
	static int enabled = -1;

	if (enabled < 0)
		enabled = config_boolean("debug.warnings_on_screen");
	return enabled;
}

#ifndef HALO_ANDROID
/* whether the window opens fullscreen (display.fullscreen), never when it
is hidden */
static BOOL platform_fullscreen_setting(void)
{
	return !config_boolean("debug.hidden_window") && config_boolean("display.fullscreen");
}

/* the display the window is on, or is to open on */
static SDL_DisplayID platform_display(void)
{
	return platform_window ? SDL_GetDisplayForWindow(platform_window) : SDL_GetPrimaryDisplay();
}

/* the display's shape, width over height */
static float platform_display_aspect(void)
{
	SDL_DisplayID display = platform_display();
	const SDL_DisplayMode *mode = display ? SDL_GetDesktopDisplayMode(display) : NULL;

	return mode && mode->h > 0 ? (float)mode->w / (float)mode->h : 16.0f / 9.0f;
}

/* the window's width for a height: the display's shape with
display.widescreen, otherwise the Xbox's 4:3 */
int platform_video_window_width(int height)
{
	float aspect = config_boolean("display.widescreen") ? platform_display_aspect() : 4.0f / 3.0f;

	if (aspect < 4.0f / 3.0f)
		aspect = 4.0f / 3.0f;
	return ((int)((float)height * aspect + 0.5f)) & ~1;
}

/* the window's height: display.window_height, or 480 lines times
display.window_scale */
static int platform_window_height_setting(void)
{
	long height = config_integer("display.window_height");
	long scale = config_integer("display.window_scale");

	if (height >= 240)
		return (int)height;
	return 480 * (int)(scale < 1 ? 1 : scale);
}

/* the size in pixels of what the game fills: the display while it is
fullscreen, otherwise the window (d3d8_gl.c draws at that resolution);
FALSE for a hidden window, which draws the Xbox's 640x480 */
BOOL platform_screen_mode(long *width, long *height)
{
	static int last_width, last_height;
	int pixel_width = 0, pixel_height = 0;

	if (config_boolean("debug.hidden_window") || !platform_sdl_initialize())
		return FALSE;
	if (platform_window)
	{
		SDL_GetWindowSizeInPixels(platform_window, &pixel_width, &pixel_height);
		/* (a minimized window keeps the size it had) */
		if (pixel_width <= 0 || pixel_height <= 0)
		{
			pixel_width = last_width;
			pixel_height = last_height;
		}
	}
	else if (platform_fullscreen_setting())
	{
		SDL_DisplayID display = platform_display();
		const SDL_DisplayMode *mode = display ? SDL_GetDesktopDisplayMode(display) : NULL;

		if (mode)
		{
			pixel_width = (int)(mode->w * mode->pixel_density + 0.5f);
			pixel_height = (int)(mode->h * mode->pixel_density + 0.5f);
		}
	}
	else
	{
		pixel_height = platform_window_height_setting();
		pixel_width = platform_video_window_width(pixel_height);
	}
	if (pixel_width <= 0 || pixel_height <= 0)
		return FALSE;
	last_width = pixel_width;
	last_height = pixel_height;
	*width = pixel_width;
	*height = pixel_height;
	return TRUE;
}

#endif
#ifndef HALO_ANDROID
/* at exit (every way the game ends calls exit()): out of fullscreen, and the
window and its context closed, as SDL does it, before the process goes.
Left to Windows, the display can keep what the driver set it to for a
fullscreen OpenGL window: an HDR display stays dark until HDR is switched
off and on again. Only from the thread the window belongs to. */
static void platform_video_shutdown(void)
{
	if (!platform_window || SDL_GetCurrentThreadID() != platform_event_thread)
		return;
	platform_mouse_capture(FALSE);
	if (SDL_GetWindowFlags(platform_window) & SDL_WINDOW_FULLSCREEN)
	{
		SDL_SetWindowFullscreen(platform_window, false);
		SDL_SyncWindow(platform_window);
	}
	if (platform_gl_context)
	{
		SDL_GL_MakeCurrent(platform_window, NULL);
		SDL_GL_DestroyContext(platform_gl_context);
		platform_gl_context = NULL;
	}
	SDL_DestroyWindow(platform_window);
	platform_window = NULL;
	/* (restores the display's mode after exclusive fullscreen) */
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

#endif
BOOL platform_video_initialize(unsigned long width, unsigned long height)
{
	int scale = (int)config_integer("display.window_scale");
	int version;

	if (platform_window)
		return TRUE;
	if (!platform_sdl_initialize())
		return FALSE;
	if (scale < 1)
		scale = 1;
#ifndef HALO_ANDROID
	/* (the window's own size, not the device's) */
	height = (unsigned long)platform_window_height_setting();
	width = (unsigned long)platform_video_window_width((int)height);
	scale = 1;
#endif

#ifdef HALO_ANDROID
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
#endif
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
	if (config_boolean("debug.gl_debug"))
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
#if !defined(HALO_ANDROID) && !defined(_WIN32)
	/* Mesa's GL thread: the renderer makes thousands of GL calls a frame
	and never waits for their results, so handing them to a thread of
	their own takes a fifth of the main thread's time off it. It leaves an
	explicit mesa_glthread setting alone and other drivers ignore it. */
	setenv("mesa_glthread", "true", 0);
#endif

#ifdef HALO_ANDROID
	platform_window = SDL_CreateWindow("Halo", (int)(width * scale), (int)(height * scale),
		SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
#else
	/* fullscreen at the desktop's resolution unless display.fullscreen is
	false; the window size is the windowed mode F11 switches to and from.
	The game draws at the resolution of either (d3d8_gl.c). */
	platform_window = SDL_CreateWindow("Halo", (int)width, (int)height,
		SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
		(config_boolean("debug.hidden_window") ? SDL_WINDOW_HIDDEN : 0));
	if (platform_window && platform_fullscreen_setting())
	{
		platform_video_set_display_mode(config_boolean("display.exclusive") ? _platform_display_exclusive :
			_platform_display_borderless);
	}
#endif
	if (!platform_window)
	{
		platform_log("SDL_CreateWindow failed: %s", SDL_GetError());
		return FALSE;
	}
	platform_gl_context = SDL_GL_CreateContext(platform_window);
#ifdef HALO_ANDROID
	/* ES 3.2 where the driver has it, otherwise the renderer makes do with
	3.0 plus extensions */
	if (!platform_gl_context)
	{
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
		platform_gl_context = SDL_GL_CreateContext(platform_window);
	}
#endif
	if (!platform_gl_context)
	{
		platform_log("cannot create an OpenGL context: %s", SDL_GetError());
		return FALSE;
	}
	SDL_GL_MakeCurrent(platform_window, platform_gl_context);
	if (!gl_functions_load())
		return FALSE;
	version = SDL_GL_SetSwapInterval(config_boolean("display.vsync") ? 1 : 0);
	(void)version;
	platform_event_thread = SDL_GetCurrentThreadID();
	platform_log("OpenGL %s on %s", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));
#ifndef HALO_ANDROID
	platform_mouse_capture(TRUE);
	atexit(platform_video_shutdown);
#endif
	return TRUE;
}

void platform_video_drawable_size(int *width, int *height)
{
	SDL_GetWindowSizeInPixels(platform_window, width, height);
}

void platform_video_swap(void)
{
	SDL_GL_SwapWindow(platform_window);
}

void platform_mouse_capture(BOOL capture)
{
	if (platform_window)
		SDL_SetWindowRelativeMouseMode(platform_window, capture ? true : false);
}

/* ---------- keyboard translation */

/* Windows virtual key code for an SDL scancode (the Xbox debug keyboard
reports virtual keys) */
static BYTE virtual_key_from_scancode(SDL_Scancode scancode)
{
	if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
		return (BYTE)('A' + (scancode - SDL_SCANCODE_A));
	if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9)
		return (BYTE)('1' + (scancode - SDL_SCANCODE_1));
	if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12)
		return (BYTE)(0x70 + (scancode - SDL_SCANCODE_F1));
	if (scancode >= SDL_SCANCODE_KP_1 && scancode <= SDL_SCANCODE_KP_9)
		return (BYTE)(0x61 + (scancode - SDL_SCANCODE_KP_1));
	switch (scancode)
	{
	case SDL_SCANCODE_0: return '0';
	case SDL_SCANCODE_KP_0: return 0x60;
	case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0d;
	case SDL_SCANCODE_ESCAPE: return 0x1b;
	case SDL_SCANCODE_BACKSPACE: return 0x08;
	case SDL_SCANCODE_TAB: return 0x09;
	case SDL_SCANCODE_SPACE: return 0x20;
	case SDL_SCANCODE_MINUS: return 0xbd;
	case SDL_SCANCODE_EQUALS: return 0xbb;
	case SDL_SCANCODE_LEFTBRACKET: return 0xdb;
	case SDL_SCANCODE_RIGHTBRACKET: return 0xdd;
	case SDL_SCANCODE_BACKSLASH: return 0xdc;
	case SDL_SCANCODE_SEMICOLON: return 0xba;
	case SDL_SCANCODE_APOSTROPHE: return 0xde;
	case SDL_SCANCODE_GRAVE: return 0xc0;
	case SDL_SCANCODE_COMMA: return 0xbc;
	case SDL_SCANCODE_PERIOD: return 0xbe;
	case SDL_SCANCODE_SLASH: return 0xbf;
	case SDL_SCANCODE_CAPSLOCK: return 0x14;
	case SDL_SCANCODE_PRINTSCREEN: return 0x2c;
	case SDL_SCANCODE_SCROLLLOCK: return 0x91;
	case SDL_SCANCODE_PAUSE: return 0x13;
	case SDL_SCANCODE_INSERT: return 0x2d;
	case SDL_SCANCODE_HOME: return 0x24;
	case SDL_SCANCODE_PAGEUP: return 0x21;
	case SDL_SCANCODE_DELETE: return 0x2e;
	case SDL_SCANCODE_END: return 0x23;
	case SDL_SCANCODE_PAGEDOWN: return 0x22;
	case SDL_SCANCODE_RIGHT: return 0x27;
	case SDL_SCANCODE_LEFT: return 0x25;
	case SDL_SCANCODE_DOWN: return 0x28;
	case SDL_SCANCODE_UP: return 0x26;
	case SDL_SCANCODE_NUMLOCKCLEAR: return 0x90;
	case SDL_SCANCODE_KP_DIVIDE: return 0x6f;
	case SDL_SCANCODE_KP_MULTIPLY: return 0x6a;
	case SDL_SCANCODE_KP_MINUS: return 0x6d;
	case SDL_SCANCODE_KP_PLUS: return 0x6b;
	case SDL_SCANCODE_KP_PERIOD: return 0x6e;
	case SDL_SCANCODE_LCTRL: return 0xa2;
	case SDL_SCANCODE_RCTRL: return 0xa3;
	case SDL_SCANCODE_LSHIFT: return 0xa0;
	case SDL_SCANCODE_RSHIFT: return 0xa1;
	case SDL_SCANCODE_LALT: return 0xa4;
	case SDL_SCANCODE_RALT: return 0xa5;
	default: return 0;
	}
}

static CHAR ascii_from_key(SDL_Keycode key, SDL_Keymod modifiers)
{
	BOOL shift = (modifiers & SDL_KMOD_SHIFT) != 0;
	static const char shifted_digits[] = ")!@#$%^&*(";

	if (key >= 'a' && key <= 'z')
		return (CHAR)((shift ^ ((modifiers & SDL_KMOD_CAPS) != 0)) ? key - 32 : key);
	if (key >= '0' && key <= '9')
		return (CHAR)(shift ? shifted_digits[key - '0'] : key);
	if (key == SDLK_RETURN || key == SDLK_KP_ENTER)
		return '\r';
	if (key == SDLK_BACKSPACE)
		return '\b';
	if (key == SDLK_TAB)
		return '\t';
	if (key == SDLK_ESCAPE)
		return 0x1b;
	if (key >= 32 && key < 127)
	{
		if (!shift)
			return (CHAR)key;
		switch (key)
		{
		case '-': return '_';
		case '=': return '+';
		case '[': return '{';
		case ']': return '}';
		case '\\': return '|';
		case ';': return ':';
		case '\'': return '"';
		case ',': return '<';
		case '.': return '>';
		case '/': return '?';
		case '`': return '~';
		default: return (CHAR)key;
		}
	}
	return 0;
}

static void queue_keystroke(const SDL_KeyboardEvent *event)
{
	struct platform_keystroke *keystroke;
	BYTE flags = 0;

	if (keystroke_count == KEYSTROKE_QUEUE_SIZE)
	{
		keystroke_head = (keystroke_head + 1) % KEYSTROKE_QUEUE_SIZE;
		keystroke_count--;
	}
	keystroke = &keystroke_queue[(keystroke_head + keystroke_count) % KEYSTROKE_QUEUE_SIZE];
	if (event->mod & SDL_KMOD_CTRL) flags |= 0x01;
	if (event->mod & SDL_KMOD_SHIFT) flags |= 0x02;
	if (event->mod & SDL_KMOD_ALT) flags |= 0x04;
	if (event->mod & SDL_KMOD_CAPS) flags |= 0x08;
	if (event->mod & SDL_KMOD_NUM) flags |= 0x10;
	if (!event->down) flags |= 0x40;
	if (event->repeat) flags |= 0x80;
	keystroke->virtual_key = virtual_key_from_scancode(event->scancode);
	keystroke->ascii = event->down ? ascii_from_key(event->key, event->mod) : 0;
	keystroke->flags = flags;
	keystroke_count++;
}

BOOL platform_next_keystroke(struct platform_keystroke *keystroke)
{
	BOOL result = FALSE;

	pthread_mutex_lock(&input_lock);
	if (keystroke_count)
	{
		*keystroke = keystroke_queue[keystroke_head];
		keystroke_head = (keystroke_head + 1) % KEYSTROKE_QUEUE_SIZE;
		keystroke_count--;
		result = TRUE;
	}
	pthread_mutex_unlock(&input_lock);
	return result;
}

/* ---------- typing (halo_text_input.h) */

void platform_text_input_set_active(BOOL active)
{
#ifdef HALO_ANDROID
	(void)active;
#else
	text_input_wanted = active ? TRUE : FALSE;
#endif
}

BOOL platform_text_input_active(void)
{
	return text_input_wanted;
}

static void queue_text_input(enum halo_text_input_kind kind, unsigned short character)
{
	struct halo_text_input *input;

	if (text_input_count == TEXT_INPUT_QUEUE_SIZE)
		return;
	input = &text_input_queue[(text_input_head + text_input_count) % TEXT_INPUT_QUEUE_SIZE];
	input->kind = (unsigned char)kind;
	input->character = character;
	text_input_count++;
}

/* the characters of SDL's UTF-8 text, those beyond the basic multilingual
plane dropped */
static void queue_text(const char *text)
{
	const unsigned char *next = (const unsigned char *)text;

	while (*next)
	{
		unsigned long code_point;
		int length, index;

		if (*next < 0x80) { code_point = *next; length = 1; }
		else if ((*next & 0xe0) == 0xc0) { code_point = *next & 0x1f; length = 2; }
		else if ((*next & 0xf0) == 0xe0) { code_point = *next & 0x0f; length = 3; }
		else if ((*next & 0xf8) == 0xf0) { code_point = *next & 0x07; length = 4; }
		else { next++; continue; }
		for (index = 1; index < length; index++)
		{
			if ((next[index] & 0xc0) != 0x80)
				break;
			code_point = (code_point << 6) | (next[index] & 0x3f);
		}
		next += index;
		if (index == length && code_point >= 0x20 && code_point < 0xd800 && code_point != 0x7f)
			queue_text_input(_halo_text_input_character, (unsigned short)code_point);
	}
}

/* the editing keys while typing (characters come as text input) */
static void queue_text_key(const SDL_KeyboardEvent *event)
{
	if (!event->down)
		return;
	switch (event->key)
	{
	case SDLK_BACKSPACE: queue_text_input(_halo_text_input_backspace, 0); break;
	case SDLK_DELETE: queue_text_input(_halo_text_input_delete, 0); break;
	case SDLK_LEFT: queue_text_input(_halo_text_input_left, 0); break;
	case SDLK_RIGHT: queue_text_input(_halo_text_input_right, 0); break;
	case SDLK_HOME: queue_text_input(_halo_text_input_home, 0); break;
	case SDLK_END: queue_text_input(_halo_text_input_end, 0); break;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		if (!event->repeat)
			queue_text_input(_halo_text_input_done, 0);
		break;
	case SDLK_ESCAPE:
		if (!event->repeat)
			queue_text_input(_halo_text_input_cancel, 0);
		break;
	default:
		break;
	}
}

/* turns SDL's text input on or off as the game wants, under input_lock on
the event thread */
static void text_input_update(void)
{
	if (text_input_wanted == text_input_started)
		return;
	if (text_input_wanted)
	{
		SDL_StartTextInput(platform_window);
	}
	else
	{
		SDL_StopTextInput(platform_window);
		memcpy(keys_held_over, input_state.keys, sizeof(keys_held_over));
		memset(keys_pressed, 0, sizeof(keys_pressed));
	}
	text_input_head = text_input_count = 0;
	text_input_started = text_input_wanted;
}

int halo_text_input_next(struct halo_text_input *input)
{
	int result = 0;

	pthread_mutex_lock(&input_lock);
	if (text_input_count)
	{
		*input = text_input_queue[text_input_head];
		text_input_head = (text_input_head + 1) % TEXT_INPUT_QUEUE_SIZE;
		text_input_count--;
		result = 1;
	}
	pthread_mutex_unlock(&input_lock);
	return result;
}

/* ---------- internet play's invite links (p2p.c) */

#ifdef HALO_ANDROID
/* SDL declares it for Android builds only, which the guest is not
(guest/runtime/guest_sdl.c passes it to the host) */
bool SDL_ShowAndroidToast(const char *message, int duration, int gravity, int xoffset, int yoffset);
#endif

/* puts a new invite on the clipboard, and joins one found there when the
game comes to the front */
static void platform_invite_clipboard(BOOL look)
{
	/* the last clipboard text looked at, so each invite is joined once */
	static char seen[256];
	const char *invite = p2p_take_clipboard_text();

	if (invite)
	{
		SDL_SetClipboardText(invite);
		snprintf(seen, sizeof(seen), "%s", invite);
		platform_log("Internet play: the invite link is on the clipboard");
#ifdef HALO_ANDROID
		SDL_ShowAndroidToast("Hosting: the invite link is on the clipboard", 1, -1, 0, 0);
#endif
	}
	if (look && config_boolean("network.join_from_clipboard"))
	{
		char *text = SDL_GetClipboardText();

		if (text && strcmp(text, seen) && strlen(text) < sizeof(seen))
		{
			snprintf(seen, sizeof(seen), "%s", text);
			if (p2p_join_invite(text))
			{
#ifdef HALO_ANDROID
				SDL_ShowAndroidToast("Joining the invite on the clipboard", 1, -1, 0, 0);
#endif
			}
		}
		SDL_free(text);
	}
}

/* ---------- messages for the player */

/* a message waiting for the event pump to show it (on the window's thread,
between frames) */
static pthread_mutex_t platform_message_lock = PTHREAD_MUTEX_INITIALIZER;
static char platform_message_title[80];
static char platform_message_text[600];
static BOOL platform_message_pending;

/* shows the player a message in a box of its own (the network code's: a
host of another version), and logs it */
void platform_show_message(const char *title, const char *message)
{
	platform_log("%s: %s", title, message);
	/* (a run nobody watches: the log only) */
	if (config_boolean("debug.hidden_window") || config_boolean("debug.null_renderer"))
		return;
	pthread_mutex_lock(&platform_message_lock);
	snprintf(platform_message_title, sizeof(platform_message_title), "%s", title);
	snprintf(platform_message_text, sizeof(platform_message_text), "%s", message);
	platform_message_pending = TRUE;
	pthread_mutex_unlock(&platform_message_lock);
}

static void platform_show_pending_message(void)
{
	char title[sizeof(platform_message_title)];
	char text[sizeof(platform_message_text)];
	BOOL pending;

	pthread_mutex_lock(&platform_message_lock);
	pending = platform_message_pending;
	platform_message_pending = FALSE;
	memcpy(title, platform_message_title, sizeof(title));
	memcpy(text, platform_message_text, sizeof(text));
	pthread_mutex_unlock(&platform_message_lock);
	if (!pending)
		return;
#ifdef HALO_ANDROID
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, title, text, NULL);
#else
	{
		/* (a box cannot show above a fullscreen game) */
		int fullscreen = (SDL_GetWindowFlags(platform_window) & SDL_WINDOW_FULLSCREEN) != 0;

		if (fullscreen)
			SDL_SetWindowFullscreen(platform_window, false);
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, title, text, platform_window);
		if (fullscreen)
			SDL_SetWindowFullscreen(platform_window, true);
	}
#endif
}

/* ---------- events */

void platform_pump_events(void)
{
	/* debug.exit_after (seconds) ends the game that long after the window
	opens, as closing it does (tools/pgo_train.py) */
	static Uint64 exit_ticks = (Uint64)-1;
	SDL_Event event;
	static BOOL looked_at_clipboard;
	BOOL look_at_clipboard = !looked_at_clipboard;
#ifndef HALO_ANDROID
	BOOL overlay_changed = FALSE;
#endif

	if (!platform_window || SDL_GetCurrentThreadID() != platform_event_thread)
		return;
	if (exit_ticks == (Uint64)-1)
	{
		double seconds = config_real("debug.exit_after");

		exit_ticks = seconds > 0.0 ? SDL_GetTicks() + (Uint64)(seconds * 1000.0) : 0;
	}
	if (exit_ticks && SDL_GetTicks() >= exit_ticks)
	{
		platform_log("exiting after debug.exit_after");
		exit(EXIT_SUCCESS);
	}
	platform_show_pending_message();
#ifndef HALO_ANDROID
	updater_poll(platform_window);
#endif
	pthread_mutex_lock(&input_lock);
	text_input_update();
	while (SDL_PollEvent(&event))
	{
#ifndef HALO_ANDROID
		/* the settings overlay takes the input while it is open */
		switch (overlay_handle_event(&event))
		{
		case _overlay_event_opened:
		case _overlay_event_closed:
			overlay_changed = TRUE;
			continue;
		case _overlay_event_consumed:
			continue;
		default:
			break;
		}
#endif
		switch (event.type)
		{
		case SDL_EVENT_QUIT:
			pthread_mutex_unlock(&input_lock);
			platform_log("window closed");
			exit(EXIT_SUCCESS);
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
			if (event.key.scancode < SDL_SCANCODE_COUNT)
			{
				input_state.keys[event.key.scancode] = event.key.down;
				if (event.key.down)
					keys_pressed[event.key.scancode] = 1;
				else
					keys_held_over[event.key.scancode] = 0;
			}
			queue_keystroke(&event.key);
			if (text_input_started)
				queue_text_key(&event.key);
			/* F12 releases or recaptures the mouse */
			if (event.key.down && !event.key.repeat && event.key.scancode == SDL_SCANCODE_F12)
			{
				input_state.mouse_released = !input_state.mouse_released;
				platform_mouse_capture(!input_state.mouse_released && !input_state.ui_pointer);
			}
#ifndef HALO_ANDROID
			/* F11 switches between fullscreen and the window (SDL keeps the
			window's size and place while fullscreen) */
			if (event.key.down && !event.key.repeat && event.key.scancode == SDL_SCANCODE_F11)
			{
				SDL_SetWindowFullscreen(platform_window,
					(SDL_GetWindowFlags(platform_window) & SDL_WINDOW_FULLSCREEN) ? false : true);
			}
#endif
			break;
		case SDL_EVENT_MOUSE_MOTION:
#ifndef HALO_ANDROID
			/* in the menus the mouse moves the pointer, not the view */
			if (input_state.ui_pointer)
			{
				ui_pointer.x = event.motion.x;
				ui_pointer.y = event.motion.y;
				ui_pointer.moved = TRUE;
				break;
			}
#endif
			input_state.mouse_dx += event.motion.xrel;
			input_state.mouse_dy += event.motion.yrel;
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
#ifndef HALO_ANDROID
			/* clicks in the menus go to the pointer; a button held down
			when the menu closes stays up until pressed again, so the click
			that resumes the game does not also fire */
			if (input_state.ui_pointer)
			{
				if (event.button.down && event.button.button == SDL_BUTTON_LEFT)
				{
					ui_pointer.left_clicks++;
					ui_pointer.click_x = event.button.x;
					ui_pointer.click_y = event.button.y;
				}
				else if (event.button.down && event.button.button == SDL_BUTTON_RIGHT)
				{
					ui_pointer.right_clicks++;
				}
				break;
			}
#endif
			if (event.button.button < PLATFORM_MOUSE_BUTTON_COUNT)
				input_state.mouse_buttons[event.button.button] = event.button.down;
			break;
		case SDL_EVENT_MOUSE_WHEEL:
#ifndef HALO_ANDROID
			if (input_state.ui_pointer)
			{
				/* whole notches: smooth-scrolling wheels send fractions */
				ui_pointer_wheel += event.wheel.y;
				while (ui_pointer_wheel >= 1.0f)
				{
					ui_pointer.wheel_steps++;
					ui_pointer_wheel -= 1.0f;
				}
				while (ui_pointer_wheel <= -1.0f)
				{
					ui_pointer.wheel_steps--;
					ui_pointer_wheel += 1.0f;
				}
				break;
			}
#endif
			input_state.mouse_wheel += event.wheel.y;
			break;
		case SDL_EVENT_TEXT_INPUT:
			if (text_input_started)
				queue_text(event.text.text);
			break;
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			memset(input_state.keys, 0, sizeof(input_state.keys));
			memset(keys_held_over, 0, sizeof(keys_held_over));
			memset(input_state.mouse_buttons, 0, sizeof(input_state.mouse_buttons));
			input_state.focused = FALSE;
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			input_state.focused = TRUE;
			look_at_clipboard = TRUE;
#ifndef HALO_ANDROID
			if (!input_state.mouse_released && !input_state.ui_pointer && !overlay_is_open())
				platform_mouse_capture(TRUE);
#endif
			break;
		case SDL_EVENT_GAMEPAD_ADDED:
			SDL_OpenGamepad(event.gdevice.which);
			break;
		default:
			break;
		}
	}
	pthread_mutex_unlock(&input_lock);
#ifndef HALO_ANDROID
	if (overlay_changed)
		platform_overlay_changed(overlay_is_open());
#endif
	looked_at_clipboard = TRUE;
	platform_invite_clipboard(look_at_clipboard);
}

#ifndef HALO_ANDROID
/* ---------- the menus' pointer */

/* While a menu is up the mouse is released, its pointer shows (centered when
the menu opens) and its motion, clicks and wheel go to the menus
(halo_ui_pointer_update, d3d8_gl.c) instead of the controller and the aim. */
void platform_ui_pointer_set_active(BOOL active)
{
	if (!platform_window || (active != FALSE) == (input_state.ui_pointer != FALSE))
		return;
	pthread_mutex_lock(&input_lock);
	input_state.ui_pointer = active;
	memset(&ui_pointer, 0, sizeof(ui_pointer));
	ui_pointer_wheel = 0.0f;
	input_state.mouse_dx = 0.0f;
	input_state.mouse_dy = 0.0f;
	input_state.mouse_wheel = 0.0f;
	memset(input_state.mouse_buttons, 0, sizeof(input_state.mouse_buttons));
	pthread_mutex_unlock(&input_lock);
	platform_mouse_capture(!active && !input_state.mouse_released && !overlay_is_open());
	if (active)
	{
		int width, height;

		SDL_GetWindowSize(platform_window, &width, &height);
		SDL_WarpMouseInWindow(platform_window, width * 0.5f, height * 0.5f);
		pthread_mutex_lock(&input_lock);
		ui_pointer.x = width * 0.5f;
		ui_pointer.y = height * 0.5f;
		pthread_mutex_unlock(&input_lock);
	}
}

/* what the pointer did since the last call; FALSE when it is not active */
BOOL platform_ui_pointer_read(struct platform_ui_pointer *pointer)
{
	BOOL active;

	pthread_mutex_lock(&input_lock);
	active = input_state.ui_pointer;
	*pointer = ui_pointer;
	ui_pointer.moved = FALSE;
	ui_pointer.left_clicks = 0;
	ui_pointer.right_clicks = 0;
	ui_pointer.wheel_steps = 0;
	pthread_mutex_unlock(&input_lock);
	return active;
}

void platform_video_window_size(int *width, int *height)
{
	SDL_GetWindowSize(platform_window, width, height);
}

/* ---------- the settings overlay's (overlay.c) */

enum platform_display_mode platform_video_display_mode(void)
{
	if (!platform_window || !(SDL_GetWindowFlags(platform_window) & SDL_WINDOW_FULLSCREEN))
		return _platform_display_windowed;
	return SDL_GetWindowFullscreenMode(platform_window) ? _platform_display_exclusive : _platform_display_borderless;
}

/* exclusive fullscreen takes the display, in its desktop mode, for the game
alone; borderless covers it with a window (SDL's fullscreen without a
mode). F11 switches between the fullscreen last chosen and the window. */
void platform_video_set_display_mode(enum platform_display_mode mode)
{
	if (!platform_window)
		return;
	if (mode == _platform_display_windowed)
	{
		SDL_SetWindowFullscreen(platform_window, false);
		return;
	}
	if (mode == _platform_display_exclusive)
	{
		SDL_DisplayID display = platform_display();
		const SDL_DisplayMode *desktop = display ? SDL_GetDesktopDisplayMode(display) : NULL;
		SDL_DisplayMode closest;

		if (desktop && SDL_GetClosestFullscreenDisplayMode(display, desktop->w, desktop->h, desktop->refresh_rate,
			true, &closest))
		{
			SDL_SetWindowFullscreenMode(platform_window, &closest);
		}
		else
		{
			platform_log("no exclusive fullscreen mode (%s); borderless instead", SDL_GetError());
			SDL_SetWindowFullscreenMode(platform_window, NULL);
		}
	}
	else
	{
		SDL_SetWindowFullscreenMode(platform_window, NULL);
	}
	SDL_SetWindowFullscreen(platform_window, true);
}

/* the window heights offered, low to high: common ones whose window, with
its frame, fits on the display; returns how many */
int platform_video_window_heights(int *heights, int maximum)
{
	static const int common[] = { 480, 600, 720, 768, 900, 960, 1080, 1200, 1440, 1600, 1800, 2160, 2880 };
	SDL_DisplayID display = platform_display();
	SDL_Rect bounds;
	int top = 32, count = 0, index;

	if (!display || !SDL_GetDisplayUsableBounds(display, &bounds))
	{
		bounds.w = 1280;
		bounds.h = 1024;
	}
	if (platform_window && platform_video_display_mode() == _platform_display_windowed)
		SDL_GetWindowBordersSize(platform_window, &top, NULL, NULL, NULL);
	for (index = 0; index < (int)(sizeof(common) / sizeof(common[0])) && count < maximum; index++)
	{
		if (index == 0 || (common[index] + top <= bounds.h && platform_video_window_width(common[index]) <= bounds.w))
			heights[count++] = common[index];
	}
	return count;
}

/* the window's size for a height (in the shape display.widescreen gives),
centered; only for the window */
void platform_video_set_window_height(int height)
{
	if (!platform_window || platform_video_display_mode() != _platform_display_windowed)
		return;
	SDL_SetWindowSize(platform_window, platform_video_window_width(height), height);
	SDL_SetWindowPosition(platform_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void platform_video_set_vsync(BOOL vsync)
{
	SDL_GL_SetSwapInterval(vsync ? 1 : 0);
}

/* the overlay opened or closed: while it is open the game gets no input
and the mouse is free */
static void platform_overlay_changed(BOOL open)
{
	pthread_mutex_lock(&input_lock);
	memset(input_state.keys, 0, sizeof(input_state.keys));
	memset(keys_pressed, 0, sizeof(keys_pressed));
	memset(input_state.mouse_buttons, 0, sizeof(input_state.mouse_buttons));
	input_state.mouse_dx = 0.0f;
	input_state.mouse_dy = 0.0f;
	input_state.mouse_wheel = 0.0f;
	ui_pointer.left_clicks = 0;
	ui_pointer.right_clicks = 0;
	ui_pointer.wheel_steps = 0;
	pthread_mutex_unlock(&input_lock);
	platform_mouse_capture(!open && !input_state.mouse_released && !input_state.ui_pointer);
	halo_overlay_pause(open);
}

#endif
void platform_input_read(struct platform_input_state *state, BOOL consume_motion)
{
	pthread_mutex_lock(&input_lock);
	*state = input_state;
	{
		int scancode;

		for (scancode = 0; scancode < SDL_SCANCODE_COUNT; scancode++)
		{
			if (consume_motion)
			{
				state->keys[scancode] |= keys_pressed[scancode];
				keys_pressed[scancode] = 0;
			}
			if (keys_held_over[scancode])
				state->keys[scancode] = 0;
		}
	}
	if (consume_motion)
	{
		input_state.mouse_dx = 0;
		input_state.mouse_dy = 0;
		input_state.mouse_wheel = 0;
	}
	pthread_mutex_unlock(&input_lock);
}
