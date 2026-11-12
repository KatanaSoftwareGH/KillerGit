// Claude-inspired look: warm paper/charcoal neutrals, terracotta accent, Work Sans throughout, soft rounded surfaces.
#include "app.h"
#include <imgui_internal.h>
#include <windows.h>
#include <algorithm>
#include <cmath>

Palette C;
Fonts F;
static float g_uiScale = 1.f;

ImU32 col(const ImVec4 &c, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * a)); }
ImVec4 hexColor(const std::string &h) {
  unsigned v = 0; if (h.size() >= 7) sscanf_s(h.c_str() + 1, "%06x", &v);
  return ImVec4(((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, 1);
}
std::string toHex(const ImVec4 &c) {
  char b[8]; snprintf(b, sizeof b, "#%02x%02x%02x", (int)(c.x * 255 + .5f), (int)(c.y * 255 + .5f), (int)(c.z * 255 + .5f)); return b;
}
static ImVec4 mix(ImVec4 a, ImVec4 b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1); }

bool systemPrefersDark() {
  DWORD v = 1, sz = sizeof v;
  RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
  return v == 0;
}

void applyTheme(bool dark, ImVec4 accent) {
  if (dark) {
    C.bg = hexColor("#262624"); C.side = hexColor("#1f1e1d"); C.card = hexColor("#30302e"); C.cardHover = hexColor("#3a3a37");
    C.border = hexColor("#3e3e3a"); C.text = hexColor("#f5f4ef"); C.muted = hexColor("#a6a39a"); C.faint = hexColor("#6f6d66");
    C.input = hexColor("#1f1e1d"); C.add = hexColor("#8fc79a"); C.del = hexColor("#e8918a");
    C.addBg = ImVec4(0.32f, 0.55f, 0.36f, 0.20f); C.delBg = ImVec4(0.75f, 0.33f, 0.30f, 0.20f); C.hunkBg = hexColor("#2b2b29");
    C.danger = hexColor("#e5675c");
  } else {
    C.bg = hexColor("#faf9f5"); C.side = hexColor("#f0eee6"); C.card = hexColor("#ffffff"); C.cardHover = hexColor("#f5f3ec");
    C.border = hexColor("#e5e2d8"); C.text = hexColor("#1f1e1d"); C.muted = hexColor("#73726c"); C.faint = hexColor("#a8a69e");
    C.input = hexColor("#ffffff"); C.add = hexColor("#2f6b3a"); C.del = hexColor("#a33a32");
    C.addBg = ImVec4(0.42f, 0.72f, 0.45f, 0.16f); C.delBg = ImVec4(0.90f, 0.42f, 0.38f, 0.14f); C.hunkBg = hexColor("#f3f1ea");
    C.danger = hexColor("#b53b30");
  }
  C.accent = accent;
  C.accentHover = mix(accent, dark ? ImVec4(1, 1, 1, 1) : ImVec4(0, 0, 0, 1), 0.12f);
  C.accentText = ImVec4(1, 1, 1, 1);
  C.sel = ImVec4(accent.x, accent.y, accent.z, dark ? 0.22f : 0.14f);

  ImGuiStyle &s = ImGui::GetStyle();
  s = ImGuiStyle();  // start clean so repeated calls (theme/DPI changes) don't compound scaling
  s.WindowRounding = 12; s.ChildRounding = 10; s.FrameRounding = 8; s.PopupRounding = 10; s.GrabRounding = 8;
  s.ScrollbarRounding = 8; s.TabRounding = 8; s.WindowBorderSize = 0; s.ChildBorderSize = 0; s.PopupBorderSize = 1;
  s.FrameBorderSize = 1; s.WindowPadding = ImVec2(18, 16); s.FramePadding = ImVec2(10, 7); s.ItemSpacing = ImVec2(8, 8);
  s.ItemInnerSpacing = ImVec2(8, 6); s.ScrollbarSize = 10; s.IndentSpacing = 14; s.SelectableTextAlign = ImVec2(0, 0.5f);
  s.WindowTitleAlign = ImVec2(0, 0.5f); s.SeparatorTextBorderSize = 1;

  ImVec4 *c = s.Colors;
  c[ImGuiCol_Text] = C.text; c[ImGuiCol_TextDisabled] = C.faint;
  c[ImGuiCol_WindowBg] = C.bg; c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_PopupBg] = C.card;
  c[ImGuiCol_Border] = C.border; c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = C.input; c[ImGuiCol_FrameBgHovered] = C.input; c[ImGuiCol_FrameBgActive] = C.input;
  c[ImGuiCol_TitleBg] = C.card; c[ImGuiCol_TitleBgActive] = C.card; c[ImGuiCol_TitleBgCollapsed] = C.card;
  c[ImGuiCol_MenuBarBg] = C.side; c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = ImVec4(C.muted.x, C.muted.y, C.muted.z, 0.25f);
  c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(C.muted.x, C.muted.y, C.muted.z, 0.45f);
  c[ImGuiCol_ScrollbarGrabActive] = ImVec4(C.muted.x, C.muted.y, C.muted.z, 0.6f);
  c[ImGuiCol_CheckMark] = C.accentText; c[ImGuiCol_SliderGrab] = C.accent; c[ImGuiCol_SliderGrabActive] = C.accentHover;
  c[ImGuiCol_Button] = C.card; c[ImGuiCol_ButtonHovered] = C.cardHover; c[ImGuiCol_ButtonActive] = C.border;
  c[ImGuiCol_Header] = C.sel; c[ImGuiCol_HeaderHovered] = ImVec4(C.muted.x, C.muted.y, C.muted.z, 0.10f);
  c[ImGuiCol_HeaderActive] = C.sel;
  c[ImGuiCol_Separator] = C.border; c[ImGuiCol_SeparatorHovered] = C.accent; c[ImGuiCol_SeparatorActive] = C.accent;
  c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0); c[ImGuiCol_ResizeGripHovered] = C.accent; c[ImGuiCol_ResizeGripActive] = C.accent;
  c[ImGuiCol_Tab] = C.side; c[ImGuiCol_TabHovered] = C.card; c[ImGuiCol_TabSelected] = C.card;
  c[ImGuiCol_TableHeaderBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_TableBorderStrong] = C.border; c[ImGuiCol_TableBorderLight] = C.border;
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_TableRowBgAlt] = ImVec4(C.muted.x, C.muted.y, C.muted.z, 0.04f);
  c[ImGuiCol_TextSelectedBg] = ImVec4(C.accent.x, C.accent.y, C.accent.z, 0.30f);
  c[ImGuiCol_NavCursor] = C.accent; c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.08f, 0.07f, 0.06f, dark ? 0.55f : 0.30f);
  c[ImGuiCol_PlotHistogram] = C.accent;
  s.ScaleAllSizes(g_uiScale);
}

void buildFonts(float scale) {
  ImGuiIO &io = ImGui::GetIO();
  g_uiScale = scale;
  io.Fonts->Clear();
  std::string dir;
  { wchar_t w[MAX_PATH]; GetWindowsDirectoryW(w, MAX_PATH); dir = narrow(w) + "\\Fonts\\"; }
  static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x0100, 0x024F, 0x0370, 0x03FF, 0x0400, 0x04FF, 0x2000, 0x206F, 0x2190, 0x21FF,
                                    0x2200, 0x22FF, 0x2300, 0x23FF, 0x2500, 0x25FF, 0x2600, 0x26FF, 0x2700, 0x27BF, 0 };
  // resId > 0: font embedded in the exe (RCDATA), with `file` from C:\Windows\Fonts as fallback.
  auto load = [&](const char *file, float size, const char *fallback = nullptr, int resId = 0) -> ImFont * {
    ImFontConfig cfg; cfg.OversampleH = 2; cfg.OversampleV = 1; cfg.PixelSnapH = false;
    ImFont *f = nullptr;
    if (resId) {
      HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(resId), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
      HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
      if (g) {
        cfg.FontDataOwnedByAtlas = false;  // resource memory belongs to the exe image
        f = io.Fonts->AddFontFromMemoryTTF(LockResource(g), (int)SizeofResource(nullptr, r), size * scale, &cfg, ranges);
        // Work Sans has no Greek/Cyrillic: borrow those from Segoe UI.
        std::string seg = dir + "segoeui.ttf";
        if (f && fileExists(seg)) {
          ImFontConfig m; m.MergeMode = true; m.OversampleH = 2;
          static const ImWchar extra[] = { 0x0370, 0x03FF, 0x0400, 0x04FF, 0x2000, 0x206F, 0 };
          io.Fonts->AddFontFromFileTTF(seg.c_str(), size * scale, &m, extra);
        }
      }
    }
    if (!f) {
      std::string p = dir + file;
      if (!fileExists(p) && fallback) p = dir + fallback;
      f = fileExists(p) ? io.Fonts->AddFontFromFileTTF(p.c_str(), size * scale, &cfg, ranges) : io.Fonts->AddFontDefault();
    }
    // Symbols (arrows, check marks, dots) come from Segoe UI Symbol.
    std::string sym = dir + "seguisym.ttf";
    if (fileExists(sym)) {
      ImFontConfig m; m.MergeMode = true; m.OversampleH = 2; m.GlyphMinAdvanceX = 0;
      static const ImWchar symRanges[] = { 0x2190, 0x21FF, 0x2200, 0x22FF, 0x2300, 0x23FF, 0x25A0, 0x25FF, 0x2600, 0x26FF, 0x2700, 0x27BF, 0 };
      io.Fonts->AddFontFromFileTTF(sym.c_str(), size * scale, &m, symRanges);
    }
    return f;
  };
  F.body = load("segoeui.ttf", 15.5f, nullptr, 101);
  F.bold = load("seguisb.ttf", 15.5f, "segoeuib.ttf", 102);
  F.small = load("segoeui.ttf", 13.f, nullptr, 101);
  F.title = load("seguisb.ttf", 20.f, "segoeuib.ttf", 102);
  // Headings: Work Sans SemiBold (the only other face is the monospace one for code).
  F.heading = load("seguisb.ttf", 18.f, "segoeuib.ttf", 102);
  F.headingLarge = load("seguisb.ttf", 28.f, "segoeuib.ttf", 102);
  F.mono = load("consola.ttf", 13.5f, "cour.ttf");
  io.FontDefault = F.body;
  io.Fonts->Build();
}

// ---------------- widgets ----------------
ImTextureID g_appIcon = 0;
void AppIcon(float size) {
  if (!g_appIcon) return;
  float lh = ImGui::GetTextLineHeight();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (lh - size) / 2);
  ImGui::Image(g_appIcon, ImVec2(size, size));
  ImGui::SameLine(0, size * 0.4f);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - (lh - size) / 2);
}

bool Button(const char *label, bool primary, bool danger, ImVec2 size) {
  int n = 0;
  if (primary) {
    ImGui::PushStyleColor(ImGuiCol_Button, danger ? C.danger : C.accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, danger ? C.danger : C.accentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, danger ? C.danger : C.accentHover);
    ImGui::PushStyleColor(ImGuiCol_Text, C.accentText);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    n = 5;
  } else if (danger) {
    ImGui::PushStyleColor(ImGuiCol_Text, C.danger); n = 1;
  }
  bool r = ImGui::Button(label, size);
  ImGui::PopStyleColor(n);
  return r;
}

bool IconButton(const char *icon, const char *tip) {
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(C.muted.x, C.muted.y, C.muted.z, 0.14f));
  bool r = ImGui::Button(icon);
  ImGui::PopStyleColor(3);
  if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tip);
  return r;
}

bool ToolbarItem(const char *id, const char *small, const std::string &value, float width, const char *tip) {
  ImVec2 pos = ImGui::GetCursorScreenPos();
  float h = ImGui::GetFrameHeight() * 1.55f;
  bool clicked = ImGui::InvisibleButton(id, ImVec2(width, h));
  bool hov = ImGui::IsItemHovered();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  if (hov) dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + h), col(C.muted, 0.10f), 10);
  ImGui::PushFont(F.small);
  dl->AddText(ImVec2(pos.x + 12, pos.y + 5), col(C.muted), small);
  ImGui::PopFont();
  ImGui::PushFont(F.bold);
  ImVec4 clip(pos.x, pos.y, pos.x + width - 8, pos.y + h);
  dl->AddText(F.bold, F.bold->FontSize, ImVec2(pos.x + 12, pos.y + h - F.bold->FontSize - 5), col(C.text), value.c_str(), nullptr, 0, &clip);
  ImGui::PopFont();
  if (tip && hov) ImGui::SetTooltip("%s", tip);
  return clicked;
}

void Heading(const char *text, bool large) {
  ImGui::PushFont(large ? F.headingLarge : F.heading);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
}
void Muted(const std::string &t) {
  ImGui::PushStyleColor(ImGuiCol_Text, C.muted);
  ImGui::TextWrapped("%s", t.c_str());
  ImGui::PopStyleColor();
}

void Spinner(float r, float th) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(r * 2, r * 2));
  ImDrawList *dl = ImGui::GetWindowDrawList();
  float t = (float)ImGui::GetTime() * 5.f;
  ImVec2 c(p.x + r, p.y + r);
  dl->PathArcTo(c, r - th, 0, 2 * IM_PI, 32); dl->PathStroke(col(C.border), 0, th);
  dl->PathArcTo(c, r - th, t, t + IM_PI * 0.6f, 24); dl->PathStroke(col(C.accent), 0, th);
}

bool Pill(const char *label, bool active) {
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 6));
  ImGui::PushStyleColor(ImGuiCol_Button, active ? C.card : ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? C.card : ImVec4(C.muted.x, C.muted.y, C.muted.z, 0.10f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, C.card);
  ImGui::PushStyleColor(ImGuiCol_Border, active ? C.border : ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_Text, active ? C.text : C.muted);
  if (active) ImGui::PushFont(F.bold);
  bool r = ImGui::Button(label);
  if (active) ImGui::PopFont();
  ImGui::PopStyleColor(5);
  ImGui::PopStyleVar(2);
  return r;
}

void Badge(char code) {
  ImVec4 c = code == 'A' || code == '?' ? hexColor("#4f8a5b") : code == 'D' ? hexColor("#c0504a") : code == 'R' ? hexColor("#6a7fd1")
           : code == 'U' ? hexColor("#d0892f") : hexColor("#b8893b");
  char t[2] = { code == '?' ? 'A' : code, 0 };
  ImVec2 p = ImGui::GetCursorScreenPos();
  float s = ImGui::GetTextLineHeight();
  ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + s, p.y + s), col(c, 0.18f), 5);
  ImVec2 ts = ImGui::CalcTextSize(t);
  ImGui::PushFont(F.bold);
  ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + (s - ts.x) / 2, p.y), col(c), t);
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(s, s));
}

void RefChip(const std::string &text, ImVec4 c) {
  ImGui::PushFont(F.small);
  ImVec2 ts = ImGui::CalcTextSize(text.c_str());
  ImVec2 p = ImGui::GetCursorScreenPos();
  float h = ImGui::GetTextLineHeight();
  p.y += 1;
  ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + ts.x + 12, p.y + h + 2), col(c, 0.16f), 6);
  ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 6, p.y + 1), col(c), text.c_str());
  ImGui::Dummy(ImVec2(ts.x + 12, h));
  ImGui::PopFont();
}

void BeginCard(const char *id) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, C.card);
  ImGui::PushStyleColor(ImGuiCol_Border, C.border);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
  ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
}
void EndCard() {
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
}

void Empty(const char *title, const std::string &sub) {
  ImVec2 avail = ImGui::GetContentRegionAvail();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail.y * 0.28f);
  ImGui::PushFont(F.heading);
  float w = ImGui::CalcTextSize(title).x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - w) / 2);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  if (!sub.empty()) {
    float sw = ImGui::CalcTextSize(sub.c_str()).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (avail.x - sw) / 2));
    ImGui::PushStyleColor(ImGuiCol_Text, C.muted); ImGui::TextUnformatted(sub.c_str()); ImGui::PopStyleColor();
  }
}

bool SelectableRow(const char *id, bool selected, float height) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  bool clicked = ImGui::InvisibleButton(id, ImVec2(w, height));
  bool hov = ImGui::IsItemHovered();
  if (selected || hov)
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + height), selected ? col(C.sel) : col(C.muted, 0.08f), 8);
  ImGui::SetCursorScreenPos(p);
  return clicked;
}
