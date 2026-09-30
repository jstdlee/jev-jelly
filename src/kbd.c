// Keyboard input for the ImGui windows (options panel, chat box). Our windows are override-redirect, so they only
// get the keyboard when we ask for it: kb_sync() takes focus while a text field is active and hands it back to
// whatever had it before. Text goes through the X input method, so Japanese / Chinese / Korean IMEs (ibus, fcitx)
// work. Ctrl+V is turned into a clipboard request; the owner window inserts the text when it arrives.

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#include "jelly.h"
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XIM im;
static Display *dpy;

void kb_global_init(Display *d) {
  dpy = d;
  XSetLocaleModifiers("");
  im = XOpenIM(dpy, NULL, NULL, NULL);
  if (!im) { XSetLocaleModifiers("@im=none"); im = XOpenIM(dpy, NULL, NULL, NULL); }
}

void kb_attach(Kbd *k, Window w) {
  memset(k, 0, sizeof *k);
  k->win = w;
  if (im)
    k->ic = XCreateIC(im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, w, XNFocusWindow, w, NULL);
}

/* GNOME's window manager won't let an override-redirect (unmanaged) window keep the keyboard, so windows you type
   in are managed but look unmanaged: no decorations, above everything, not in the taskbar or pager. Call before
   mapping. */
void kb_prepare_window(Window w) {
  Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False), util = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
  XChangeProperty(dpy, w, type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&util, 1);
  Atom states[3] = {XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False), XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False),
                    XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False)};
  XChangeProperty(dpy, w, XInternAtom(dpy, "_NET_WM_STATE", False), XA_ATOM, 32, PropModeReplace, (unsigned char *)states, 3);
  struct { unsigned long flags, functions, decorations; long input; unsigned long status; } mwm = {2, 0, 0, 0, 0};
  Atom motif = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
  XChangeProperty(dpy, w, motif, motif, 32, PropModeReplace, (unsigned char *)&mwm, 5); // no title bar or border
  XWMHints wh = {0}; wh.flags = InputHint; wh.input = True;
  XSetWMHints(dpy, w, &wh);
  XSizeHints sh = {0}; sh.flags = USPosition | PPosition; // keep the position we give it
  XSetWMNormalHints(dpy, w, &sh);
}

/* Focus is tracked from FocusIn / FocusOut, not assumed: if the user clicks another window, the text field is
   deactivated and we stop asking for the keyboard until they click back into it. */
void kb_grab(Kbd *k) {
  Window f; int rv;
  XGetInputFocus(dpy, &f, &rv);
  if (f != k->win) { k->prevFocus = f; k->prevRevert = rv; }
  XSync(dpy, False); // the window must be viewable before it can take focus
  // ask the window manager to activate us (as a pager would: it's the user's own click that opened this)
  XEvent ev = {0};
  ev.xclient.type = ClientMessage; ev.xclient.window = k->win; ev.xclient.format = 32;
  ev.xclient.message_type = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
  ev.xclient.data.l[0] = 2; ev.xclient.data.l[1] = CurrentTime;
  XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
  XSetInputFocus(dpy, k->win, RevertToParent, CurrentTime);
  if (k->ic) XSetICFocus(k->ic);
  k->focused = 1;
  if (getenv("JELLY_DEBUG")) fprintf(stderr, "kb: grab 0x%lx (was 0x%lx)\n", k->win, f);
}
/* managed windows are mapped by the window manager a moment after we ask: activate once it's really up, and check
   that it stuck (try once more if not) */
void kb_grab_soon(Kbd *k, double now) { k->grabAt = now + 0.2; k->grabTries = 2; }
void kb_tick(Kbd *k, double now) {
  if (!k->grabTries || now < k->grabAt) return;
  XWindowAttributes wa;
  if (!XGetWindowAttributes(dpy, k->win, &wa) || wa.map_state != IsViewable) { k->grabAt = now + 0.1; return; }
  Window f; int rv;
  XGetInputFocus(dpy, &f, &rv);
  if (f == k->win) { k->grabTries = 0; k->focused = 1; return; }
  kb_grab(k);
  k->grabTries--; k->grabAt = now + 0.35;
}
void kb_release(Kbd *k) { // managed windows: the window manager hands focus on when we unmap or the user clicks away
  if (!k->focused) return;
  if (k->ic) XUnsetICFocus(k->ic);
  k->focused = 0;
}
/* called after each frame: grab when a text field has just been activated (never re-grab in a loop after the user
   moved focus elsewhere), release when editing ends */
void kb_sync(Kbd *k, int wantText) {
  if (wantText && !k->prevWant && !k->focused) kb_grab(k);
  else if (!wantText && k->focused) kb_release(k);
  k->prevWant = wantText;
}
/* a click inside the window: if a field is still active but we lost the keyboard, take it back */
void kb_clicked(Kbd *k) { if (k->prevWant && !k->focused) kb_grab(k); }

static ImGuiKey map_key(KeySym s) {
  switch (s) {
  case XK_Return: return ImGuiKey_Enter;
  case XK_KP_Enter: return ImGuiKey_KeypadEnter;
  case XK_BackSpace: return ImGuiKey_Backspace;
  case XK_Delete: return ImGuiKey_Delete;
  case XK_Tab: case XK_ISO_Left_Tab: return ImGuiKey_Tab;
  case XK_Escape: return ImGuiKey_Escape;
  case XK_Left: return ImGuiKey_LeftArrow;
  case XK_Right: return ImGuiKey_RightArrow;
  case XK_Up: return ImGuiKey_UpArrow;
  case XK_Down: return ImGuiKey_DownArrow;
  case XK_Home: return ImGuiKey_Home;
  case XK_End: return ImGuiKey_End;
  case XK_Page_Up: return ImGuiKey_PageUp;
  case XK_Page_Down: return ImGuiKey_PageDown;
  case XK_Insert: return ImGuiKey_Insert;
  }
  if (s >= XK_a && s <= XK_z) return (ImGuiKey)(ImGuiKey_A + (int)(s - XK_a));
  if (s >= XK_A && s <= XK_Z) return (ImGuiKey)(ImGuiKey_A + (int)(s - XK_A));
  return ImGuiKey_None;
}

/* feeds a KeyPress / KeyRelease into ImGui; returns 1 if it was a key event */
int kb_event(Kbd *k, XEvent *e, void *iov) {
  ImGuiIO *io = iov;
  if ((e->type == FocusOut || e->type == FocusIn) && getenv("JELLY_DEBUG"))
    fprintf(stderr, "kb: %s 0x%lx mode %d detail %d\n", e->type == FocusIn ? "FocusIn" : "FocusOut", k->win, e->xfocus.mode, e->xfocus.detail);
  if (e->type == FocusOut && e->xfocus.detail != NotifyInferior && e->xfocus.mode != NotifyGrab) {
    k->focused = 0; // someone else has the keyboard now: stop editing
    ImGuiIO_AddFocusEvent(io, false);
    return 1;
  }
  if (e->type == FocusIn) { ImGuiIO_AddFocusEvent(io, true); return 1; }
  if (e->type != KeyPress && e->type != KeyRelease) return 0;
  XKeyEvent *ke = &e->xkey;
  int down = e->type == KeyPress;
  ImGuiIO_AddKeyEvent(io, ImGuiMod_Ctrl, (ke->state & ControlMask) != 0);
  ImGuiIO_AddKeyEvent(io, ImGuiMod_Shift, (ke->state & ShiftMask) != 0);
  ImGuiIO_AddKeyEvent(io, ImGuiMod_Alt, (ke->state & Mod1Mask) != 0);
  KeySym sym = ke->keycode ? XLookupKeysym(ke, 0) : NoSymbol;
  if (down && (ke->state & ControlMask) && (sym == XK_v || sym == XK_V)) { k->wantPaste = 1; return 1; }
  ImGuiKey key = map_key(sym);
  if (key != ImGuiKey_None) ImGuiIO_AddKeyEvent(io, key, down);
  if (!down || (ke->state & ControlMask)) return 1;
  char buf[256]; KeySym ks; Status st = XLookupNone; int n;
  if (k->ic) n = Xutf8LookupString(k->ic, ke, buf, sizeof buf - 1, &ks, &st);
  else { n = XLookupString(ke, buf, sizeof buf - 1, &ks, NULL); st = n > 0 ? XLookupChars : XLookupNone; }
  if ((st == XLookupChars || st == XLookupBoth) && n > 0) {
    buf[n] = 0;
    if ((unsigned char)buf[0] >= 0x20 && buf[0] != 0x7f) ImGuiIO_AddInputCharactersUTF8(io, buf);
  }
  return 1;
}

/* asks for the clipboard as UTF-8; it arrives as a SelectionNotify with property JELLY_PASTE */
void kb_request_paste(Kbd *k) {
  XConvertSelection(dpy, XInternAtom(dpy, "CLIPBOARD", False), XInternAtom(dpy, "UTF8_STRING", False),
                    XInternAtom(dpy, "JELLY_PASTE", False), k->win, CurrentTime);
  k->wantPaste = 0;
}
/* if this SelectionNotify is a paste, inserts the text; returns 1 if it was one */
int kb_paste_arrived(Kbd *k, XEvent *e, void *iov, int singleLine) {
  if (e->type != SelectionNotify || e->xselection.property != XInternAtom(dpy, "JELLY_PASTE", False)) return 0;
  Atom type; int fmt; unsigned long n, left; unsigned char *data = NULL;
  if (XGetWindowProperty(dpy, k->win, e->xselection.property, 0, 1 << 16, True, AnyPropertyType, &type, &fmt, &n, &left, &data) == Success && data) {
    if (singleLine) for (unsigned char *p = data; *p; p++) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    ImGuiIO_AddInputCharactersUTF8((ImGuiIO *)iov, (const char *)data);
    XFree(data);
  }
  return 1;
}
