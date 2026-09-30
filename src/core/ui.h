// An ImGui surface: a platform window with its own ImGui context (the options panel, the chat box, the reminder
// words). Feeds platform events into ImGui and wraps each frame.
#pragma once
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#include "../platform/plat.h"

typedef struct {
  PWin *win;
  char name[32];            // (for screenshots)
  ImGuiContext *ig;
  int w, h;
  int x, y;                 // where it is on screen
  double clock;             // seconds since it was created (for keyboard focus timing)
  int dragging;             // being moved by the pointer
  float dragRX, dragRY;
  int dragWX, dragWY;
} Ui;

/* fonts: 0 = the UI font with CJK merged in; bigSize > 0 also adds the three CJK faces (JP, KR, SC) at that size */
void ui_init(Ui *u, const char *name, int w, int h, int flags, float fontSize);
int ui_event(Ui *u, const PEvent *e);    // feeds ImGui; 1 if it was ours
void ui_frame_begin(Ui *u, double dt);   // binds the surface and starts an ImGui frame
void ui_frame_end(Ui *u);                // renders and presents; keeps the keyboard while a text field is active
void ui_move(Ui *u, int x, int y);
void ui_drag_start(Ui *u, const PEvent *e);
int ui_drag(Ui *u, const PEvent *e);     // moves the surface while dragging; 1 if it consumed the event
