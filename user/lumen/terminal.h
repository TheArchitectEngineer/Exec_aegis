/* terminal.h -- PTY-based terminal emulator window for Lumen */
#ifndef LUMEN_TERMINAL_H
#define LUMEN_TERMINAL_H

#include <glyph.h>

glyph_window_t *terminal_create(int cols, int rows, int *master_fd_out);
glyph_window_t *terminal_create_dropdown(int screen_w, int screen_h,
                                         int *master_fd_out);
void terminal_write(glyph_window_t *win, const char *data, int len);

#endif
