#pragma once
#include <stddef.h>

/**
 * UTF-8 → printable-ASCII sanitizer for the on-device LCD.
 *
 * The LVGL Montserrat font we ship covers ASCII only; non-ASCII codepoints
 * (e.g. iOS's curly apostrophe U+2019 in "Alex's iPhone…", ellipsis
 * U+2026, em-dash U+2014, NBSP U+00A0) render as the .notdef block. This
 * helper walks the UTF-8 input and maps a small table of common typographic
 * codepoints to ASCII equivalents.
 *
 * Mappings:
 *   U+2018  '   →  '
 *   U+2019  '   →  '       (iOS hotspot SSIDs use this)
 *   U+201A  ‚   →  ,
 *   U+201B  ‛   →  '
 *   U+201C  "   →  "
 *   U+201D  "   →  "
 *   U+201E  „   →  "
 *   U+2013  –   →  -       (en-dash)
 *   U+2014  —   →  -       (em-dash)
 *   U+2026  …   →  ...     (single ellipsis → three dots)
 *   U+00A0  NBSP →  space
 *
 *   U+2600..U+27BF  (Misc Symbols + Dingbats, e.g. ❤ ★ ☎ ✓ ✗ ⚠)   →  *
 *   U+1F000..U+1FAFF (main emoji blocks, e.g. 😀 🚀 🤖 🩺)        →  *
 *
 *   U+200B, U+200C, U+200D    (zero-width spaces / ZWJ)            →  (drop)
 *   U+FE00..U+FE0F            (variation selectors, incl. emoji VS) →  (drop)
 *   U+FEFF                    (byte-order mark)                    →  (drop)
 *   U+1F3FB..U+1F3FF          (Fitzpatrick skin-tone modifiers)    →  (drop)
 *
 *   Anything else outside U+0020..U+007E (control chars, unmapped
 *   non-ASCII like Latin-1 accents or CJK, malformed UTF-8)        →  ?
 *
 * The output is always shorter or equal in bytes to the input (ellipsis is
 * 3 in, 3 out; everything else is 2-4 in, 1 out), so a buffer the same size
 * as the source is always large enough. Output is NUL-terminated as long as
 * dst_len >= 1; truncation can't split a multi-byte sequence because all
 * outputs are single-byte ASCII (except the 3-byte ellipsis, which is split
 * cleanly if it lands at the boundary).
 *
 * Pure CPU work, no allocation, safe on small task stacks.
 *
 * @param dst      Destination buffer (must be non-NULL if dst_len > 0).
 * @param dst_len  Size of dst in bytes (including the NUL terminator).
 * @param src      Source UTF-8 string (NUL-terminated). NULL is treated as "".
 * @return Number of bytes written to dst (excluding the NUL terminator).
 */
size_t wd_str_sanitize_for_lcd(char *dst, size_t dst_len, const char *src);
