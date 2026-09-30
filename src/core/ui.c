#include "ui.h"
#define CIMGUI_USE_OPENGL3
#include "cimgui_impl.h"
#include "gl.h"
#include "shot.h"
#include <stdio.h>
#include <string.h>

void ui_init(Ui *u, const char *name, int w, int h, int flags, float fontSize) {
  memset(u, 0, sizeof *u);
  u->w = w; u->h = h;
  snprintf(u->name, sizeof u->name, "%s", name);
  for (char *c = u->name; *c; c++) if (*c == ' ') *c = '-';
  u->win = pw_create(name, 0, 0, w, h, flags);
  // GL objects belong to the shared context, created while the jelly's surface is current (see plat)
  u->ig = igCreateContext(NULL);
  igSetCurrentContext(u->ig);
  ImGuiIO *io = igGetIO_Nil();
  io->IniFilename = NULL;
  if (fontSize > 0) {
    char path[512];
    if (plat_font(0, path, sizeof path)) ImFontAtlas_AddFontFromFileTTF(io->Fonts, path, fontSize, NULL, NULL);
    else ImFontAtlas_AddFontDefault(io->Fonts, NULL);
    // Japanese / Chinese / Korean text: merge a CJK face in (glyphs load on demand)
    if (plat_font(1, path, sizeof path)) {
      ImFontConfig *fc = ImFontConfig_ImFontConfig();
      fc->MergeMode = true;
      ImFontAtlas_AddFontFromFileTTF(io->Fonts, path, fontSize, fc, NULL);
      ImFontConfig_destroy(fc);
    }
  }
  ImGui_ImplOpenGL3_Init("#version 330 core");
}

void ui_move(Ui *u, int x, int y) { u->x = x; u->y = y; pw_move(u->win, x, y); }
void ui_drag_start(Ui *u, const PEvent *e) { u->dragging = 1; u->dragRX = e->rx; u->dragRY = e->ry; u->dragWX = u->x; u->dragWY = u->y; }
int ui_drag(Ui *u, const PEvent *e) {
  if (!u->dragging) return 0;
  if (e->type == PE_MOVE) { ui_move(u, u->dragWX + (int)(e->rx - u->dragRX), u->dragWY + (int)(e->ry - u->dragRY)); return 1; }
  if (e->type == PE_BUTTON && e->button == PB_LEFT && !e->down) { u->dragging = 0; return 1; }
  return 0;
}

int ui_event(Ui *u, const PEvent *e) {
  if (e->win != u->win) return 0;
  igSetCurrentContext(u->ig);
  ImGuiIO *io = igGetIO_Nil();
  switch (e->type) {
  case PE_MOVE: ImGuiIO_AddMousePosEvent(io, e->x, e->y); break;
  case PE_LEAVE: if (!u->dragging) ImGuiIO_AddMousePosEvent(io, -3.4e38f, -3.4e38f); break;
  case PE_BUTTON:
    if (e->down) pw_clicked(u->win); // clicking back in takes the keyboard again
    ImGuiIO_AddMousePosEvent(io, e->x, e->y);
    ImGuiIO_AddMouseButtonEvent(io, e->button == PB_LEFT ? 0 : e->button == PB_RIGHT ? 1 : 2, e->down);
    break;
  case PE_WHEEL: ImGuiIO_AddMouseWheelEvent(io, 0, e->wheel); break;
  case PE_KEY:
    ImGuiIO_AddKeyEvent(io, ImGuiMod_Ctrl, (e->mods & PM_CTRL) != 0);
    ImGuiIO_AddKeyEvent(io, ImGuiMod_Shift, (e->mods & PM_SHIFT) != 0);
    ImGuiIO_AddKeyEvent(io, ImGuiMod_Alt, (e->mods & PM_ALT) != 0);
    if (e->down && (e->mods & PM_CTRL) && e->key == ImGuiKey_V) { pw_request_paste(u->win, 0); break; } // Ctrl+V
    if (e->key != ImGuiKey_None) ImGuiIO_AddKeyEvent(io, (ImGuiKey)e->key, e->down);
    break;
  case PE_TEXT: ImGuiIO_AddInputCharactersUTF8(io, e->text); break;
  case PE_PASTE: if (e->tag == 0 && e->paste) ImGuiIO_AddInputCharactersUTF8(io, e->paste); break;
  case PE_FOCUS: ImGuiIO_AddFocusEvent(io, e->down); break;
  default: return 0;
  }
  return 1;
}

void ui_frame_begin(Ui *u, double dt) {
  u->clock += dt;
  pw_begin(u->win);
  if (u->clock == dt) pw_vsync(u->win, 0); // (first frame) no vsync wait here: the jelly's own swap paces the loop
  igSetCurrentContext(u->ig);
  ImGuiIO *io = igGetIO_Nil();
  io->DisplaySize = (ImVec2_c){(float)u->w, (float)u->h};
  io->DeltaTime = dt > 0 ? (float)dt : 1.0f / 60;
  ImGui_ImplOpenGL3_NewFrame();
  igNewFrame();
}

void ui_frame_end(Ui *u) {
  igRender();
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());
  shot_maybe(u->name, u->w, u->h, u->clock);
  pw_present(u->win);
  pw_tick(u->win, u->clock);
}
