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
#include <stdarg.h>

/* mini vMac's window title always contains this (see OSGLUXWN.c's
 * win_name literal); matched as a substring so we don't depend on the
 * rest of the L:A_N:application_... layer-tag formatting. */
#define MINIVMAC_TITLE_NEEDLE "net.gryphel.minivmac"

static Display *xdpy;
static Window minivmac_window = None;
static int minivmac_w = 1024, minivmac_h = 684; /* refreshed on lookup */
static gboolean caps_on = FALSE;
static gboolean shift_on = FALSE;
static gboolean option_on = FALSE;
static gboolean command_on = FALSE;
static gboolean control_on = FALSE;
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

/* Generalised key tap honouring the latched modifiers (shift/option/
 * command/control). Modifiers are sent as their own key events before
 * the main key and released after, exactly like holding them on a real
 * keyboard -- mini vMac tracks each modifier as its own Mac key, not via
 * event.state. `shifted` is the keysym produced when shift/caps is on
 * (e.g. XK_at for XK_2); pass the same value twice for keys with no
 * shifted form. */
static void send_key_combo(KeySym base, KeySym shifted) {
	gboolean sh = shift_on || caps_on;
	KeySym ks = sh ? shifted : base;
	KeyCode mods[4];
	int n = 0;
	if (sh) mods[n++] = XKeysymToKeycode(xdpy, XK_Shift_L);
	if (option_on) mods[n++] = XKeysymToKeycode(xdpy, XK_Super_L);
	if (command_on) mods[n++] = XKeysymToKeycode(xdpy, XK_Meta_L);
	if (control_on) mods[n++] = XKeysymToKeycode(xdpy, XK_Control_L);

	for (int i = 0; i < n; ++i) { send_key(mods[i], TRUE, 0); XFlush(xdpy); }
	if (n > 0) g_usleep(KEY_HOLD_US);

	KeyCode kc = XKeysymToKeycode(xdpy, ks);
	send_key(kc, TRUE, 0);
	XFlush(xdpy);
	g_usleep(KEY_HOLD_US);
	send_key(kc, FALSE, 0);
	XFlush(xdpy);
	g_usleep(KEY_HOLD_US);

	for (int i = n - 1; i >= 0; --i) { send_key(mods[i], FALSE, 0); XFlush(xdpy); }
	if (n > 0) g_usleep(KEY_HOLD_US);
}

/* Latch-toggle for a modifier; a trailing `*` on the label shows it's
 * latched on. */
static void toggle_mod(GtkWidget *w, const char *base_label, gboolean *flag) {
	*flag = !*flag;
	char buf[32];
	snprintf(buf, sizeof(buf), "%s%s", base_label, *flag ? "*" : "");
	gtk_button_set_label(GTK_BUTTON(w), buf);
}

static void on_shift(GtkWidget *w, gpointer d) { (void)d; toggle_mod(w, "shift", &shift_on); }
static void on_caps(GtkWidget *w, gpointer d) { (void)d; toggle_mod(w, "caps", &caps_on); }
static void on_option(GtkWidget *w, gpointer d) { (void)d; toggle_mod(w, "opt", &option_on); }
static void on_command(GtkWidget *w, gpointer d) { (void)d; toggle_mod(w, "cmd", &command_on); }
static void on_control(GtkWidget *w, gpointer d) { (void)d; toggle_mod(w, "ctrl", &control_on); }

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

/* Touch event device node. Override with WARIO_TOUCH_DEV env var. */
#define TOUCH_EVENT_DEV_DEFAULT "/dev/input/event4"
static const char *get_touch_dev(void) {
	const char *e = getenv("WARIO_TOUCH_DEV");
	return e && e[0] ? e : TOUCH_EVENT_DEV_DEFAULT;
}
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
	touch_evdev_fd = open(get_touch_dev(), O_RDONLY | O_NONBLOCK);
	if (touch_evdev_fd < 0) {
		fprintf(stderr,
			"wario-companion: couldn't open %s for pen mode: %s\n",
			get_touch_dev(), strerror(errno));
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

static GtkWidget *make_key(const char *label, GCallback cb, gpointer data) {
	GtkWidget *btn = gtk_button_new_with_label(label);
	g_signal_connect(btn, "clicked", cb, data);
	return btn;
}

/* ---- Key symbol handlers ---- */

typedef struct { KeySym base; KeySym shifted; } KeyPair;
static void on_key_sym(GtkWidget *w, gpointer d) {
	(void)w; KeyPair *kp = (KeyPair *)d;
	send_key_combo(kp->base, kp->shifted);
}
static GtkWidget *make_key_sym(const char *label, KeySym base, KeySym shifted) {
	KeyPair *kp = g_new(KeyPair, 1);
	kp->base = base; kp->shifted = shifted;
	GtkWidget *btn = gtk_button_new_with_label(label);
	g_signal_connect_data(btn, "clicked", G_CALLBACK(on_key_sym), kp,
		(GClosureNotify)g_free, 0);
	return btn;
}
/* Letter key: base lowercase, shifted uppercase (for shift/caps latch). */
static GtkWidget *make_letter(char lower) {
	char label[2] = { lower, 0 };
	return make_key_sym(label, XK_a + (lower - 'a'), XK_A + (lower - 'a'));
}

/* Common key callbacks (just re-using the generic on_key_sym). */
#define DEF_KEY(name, label, base, shifted) \
	static GtkWidget *key_##name(void) { return make_key_sym(label, base, shifted); }

DEF_KEY(esc, "esc", XK_Escape, XK_Escape)
DEF_KEY(tab, "tab", XK_Tab, XK_Tab)
DEF_KEY(del, "del", XK_BackSpace, XK_BackSpace)
DEF_KEY(fwddel, "fwddel", XK_Delete, XK_Delete)
DEF_KEY(ret, "ret", XK_Return, XK_Return)
DEF_KEY(space, "space", XK_space, XK_space)
DEF_KEY(1, "1", XK_1, XK_exclam)
DEF_KEY(2, "2", XK_2, XK_at)
DEF_KEY(3, "3", XK_3, XK_numbersign)
DEF_KEY(4, "4", XK_4, XK_dollar)
DEF_KEY(5, "5", XK_5, XK_percent)
DEF_KEY(6, "6", XK_6, XK_asciicircum)
DEF_KEY(7, "7", XK_7, XK_ampersand)
DEF_KEY(8, "8", XK_8, XK_asterisk)
DEF_KEY(9, "9", XK_9, XK_parenleft)
DEF_KEY(0, "0", XK_0, XK_parenright)
DEF_KEY(minus, "-", XK_minus, XK_underscore)
DEF_KEY(equal, "=", XK_equal, XK_plus)
DEF_KEY(lbrack, "[", XK_bracketleft, XK_braceleft)
DEF_KEY(rbrack, "]", XK_bracketright, XK_braceright)
DEF_KEY(bslash, "\\", XK_backslash, XK_bar)
DEF_KEY(semi, ";", XK_semicolon, XK_colon)
DEF_KEY(quote, "'", XK_apostrophe, XK_quotedbl)
DEF_KEY(comma, ",", XK_comma, XK_less)
DEF_KEY(period, ".", XK_period, XK_greater)
DEF_KEY(slash, "/", XK_slash, XK_question)
DEF_KEY(grave, "`", XK_grave, XK_asciitilde)
DEF_KEY(up, "^", XK_Up, XK_Up)
DEF_KEY(down, "v", XK_Down, XK_Down)
DEF_KEY(left, "<", XK_Left, XK_Left)
DEF_KEY(right, ">", XK_Right, XK_Right)
DEF_KEY(home, "home", XK_Home, XK_Home)
DEF_KEY(end, "end", XK_End, XK_End)
DEF_KEY(pgup, "pg↑", XK_Page_Up, XK_Page_Up)
DEF_KEY(pgdn, "pg↓", XK_Page_Down, XK_Page_Down)
DEF_KEY(help, "help", XK_Help, XK_Help)
DEF_KEY(clear, "clear", XK_Num_Lock, XK_Num_Lock)
DEF_KEY(f1, "F1", XK_F1, XK_F1)
DEF_KEY(f2, "F2", XK_F2, XK_F2)
DEF_KEY(f3, "F3", XK_F3, XK_F3)
DEF_KEY(f4, "F4", XK_F4, XK_F4)
DEF_KEY(f5, "F5", XK_F5, XK_F5)
DEF_KEY(f6, "F6", XK_F6, XK_F6)
DEF_KEY(f7, "F7", XK_F7, XK_F7)
DEF_KEY(f8, "F8", XK_F8, XK_F8)
DEF_KEY(f9, "F9", XK_F9, XK_F9)
DEF_KEY(f10, "F10", XK_F10, XK_F10)
DEF_KEY(f11, "F11", XK_F11, XK_F11)
DEF_KEY(f12, "F12", XK_F12, XK_F12)
DEF_KEY(kp_add, "+", XK_KP_Add, XK_KP_Add)
DEF_KEY(kp_sub, "-", XK_KP_Subtract, XK_KP_Subtract)
DEF_KEY(kp_mul, "*", XK_KP_Multiply, XK_KP_Multiply)
DEF_KEY(kp_div, "/", XK_KP_Divide, XK_KP_Divide)
DEF_KEY(kp_ent, "enter", XK_KP_Enter, XK_KP_Enter)
DEF_KEY(kp_eq, "=", XK_KP_Equal, XK_KP_Equal)
DEF_KEY(kp_0, "0", XK_KP_0, XK_KP_0)
DEF_KEY(kp_1, "1", XK_KP_1, XK_KP_1)
DEF_KEY(kp_2, "2", XK_KP_2, XK_KP_2)
DEF_KEY(kp_3, "3", XK_KP_3, XK_KP_3)
DEF_KEY(kp_4, "4", XK_KP_4, XK_KP_4)
DEF_KEY(kp_5, "5", XK_KP_5, XK_KP_5)
DEF_KEY(kp_6, "6", XK_KP_6, XK_KP_6)
DEF_KEY(kp_7, "7", XK_KP_7, XK_KP_7)
DEF_KEY(kp_8, "8", XK_KP_8, XK_KP_8)
DEF_KEY(kp_9, "9", XK_KP_9, XK_KP_9)
DEF_KEY(kp_dec, ".", XK_KP_Decimal, XK_KP_Decimal)

/* Helper: pack a row of buttons into a vbox, returning the hbox. */
static GtkWidget *key_row(GtkWidget *vbox, GtkWidget *first, ...) {
	va_list ap;
	GtkWidget *hbox = gtk_hbox_new(TRUE, 2);
	if (first) {
		gtk_box_pack_start(GTK_BOX(hbox), first, TRUE, TRUE, 0);
		va_start(ap, first);
		GtkWidget *w;
		while ((w = va_arg(ap, GtkWidget *)) != NULL)
			gtk_box_pack_start(GTK_BOX(hbox), w, TRUE, TRUE, 0);
		va_end(ap);
	}
	gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);
	return hbox;
}

/* ---- Keyboard panel: full Macintosh Plus layout ---- */
static GtkWidget *build_keyboard_panel(void) {
	GtkWidget *vbox = gtk_vbox_new(TRUE, 2);
	key_row(vbox, key_esc(), key_grave(), key_1(), key_2(), key_3(), key_4(), key_5(),
		key_6(), key_7(), key_8(), key_9(), key_0(),
		key_minus(), key_equal(), key_del(), NULL);
	key_row(vbox, key_tab(),
		make_letter('q'), make_letter('w'),
		make_letter('e'), make_letter('r'),
		make_letter('t'), make_letter('y'),
		make_letter('u'), make_letter('i'),
		make_letter('o'), make_letter('p'),
		key_lbrack(), key_rbrack(), key_bslash(), NULL);
	key_row(vbox,
		make_key("caps", G_CALLBACK(on_caps), NULL),
		make_letter('a'), make_letter('s'),
		make_letter('d'), make_letter('f'),
		make_letter('g'), make_letter('h'),
		make_letter('j'), make_letter('k'),
		make_letter('l'), key_semi(), key_quote(), key_ret(), NULL);
	key_row(vbox,
		make_key("shift", G_CALLBACK(on_shift), NULL),
		make_letter('z'), make_letter('x'),
		make_letter('c'), make_letter('v'),
		make_letter('b'), make_letter('n'),
		make_letter('m'), key_comma(), key_period(), key_slash(),
		make_key("shift", G_CALLBACK(on_shift), NULL), NULL);
	key_row(vbox,
		make_key("ctrl", G_CALLBACK(on_control), NULL),
		make_key("opt", G_CALLBACK(on_option), NULL),
		make_key("cmd", G_CALLBACK(on_command), NULL),
		key_space(),
		make_key("cmd", G_CALLBACK(on_command), NULL),
		make_key("opt", G_CALLBACK(on_option), NULL),
		make_key("ctrl", G_CALLBACK(on_control), NULL), NULL);
	key_row(vbox, key_up(), key_left(), key_down(), key_right(),
		key_home(), key_end(), key_pgup(), key_pgdn(),
		key_help(), key_fwddel(), NULL);
	return vbox;
}

/* ---- Keypad / function-key panel ---- */
static GtkWidget *build_keypad_panel(void) {
	GtkWidget *vbox = gtk_vbox_new(TRUE, 2);
	key_row(vbox, key_f1(), key_f2(), key_f3(), key_f4(),
		key_f5(), key_f6(), key_f7(), key_f8(),
		key_f9(), key_f10(), key_f11(), key_f12(), NULL);
	key_row(vbox, key_clear(), key_kp_div(), key_kp_mul(), key_kp_sub(), key_kp_add(), NULL);
	key_row(vbox, key_kp_7(), key_kp_8(), key_kp_9(), key_kp_ent(), NULL);
	key_row(vbox, key_kp_4(), key_kp_5(), key_kp_6(), key_kp_eq(), NULL);
	key_row(vbox, key_kp_1(), key_kp_2(), key_kp_3(), NULL);
	key_row(vbox, key_kp_0(), key_kp_dec(), NULL);
	return vbox;
}

/* ---- Trackpad panel ---- */

#define TAP_MAX_MOVE_PX 12.0 /* stay under this total movement to count
                                 as a tap rather than a drag */
#define DOUBLE_TAP_WINDOW_MS 400 /* real wall-clock gap we treat as
                                     "the same double-tap gesture";
                                     unrelated to mini vMac's own -dct,
                                     which send_click_burst() already
                                     handles with correct internal
                                     spacing regardless of how far apart
                                     the two real taps were */

static gboolean fire_pending_tap_click(gpointer data) {
	(void)data;
	pending_tap_click_timer = 0;
	/* Single tap with no second tap following within the double-tap
	 * window: a plain click (select an icon, press a button). Only
	 * taps that MOVED (drag) are excluded -- see on_pad_button_release. */
	send_click_burst_at(1, pending_tap_x, pending_tap_y);
	return FALSE; /* one-shot */
}

static gboolean on_pad_button_press(GtkWidget *w, GdkEventButton *ev, gpointer d) {
	(void)w; (void)d;
	trackpad_dragging = TRUE;
	trackpad_last_x = ev->x;
	trackpad_last_y = ev->y;
	tap_move_accum = 0.0;
	return TRUE;
}

static gboolean on_pad_button_release(GtkWidget *w, GdkEventButton *ev, gpointer d) {
	(void)w; (void)ev; (void)d;
	trackpad_dragging = FALSE;
	/* A drag (finger crossed the movement threshold while held) is not
	 * a tap: on the PW5 the trackpad ONLY moves the cursor, it never
	 * presses a button, so a dragged stroke sends no click at all. */
	if (tap_move_accum > TAP_MAX_MOVE_PX) {
		return TRUE;
	}
	/* Plain tap (finger stayed put): single tap = click, double tap =
	 * double-click, exactly like a real Mac trackpad. */
	if (pending_tap_click_timer != 0) {
		/* a first tap is already waiting -- this is its second tap,
		 * so it's a double-tap = a double-click (open an icon). */
		g_source_remove(pending_tap_click_timer);
		pending_tap_click_timer = 0;
		send_click_burst(2);
	} else {
		/* wait briefly for a possible second tap before committing
		 * to a single click. */
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

	/* Trackpad drag = cursor move ONLY. No button state is touched
	 * here -- the button is held only when the user presses the
	 * mouse-down button (a latch), and released by mouse-up. This is
	 * what makes it behave like a real trackpad instead of always
	 * drawing. */
	send_motion();
	return TRUE;
}

static void on_click1(GtkWidget *w, gpointer d) { (void)w; (void)d; send_click_burst(1); }
static void on_click2(GtkWidget *w, gpointer d) { (void)w; (void)d; send_click_burst(2); }
static void on_click3(GtkWidget *w, gpointer d) { (void)w; (void)d; send_click_burst(3); }
static void on_mousedown(GtkWidget *w, gpointer d) { (void)w; (void)d; send_button(TRUE); }
static void on_mouseup(GtkWidget *w, gpointer d) { (void)w; (void)d; send_button(FALSE); }

/* Close button: shut down the whole emulated Mac session -- both the
 * emulator and this companion. Sends the Mac a "shut down" keystroke
 * first (Command+Q is too modern for System 6; the Mac Plus power key is
 * the special keyboard power key, but mini vMac maps the Mac's power key
 * to F13 on a Mac keyboard -- send a clean shutdown via the Ctrl+Power
 * hack is unreliable, so just kill the processes: the Mac disk images
 * are read-only, nothing to save). */
static void on_close(GtkWidget *w, gpointer d) {
	(void)w; (void)d;
	/* Give the emulator a moment to flush, then take everything down. */
	system("pkill -9 minivmac 2>/dev/null; pkill -9 wario-companion 2>/dev/null; exit 0");
	gtk_main_quit();
}

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
	gtk_box_pack_start(GTK_BOX(hbox), make_key("close", G_CALLBACK(on_close), NULL), TRUE, TRUE, 0);
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
static void show_keypad_mode_bar(void);

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

static void on_switch_to_keypad(GtkWidget *w, gpointer d) {
	(void)w; (void)d;
	gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 2);
	show_keypad_mode_bar();
}

/* A two-button horizontal bar in mode_bar. */
static void mode_bar_two(const char *l1, GCallback c1, const char *l2, GCallback c2) {
	clear_container(mode_bar);
	GtkWidget *hbox = gtk_hbox_new(TRUE, 2);
	GtkWidget *b1 = gtk_button_new_with_label(l1);
	gtk_widget_set_size_request(b1, -1, 80);
	g_signal_connect(b1, "clicked", c1, NULL);
	gtk_box_pack_start(GTK_BOX(hbox), b1, TRUE, TRUE, 0);
	GtkWidget *b2 = gtk_button_new_with_label(l2);
	gtk_widget_set_size_request(b2, -1, 80);
	g_signal_connect(b2, "clicked", c2, NULL);
	gtk_box_pack_start(GTK_BOX(hbox), b2, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(mode_bar), hbox);
	gtk_widget_show_all(mode_bar);
}

static void show_keyboard_mode_bar(void) {
	mode_bar_two("switch to TRACKPAD", G_CALLBACK(on_switch_to_trackpad),
		"switch to NUM", G_CALLBACK(on_switch_to_keypad));
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

static void show_keypad_mode_bar(void) {
	mode_bar_two("switch to KBD", G_CALLBACK(on_switch_to_keyboard),
		"switch to TRACKPAD", G_CALLBACK(on_switch_to_trackpad));
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
	 * on a 1236x1648 physical panel, leaving ~860px already unused
	 * below it -- sized to dock in exactly that space, no overlap. */
	const char *window_title = getenv("WARIO_WINDOW_TITLE");
	if (window_title == NULL || window_title[0] == '\0') {
		window_title =
			"L:KB_N:keyboard_ID:net.gryphel.wariocompanion_M:false_PC:N_RC:true_O:U";
	}
	gtk_window_set_title(GTK_WINDOW(window), window_title);
	/* Height sized so the keyboard layer docks the window right up
	 * against the bottom of mini vMac's screen (which ends at y=820 on
	 * the 1648px-tall PW5 panel) -- no dead gap between them. */
	gtk_window_set_default_size(GTK_WINDOW(window), 1236, 828);

	GtkWidget *vbox = gtk_vbox_new(FALSE, 4);
	gtk_container_add(GTK_CONTAINER(window), vbox);

	mode_bar = gtk_vbox_new(FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), mode_bar, FALSE, FALSE, 0);

	notebook = gtk_notebook_new();
	gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_keyboard_panel(), NULL);
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_trackpad_panel(), NULL);
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_keypad_panel(), NULL);
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
