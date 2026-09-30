// App themes for the ImGui windows (options panel, chat box, events / history lists): White, Dark (gpu-hud) and
// Tokyo Night. Custom-drawn widgets read the same palette through theme().

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#include "jelly.h"
#include <math.h>
#include <string.h>

static const Theme THEMES[3] = {
    // White: soft paper panel, dark ink, green accent
    {0xfbfbfd, 0.97f, 0x000000, 0.06f, 0x1f2328, 0x57606a, 0x1a7f37, 0x8250df, 0xcf222e, 0x9a6700, 1},
    // Dark: gpu-hud
    {0x121419, 0.94f, 0xffffff, 0.08f, 0xf0f2f5, 0xbdc2cc, 0x4dc778, 0xa371f7, 0xf85149, 0xd29922, 0},
    // Tokyo Night
    {0x1a1b26, 0.96f, 0x7aa2f7, 0.10f, 0xc0caf5, 0x9aa5ce, 0x7aa2f7, 0xbb9af7, 0xf7768e, 0xe0af68, 0},
};
static int current = THEME_DARK;

const Theme *theme(void) { return &THEMES[current]; }
void theme_select(int id) { current = id >= 0 && id < 3 ? id : THEME_DARK; }

unsigned th_u32(unsigned hex, float a) {
  if (a < 0) a = 0;
  if (a > 1) a = 1;
  return ((unsigned)(a * 255) << 24) | ((hex & 255) << 16) | (((hex >> 8) & 255) << 8) | ((hex >> 16) & 255);
}
static ImVec4_c v4(unsigned hex, float a) {
  return (ImVec4_c){((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f, a};
}

void theme_style(void *st, float bgAlpha) {
  const Theme *t = theme();
  ImGuiStyle *s = st;
  if (t->light) igStyleColorsLight(s); else igStyleColorsDark(s);
  s->WindowRounding = 10; s->FrameRounding = 5; s->GrabRounding = 7; s->PopupRounding = 6; s->ChildRounding = 5;
  s->WindowPadding = (ImVec2_c){14, 12}; s->FramePadding = (ImVec2_c){10, 6}; s->ItemSpacing = (ImVec2_c){8, 6};
  s->WindowBorderSize = 0; s->FrameBorderSize = 0; s->PopupBorderSize = 1; s->GrabMinSize = 14; s->ScrollbarSize = 8;
  ImVec4_c *c = s->Colors, acc = v4(t->accent, 1);
  c[ImGuiCol_WindowBg] = v4(t->bg, bgAlpha > 0 ? bgAlpha : t->bgAlpha);
  c[ImGuiCol_ChildBg] = v4(0, 0);
  c[ImGuiCol_PopupBg] = v4(t->bg, 0.99f);
  c[ImGuiCol_Border] = v4(t->frame, 0.18f);
  c[ImGuiCol_Text] = v4(t->text, 1);
  c[ImGuiCol_TextDisabled] = v4(t->muted, 1);
  c[ImGuiCol_CheckMark] = acc; c[ImGuiCol_SliderGrab] = acc; c[ImGuiCol_SliderGrabActive] = acc;
  c[ImGuiCol_Button] = v4(t->frame, t->frameAlpha);
  c[ImGuiCol_ButtonHovered] = (ImVec4_c){acc.x, acc.y, acc.z, 0.45f};
  c[ImGuiCol_ButtonActive] = (ImVec4_c){acc.x, acc.y, acc.z, 0.70f};
  c[ImGuiCol_FrameBg] = v4(t->frame, t->frameAlpha);
  c[ImGuiCol_FrameBgHovered] = v4(t->frame, t->frameAlpha * 1.7f);
  c[ImGuiCol_FrameBgActive] = (ImVec4_c){acc.x, acc.y, acc.z, 0.25f};
  c[ImGuiCol_Header] = (ImVec4_c){acc.x, acc.y, acc.z, 0.28f};
  c[ImGuiCol_HeaderHovered] = v4(t->frame, t->frameAlpha);
  c[ImGuiCol_HeaderActive] = (ImVec4_c){acc.x, acc.y, acc.z, 0.40f};
  c[ImGuiCol_Separator] = v4(t->frame, t->frameAlpha * 1.4f);
  c[ImGuiCol_ScrollbarBg] = v4(0, 0);
  c[ImGuiCol_ScrollbarGrab] = v4(t->frame, 0.22f);
  c[ImGuiCol_ScrollbarGrabHovered] = v4(t->frame, 0.32f);
  c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = v4(t->frame, t->frameAlpha * 0.9f);
  c[ImGuiCol_TabHovered] = (ImVec4_c){acc.x, acc.y, acc.z, 0.45f};
  c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = (ImVec4_c){acc.x, acc.y, acc.z, 0.30f};
  c[ImGuiCol_TabSelectedOverline] = c[ImGuiCol_TabDimmedSelectedOverline] = acc;
  c[ImGuiCol_TextSelectedBg] = (ImVec4_c){acc.x, acc.y, acc.z, 0.35f};
  c[ImGuiCol_NavCursor] = v4(0, 0);
}

/* ---- gummy "jelly" buttons ----
   A button is a little slab of jelly: tinted translucent body, darker belly, a glossy cap and a specular dot. It
   swells a pixel on hover and squishes on press (wider, flatter, anchored at the bottom). A spring drives the
   squish, so it wobbles back when released. */

static ImU32 mixu(unsigned a, unsigned b, float t, float alpha) { // blend two 0xRRGGBB colors, then pack
  float r = ((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t, g = ((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t,
        bl = (a & 255) * (1 - t) + (b & 255) * t;
  return th_u32(((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)bl, alpha * igGetStyle()->Alpha);
}

void th_jelly(void *drawList, float x0, float y0, float x1, float y1, int kind, float hover, float squish) {
  const Theme *t = theme();
  ImDrawList *dl = drawList;
  float w = x1 - x0, h = y1 - y0;
  // squish: wider and flatter, feet stay on the floor; hover: swell a little
  float sx = w * (0.05f * squish) / 2 + hover, sy = h * 0.14f * squish;
  x0 -= sx; x1 += sx; y0 += sy - hover; y1 += hover * 0.5f;
  h = y1 - y0;
  float r = h * 0.42f;
  unsigned body = kind == TH_BTN_DANGER ? t->danger : t->accent;
  float a = kind == TH_BTN_ON ? 0.62f : kind == TH_BTN_QUIET ? 0.10f : 0.24f;
  a += hover * (kind == TH_BTN_ON ? 0.12f : 0.16f) + squish * 0.10f;
  float glossA = t->light ? 0.55f : 0.20f, shadeA = t->light ? 0.10f : 0.28f;
  if (kind == TH_BTN_QUIET) glossA *= 0.5f, shadeA *= 0.5f;

  ImDrawList_AddRectFilled(dl, (ImVec2_c){x0, y0 + 2}, (ImVec2_c){x1, y1 + 2}, mixu(0, 0, 0, shadeA * 0.6f), r, 0); // drop
  ImDrawList_AddRectFilled(dl, (ImVec2_c){x0, y0}, (ImVec2_c){x1, y1}, mixu(t->bg, body, a, 1), r, 0);          // body
  ImDrawList_AddRectFilled(dl, (ImVec2_c){x0 + 1, y0 + h * 0.52f}, (ImVec2_c){x1 - 1, y1 - 1},                  // belly
                           mixu(0, 0, 0, shadeA * 0.55f), r * 0.9f, ImDrawFlags_RoundCornersBottom);
  for (int i = 0; i < 4; i++) { // glossy cap: stacked insets, so it fades out downwards instead of ending in an edge
    float k = i / 4.f, ga = glossA * 0.32f * (1 - 0.35f * squish);
    ImDrawList_AddRectFilled(dl, (ImVec2_c){x0 + r * (0.5f + k * 0.5f), y0 + 2 + k}, (ImVec2_c){x1 - r * (0.5f + k * 0.5f), y0 + h * (0.50f - k * 0.08f)},
                             mixu(0xffffff, 0xffffff, 0, ga), r * 0.6f, 0);
  }
  float sa = (t->light ? 0.95f : 0.75f) * (kind == TH_BTN_QUIET ? 0.5f : 1); // shine: a dot and a short streak
  ImDrawList_AddCircleFilled(dl, (ImVec2_c){x0 + r * 1.05f, y0 + h * 0.26f}, h * 0.07f, mixu(0xffffff, 0xffffff, 0, sa), 12);
  ImDrawList_AddLine(dl, (ImVec2_c){x0 + r * 1.35f, y0 + h * 0.21f}, (ImVec2_c){x0 + r * 2.1f, y0 + h * 0.19f},
                     mixu(0xffffff, 0xffffff, 0, sa * 0.6f), h * 0.07f);
  ImDrawList_AddRect(dl, (ImVec2_c){x0, y0}, (ImVec2_c){x1, y1}, mixu(t->bg, body, 0.55f + 0.3f * hover, kind == TH_BTN_QUIET ? 0.25f : 0.7f),
                     r, 1.2f, 0); // rim
}

/* per-button animation state lives in ImGui's storage, keyed off the item id */
static void anim(ImGuiID id, int hovered, int held, float *hover, float *squish) {
  ImGuiStorage *st = igGetStateStorage();
  float *hv = ImGuiStorage_GetFloatRef(st, id, 0), *q = ImGuiStorage_GetFloatRef(st, id + 1, 0),
        *qv = ImGuiStorage_GetFloatRef(st, id + 2, 0);
  float dt = igGetIO_Nil()->DeltaTime;
  if (dt > 1 / 30.f) dt = 1 / 30.f;
  *hv += ((hovered ? 1.f : 0.f) - *hv) * (1 - expf(-dt * 14));
  for (int i = 0; i < 4; i++) { // underdamped spring, substepped for stability
    float h = dt / 4, acc = 700 * ((held ? 1.f : 0.f) - *q) - 16 * *qv;
    *qv += acc * h; *q += *qv * h;
  }
  if (*q < -0.6f) *q = -0.6f; // a rebound stretches it taller for a moment
  *hover = *hv; *squish = *q;
}

void th_jelly_item(void *drawList, float x0, float y0, float x1, float y1, int kind) {
  float hv, q;
  anim(igGetItemID(), igIsItemHovered(0), igIsItemActive(), &hv, &q);
  th_jelly(drawList, x0, y0, x1, y1, kind, hv, q);
}

int th_button(const char *label, float w, float h, int kind) {
  const Theme *t = theme();
  const char *end = strstr(label, "##");
  ImVec2_c ts = igCalcTextSize(label, end, false, -1);
  ImGuiStyle *s = igGetStyle();
  if (w <= 0) w = ts.x + s->FramePadding.x * 2 + 8;
  if (h <= 0) h = igGetFrameHeight() + 4;
  ImVec2_c p = igGetCursorScreenPos();
  int hit = igInvisibleButton(label, (ImVec2_c){w, h}, 0);
  float hv, q;
  anim(igGetItemID(), igIsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && s->Alpha > 0.9f, igIsItemActive(), &hv, &q);
  ImDrawList *dl = igGetWindowDrawList();
  th_jelly(dl, p.x, p.y, p.x + w, p.y + h, kind, hv, q);
  unsigned tc = kind == TH_BTN_ON && !t->light ? 0xffffff : kind != TH_BTN_DANGER ? t->text : t->light ? t->danger : 0xffb3b3;
  float ty = p.y + (h - ts.y) / 2 + h * 0.07f * (q > 0 ? q : 0) - 0.5f;
  if (kind == TH_BTN_ON || !t->light)
    ImDrawList_AddText_Vec2(dl, (ImVec2_c){p.x + (w - ts.x) / 2, ty + 1}, mixu(0, 0, 0, t->light ? 0.15f : 0.35f), label, end);
  ImDrawList_AddText_Vec2(dl, (ImVec2_c){p.x + (w - ts.x) / 2, ty}, mixu(tc, tc, 0, 1), label, end);
  return hit;
}
