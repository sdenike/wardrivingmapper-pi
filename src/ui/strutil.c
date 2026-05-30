/**
 * strutil.c — UTF-8 → printable-ASCII sanitizer for LCD rendering.
 * See strutil.h for the rationale and codepoint table.
 */

#include "strutil.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Append one ASCII byte to dst[*pos], stopping when we'd overflow the
// NUL-terminator slot. Returns true if the byte was written.
static inline bool put(char *dst, size_t dst_len, size_t *pos, char c)
{
    if (*pos + 1 >= dst_len) return false;
    dst[(*pos)++] = c;
    return true;
}

// Map a Unicode codepoint to its ASCII replacement and append to dst.
// Returns false on dst overflow (caller should stop walking).
static bool emit_codepoint(char *dst, size_t dst_len, size_t *pos, uint32_t cp)
{
    // ASCII printable range — pass through unchanged.
    if (cp >= 0x20 && cp <= 0x7E) {
        return put(dst, dst_len, pos, (char)cp);
    }

    switch (cp) {
        // Typographic single quotes
        case 0x2018: /* ' */
        case 0x2019: /* ' — iOS auto-substitutes this in hotspot SSIDs */
        case 0x201B: /* ‛ */
            return put(dst, dst_len, pos, '\'');
        case 0x201A: /* ‚ */
            return put(dst, dst_len, pos, ',');

        // Typographic double quotes
        case 0x201C: /* " */
        case 0x201D: /* " */
        case 0x201E: /* „ */
            return put(dst, dst_len, pos, '"');

        // Dashes
        case 0x2013: /* – en-dash */
        case 0x2014: /* — em-dash */
            return put(dst, dst_len, pos, '-');

        // Ellipsis → three literal dots. Output is 3 bytes, input was 3 bytes,
        // so this never grows the string beyond the source length.
        case 0x2026: /* … */
            if (!put(dst, dst_len, pos, '.')) return false;
            if (!put(dst, dst_len, pos, '.')) return false;
            return put(dst, dst_len, pos, '.');

        // No-break space → regular space
        case 0x00A0:
            return put(dst, dst_len, pos, ' ');

        default:
            // Zero-width formatting characters and emoji modifiers — drop
            // silently. These appear as part of compound emoji sequences
            // (e.g. ZWJ-joined family glyphs, skin-tone modifiers on faces,
            // emoji-style variation selector U+FE0F). Emitting a placeholder
            // for each would produce trailing '*' or '?' after the base
            // emoji, which looks like garbage.
            if (cp == 0x200B || cp == 0x200C || cp == 0x200D ||
                (cp >= 0xFE00 && cp <= 0xFE0F) ||
                cp == 0xFEFF ||
                (cp >= 0x1F3FB && cp <= 0x1F3FF)) {
                return true;
            }

            // Emoji + emoji-adjacent symbols → '*'. Covers Miscellaneous
            // Symbols + Dingbats (U+2600..U+27BF, includes ❤ ★ ☎ ✓ ✗ ⚠)
            // and the main supplementary-plane emoji blocks
            // (U+1F000..U+1FAFF, includes 😀 🚀 🤖 🩺 etc.). We don't bake
            // a monochrome emoji font into flash — '*' tells the user
            // "something visual was here" without the megabytes.
            if ((cp >= 0x2600 && cp <= 0x27BF) ||
                (cp >= 0x1F000 && cp <= 0x1FAFF)) {
                return put(dst, dst_len, pos, '*');
            }

            // Control chars, unmapped non-ASCII (Latin-1 accents, CJK,
            // malformed input) → '?'. Keeps the visible string legible
            // without pulling in extra glyphs.
            return put(dst, dst_len, pos, '?');
    }
}

size_t wd_str_sanitize_for_lcd(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return 0;
    size_t pos = 0;

    if (!src) {
        dst[0] = '\0';
        return 0;
    }

    const uint8_t *p = (const uint8_t *)src;

    while (*p) {
        uint8_t b0 = *p;
        uint32_t cp;
        int extra;  // number of continuation bytes expected after b0

        if (b0 < 0x80) {
            // 1-byte ASCII (or control char — emit_codepoint will map it).
            cp = b0;
            extra = 0;
        } else if ((b0 & 0xE0) == 0xC0) {
            // 110xxxxx — 2-byte sequence
            cp = b0 & 0x1F;
            extra = 1;
        } else if ((b0 & 0xF0) == 0xE0) {
            // 1110xxxx — 3-byte sequence (covers all the BMP typographic
            // chars we care about: U+2013..U+2026, U+00A0 is actually
            // 2-byte but handled by the C0 branch above)
            cp = b0 & 0x0F;
            extra = 2;
        } else if ((b0 & 0xF8) == 0xF0) {
            // 11110xxx — 4-byte sequence (outside the BMP; we never map
            // anything here, but decode so we advance correctly).
            cp = b0 & 0x07;
            extra = 3;
        } else {
            // Invalid lead byte (10xxxxxx orphaned continuation, or
            // 11111xxx). Emit '?' and skip one byte.
            if (!emit_codepoint(dst, dst_len, &pos, 0xFFFD)) break;
            p++;
            continue;
        }

        // Validate continuation bytes. Bail to '?' on malformed sequences
        // and resync on the offending byte.
        p++;
        bool ok = true;
        for (int i = 0; i < extra; i++) {
            if ((*p & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (*p & 0x3F);
            p++;
        }
        if (!ok) {
            if (!emit_codepoint(dst, dst_len, &pos, 0xFFFD)) break;
            // Don't advance p past the bad byte — let the outer loop
            // re-examine it as a possible new lead.
            continue;
        }

        if (!emit_codepoint(dst, dst_len, &pos, cp)) break;
    }

    if (pos < dst_len) dst[pos] = '\0';
    else               dst[dst_len - 1] = '\0';  // dst_len >= 1 from guard above
    return pos;
}
