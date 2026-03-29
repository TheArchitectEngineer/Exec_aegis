/* compositor.h -- Lumen window management and dirty-rect compositing */
#ifndef LUMEN_COMPOSITOR_H
#define LUMEN_COMPOSITOR_H

#include <glyph.h>
#include <stdint.h>

#define MAX_WINDOWS     16
#define TASKBAR_HEIGHT  36
#define MAX_DIRTY_RECTS 32

typedef struct {
    surface_t fb;
    surface_t back;
    glyph_window_t *windows[MAX_WINDOWS];
    int nwindows;
    glyph_window_t *focused;
    int cursor_x, cursor_y;
    int dragging;
    glyph_window_t *drag_win;
    int drag_dx, drag_dy;
    int prev_buttons;

    /* Dirty rect accumulator */
    glyph_rect_t dirty_rects[MAX_DIRTY_RECTS];
    int ndirty;
    int full_redraw; /* force full frame composite */

    /* Background rendered flag -- skip gradient after first frame */
    int bg_rendered;

    /* Desktop background callback — called after bg fill, before windows */
    void (*on_draw_desktop)(surface_t *back, int w, int h);
} compositor_t;

void comp_init(compositor_t *c, uint32_t *fb, uint32_t *backbuf, int w, int h, int pitch);
void comp_add_window(compositor_t *c, glyph_window_t *win);
void comp_remove_window(compositor_t *c, glyph_window_t *win);
void comp_raise_window(compositor_t *c, glyph_window_t *win);
glyph_window_t *comp_window_at(compositor_t *c, int x, int y);
void comp_add_dirty(compositor_t *c, glyph_rect_t r);
int comp_composite(compositor_t *c);
void comp_handle_mouse(compositor_t *c, uint8_t buttons, int16_t dx, int16_t dy);
void comp_handle_key(compositor_t *c, char key);
void taskbar_draw(surface_t *s, int screen_w, int screen_h);

#endif
