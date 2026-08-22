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
 */

#include <gtk/gtk.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
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
static GtkWidget *companion_window; /* set in main(), used by touch indicator */

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

static void send_button(gboolean press) {
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
	ev.xbutton.x = virt_x; ev.xbutton.y = virt_y;
	ev.xbutton.same_screen = True;
	ev.xbutton.button = Button1;
	ev.xbutton.state = press ? 0 : Button1Mask;
	XSendEvent(xdpy, target, False, NoEventMask, &ev);
	XFlush(xdpy);
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

static void send_click_burst(int n) {
	for (int i = 0; i < n; ++i) {
		send_button(TRUE);
		g_usleep(KEY_HOLD_US); /* same drain-before-CPU-step issue as
		                          send_key_tap -- see its comment */
		send_button(FALSE);
		if (i + 1 < n) {
			g_usleep(80000); /* comfortably inside mini vMac's
			                     double-click window (-dct default) */
		}
	}
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

static gboolean on_pad_button_press(GtkWidget *w, GdkEventButton *ev, gpointer d) {
	(void)w; (void)d;
	trackpad_dragging = TRUE;
	trackpad_last_x = ev->x;
	trackpad_last_y = ev->y;
	return TRUE;
}

static gboolean on_pad_button_release(GtkWidget *w, GdkEventButton *ev, gpointer d) {
	(void)w; (void)ev; (void)d;
	trackpad_dragging = FALSE;
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

	/* Make sure minivmac_w/h are current before clamping. */
	(void)minivmac();
	virt_x += (int)dx;
	virt_y += (int)dy;
	if (virt_x < 0) virt_x = 0;
	if (virt_y < 0) virt_y = 0;
	if (virt_x >= minivmac_w) virt_x = minivmac_w - 1;
	if (virt_y >= minivmac_h) virt_y = minivmac_h - 1;
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
	gtk_box_pack_start(GTK_BOX(hbox), make_key("1-click", G_CALLBACK(on_click1), NULL), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), make_key("2-click", G_CALLBACK(on_click2), NULL), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), make_key("3-click", G_CALLBACK(on_click3), NULL), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), make_key("mouse down", G_CALLBACK(on_mousedown), NULL), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), make_key("mouse up", G_CALLBACK(on_mouseup), NULL), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

	return vbox;
}

/* ---- Mode toggle ---- */

static void on_mode_toggle(GtkWidget *w, gpointer d) {
	(void)d;
	gint page = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
	gint next = (page == 0) ? 1 : 0;
	gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), next);
	gtk_button_set_label(GTK_BUTTON(w),
		next == 0 ? "switch to TRACKPAD" : "switch to KEYBOARD");
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
	gtk_window_set_title(GTK_WINDOW(window),
		"L:KB_N:keyboard_ID:net.gryphel.wariocompanion_M:false_PC:N_RC:true_O:U");
	gtk_window_set_default_size(GTK_WINDOW(window), 1072, 760);

	GtkWidget *vbox = gtk_vbox_new(FALSE, 4);
	gtk_container_add(GTK_CONTAINER(window), vbox);

	GtkWidget *mode_button = gtk_button_new_with_label("switch to TRACKPAD");
	gtk_widget_set_size_request(mode_button, -1, 80);
	g_signal_connect(mode_button, "clicked", G_CALLBACK(on_mode_toggle), NULL);
	gtk_box_pack_start(GTK_BOX(vbox), mode_button, FALSE, FALSE, 0);

	notebook = gtk_notebook_new();
	gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_keyboard_panel(), NULL);
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_trackpad_panel(), NULL);
	gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);

	g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	gtk_widget_show_all(window);

	/* Must be installed on a real GdkWindow (post-show/realize) since
	 * touch_indicator_show() draws directly onto it. */
	gdk_event_handler_set(global_event_snoop, NULL, NULL);
	gtk_main();
	return 0;
}
