/* tabs.c -- Tabbed container widget for Glyph toolkit */
#include "glyph.h"
#include <stdlib.h>
#include <string.h>

#define TAB_H        (FONT_H + 8)
#define TAB_PAD      12
#define TAB_BG       0x00E0E0E0
#define TAB_ACTIVE   0x00FFFFFF
#define TAB_FG       0x00202030
#define TAB_BORDER   0x00C0C0C0

static int
tab_label_x(glyph_tabs_t *tabs, int i)
{
    int x = 0;
    for (int j = 0; j < i; j++) {
        int len = 0;
        const char *p = tabs->labels[j];
        while (*p++) len++;
        x += len * FONT_W + 2 * TAB_PAD;
    }
    return x;
}

static int
tab_label_w(glyph_tabs_t *tabs, int i)
{
    int len = 0;
    const char *p = tabs->labels[i];
    while (*p++) len++;
    return len * FONT_W + 2 * TAB_PAD;
}

static void
tabs_draw(glyph_widget_t *self, surface_t *surf, int ox, int oy)
{
    glyph_tabs_t *tabs = (glyph_tabs_t *)self;

    /* Tab header background */
    draw_fill_rect(surf, ox, oy, self->w, TAB_H, TAB_BG);

    /* Draw tab labels */
    for (int i = 0; i < tabs->ntabs; i++) {
        int lx = ox + tab_label_x(tabs, i);
        int lw = tab_label_w(tabs, i);

        if (i == tabs->active) {
            draw_fill_rect(surf, lx, oy, lw, TAB_H, TAB_ACTIVE);
            draw_rect(surf, lx, oy, lw, TAB_H, TAB_BORDER);
            /* Erase bottom border to connect with content */
            for (int px = lx + 1; px < lx + lw - 1; px++)
                draw_px(surf, px, oy + TAB_H - 1, TAB_ACTIVE);
        }

        draw_text_t(surf, lx + TAB_PAD, oy + 4, tabs->labels[i], TAB_FG);
    }

    /* Bottom border */
    for (int px = 0; px < self->w; px++)
        draw_px(surf, ox + px, oy + TAB_H - 1, TAB_BORDER);

    /* Content area background */
    draw_fill_rect(surf, ox, oy + TAB_H, self->w, self->h - TAB_H, TAB_ACTIVE);
    draw_rect(surf, ox, oy + TAB_H - 1, self->w, self->h - TAB_H + 1, TAB_BORDER);

    /* Only the active panel's children are drawn by the tree walker.
     * We need to make inactive panels invisible. */
}

static void
tabs_on_mouse(glyph_widget_t *self, int btn, int local_x, int local_y)
{
    (void)btn;

    glyph_tabs_t *tabs = (glyph_tabs_t *)self;

    /* Click in tab header area */
    if (local_y < TAB_H) {
        for (int i = 0; i < tabs->ntabs; i++) {
            int lx = tab_label_x(tabs, i);
            int lw = tab_label_w(tabs, i);
            if (local_x >= lx && local_x < lx + lw) {
                if (tabs->active != i) {
                    /* Hide old panel */
                    if (tabs->active >= 0 && tabs->active < tabs->ntabs && tabs->panels[tabs->active])
                        tabs->panels[tabs->active]->visible = 0;
                    /* Show new panel */
                    tabs->active = i;
                    if (tabs->panels[i])
                        tabs->panels[i]->visible = 1;
                    glyph_widget_mark_dirty(self);
                    if (tabs->on_change)
                        tabs->on_change(self, i);
                }
                return;
            }
        }
    }
}

glyph_tabs_t *
glyph_tabs_create(void (*on_change)(glyph_widget_t *, int))
{
    glyph_tabs_t *tabs = calloc(1, sizeof(*tabs));
    if (!tabs)
        return NULL;

    glyph_widget_init(&tabs->base, GLYPH_WIDGET_TABS);
    tabs->base.draw_fn = tabs_draw;
    tabs->base.on_mouse = tabs_on_mouse;
    tabs->base.focusable = 1;
    tabs->on_change = on_change;
    tabs->active = 0;

    return tabs;
}

void
glyph_tabs_add(glyph_tabs_t *tabs, const char *label, glyph_widget_t *panel)
{
    if (!tabs || tabs->ntabs >= GLYPH_TABS_MAX)
        return;

    /* Copy label */
    int len = 0;
    if (label) {
        while (label[len] && len < 31) {
            tabs->labels[tabs->ntabs][len] = label[len];
            len++;
        }
    }
    tabs->labels[tabs->ntabs][len] = '\0';

    /* Store panel and add as child */
    tabs->panels[tabs->ntabs] = panel;
    if (panel) {
        panel->x = 0;
        panel->y = TAB_H;
        /* Make inactive panels invisible */
        panel->visible = (tabs->ntabs == tabs->active) ? 1 : 0;
        glyph_widget_add_child(&tabs->base, panel);
    }

    tabs->ntabs++;
    glyph_widget_mark_dirty(&tabs->base);
}
