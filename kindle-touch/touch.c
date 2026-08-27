/* touch.c -- native evdev touch injection for the Kindle Oasis.
 *
 * Replaces kindle-agent's Python-in-Alpine-chroot tap/swipe primitives.
 * The chroot dependency was fragile (its ext2 image lost its own libc
 * to filesystem corruption once already) and unnecessary -- this needs
 * nothing beyond raw writes to /dev/input/event4, which runs fine
 * directly on the Kindle's own rootfs with a static binary.
 *
 * Device: cyttsp4_mt multitouch touchscreen, /dev/input/event4.
 * Protocol: ABS_MT_TRACKING_ID (57), ABS_MT_POSITION_X (53),
 * ABS_MT_POSITION_Y (54), BTN_TOUCH (key 330), SYN_REPORT (0/0).
 * Same event sequence kindle-agent's tap()/swipe() already used --
 * this is a straight port, not a redesign of the protocol.
 *
 * Usage:
 *   touch tap X Y
 *   touch drag X1 Y1 X2 Y2 [STEPS] [DURATION_MS]
 *   touch doubletap X Y [GAP_MS]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <linux/input.h>

#define EVENT_DEV "/dev/input/event4"

static void write_ev(int fd, unsigned short type, unsigned short code, int value) {
	struct input_event ev;
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	memset(&ev, 0, sizeof(ev));
	ev.input_event_sec = ts.tv_sec;
	ev.input_event_usec = ts.tv_nsec / 1000;
	ev.type = type;
	ev.code = code;
	ev.value = value;
	if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
		perror("write event");
		exit(1);
	}
}

static void sleep_ms(int ms) {
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

static void touch_down(int fd, int x, int y) {
	write_ev(fd, EV_ABS, ABS_MT_TRACKING_ID, 1);
	write_ev(fd, EV_ABS, ABS_MT_POSITION_X, x);
	write_ev(fd, EV_ABS, ABS_MT_POSITION_Y, y);
	write_ev(fd, EV_KEY, BTN_TOUCH, 1);
	write_ev(fd, EV_SYN, SYN_REPORT, 0);
}

static void touch_move(int fd, int x, int y) {
	write_ev(fd, EV_ABS, ABS_MT_POSITION_X, x);
	write_ev(fd, EV_ABS, ABS_MT_POSITION_Y, y);
	write_ev(fd, EV_SYN, SYN_REPORT, 0);
}

static void touch_up(int fd) {
	write_ev(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
	write_ev(fd, EV_KEY, BTN_TOUCH, 0);
	write_ev(fd, EV_SYN, SYN_REPORT, 0);
}

static int open_dev(void) {
	int fd = open(EVENT_DEV, O_WRONLY);
	if (fd < 0) {
		perror("open " EVENT_DEV);
		exit(1);
	}
	return fd;
}

static void cmd_tap(int x, int y) {
	int fd = open_dev();
	touch_down(fd, x, y);
	sleep_ms(120);
	touch_up(fd);
	close(fd);
}

static void cmd_doubletap(int x, int y, int gap_ms) {
	int fd = open_dev();
	touch_down(fd, x, y);
	sleep_ms(100);
	touch_up(fd);
	sleep_ms(gap_ms);
	touch_down(fd, x, y);
	sleep_ms(100);
	touch_up(fd);
	close(fd);
}

static void cmd_drag(int x1, int y1, int x2, int y2, int steps, int duration_ms) {
	int fd = open_dev();
	int step_ms = duration_ms / (steps > 1 ? steps : 1);
	touch_down(fd, x1, y1);
	for (int i = 1; i < steps; ++i) {
		int x = x1 + (x2 - x1) * i / (steps - 1);
		int y = y1 + (y2 - y1) * i / (steps - 1);
		sleep_ms(step_ms);
		touch_move(fd, x, y);
	}
	sleep_ms(step_ms);
	touch_up(fd);
	close(fd);
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr,
			"usage:\n"
			"  %s tap X Y\n"
			"  %s doubletap X Y [GAP_MS=150]\n"
			"  %s drag X1 Y1 X2 Y2 [STEPS=10] [DURATION_MS=300]\n",
			argv[0], argv[0], argv[0]);
		return 1;
	}

	if (0 == strcmp(argv[1], "tap") && argc == 4) {
		cmd_tap(atoi(argv[2]), atoi(argv[3]));
	} else if (0 == strcmp(argv[1], "doubletap") && (argc == 4 || argc == 5)) {
		int gap = (argc == 5) ? atoi(argv[4]) : 150;
		cmd_doubletap(atoi(argv[2]), atoi(argv[3]), gap);
	} else if (0 == strcmp(argv[1], "drag") && (argc == 6 || argc == 7 || argc == 8)) {
		int steps = (argc >= 7) ? atoi(argv[6]) : 10;
		int duration = (argc >= 8) ? atoi(argv[7]) : 300;
		cmd_drag(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]),
			steps, duration);
	} else {
		fprintf(stderr, "bad arguments\n");
		return 1;
	}

	printf("ok\n");
	return 0;
}
