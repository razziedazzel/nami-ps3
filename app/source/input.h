#ifndef INPUT_H
#define INPUT_H

/* The same actions from a controller or the Blu-ray remote. */
enum input_action {
	ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT, ACT_OK, ACT_BACK, ACT_OPTION, ACT_MENU, ACT_SELECT,
	ACT_PLAY, ACT_PAUSE, ACT_STOP, ACT_NEXT, ACT_PREV, ACT_FORWARD, ACT_REWIND,
	ACT_SUBTITLE, ACT_AUDIO, ACT_COUNT
};

void input_init(void);
/* Call once per frame. */
void input_update(void);
/* True once on the press, then repeatedly while held for directions. */
int input_pressed(enum input_action action);
/* What was used last: 0 nothing yet, 1 controller, 2 remote. */
int input_last_device(void);

#endif
