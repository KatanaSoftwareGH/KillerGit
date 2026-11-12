// Linux counterpart of res/app.rc: embeds the fonts and icon into the binary with the assembler's .incbin.
#include "util.h"

#define KG_EMBED(name, file)                                  \
  asm(".section .rodata\n"                                    \
      ".balign 16\n"                                          \
      ".global " #name "_start\n" #name "_start:\n"           \
      ".incbin \"" KG_RES_DIR "/" file "\"\n"                 \
      ".global " #name "_end\n" #name "_end:\n"               \
      ".byte 0\n"                                             \
      ".previous\n");                                         \
  extern "C" const unsigned char name##_start[], name##_end[];

KG_EMBED(kg_font_regular, "fonts/WorkSans-Regular.ttf")
KG_EMBED(kg_font_semibold, "fonts/WorkSans-SemiBold.ttf")
KG_EMBED(kg_icon_png, "icon.png")

bool embeddedResource(int id, const void *&data, size_t &size) {
  const unsigned char *a = nullptr, *b = nullptr;
  switch (id) {
    case 101: a = kg_font_regular_start; b = kg_font_regular_end; break;
    case 102: a = kg_font_semibold_start; b = kg_font_semibold_end; break;
    case 103: a = kg_icon_png_start; b = kg_icon_png_end; break;
    default: return false;
  }
  data = a; size = (size_t)(b - a);
  return size > 0;
}
