/* wario-companion -- keyboard/trackpad companion window for mini vMac
 * on the Kindle Oasis.
 *
 * One window, one mode-toggle button, two panels:
 *   - Keyboard: a basic QWERTY grid.
 *   - Trackpad: a blank draggable rectangle translating finger motion
 *     into a software-tracked virtual cursor position, plus explicit
 *     click/double-click/triple-click/mouse-down/mouse-up buttons.
 *
 * Input delivery to mini vMac: XSendEvent targeted directly at its window
 * (found by title substring match), NOT XTest. mini vMac's own event
 * loop (OSGLUXWN.c) checks `event->window == my_main_wind` explicitly
 * for every event type and never inspects the send_event flag, so a
 * correctly-addressed synthetic event is indistinguishable from a real
 * one to it. This matters because XTestFakeButtonEvent/FakeKeyEvent
 * inject at the REAL pointer position / REAL input focus -- which,
 * for a touchscreen companion app, is wherever the user's finger just
 * tapped one of THIS window's own buttons, not mini vMac. XSendEvent
 * sidesteps that dependency entirely by naming the target window.
 *
 * Startup focus/visibility: lab126's WM (see
 * /etc/xdg/awesome/lab126_application_layer.lua's applicationLayer_layout)
 * auto-focuses every newly-mapped window whose title parses as an L:A
 * N:application client -- no self-raise/self-tap code needed at all.
 * The real, non-obvious bug here was that the app ID in this window's
 * title (`net.gryphel.wario_companion`) contained an underscore, which
 * collided with `_` as the field separator in the
 * `L:A_N:application_ID:..._M:false_...` title format itself, corrupting
 * the WM's parse. Confirmed via /var/log/messages on-device:
 * `WindowManager:bad-client-name ... window does not conform to winmgr
 * naming convention - leaving hidden`. The window was silently HIDDEN
 * the entire time -- not unfocused, not mis-stacked, just never shown.
 * Fixed by dropping the underscore (`wariocompanion`); XRaiseWindow,
 * EWMH _NET_ACTIVE_WINDOW, and XTestFakeButtonEvent self-taps were all
 * tried and discarded chasing this before the real cause was found in
 * the WM's own log.
 *
 * Own code, own license (MIT, matches wario-sdk) -- no GPL question:
 * unlike kterm's keyboard.c (GPLv3) or matchbox-keyboard (LGPLv2.1),
 * this never touches mini vMac's GPLv2 source, and reuse of either
 * existing keyboard would mean juggling separate windows/processes for
 * a UI that's supposed to be one window with one toggle button. Reuses
 * the same lab126 awesome-WM WM_NAME trick as mini vMac's own windows.
 *
 * Touch feedback: a gray circle flashes wherever the user touches,
 * regardless of which widget (button or trackpad) is underneath --
 * implemented via gdk_event_handler_set(), GDK's global event-snooping
 * hook, since a normal per-widget signal handler only sees events for
 * that one widget and GTK2 has no compositor/overlay layer to draw a
 * layer above arbitrary children.
 *
 * Pen mode: real touches on mini vMac's own screen (see the "Pen mode"
 * section) don't go through mini vMac's normal ButtonPress/
 * ButtonRelease path at all -- that path has an unreliable timing quirk
 * (confirmed: drags get a spurious release ~400ms in, regardless of
 * the real finger's actual state), so mini vMac now unconditionally
 * ignores real button events and trusts only synthetic ones. Instead
 * this reads /dev/input/event4 directly (non-exclusively, in parallel
 * with the normal X11/Xorg touch pipeline -- Linux evdev supports
 * multiple concurrent readers), watches BTN_TOUCH transitions, and
 * sends a clean synthetic ButtonPress on touch-down / ButtonRelease on
 * touch-up when the touch lands within mini vMac's own screen bounds
 * and pen mode is on. Real MotionNotify already works fine natively
 * and is left completely untouched; only the button-state edges are
 * replaced.
 */

#include <gtk/gtk.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

/* mini vMac's window title always contains this (see OSGLUXWN.c's
 * win_name literal); matched as a substring so we don't depend on the
 * rest of the L:A_N:application_... layer-tag formatting. */
#define MINIVMAC_TITLE_NEEDLE "net.gryphel.minivmac"

static Display *xdpy;
static Window minivmac_window = None;
static int minivmac_w = 1024, minivmac_h = 684; /* refreshed on lookup */
static gboolean caps_on = FALSE;
static GtkWidget *notebook;
static int virt_x, virt_y; /* software-tracked cursor pos, window-relative */
static gboolean trackpad_dragging = FALSE;
static double trackpad_last_x, trackpad_last_y;
static double tap_move_accum = 0.0; /* total |dx|+|dy| since press, for
                                        tap-vs-drag on the trackpad */
static guint pending_tap_click_timer = 0; /* nonzero while a single tap
                                              is waiting to see if a
                                              second tap upgrades it to
                                              a double-click */
static int pending_tap_x, pending_tap_y; /* cursor position AT THE TIME
                                             of that tap -- the deferred
                                             click must land here, not
                                             wherever virt_x/virt_y have
                                             since drifted to (see
                                             on_pad_button_release) */
static gboolean drag_committed = FALSE; /* this touch already crossed
                                            TAP_MAX_MOVE_PX while still
                                            held, so it's a real click-
                                            and-drag (button down since
                                            the moment it committed),
                                            not a plain cursor move */
static GtkWidget *companion_window; /* set in main(), used by touch indicator */
static gboolean pen_mode_active = TRUE; /* TRUE: real touches on mini
                                            vMac's own screen behave as
                                            they always have -- button
                                            down while moving, since
                                            touching the digitizer IS
                                            pressing. FALSE: button
                                            press/release on mini vMac's
                                            window get swallowed (never
                                            forwarded), motion still
                                            passes through cleanly, so
                                            you can nudge the real
                                            on-screen cursor without it
                                            drawing/clicking anything. */

/* ---- Touch feedback: gray circle flashes wherever the user touches ---- */

#define TOUCH_INDICATOR_RADIUS 26
#define TOUCH_INDICATOR_MS 900 /* e-ink needs real time to render a
                                   partial refresh at all -- a fast
                                   ~200ms fade would likely get
                                   coalesced away by the EPDC driver
                                   before ever becoming visible */

typedef struct {
	GdkWindow *win;
	GdkRectangle rect;
} TouchFadeCtx;

static gboolean touch_indicator_fade(gpointer data) {
	TouchFadeCtx *ctx = data;
	gdk_window_invalidate_rect(ctx->win, &ctx->rect, TRUE);
	g_free(ctx);
	return FALSE; /* one-shot */
}

/* Draws directly onto the toplevel's own GdkWindow, bypassing normal
 * widget expose/redraw -- simplest way to paint something that spans
 * every child widget (buttons, trackpad) without a GTK3-style overlay
 * container, which GTK2 doesn't have. Self-erasing: schedules a
 * one-shot invalidate of just that rectangle shortly after, which
 * triggers GTK's normal expose machinery to redraw whatever was
 * actually there underneath. */
static void touch_indicator_show(gint x, gint y) {
	GdkWindow *win = gtk_widget_get_window(companion_window);
	if (win == NULL) return;

	cairo_t *cr = gdk_cairo_create(win);
	cairo_set_source_rgba(cr, 0.35, 0.35, 0.35, 0.5);
	cairo_arc(cr, x, y, TOUCH_INDICATOR_RADIUS, 0, 2 * G_PI);
	cairo_fill(cr);
	cairo_destroy(cr);

	TouchFadeCtx *ctx = g_new(TouchFadeCtx, 1);
	ctx->win = win;
	ctx->rect.x = x - TOUCH_INDICATOR_RADIUS - 2;
	ctx->rect.y = y - TOUCH_INDICATOR_RADIUS - 2;
	ctx->rect.width = ctx->rect.height = 2 * (TOUCH_INDICATOR_RADIUS + 2);
	g_timeout_add(TOUCH_INDICATOR_MS, touch_indicator_fade, ctx);
}

/* GDK's global event hook -- sees every event bound for any widget in
 * the window BEFORE normal per-widget dispatch, which a regular signal
 * handler on one widget (a button, the trackpad) cannot do; this is
 * what lets the indicator appear no matter what's under the finger.
 * Must forward every event to gtk_main_do_event() -- installing this
 * handler replaces GTK's own dispatch entirely, app-wide. */
static void global_event_snoop(GdkEvent *event, gpointer data) {
	(void)data;
	if (event->type == GDK_BUTTON_PRESS) {
		GdkWindow *toplevel = gdk_window_get_toplevel(event->button.window);
		gint origin_x, origin_y;
		gdk_window_get_origin(toplevel, &origin_x, &origin_y);
		touch_indicator_show(
			(gint)event->button.x_root - origin_x,
			(gint)event->button.y_root - origin_y);
	}
	gtk_main_do_event(event);
}

/* ---- Window discovery ---- */

static Window find_window_by_title(Display *dpy, Window w, const char *needle) {
	char *name = NULL;
	if (XFetchName(dpy, w, &name) && name != NULL) {
		gboolean match = (strstr(name, needle) != NULL);
		XFree(name);
		if (match) return w;
	}

	Window root_ret, parent_ret, *children = NULL;
	unsigned int nchildren = 0;
	if (!XQueryTree(dpy, w, &root_ret, &parent_ret, &children, &nchildren)) {
		return None;
	}
	Window found = None;
	for (unsigned int i = 0; i < nchildren; ++i) {
		found = find_window_by_title(dpy, children[i], needle);
		if (found != None) break;
	}
	if (children != NULL) XFree(children);
	return found;
}

/* Re-resolves mini vMac's window (and caches its content size) if we
 * don't have it yet. Returns None if it's not up. Called lazily
 * before every injection rather than once at startup, so a restarted
 * mini vMac gets picked up without restarting the companion. */
static Window minivmac(void) {
	if (minivmac_window != None) {
		/* Confirm it's still alive/valid before trusting the cache. */
		XWindowAttributes attrs;
		if (XGetWindowAttributes(xdpy, minivmac_window, &attrs)) {
			return minivmac_window;
		}
		minivmac_window = None;
	}
	Window w = find_window_by_title(xdpy, DefaultRootWindow(xdpy),
		MINIVMAC_TITLE_NEEDLE);
	if (w == None) {
		fprintf(stderr, "wario-companion: mini vMac window not found\n");
		return None;
	}
	XWindowAttributes attrs;
	if (XGetWindowAttributes(xdpy, w, &attrs)) {
		minivmac_w = attrs.width;
		minivmac_h = attrs.height;
	}
	minivmac_window = w;
	virt_x = minivmac_w / 2;
	virt_y = minivmac_h / 2;
	return w;
}

/* ---- Synthetic event delivery ---- */

static void send_key(KeyCode kc, gboolean press, unsigned int state) {
	Window target = minivmac();
	if (target == None || kc == 0) return;
	XEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = press ? KeyPress : KeyRelease;
	ev.xkey.display = xdpy;
	ev.xkey.window = target;
	ev.xkey.root = DefaultRootWindow(xdpy);
	ev.xkey.subwindow = None;
	ev.xkey.time = CurrentTime;
	ev.xkey.x = virt_x; ev.xkey.y = virt_y;
	ev.xkey.same_screen = True;
	ev.xkey.state = state;
	ev.xkey.keycode = kc;
	XSendEvent(xdpy, target, False, NoEventMask, &ev);
}

/* mini vMac's main loop drains its whole pending X event queue before
 * running any 68k CPU emulation; XSendEvent-ing a press immediately
 * followed by a release (zero delay) means it can see BOTH in the same
 * drain and only ever observe the final (released) state -- the down
 * edge is never visible to the emulated ROM. A real keypress naturally
 * holds for tens of ms; this holds synthetic ones the same way. */
#define KEY_HOLD_US 40000

static void send_key_tap(KeySym ks) {
	KeyCode kc = XKeysymToKeycode(xdpy, ks);
	send_key(kc, TRUE, 0);
	XFlush(xdpy);
	g_usleep(KEY_HOLD_US);
	send_key(kc, FALSE, 0);
	XFlush(xdpy);
}

/* Sends a real Shift keydown/up around the letter, matching how a
 * physical keyboard produces uppercase -- mini vMac tracks Shift as
 * its own key, not via event.state. */
static void send_key_tap_shifted(KeySym ks) {
	KeyCode shift = XKeysymToKeycode(xdpy, XK_Shift_L);
	send_key(shift, TRUE, 0);
	XFlush(xdpy);
	g_usleep(KEY_HOLD_US);
	KeyCode kc = XKeysymToKeycode(xdpy, ks);
	send_key(kc, TRUE, ShiftMask);
	XFlush(xdpy);
	g_usleep(KEY_HOLD_US);
	send_key(kc, FALSE, ShiftMask);
	XFlush(xdpy);
	g_usleep(KEY_HOLD_US);
	send_key(shift, FALSE, 0);
	XFlush(xdpy);
}

static void send_button_at(gboolean press, int x, int y) {
	Window target = minivmac();
	if (target == None) return;
	XEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = press ? ButtonPress : ButtonRelease;
	ev.xbutton.display = xdpy;
	ev.xbutton.window = target;
	ev.xbutton.root = DefaultRootWindow(xdpy);
	ev.xbutton.subwindow = None;
	ev.xbutton.time = CurrentTime;
	ev.xbutton.x = x; ev.xbutton.y = y;
	ev.xbutton.same_screen = True;
	ev.xbutton.button = Button1;
	ev.xbutton.state = press ? 0 : Button1Mask;
	XSendEvent(xdpy, target, False, NoEventMask, &ev);
	XFlush(xdpy);
}

static void send_button(gboolean press) {
	send_button_at(press, virt_x, virt_y);
}

static void send_motion(void) {
	Window target = minivmac();
	if (target == None) return;
	XEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = MotionNotify;
	ev.xmotion.display = xdpy;
	ev.xmotion.window = target;
	ev.xmotion.root = DefaultRootWindow(xdpy);
	ev.xmotion.subwindow = None;
	ev.xmotion.time = CurrentTime;
	ev.xmotion.x = virt_x; ev.xmotion.y = virt_y;
	ev.xmotion.same_screen = True;
	ev.xmotion.is_hint = 0;
	XSendEvent(xdpy, target, False, NoEventMask, &ev);
	XFlush(xdpy);
}

/* Positional variant used by the trackpad's deferred tap-click timer
 * (see on_pad_button_release): the click must land where the ORIGINAL
 * tap happened, not wherever virt_x/virt_y have drifted to by the time
 * the timer fires -- see the fix's own comment there for why this
 * matters. */
static void send_click_burst_at(int n, int x, int y) {
	for (int i = 0; i < n; ++i) {
		send_button_at(TRUE, x, y);
		g_usleep(KEY_HOLD_US); /* same drain-before-CPU-step issue as
		                          send_key_tap -- see its comment */
		send_button_at(FALSE, x, y);
		if (i + 1 < n) {
			g_usleep(80000); /* comfortably inside mini vMac's
			                     double-click window (-dct default) */
		}
	}
}

static void send_click_burst(int n) {
	send_click_burst_at(n, virt_x, virt_y);
}

/* ---- Pen mode: real digitizer touches on mini vMac's own window ----
 *
 * Ryan's spec: touching mini vMac's own screen area with pen mode on
 * IS a mouse-down, for as long as the touch lasts -- mouse-down on
 * touch, mouse-up on un-touch, full stop. With it off, the same
 * touches just nudge the cursor.
 *
 * First attempt relied on mini vMac's own native touch-to-mouse
 * translation (i.e. did nothing extra for "pen mode on" beyond passing
 * events through) plus XGrabButton to swallow button state for "off".
 * That crashed outright: BadAccess (request_code 28 = X_GrabButton) --
 * lab126's awesome WM almost certainly already holds its own grab on
 * this window for tap-to-focus (lab126_button_handling.lua), so a
 * second app-level grab on the same button/window conflicts. It also
 * turned out the native translation's own button-release timing is
 * unreliable regardless: real drags on mini vMac's screen got a
 * spurious release ~400ms in no matter what, since pen mode was pure
 * passthrough and never touched that path at all.
 *
 * Fixed properly: don't trust the native touch-to-mouse translation's
 * button-state timing at all, in either mode. mini vMac's own
 * ButtonPress/ButtonRelease case now unconditionally ignores every
 * real (send_event=False) button event and only ever trusts synthetic
 * ones (see OSGLUXWN.c) -- real MotionNotify is untouched since that
 * part already worked fine. This reads /dev/input/event4 directly,
 * non-exclusively (Linux evdev supports multiple concurrent readers;
 * the normal X11/Xorg touch pipeline keeps running alongside this,
 * untouched, still driving companion's own widgets and real cursor
 * motion on mini vMac normally), and watches BTN_TOUCH transitions
 * directly -- these fire the instant the digitizer reports finger
 * contact/release, with none of the driver's own gesture-recognition
 * layered on top. On touch-down, if pen mode is on and the touch is
 * within mini vMac's screen bounds, sends one synthetic ButtonPress;
 * on touch-up, if that press was sent, one matching ButtonRelease.
 * Nothing else -- no click-counting, no double/triple-click logic (Ryan's
 * point: a direct 1:1 touch-down/touch-up mirror of real mouse
 * behavior lets mini vMac's own ROM-level click recognition work
 * naturally off real timing, exactly like it would with a real mouse).
 */

#define TOUCH_EVENT_DEV "/dev/input/event4"
static int touch_evdev_fd = -1;
static int touch_cur_x = -1, touch_cur_y = -1;
static gboolean touch_press_sent = FALSE;
static gboolean touch_down_pending = FALSE; /* BTN_TOUCH=1 seen, but not
                                                yet decided whether to
                                                send a press -- deferred
                                                to the next SYN_REPORT
                                                since real hardware
                                                reports BTN_TOUCH BEFORE
                                                the touch's own position
                                                (confirmed live: the
                                                opposite order from
                                                kindle-touch's synthetic
                                                sequence, which is why
                                                scripted testing never
                                                caught this -- deciding
                                                at BTN_TOUCH=1 itself
                                                used stale/uninitialized
                                                position and silently
                                                never sent a press at
                                                all, so the touch-up
                                                had nothing to release
                                                either, leaving mini
                                                vMac's button state
                                                stuck down across
                                                strokes -- Ryan's "two
                                                circles joined by a
                                                straight line" bug). */

static gboolean on_touch_evdev_event(GIOChannel *source, GIOCondition condition,
	gpointer data)
{
	(void)source; (void)condition; (void)data;
	struct input_event ev;
	ssize_t n;
	while ((n = read(touch_evdev_fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
		if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) {
			touch_cur_x = ev.value;
		} else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) {
			touch_cur_y = ev.value;
		} else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
			if (ev.value == 1) {
				touch_down_pending = TRUE;
			} else if (ev.value == 0) {
				touch_down_pending = FALSE;
				if (touch_press_sent) {
					send_button_at(FALSE, touch_cur_x, touch_cur_y);
					touch_press_sent = FALSE;
				}
			}
		} else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			if (touch_down_pending) {
				Window target = minivmac();
				if (pen_mode_active && target != None &&
					touch_cur_x >= 0 && touch_cur_y >= 0 &&
					touch_cur_x < minivmac_w && touch_cur_y < minivmac_h)
				{
					send_button_at(TRUE, touch_cur_x, touch_cur_y);
					touch_press_sent = TRUE;
				}
				touch_down_pending = FALSE;
			}
		}
	}
	return TRUE; /* keep watching */
}

/* Opens the touch device and installs the watch; call once from main()
 * after the GTK main loop is set up. Non-fatal if it fails -- pen mode
 * just won't do anything extra, same as before this feature existed. */
static void pen_mode_engine_start(void) {
	touch_evdev_fd = open(TOUCH_EVENT_DEV, O_RDONLY | O_NONBLOCK);
	if (touch_evdev_fd < 0) {
		fprintf(stderr,
			"wario-companion: couldn't open %s for pen mode: %s\n",
			TOUCH_EVENT_DEV, strerror(errno));
		return;
	}
	GIOChannel *chan = g_io_channel_unix_new(touch_evdev_fd);
	g_io_add_watch(chan, G_IO_IN, on_touch_evdev_event, NULL);
}

static void on_toggle_pen_mode(GtkWidget *w, gpointer d) {
	(void)d;
	pen_mode_active = !pen_mode_active;
	gtk_button_set_label(GTK_BUTTON(w),
		pen_mode_active ? "pen mode: ON" : "pen mode: OFF (nudge only)");
}

/* ---- Keyboard panel ---- */

static void on_letter(GtkWidget *w, gpointer data) {
	(void)w;
	char lower = ((const char *)data)[0];
	char upper = (lower >= 'a' && lower <= 'z') ? (lower - 32) : lower;
	if (caps_on) {
		send_key_tap_shifted(XK_A + (upper - 'A'));
	} else {
		send_key_tap(XK_a + (lower - 'a'));
	}
}

static void free_label(gpointer data) { g_free(data); }

static void on_space(GtkWidget *w, gpointer d) { (void)w; (void)d; send_key_tap(XK_space); }
static void on_return(GtkWidget *w, gpointer d) { (void)w; (void)d; send_key_tap(XK_Return); }
static void on_backspace(GtkWidget *w, gpointer d) { (void)w; (void)d; send_key_tap(XK_BackSpace); }
static void on_shift(GtkWidget *w, gpointer d) {
	(void)d;
	caps_on = !caps_on;
	gtk_button_set_label(GTK_BUTTON(w), caps_on ? "SHIFT*" : "shift");
}

static GtkWidget *make_key(const char *label, GCallback cb, gpointer data) {
	GtkWidget *btn = gtk_button_new_with_label(label);
	g_signal_connect(btn, "clicked", cb, data);
	return btn;
}

/* Like make_key, but takes ownership of a heap-allocated label/data
 * string and frees it when the button is destroyed. */
static GtkWidget *make_key_owned(char *label, GCallback cb) {
	GtkWidget *btn = gtk_button_new_with_label(label);
	g_signal_connect_data(btn, "clicked", cb, label, (GClosureNotify)free_label, 0);
	return btn;
}

static GtkWidget *build_keyboard_panel(void) {
	GtkWidget *vbox = gtk_vbox_new(TRUE, 2);
	const char *rows[3] = {
		"qwertyuiop",
		"asdfghjkl",
		"zxcvbnm",
	};

	for (int r = 0; r < 3; ++r) {
		GtkWidget *hbox = gtk_hbox_new(TRUE, 2);
		if (r == 2) {
			gtk_box_pack_start(GTK_BOX(hbox),
				make_key("shift", G_CALLBACK(on_shift), NULL),
				TRUE, TRUE, 0);
		}
		for (const char *c = rows[r]; *c; ++c) {
			char *label = g_strdup_printf("%c", *c);
			gtk_box_pack_start(GTK_BOX(hbox),
				make_key_owned(label, G_CALLBACK(on_letter)),
				TRUE, TRUE, 0);
		}
		if (r == 2) {
			gtk_box_pack_start(GTK_BOX(hbox),
				make_key("<-", G_CALLBACK(on_backspace), NULL),
				TRUE, TRUE, 0);
		}
		gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);
	}

	{
		GtkWidget *hbox = gtk_hbox_new(TRUE, 2);
		gtk_box_pack_start(GTK_BOX(hbox), make_key("space", G_CALLBACK(on_space), NULL), TRUE, TRUE, 0);
		gtk_box_pack_start(GTK_BOX(hbox), make_key("return", G_CALLBACK(on_return), NULL), TRUE, TRUE, 0);
		gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);
	}

	return vbox;
}

/* ---- Trackpad panel ---- */

#define TAP_MAX_MOVE_PX 12.0 /* stay under this total movement to count
                                 as a tap rather than a drag */
#define DOUBLE_TAP_WINDOW_MS 400 /* real wall-clock gap we treat as
                                     "the same double-tap gesture";
                                     unrelated to mini vMac's own -dct,
                                     which send_click_burst(2) already
                                     handles with correct internal
                                     spacing regardless of how far apart
                                     the two real taps were */

static gboolean fire_pending_tap_click(gpointer data) {
	(void)data;
	pending_tap_click_timer = 0;
	/* Position captured at tap time, NOT live virt_x/virt_y -- if a
	 * later, unrelated drag has since moved the cursor mid-stroke
	 * (e.g. drawing in MacPaint), firing at the CURRENT position would
	 * inject a stray click into the middle of that drag, ending it
	 * with an unwanted button-up. Ryan hit exactly this: "clicks and
	 * drags draw for a fixed amount of time, then are mouseup'd." */
	send_click_burst_at(1, pending_tap_x, pending_tap_y);
	return FALSE; /* one-shot */
}

static gboolean on_pad_button_press(GtkWidget *w, GdkEventButton *ev, gpointer d) {
	(void)w; (void)d;
	trackpad_dragging = TRUE;
	trackpad_last_x = ev->x;
	trackpad_last_y = ev->y;
	tap_move_accum = 0.0;
	drag_committed = FALSE;
	return TRUE;
}

static gboolean on_pad_button_release(GtkWidget *w, GdkEventButton *ev, gpointer d) {
	(void)w; (void)ev; (void)d;
	trackpad_dragging = FALSE;
	if (drag_committed) {
		/* This touch crossed the movement threshold while still held,
		 * so it's a real click-and-drag (see on_pad_motion) -- release
		 * the button we've been holding since it committed. */
		send_button(FALSE);
		return TRUE;
	}
	/* Never crossed the threshold: a plain tap. */
	if (pending_tap_click_timer != 0) {
		/* a first tap is already waiting to resolve -- this is its
		 * second tap, so upgrade to a real double-click instead of
		 * two separate single clicks. */
		g_source_remove(pending_tap_click_timer);
		pending_tap_click_timer = 0;
		send_click_burst(2);
	} else {
		/* wait briefly to see if a second tap follows before
		 * committing to a single click. Capture position NOW --
		 * see fire_pending_tap_click for why. */
		pending_tap_x = virt_x;
		pending_tap_y = virt_y;
		pending_tap_click_timer = g_timeout_add(
			DOUBLE_TAP_WINDOW_MS, fire_pending_tap_click, NULL);
	}
	return TRUE;
}

static gboolean on_pad_motion(GtkWidget *w, GdkEventMotion *ev, gpointer d) {
	(void)w; (void)d;
	if (!trackpad_dragging) return TRUE;
	double dx = ev->x - trackpad_last_x;
	double dy = ev->y - trackpad_last_y;
	trackpad_last_x = ev->x;
	trackpad_last_y = ev->y;
	if (dx == 0.0 && dy == 0.0) return TRUE;
	tap_move_accum += (dx < 0.0 ? -dx : dx) + (dy < 0.0 ? -dy : dy);

	/* Make sure minivmac_w/h are current before clamping. */
	(void)minivmac();
	virt_x += (int)dx;
	virt_y += (int)dy;
	if (virt_x < 0) virt_x = 0;
	if (virt_y < 0) virt_y = 0;
	if (virt_x >= minivmac_w) virt_x = minivmac_w - 1;
	if (virt_y >= minivmac_h) virt_y = minivmac_h - 1;

	if (!drag_committed && tap_move_accum > TAP_MAX_MOVE_PX) {
		/* Real movement while still held -- this is a tap-and-drag,
		 * not a plain cursor reposition. Press down now, at the
		 * current (post-motion) position; the button stays down
		 * through every subsequent motion event until release, so
		 * e.g. MacPaint draws a continuous line following the drag
		 * instead of just moving the cursor with the pen up. */
		drag_committed = TRUE;
		send_button(TRUE);
	}
	send_motion();
	return TRUE;
}

static void on_click1(GtkWidget *w, gpointer d) { (void)w; (void)d; send_click_burst(1); }
static void on_click2(GtkWidget *w, gpointer d) { (void)w; (void)d; send_click_burst(2); }
static void on_click3(GtkWidget *w, gpointer d) { (void)w; (void)d; send_click_burst(3); }
static void on_mousedown(GtkWidget *w, gpointer d) { (void)w; (void)d; send_button(TRUE); }
static void on_mouseup(GtkWidget *w, gpointer d) { (void)w; (void)d; send_button(FALSE); }

static GtkWidget *build_trackpad_panel(void) {
	GtkWidget *vbox = gtk_vbox_new(FALSE, 4);

	GtkWidget *pad = gtk_drawing_area_new();
	gtk_widget_set_size_request(pad, -1, 300);
	gtk_widget_add_events(pad,
		GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
		GDK_POINTER_MOTION_MASK);
	{
		GtkWidget *frame = gtk_frame_new("drag here to move the cursor");
		gtk_container_add(GTK_CONTAINER(frame), pad);
		gtk_box_pack_start(GTK_BOX(vbox), frame, TRUE, TRUE, 0);
	}
	g_signal_connect(pad, "button-press-event", G_CALLBACK(on_pad_button_press), NULL);
	g_signal_connect(pad, "button-release-event", G_CALLBACK(on_pad_button_release), NULL);
	g_signal_connect(pad, "motion-notify-event", G_CALLBACK(on_pad_motion), NULL);

	GtkWidget *hbox = gtk_hbox_new(TRUE, 2);
	/* Ryan's feedback: real fingers had a harder time with this row than
	 * the keyboard rows above -- it was sized to GTK's natural minimum
	 * (a single line of button-label text plus default padding, maybe
	 * ~50-60px), much shorter than a comfortable touch target. 2.5x
	 * taller; the drag pad above it (packed expand/fill) shrinks to
	 * absorb the difference automatically. */
	gtk_widget_set_size_request(hbox, -1, 150);
	gtk_box_pack_start(GTK_BOX(hbox), make_key("1-click", G_CALLBACK(on_click1), NULL), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), make_key("2-click", G_CALLBACK(on_click2), NULL), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), make_key("3-click", G_CALLBACK(on_click3), NULL), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), make_key("mouse\ndown", G_CALLBACK(on_mousedown), NULL), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), make_key("mouse\nup", G_CALLBACK(on_mouseup), NULL), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

	return vbox;
}

/* ---- Mode toggle ---- */

static GtkWidget *mode_bar; /* container swapped between a single
                                "switch to TRACKPAD" button (keyboard
                                panel active) and a split "switch to
                                KBD" / pen-mode toggle row (trackpad
                                panel active). */

static void clear_container(GtkWidget *container) {
	GList *children = gtk_container_get_children(GTK_CONTAINER(container));
	for (GList *l = children; l; l = l->next) {
		gtk_container_remove(GTK_CONTAINER(container), GTK_WIDGET(l->data));
	}
	g_list_free(children);
}

static void show_keyboard_mode_bar(void);
static void show_trackpad_mode_bar(void);

static void on_switch_to_trackpad(GtkWidget *w, gpointer d) {
	(void)w; (void)d;
	gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 1);
	show_trackpad_mode_bar();
}

static void on_switch_to_keyboard(GtkWidget *w, gpointer d) {
	(void)w; (void)d;
	gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 0);
	show_keyboard_mode_bar();
}

static void show_keyboard_mode_bar(void) {
	clear_container(mode_bar);
	GtkWidget *btn = gtk_button_new_with_label("switch to TRACKPAD");
	gtk_widget_set_size_request(btn, -1, 80);
	g_signal_connect(btn, "clicked", G_CALLBACK(on_switch_to_trackpad), NULL);
	gtk_container_add(GTK_CONTAINER(mode_bar), btn);
	gtk_widget_show_all(mode_bar);
}

static void show_trackpad_mode_bar(void) {
	clear_container(mode_bar);
	GtkWidget *hbox = gtk_hbox_new(TRUE, 2);

	GtkWidget *kbd_btn = gtk_button_new_with_label("switch to KBD");
	gtk_widget_set_size_request(kbd_btn, -1, 80);
	g_signal_connect(kbd_btn, "clicked", G_CALLBACK(on_switch_to_keyboard), NULL);
	gtk_box_pack_start(GTK_BOX(hbox), kbd_btn, TRUE, TRUE, 0);

	GtkWidget *pen_btn = gtk_button_new_with_label(
		pen_mode_active ? "pen mode: ON" : "pen mode: OFF (nudge only)");
	gtk_widget_set_size_request(pen_btn, -1, 80);
	g_signal_connect(pen_btn, "clicked", G_CALLBACK(on_toggle_pen_mode), NULL);
	gtk_box_pack_start(GTK_BOX(hbox), pen_btn, TRUE, TRUE, 0);

	gtk_container_add(GTK_CONTAINER(mode_bar), hbox);
	gtk_widget_show_all(mode_bar);
}

int main(int argc, char **argv) {
	gtk_init(&argc, &argv);

	xdpy = XOpenDisplay(NULL);
	if (xdpy == NULL) {
		fprintf(stderr, "wario-companion: XOpenDisplay failed\n");
		return 1;
	}

	GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	companion_window = window;
	/* L:A_N:application (mini vMac's own tag) makes the WM's app-layer
	 * logic (lab126_application_layer.lua's prv_position_application)
	 * force EVERY app-layer window to fill the whole available area --
	 * that's why this used to cover mini vMac entirely regardless of
	 * the size requested below. L:KB_N:keyboard is a different, real
	 * WM layer (lab126_keyboard_layer.lua) built for exactly this: it
	 * anchors the window to the bottom of the screen at full width but
	 * PRESERVES whatever height the window already has when first
	 * managed -- unlike the app layer, it never forces full-screen.
	 * mini vMac's own window is a fixed 1024x684 (2x-scaled 512x342),
	 * on a 1072x1448 physical panel, leaving ~764px already unused
	 * below it -- sized to dock in exactly that space, no overlap. */
	const char *window_title = getenv("WARIO_WINDOW_TITLE");
	if (window_title == NULL || window_title[0] == '\0') {
		window_title =
			"L:KB_N:keyboard_ID:net.gryphel.wariocompanion_M:false_PC:N_RC:true_O:U";
	}
	gtk_window_set_title(GTK_WINDOW(window), window_title);
	gtk_window_set_default_size(GTK_WINDOW(window), 1072, 760);

	GtkWidget *vbox = gtk_vbox_new(FALSE, 4);
	gtk_container_add(GTK_CONTAINER(window), vbox);

	mode_bar = gtk_vbox_new(FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), mode_bar, FALSE, FALSE, 0);

	notebook = gtk_notebook_new();
	gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_keyboard_panel(), NULL);
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_trackpad_panel(), NULL);
	gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);

	show_keyboard_mode_bar(); /* matches default notebook page 0 */

	g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	gtk_widget_show_all(window);

	/* Must be installed on a real GdkWindow (post-show/realize) since
	 * touch_indicator_show() draws directly onto it. */
	gdk_event_handler_set(global_event_snoop, NULL, NULL);
	pen_mode_engine_start();
	gtk_main();
	return 0;
}
