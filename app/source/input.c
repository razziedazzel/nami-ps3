#include "input.h"

#include <io/pad.h>
#include <string.h>

#include "config.h"

#define REPEAT_START_FRAMES 22
#define REPEAT_EVERY_FRAMES 7
#define STICK_CENTER 128
#define STICK_DEAD 70
#define BD_NO_KEY 0xFFFF
#define DEVICE_REMOTE 4

static int held[ACT_COUNT];
static int fired[ACT_COUNT];
static int last_device;
static padData last_data[MAX_PORT_NUM];
static int have_data[MAX_PORT_NUM];
static unsigned last_logged_code = BD_NO_KEY;

void input_init(void)
{
	ioPadInit(7);
	memset(last_data, 0, sizeof(last_data));
}

/* A remote key press as an action, or -1. Remotes also have shape buttons and a d-pad of their own. */
static int action_for_bd(unsigned code)
{
	switch (code) {
	case BTN_BD_UP: return ACT_UP;
	case BTN_BD_DOWN: return ACT_DOWN;
	case BTN_BD_LEFT: return ACT_LEFT;
	case BTN_BD_RIGHT: return ACT_RIGHT;
	case BTN_BD_ENTER: case BTN_BD_CROSS: return ACT_OK;
	case BTN_BD_RETURN: case BTN_BD_CIRCLE: return ACT_BACK;
	case BTN_BD_TRIANGLE: case BTN_BD_POPUP_MENU: return ACT_OPTION;
	case BTN_BD_SQUARE: case BTN_BD_TOPMENU: return ACT_MENU;
	case BTN_BD_SELECT: return ACT_SELECT;
	case BTN_BD_PLAY: case BTN_BD_START: return ACT_PLAY;
	case BTN_BD_PAUSE: return ACT_PAUSE;
	case BTN_BD_STOP: return ACT_STOP;
	case BTN_BD_NEXT: case BTN_BD_R1: return ACT_NEXT;
	case BTN_BD_PREV: case BTN_BD_L1: return ACT_PREV;
	/* L2 and R2 stay unused on purpose: they're too easy to press by accident. */
	case BTN_BD_SCAN_FWD: return ACT_FORWARD;
	case BTN_BD_SCAN_REV: return ACT_REWIND;
	case BTN_BD_SUBTITLE: return ACT_SUBTITLE;
	case BTN_BD_AUDIO: return ACT_AUDIO;
	default: return -1;
	}
}

void input_update(void)
{
	padInfo2 info;
	int down[ACT_COUNT];
	memset(down, 0, sizeof(down));

	ioPadGetInfo2(&info);
	for (int i = 0; i < MAX_PORT_NUM; i++) {
		if (!(info.port_status[i] & 1))
			continue;

		padData data;
		/* No new data means nothing changed, so a held button keeps its last state. */
		if (ioPadGetData(i, &data) == 0 && data.len > 0) {
			last_data[i] = data;
			have_data[i] = 1;
		}
		if (!have_data[i])
			continue;
		const padData *d = &last_data[i];

		if (info.device_type[i] != DEVICE_REMOTE) {
			int before[ACT_COUNT];
			memcpy(before, down, sizeof(down));
			down[ACT_UP] |= d->BTN_UP || d->ANA_L_V < STICK_CENTER - STICK_DEAD;
			down[ACT_DOWN] |= d->BTN_DOWN || d->ANA_L_V > STICK_CENTER + STICK_DEAD;
			down[ACT_LEFT] |= d->BTN_LEFT || d->ANA_L_H < STICK_CENTER - STICK_DEAD;
			down[ACT_RIGHT] |= d->BTN_RIGHT || d->ANA_L_H > STICK_CENTER + STICK_DEAD;
			down[ACT_OK] |= d->BTN_CROSS;
			down[ACT_BACK] |= d->BTN_CIRCLE;
			down[ACT_OPTION] |= d->BTN_TRIANGLE;
			down[ACT_MENU] |= d->BTN_SQUARE;
			down[ACT_SELECT] |= d->BTN_SELECT;
			down[ACT_PLAY] |= d->BTN_START;
			down[ACT_NEXT] |= d->BTN_R1;
			down[ACT_PREV] |= d->BTN_L1;
			if (memcmp(before, down, sizeof(down)) != 0)
				last_device = 1;
		}

		/* Only a remote's BD fields mean anything; on a controller they hold other data. */
		unsigned code = d->BTN_BDCODE;
		if (info.device_type[i] == DEVICE_REMOTE && d->BTN_BDLEN > 0 && code != BD_NO_KEY) {
			int action = action_for_bd(code);
			if (action >= 0)
				down[action] = 1;
			last_device = 2;
			if (code != last_logged_code) {
				app_log("remote port=%d type=%u code=%02x action=%d\n", i,
				        info.device_type[i], code, action);
				last_logged_code = code;
			}
		} else {
			last_logged_code = BD_NO_KEY;
		}
	}

	for (int a = 0; a < ACT_COUNT; a++) {
		fired[a] = 0;
		if (!down[a]) {
			held[a] = 0;
			continue;
		}
		held[a]++;
		int repeats = a <= ACT_RIGHT;
		if (held[a] == 1 || (repeats && held[a] >= REPEAT_START_FRAMES &&
		                     (held[a] - REPEAT_START_FRAMES) % REPEAT_EVERY_FRAMES == 0))
			fired[a] = 1;
	}
}

int input_pressed(enum input_action action)
{
	return fired[action];
}

int input_last_device(void)
{
	return last_device;
}
