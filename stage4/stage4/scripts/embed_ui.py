#!/usr/bin/env python3
"""Embed ui.html into src/core/ui_html.h as a C string constant."""
import sys

src = "ui.html"
dst = "src/core/ui_html.h"

with open(src, "rb") as f:
    data = f.read()

# Escape for C string literal
out = []
for byte in data:
    if byte == ord('\\'):
        out.append('\\\\')
    elif byte == ord('"'):
        out.append('\\"')
    elif byte == ord('\n'):
        out.append('\\n"\n    "')
    elif byte == ord('\r'):
        pass  # skip CR, only keep LF
    elif byte == ord('\t'):
        out.append('\\t')
    elif 32 <= byte < 127:
        out.append(chr(byte))
    else:
        out.append(f'\\x{byte:02x}')

content = f"""/* ui_html.h — Auto-generated from ui.html. Do not edit. */
#ifndef HUGO_UI_HTML_H
#define HUGO_UI_HTML_H

static const char UI_HTML[] =
    "{''.join(out)}";

#endif
"""

with open(dst, "w", encoding="utf-8") as f:
    f.write(content)

print(f"embedded {len(data)} bytes → {dst}")
