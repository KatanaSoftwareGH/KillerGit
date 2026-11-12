// GitHub-flavoured markdown renderer for Dear ImGui.
// Blocks: ATX/setext headings, paragraphs (soft-wrap joined, hard breaks), nested/ordered/task lists,
// blockquotes + GitHub alerts, fenced/indented code, tables, rules, and common HTML (<p align>, <h1>, <img>, <a>, <br>, comments).
// Inline: **bold**, *italic*, ~~strike~~, `code`, [links](url), ![images](src), autolinks, entities, escapes.
#include "app.h"
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <cfloat>
#include <cstring>

static float S(float v) { return v * ImGui::GetFontSize() / 15.5f; }

namespace {

struct Run { std::string text; ImFont *font; ImVec4 color; bool code = false, strike = false, under = false; std::string url; };

std::string htmlAttr(const std::string &tag, const char *name) {
  std::string lt = lower(tag), key = std::string(" ") + name + "=";
  auto p = lt.find(key);
  if (p == std::string::npos) return "";
  p += key.size();
  if (p < tag.size() && (tag[p] == '"' || tag[p] == '\'')) {
    char q = tag[p]; auto e = tag.find(q, p + 1);
    return e == std::string::npos ? "" : tag.substr(p + 1, e - p - 1);
  }
  auto e = tag.find_first_of(" >/", p);
  return tag.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

std::string decodeEntities(std::string s) {
  static const std::pair<const char *, const char *> E[] = {
    { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&#39;", "'" }, { "&apos;", "'" },
    { "&nbsp;", " " }, { "&copy;", "\xC2\xA9" }, { "&reg;", "\xC2\xAE" }, { "&mdash;", "\xE2\x80\x94" }, { "&ndash;", "\xE2\x80\x93" },
    { "&hellip;", "\xE2\x80\xA6" }, { "&middot;", "\xC2\xB7" } };
  for (auto &[a, b] : E)
    for (size_t p = 0; (p = s.find(a, p)) != std::string::npos; p += strlen(b)) s.replace(p, strlen(a), b);
  return s;
}

bool isUrl(const std::string &u) { return startsWith(u, "http://") || startsWith(u, "https://") || startsWith(u, "mailto:"); }

// Splits a "url "title"" link target down to the url.
std::string linkTarget(std::string u) {
  u = trim(u);
  if (!u.empty() && u[0] == '<') { auto e = u.find('>'); return e == std::string::npos ? u : u.substr(1, e - 1); }
  return u.substr(0, u.find(' '));
}

// Finds the ')' closing a link target starting after '(' at `open`, allowing balanced parens in URLs.
size_t closeParen(const std::string &s, size_t open) {
  int depth = 0;
  for (size_t k = open; k < s.size(); k++) {
    if (s[k] == '(') depth++;
    else if (s[k] == ')' && --depth == 0) return k;
  }
  return std::string::npos;
}

size_t closeBracket(const std::string &s, size_t open) {
  int depth = 0;
  for (size_t k = open; k < s.size(); k++) {
    if (s[k] == '\\') { k++; continue; }
    if (s[k] == '[') depth++;
    else if (s[k] == ']' && --depth == 0) return k;
  }
  return std::string::npos;
}

void parseInline(const std::string &s, ImFont *base, std::vector<Run> &out, bool bold = false, bool strike = false, const std::string &url = "") {
  ImFont *font = bold ? F.bold : base;
  ImVec4 textCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
  ImVec4 color = url.empty() ? textCol : C.accent;
  std::string buf;
  auto push = [&](Run r) { r.strike = r.strike || strike; if (r.url.empty()) r.url = url; r.under = r.under || !url.empty(); out.push_back(std::move(r)); };
  auto flush = [&] { if (!buf.empty()) { push(Run{ decodeEntities(buf), font, color }); buf.clear(); } };
  auto image = [&](const std::string &alt, const std::string &src) {
    Run r{ "[" + (alt.empty() ? std::string("image") : alt) + "]", font, C.muted };
    r.url = url.empty() ? src : url; r.under = false;
    out.push_back(r);
  };

  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '\\' && i + 1 < s.size() && ispunct((unsigned char)s[i + 1])) { buf += s[++i]; continue; }

    if (c == '`') {
      size_t n = 1; while (i + n < s.size() && s[i + n] == '`') n++;
      auto e = s.find(std::string(n, '`'), i + n);
      if (e != std::string::npos) {
        flush();
        std::string code = s.substr(i + n, e - i - n);
        if (code.size() > 1 && code.front() == ' ' && code.back() == ' ') code = code.substr(1, code.size() - 2);
        Run r{ code, F.mono, url.empty() ? textCol : C.accent }; r.code = true; push(r);
        i = e + n - 1; continue;
      }
      buf.append(n, '`'); i += n - 1; continue;
    }

    if ((c == '*' || c == '_') && i + 2 < s.size() && s[i + 1] == c && s[i + 2] == c) {   // ***bold italic***
      auto e = s.find(std::string(3, c), i + 3);
      if (e != std::string::npos) { flush(); parseInline(s.substr(i + 3, e - i - 3), base, out, true, strike, url); i = e + 2; continue; }
    }
    if ((c == '*' || c == '_') && i + 1 < s.size() && s[i + 1] == c && i + 2 < s.size() && !isspace((unsigned char)s[i + 2])) {
      auto e = s.find(std::string(2, c), i + 2);
      if (e != std::string::npos && e > i + 2) { flush(); parseInline(s.substr(i + 2, e - i - 2), base, out, true, strike, url); i = e + 1; continue; }
    }
    if (c == '~' && i + 1 < s.size() && s[i + 1] == '~') {
      auto e = s.find("~~", i + 2);
      if (e != std::string::npos) { flush(); parseInline(s.substr(i + 2, e - i - 2), base, out, bold, true, url); i = e + 1; continue; }
    }
    // *italic* / _italic_ (underscore only at word boundaries, so snake_case survives). No italic face: render plain.
    if ((c == '*' || c == '_') && i + 1 < s.size() && !isspace((unsigned char)s[i + 1]) && s[i + 1] != c &&
        (c == '*' || i == 0 || !isalnum((unsigned char)s[i - 1]))) {
      size_t e = i + 1;
      while ((e = s.find(c, e)) != std::string::npos) {
        bool okEnd = !isspace((unsigned char)s[e - 1]) && (c == '*' || e + 1 >= s.size() || !isalnum((unsigned char)s[e + 1]));
        if (okEnd) break;
        e++;
      }
      if (e != std::string::npos) { flush(); parseInline(s.substr(i + 1, e - i - 1), base, out, bold, strike, url); i = e; continue; }
    }

    if (c == '!' && i + 1 < s.size() && s[i + 1] == '[') {
      size_t close = closeBracket(s, i + 1);
      if (close != std::string::npos && close + 1 < s.size() && s[close + 1] == '(') {
        size_t end = closeParen(s, close + 1);
        if (end != std::string::npos) { flush(); image(s.substr(i + 2, close - i - 2), linkTarget(s.substr(close + 2, end - close - 2))); i = end; continue; }
      }
    }
    if (c == '[') {
      size_t close = closeBracket(s, i);
      if (close != std::string::npos && close + 1 < s.size() && s[close + 1] == '(') {
        size_t end = closeParen(s, close + 1);
        if (end != std::string::npos) {
          flush();
          parseInline(s.substr(i + 1, close - i - 1), base, out, bold, strike, linkTarget(s.substr(close + 2, end - close - 2)));
          i = end; continue;
        }
      }
    }

    if (c == '<') {
      auto e = s.find('>', i);
      if (e != std::string::npos) {
        std::string tag = s.substr(i, e - i + 1), lt = lower(tag);
        auto between = [&](const char *closeTag, auto fn) {
          auto ce = lt.find(closeTag, e);
          if (ce == std::string::npos) return false;
          flush(); fn(s.substr(e + 1, ce - e - 1)); i = ce + strlen(closeTag) - 1; return true;
        };
        if (startsWith(lt, "<http://") || startsWith(lt, "<https://") || startsWith(lt, "<mailto:")) {
          flush(); std::string u = tag.substr(1, tag.size() - 2);
          Run r{ u, font, C.accent }; r.url = u; r.under = true; push(r); i = e; continue;
        }
        if (startsWith(lt, "<br")) { flush(); out.push_back(Run{ "\n", font, color }); i = e; continue; }
        if (startsWith(lt, "<img")) { flush(); image(htmlAttr(tag, "alt"), htmlAttr(tag, "src")); i = e; continue; }
        if (startsWith(lt, "<a ") || lt == "<a>") {
          std::string href = htmlAttr(tag, "href");
          if (between("</a>", [&](const std::string &in) { parseInline(in, base, out, bold, strike, href.empty() ? url : href); })) continue;
        }
        if (lt == "<b>" || lt == "<strong>") {
          if (between(lt == "<b>" ? "</b>" : "</strong>", [&](const std::string &in) { parseInline(in, base, out, true, strike, url); })) continue;
        }
        if (lt == "<s>" || lt == "<del>" || lt == "<strike>") {
          std::string ct = "</" + lt.substr(1);
          if (between(ct.c_str(), [&](const std::string &in) { parseInline(in, base, out, bold, true, url); })) continue;
        }
        if (lt == "<code>" || lt == "<kbd>") {
          std::string ct = "</" + lt.substr(1);
          if (between(ct.c_str(), [&](const std::string &in) { Run r{ decodeEntities(in), F.mono, textCol }; r.code = true; push(r); })) continue;
        }
        if (e - i < 400 && i + 1 < s.size() && (isalpha((unsigned char)s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '!')) { flush(); i = e; continue; }   // other tags: drop
      }
    }

    if (c == 'h' && url.empty() && (s.compare(i, 7, "http://") == 0 || s.compare(i, 8, "https://") == 0) && (i == 0 || !isalnum((unsigned char)s[i - 1]))) {
      size_t e = i;
      while (e < s.size() && !isspace((unsigned char)s[e]) && s[e] != '<') e++;
      while (e > i && strchr(".,;:!?)'\"", s[e - 1])) e--;
      flush();
      std::string u = s.substr(i, e - i);
      Run r{ u, font, C.accent }; r.url = u; r.under = true; push(r);
      i = e - 1; continue;
    }
    buf += c;
  }
  flush();
}

// Word-wraps runs into `width` from the cursor, draws them, and advances the cursor.
void drawRuns(const std::vector<Run> &runs, float width, bool center) {
  if (runs.empty()) return;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 start = ImGui::GetCursorScreenPos();
  float maxFont = 0; for (auto &r : runs) maxFont = std::max(maxFont, r.font->FontSize);
  float lineH = maxFont + S(5);
  struct Piece { const Run *r; std::string t; float w, ink; };
  std::vector<std::vector<Piece>> lines(1);
  float x = 0;
  for (auto &r : runs) {
    if (r.text == "\n") { lines.emplace_back(); x = 0; continue; }
    float pad = r.code ? S(8) : 0;
    size_t i = 0;
    while (i < r.text.size()) {
      size_t j = i;
      while (j < r.text.size() && r.text[j] != ' ') j++;
      size_t k = j; while (k < r.text.size() && r.text[k] == ' ') k++;
      std::string word = r.text.substr(i, j - i), sp = r.text.substr(j, k - j);
      float ink = r.font->CalcTextSizeA(r.font->FontSize, FLT_MAX, 0, word.c_str()).x + pad;
      float spw = r.font->CalcTextSizeA(r.font->FontSize, FLT_MAX, 0, sp.c_str()).x;
      if (word.empty()) {   // leading spaces of a run
        if (x > 0) { lines.back().push_back({ &r, sp, spw, 0 }); x += spw; }
        i = k; continue;
      }
      if (x > 0 && x + ink > width) { lines.emplace_back(); x = 0; }
      // a single word wider than the line: let it overflow rather than loop
      lines.back().push_back({ &r, word, ink + spw, ink }); x += ink + spw;
      i = k;
    }
  }
  float y = start.y;
  for (auto &ln : lines) {
    float lw = 0; for (auto &p : ln) lw += p.w;
    if (!ln.empty()) lw -= ln.back().w - ln.back().ink;
    float px = start.x + (center ? std::max(0.f, (width - lw) / 2) : 0);
    for (auto &p : ln) {
      const Run &r = *p.r;
      float fy = y + (maxFont - r.font->FontSize) * 0.75f + S(2);
      if (p.ink > 0) {
        if (r.code) {
          dl->AddRectFilled(ImVec2(px, fy - S(1)), ImVec2(px + p.ink, fy + r.font->FontSize + S(1)), col(C.side), S(3));
          dl->AddText(r.font, r.font->FontSize, ImVec2(px + S(4), fy), col(r.color), p.t.c_str());
        } else dl->AddText(r.font, r.font->FontSize, ImVec2(px, fy), col(r.color), p.t.c_str());
        float base = fy + r.font->FontSize;
        if (r.strike) dl->AddLine(ImVec2(px, fy + r.font->FontSize * 0.55f), ImVec2(px + p.ink, fy + r.font->FontSize * 0.55f), col(r.color), S(1));
        if (r.under) dl->AddLine(ImVec2(px, base), ImVec2(px + p.ink, base), col(r.color, 0.5f), S(1));
      }
      if (!r.url.empty() && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
          ImGui::IsMouseHoveringRect(ImVec2(px, y), ImVec2(px + p.w, y + lineH))) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s", r.url.c_str());
        if (ImGui::IsMouseClicked(0) && isUrl(r.url)) openUrl(r.url);
      }
      px += p.w;
    }
    y += lineH;
  }
  ImGui::Dummy(ImVec2(width, y - start.y));
}

void inlineBlock(const std::string &text, ImFont *font, float width, bool center = false) {
  std::vector<Run> runs;
  parseInline(trim(text), font, runs);
  drawRuns(runs, width, center);
}

bool isRule(const std::string &t) {
  if (t.size() < 3) return false;
  char c = t[0];
  if (c != '-' && c != '*' && c != '_') return false;
  int n = 0;
  for (char x : t) { if (x == c) n++; else if (x != ' ') return false; }
  return n >= 3;
}

bool listItem(const std::string &t, bool &ordered, size_t &skip) {
  if (t.size() > 1 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') { ordered = false; skip = 2; return true; }
  if (t == "-" || t == "*") { ordered = false; skip = 1; return true; }
  size_t d = 0; while (d < t.size() && d < 9 && isdigit((unsigned char)t[d])) d++;
  if (d && d + 1 < t.size() && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ') { ordered = true; skip = d + 2; return true; }
  return false;
}

Strings tableCells(std::string t) {
  t = trim(t);
  if (!t.empty() && t[0] == '|') t.erase(0, 1);
  if (!t.empty() && t.back() == '|' && (t.size() < 2 || t[t.size() - 2] != '\\')) t.pop_back();
  Strings cells; std::string cur; bool inCode = false;
  for (size_t i = 0; i < t.size(); i++) {
    if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') { cur += '|'; i++; }
    else if (t[i] == '`') { inCode = !inCode; cur += '`'; }
    else if (t[i] == '|' && !inCode) { cells.push_back(trim(cur)); cur.clear(); }
    else cur += t[i];
  }
  cells.push_back(trim(cur));
  return cells;
}

bool isTableSep(const std::string &t) {
  if (t.find('-') == std::string::npos) return false;
  for (char c : t) if (!strchr("|-: ", c)) return false;
  return t.find('|') != std::string::npos || t.find(':') != std::string::npos;
}

size_t indentOf(const std::string &l) { auto p = l.find_first_not_of(' '); return p == std::string::npos ? l.size() : p; }

bool isFence(const std::string &t) { return startsWith(t, "```") || startsWith(t, "~~~"); }

bool startsBlock(const std::string &t) {
  bool o; size_t k;
  return t[0] == '#' || t[0] == '>' || isFence(t) || isRule(t) || listItem(t, o, k) || startsWith(t, "<!--") ||
         startsWith(lower(t), "<p") || startsWith(lower(t), "<div") || startsWith(lower(t), "</p") || startsWith(lower(t), "</div") ||
         (startsWith(lower(t), "<h") && t.size() > 2 && isdigit((unsigned char)t[2]));
}

void codeBlock(const Strings &code, float W) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImGui::PushFont(F.mono);
  float lh = ImGui::GetTextLineHeightWithSpacing();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float h = lh * (float)code.size() + S(20);
  dl->AddRectFilled(p, ImVec2(p.x + W, p.y + h), col(C.side), S(6));
  ImGui::PushClipRect(p, ImVec2(p.x + W - S(6), p.y + h), true);
  ImGui::SetCursorScreenPos(ImVec2(p.x + S(12), p.y + S(10)));
  ImGui::BeginGroup();
  for (auto &c : code) ImGui::TextUnformatted(c.empty() ? " " : c.c_str());
  ImGui::EndGroup();
  ImGui::PopClipRect();
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
  ImGui::Dummy(ImVec2(W, 0));
}

struct Ctx { int center = 0; int tables = 0; };

void renderBlocks(Strings L, float W, Ctx &cx);

void heading(const std::string &txt, int lvl, float W, bool center) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImGui::Dummy(ImVec2(0, lvl <= 2 ? S(6) : S(2)));
  ImFont *f = lvl == 1 ? F.headingLarge : lvl == 2 ? F.heading : F.bold;
  inlineBlock(txt, f, W, center);
  if (lvl <= 2) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(p.x, p.y + S(1)), ImVec2(p.x + W, p.y + S(1)), col(C.border), S(1));
    ImGui::Dummy(ImVec2(0, S(6)));
  }
}

void renderBlocks(Strings L, float W, Ctx &cx) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float gap = S(10);

  for (size_t i = 0; i < L.size();) {
    std::string raw = L[i], t = trim(raw), lt = lower(t);
    size_t ind = indentOf(raw);
    if (t.empty()) { i++; continue; }

    if (startsWith(t, "<!--")) {
      while (i < L.size() && L[i].find("-->") == std::string::npos) i++;
      i++; continue;
    }

    if (isFence(t)) {
      std::string fence = t.substr(0, 3);
      Strings code;
      for (i++; i < L.size() && !startsWith(trim(L[i]), fence); i++) {
        std::string c = L[i];
        size_t strip = std::min(ind, indentOf(c));   // fences indented inside lists
        code.push_back(c.substr(std::min(strip, c.size())));
      }
      i++;
      codeBlock(code, W);
      ImGui::Dummy(ImVec2(0, gap));
      continue;
    }

    // HTML block wrappers: <p align="center">, <div ...>, </p>, </div>
    if ((startsWith(lt, "<p") && (lt.size() == 2 || lt[2] == '>' || lt[2] == ' ')) || startsWith(lt, "<div")) {
      bool c = lower(htmlAttr(t, "align")) == "center";
      auto gt = t.find('>');
      std::string rest = gt == std::string::npos ? "" : t.substr(gt + 1);
      std::string lr = lower(rest);
      size_t closeAt = std::min(lr.find("</p>"), lr.find("</div>"));
      if (closeAt != std::string::npos) {
        // single-line <p ...>content</p>
        inlineBlock(rest.substr(0, closeAt), F.body, W, c || cx.center);
        ImGui::Dummy(ImVec2(0, gap));
        i++; continue;
      }
      if (c) cx.center++;
      if (trim(rest).empty()) { i++; continue; }
      L[i] = rest;   // content follows the opening tag on the same line
      continue;
    }
    if (startsWith(lt, "</p") || startsWith(lt, "</div")) { if (cx.center) cx.center--; i++; continue; }

    // <h1>..</h1>
    if (startsWith(lt, "<h") && lt.size() > 2 && lt[2] >= '1' && lt[2] <= '6') {
      int lvl = lt[2] - '0';
      auto o = t.find('>');
      std::string inner = o == std::string::npos ? "" : t.substr(o + 1);
      size_t e = lower(inner).find("</h");
      // heading may span lines until </hN>
      while (e == std::string::npos && i + 1 < L.size()) { inner += " " + trim(L[++i]); e = lower(inner).find("</h"); }
      if (e != std::string::npos) inner = inner.substr(0, e);
      heading(inner, lvl, W, cx.center || lower(htmlAttr(t, "align")) == "center");
      ImGui::Dummy(ImVec2(0, gap * 0.5f));
      i++; continue;
    }

    if (t[0] == '#') {
      size_t lvl = t.find_first_not_of('#');
      if (lvl == std::string::npos || (lvl <= 6 && t[lvl] == ' ')) {
        std::string txt = lvl == std::string::npos ? "" : trim(t.substr(lvl));
        size_t h = txt.find_last_not_of('#');   // closing #s
        if (h != std::string::npos && h + 1 < txt.size() && txt[h] == ' ') txt = trim(txt.substr(0, h));
        heading(txt, (int)std::min<size_t>(lvl == std::string::npos ? t.size() : lvl, 6), W, cx.center);
        ImGui::Dummy(ImVec2(0, gap * 0.5f));
        i++; continue;
      }
    }

    if (isRule(t)) {
      ImVec2 p = ImGui::GetCursorScreenPos();
      dl->AddLine(ImVec2(p.x, p.y + S(6)), ImVec2(p.x + W, p.y + S(6)), col(C.border), S(2));
      ImGui::Dummy(ImVec2(W, S(12) + gap));
      i++; continue;
    }

    if (t.find('|') != std::string::npos && i + 1 < L.size() && isTableSep(trim(L[i + 1]))) {
      Strings head = tableCells(t), sep = tableCells(L[i + 1]);
      std::vector<int> align;   // 0 left, 1 center, 2 right
      for (auto &a : sep) align.push_back(endsWith(a, ":") ? (startsWith(a, ":") ? 1 : 2) : 0);
      std::vector<Strings> rows;
      for (i += 2; i < L.size() && !trim(L[i]).empty() && trim(L[i]).find('|') != std::string::npos; i++) rows.push_back(tableCells(L[i]));
      int nc = std::max(1, (int)head.size());
      ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(10), S(6)));
      if (ImGui::BeginTable(("##mdt" + std::to_string(cx.tables++)).c_str(), nc,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp, ImVec2(W, 0))) {
        auto cell = [&](const std::string &s, int c, ImFont *f) {
          std::vector<Run> runs; parseInline(s, f, runs);
          float cw = ImGui::GetContentRegionAvail().x;
          int a = c < (int)align.size() ? align[c] : 0;
          if (a == 2) {   // right-align: measure and offset
            float w = 0; for (auto &r : runs) w += r.font->CalcTextSizeA(r.font->FontSize, FLT_MAX, 0, r.text.c_str()).x + (r.code ? S(8) : 0);
            if (w < cw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cw - w);
            drawRuns(runs, std::min(w + 1, cw), false);
          } else drawRuns(runs, cw, a == 1);
        };
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int c = 0; c < nc; c++) { ImGui::TableNextColumn(); cell(c < (int)head.size() ? head[c] : "", c, F.bold); }
        for (auto &r : rows) {
          ImGui::TableNextRow();
          for (int c = 0; c < nc; c++) { ImGui::TableNextColumn(); if (c < (int)r.size()) cell(r[c], c, F.body); }
        }
        ImGui::EndTable();
      }
      ImGui::PopStyleVar();
      ImGui::Dummy(ImVec2(0, gap));
      continue;
    }

    if (t[0] == '>') {
      Strings q;
      while (i < L.size()) {
        std::string x = trim(L[i]);
        if (x.empty()) break;
        if (x[0] == '>') { x = x.substr(1); if (!x.empty() && x[0] == ' ') x.erase(0, 1); }
        else if (q.empty() || startsBlock(x)) break;   // lazy continuation
        q.push_back(x); i++;
      }
      std::string alert;
      if (!q.empty()) {
        std::string f = trim(q[0]);
        if (startsWith(f, "[!") && endsWith(f, "]")) { alert = lower(f.substr(2, f.size() - 3)); q.erase(q.begin()); }
      }
      static const std::pair<const char *, const char *> A[] = {
        { "note", "#5b8def" }, { "tip", "#4f9d69" }, { "important", "#a26bd1" }, { "warning", "#c58a2c" }, { "caution", "#d0566c" } };
      ImVec4 bar = C.border;
      for (auto &[k, v] : A) if (alert == k) bar = hexColor(v);
      ImVec2 p = ImGui::GetCursorScreenPos();
      float pad = S(16);
      ImGui::Indent(pad);
      ImGui::BeginGroup();
      if (!alert.empty()) {
        std::string title = alert; title[0] = (char)toupper((unsigned char)title[0]);
        ImGui::PushStyleColor(ImGuiCol_Text, bar);
        ImGui::PushFont(F.bold); ImGui::TextUnformatted(title.c_str()); ImGui::PopFont();
        ImGui::PopStyleColor();
      }
      ImGui::PushStyleColor(ImGuiCol_Text, alert.empty() ? C.muted : C.text);
      renderBlocks(q, W - pad, cx);
      ImGui::PopStyleColor();
      ImGui::EndGroup();
      ImGui::Unindent(pad);
      float bottom = ImGui::GetItemRectMax().y - gap;   // inner blocks end with a gap
      dl->AddRectFilled(p, ImVec2(p.x + S(3), std::max(p.y + S(4), bottom)), col(bar));
      ImGui::SetCursorScreenPos(ImVec2(p.x, bottom));
      ImGui::Dummy(ImVec2(0, gap));
      continue;
    }

    bool ordered; size_t skip;
    if (listItem(t, ordered, skip)) {
      // Gather one list: items at >= ind, their nested lines, and lazy continuations.
      size_t base = ind;
      int number = ordered ? atoi(t.c_str()) : 0;
      bool first = true;
      while (i < L.size()) {
        std::string r2 = L[i], t2 = trim(r2);
        if (t2.empty()) {
          size_t n = i + 1; while (n < L.size() && trim(L[n]).empty()) n++;
          bool o2; size_t s2;
          if (n < L.size() && indentOf(L[n]) >= base && (listItem(trim(L[n]), o2, s2) || indentOf(L[n]) > base)) { i = n; continue; }
          break;
        }
        size_t in2 = indentOf(r2);
        bool o2; size_t s2;
        if (in2 < base || (in2 == base && !listItem(t2, o2, s2))) break;
        if (in2 == base && !first && o2 != ordered) break;   // switching list types starts a new list
        if (in2 > base + 1) {
          // nested content belonging to the previous item: render recursively
          Strings sub; size_t subInd = in2;
          while (i < L.size()) {
            std::string x = L[i];
            if (!trim(x).empty() && indentOf(x) < subInd) break;
            if (trim(x).empty()) {
              size_t n = i + 1; while (n < L.size() && trim(L[n]).empty()) n++;
              if (n >= L.size() || indentOf(L[n]) < subInd) break;
            }
            sub.push_back(x.size() >= subInd ? x.substr(subInd) : ""); i++;
          }
          float indent = S(26);
          ImGui::Indent(indent);
          renderBlocks(sub, W - indent, cx);
          ImGui::Unindent(indent);
          continue;
        }
        listItem(t2, ordered, skip);
        first = false;
        std::string body = t2.substr(std::min(skip, t2.size()));
        int task = -1;
        if (startsWith(body, "[ ] ") || body == "[ ]") { task = 0; body = body.substr(std::min<size_t>(4, body.size())); }
        else if (startsWith(lower(body), "[x] ") || lower(body) == "[x]") { task = 1; body = body.substr(std::min<size_t>(4, body.size())); }
        // lazy continuation lines of this item
        size_t contentInd = in2 + skip;
        while (i + 1 < L.size()) {
          std::string nx = L[i + 1], nt = trim(nx);
          if (nt.empty() || startsBlock(nt)) break;
          if (indentOf(nx) > in2 + 1 && indentOf(nx) < contentInd) {}   // still a continuation
          else if (indentOf(nx) >= contentInd && listItem(nt, o2, s2)) break;
          bool hard = endsWith(body, "  ") || endsWith(body, "\\");
          if (endsWith(body, "\\")) body.pop_back();
          body = trim(body) + (hard ? "<br>" : " ") + nt;
          i++;
        }
        float indent = S(26);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float fs = F.body->FontSize;
        if (task >= 0) {
          float b = fs * 0.85f;
          ImVec2 a(p.x + indent - b - S(8), p.y + S(2) + (fs - b) / 2 + S(1));
          if (task) {
            dl->AddRectFilled(a, ImVec2(a.x + b, a.y + b), col(C.accent), S(3));
            dl->AddPolyline(std::array<ImVec2, 3>{ ImVec2(a.x + b * 0.22f, a.y + b * 0.52f), ImVec2(a.x + b * 0.42f, a.y + b * 0.72f),
                                                   ImVec2(a.x + b * 0.78f, a.y + b * 0.3f) }.data(), 3, IM_COL32_WHITE, 0, S(1.6f));
          } else dl->AddRect(a, ImVec2(a.x + b, a.y + b), col(C.muted), S(3), 0, S(1.2f));
        } else {
          std::string marker = ordered ? std::to_string(number++) + "." : "\xE2\x80\xA2";
          float mw = F.body->CalcTextSizeA(fs, FLT_MAX, 0, marker.c_str()).x;
          dl->AddText(F.body, fs, ImVec2(p.x + indent - mw - S(8), p.y + S(2)), col(C.muted), marker.c_str());
        }
        ImGui::Indent(indent);
        if (body.empty()) ImGui::Dummy(ImVec2(0, fs + S(5)));
        else if (task == 1) {
          ImGui::PushStyleColor(ImGuiCol_Text, C.muted);
          inlineBlock(body, F.body, W - indent);
          ImGui::PopStyleColor();
        } else inlineBlock(body, F.body, W - indent);
        ImGui::Unindent(indent);
        ImGui::Dummy(ImVec2(0, S(2)));
        i++;
      }
      ImGui::Dummy(ImVec2(0, gap));
      continue;
    }

    if (ind >= 4) {
      Strings code;
      while (i < L.size() && (trim(L[i]).empty() || indentOf(L[i]) >= 4)) { code.push_back(L[i].size() >= 4 ? L[i].substr(4) : ""); i++; }
      while (!code.empty() && trim(code.back()).empty()) code.pop_back();
      codeBlock(code, W);
      ImGui::Dummy(ImVec2(0, gap));
      continue;
    }

    // Paragraph: join soft-wrapped lines; a following === / --- line makes it a setext heading.
    std::string para; int setext = 0;
    while (i < L.size()) {
      std::string r2 = L[i], t2 = trim(r2);
      if (t2.empty()) break;
      if (!para.empty()) {
        if (t2.find_first_not_of('=') == std::string::npos) { setext = 1; i++; break; }
        if (t2.size() >= 2 && t2.find_first_not_of('-') == std::string::npos) { setext = 2; i++; break; }
        if (startsBlock(t2) || (t2.find('|') != std::string::npos && i + 1 < L.size() && isTableSep(trim(L[i + 1])))) break;
        bool hard = endsWith(para, "  ") || endsWith(para, "\\");
        if (endsWith(para, "\\")) para.pop_back();
        para = trim(para) + (hard ? "<br>" : " ");
      }
      para += r2.substr(indentOf(r2));
      i++;
    }
    if (setext) heading(para, setext, W, cx.center);
    else {
      // trailing </p> / </div> on the paragraph's last line
      std::string lp = lower(trim(para));
      bool closes = false;
      for (const char *tag : { "</p>", "</div>" })
        if (endsWith(lp, tag)) { para = trim(para); para.erase(para.size() - strlen(tag)); closes = true; break; }
      inlineBlock(para, F.body, W, cx.center > 0);
      if (closes && cx.center) cx.center--;
    }
    ImGui::Dummy(ImVec2(0, gap));
  }
}

}  // namespace

void Markdown(const std::string &md) {
  Strings L = split(md, '\n', true);
  for (auto &l : L) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    for (size_t p; (p = l.find('\t')) != std::string::npos;) l.replace(p, 1, "    ");
  }
  Ctx cx;
  ImGui::PushID("##md");
  renderBlocks(L, ImGui::GetContentRegionAvail().x, cx);
  ImGui::PopID();
}
