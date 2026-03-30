/* taskbar.c -- Top bar and context menu for Lumen compositor
 *
 * This file replaces the old bottom taskbar with a macOS-style top bar.
 * The "Aegis" text on the left side is clickable and opens a context menu.
 */
#include "compositor.h"
#include <glyph.h>
#include <string.h>

#define TOPBAR_BG       0x001A1A2E
#define TOPBAR_TEXT     0x00C0C0D0
#define AEGIS_AREA_W    80  /* clickable area width for "Aegis" text */

void
topbar_draw(surface_t *s, int screen_w, const char *clock_str)
{
    /* Top bar background */
    draw_fill_rect(s, 0, 0, screen_w, TOPBAR_HEIGHT, TOPBAR_BG);

    /* Accent dot before "Aegis" text */
    draw_circle_filled(s, 14, TOPBAR_HEIGHT / 2, 4, C_ACCENT);

    /* "Aegis" text */
    draw_text(s, 24, 4, "Aegis", 0x00FFFFFF, TOPBAR_BG);

    /* Clock on right side */
    if (clock_str && clock_str[0]) {
        int len = (int)strlen(clock_str);
        int cx = screen_w - len * FONT_W - 12;
        draw_text(s, cx, 4, clock_str, TOPBAR_TEXT, TOPBAR_BG);
    } else {
        draw_text(s, screen_w - 5 * FONT_W - 12, 4, "00:00", TOPBAR_TEXT, TOPBAR_BG);
    }
}

int
topbar_hit_aegis(int mx, int my, int screen_w)
{
    (void)screen_w;
    return mx >= 0 && mx < AEGIS_AREA_W &&
           my >= 0 && my < TOPBAR_HEIGHT;
}
