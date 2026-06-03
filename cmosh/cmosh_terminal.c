#include "cmosh_terminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CMOSH_CELL_TEXT_MAX 32

enum cmosh_color_kind {
    CMOSH_COLOR_DEFAULT,
    CMOSH_COLOR_ANSI,
    CMOSH_COLOR_256,
    CMOSH_COLOR_TRUE
};

struct cmosh_color {
    enum cmosh_color_kind kind;
    unsigned int value;
    unsigned char r, g, b;
};

struct cmosh_attr {
    unsigned int bold : 1;
    unsigned int italic : 1;
    unsigned int underline : 1;
    unsigned int blink : 1;
    unsigned int inverse : 1;
    unsigned int invisible : 1;
    struct cmosh_color fg, bg;
};

struct cmosh_cell {
    char text[CMOSH_CELL_TEXT_MAX];
    unsigned char len;
    unsigned char width;
    unsigned int dirty : 1;
    struct cmosh_attr attr;
};

enum cmosh_parse_state {
    CMOSH_PARSE_NORMAL,
    CMOSH_PARSE_ESC,
    CMOSH_PARSE_CHARSET,
    CMOSH_PARSE_CSI,
    CMOSH_PARSE_OSC,
    CMOSH_PARSE_OSC_ESC,
};

struct cmosh_terminal {
    unsigned int cols, rows;
    struct cmosh_cell *cells;
    struct cmosh_cell *primary_cells;
    struct cmosh_cell *alternate_cells;
    unsigned int cursor_row, cursor_col;
    unsigned int saved_cursor_row, saved_cursor_col;
    unsigned int scroll_top, scroll_bottom;
    int cursor_visible;
    int wrap_mode;
    int origin_mode;
    int insert_mode;
    int app_cursor_keys;
    int app_keypad_keys;
    int pending_wrap;
    int reverse_video;
    int using_alternate;
    int g0_acs;
    unsigned char charset_select;
    struct cmosh_attr attr;
    enum cmosh_parse_state state;
    char csi[128];
    size_t csi_len;
    unsigned char tabs[256];
    unsigned int utf8_codepoint;
    unsigned int utf8_remaining;
    unsigned char utf8_buf[8];
    unsigned int utf8_len;
    char last_text[CMOSH_CELL_TEXT_MAX];
    unsigned char last_len;
    unsigned char last_width;
};

static struct cmosh_color cmosh_default_color(void)
{
    struct cmosh_color c;
    c.kind = CMOSH_COLOR_DEFAULT;
    c.value = 0;
    c.r = c.g = c.b = 0;
    return c;
}

static struct cmosh_attr cmosh_default_attr(void)
{
    struct cmosh_attr a;
    memset(&a, 0, sizeof(a));
    a.fg = cmosh_default_color();
    a.bg = cmosh_default_color();
    return a;
}

static int cmosh_attr_equal(const struct cmosh_attr *a,
                            const struct cmosh_attr *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

static struct cmosh_cell *cmosh_cell_at(struct cmosh_terminal *term,
                                        unsigned int row, unsigned int col)
{
    return &term->cells[(size_t)row * term->cols + col];
}

static void cmosh_move_cursor(struct cmosh_terminal *term, int row, int col);

static void cmosh_clear_cell(struct cmosh_cell *cell,
                             const struct cmosh_attr *attr)
{
    cell->text[0] = '\0';
    cell->len = 0;
    cell->width = 1;
    cell->dirty = 1;
    cell->attr = *attr;
}

static void cmosh_clear_region(struct cmosh_terminal *term, unsigned int row0,
                               unsigned int col0, unsigned int row1,
                               unsigned int col1)
{
    unsigned int r, c;

    if (!term->rows || !term->cols)
        return;
    if (row1 >= term->rows)
        row1 = term->rows - 1;
    if (col1 >= term->cols)
        col1 = term->cols - 1;
    for (r = row0; r <= row1; r++)
        for (c = (r == row0 ? col0 : 0);
             c <= (r == row1 ? col1 : term->cols - 1); c++)
            cmosh_clear_cell(cmosh_cell_at(term, r, c), &term->attr);
}

static void cmosh_reset_cell_buffer(struct cmosh_cell *cells,
                                    unsigned int cols, unsigned int rows)
{
    unsigned int r, c;
    struct cmosh_attr attr = cmosh_default_attr();

    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++)
            cmosh_clear_cell(&cells[(size_t)r * cols + c], &attr);
}

static void cmosh_reset_cells(struct cmosh_terminal *term)
{
    cmosh_reset_cell_buffer(term->cells, term->cols, term->rows);
}

static void cmosh_clear_tabstops(struct cmosh_terminal *term)
{
    memset(term->tabs, 0, sizeof(term->tabs));
}

static int cmosh_tab_is_set(struct cmosh_terminal *term, unsigned int col)
{
    if (col >= sizeof(term->tabs) * 8)
        return col % 8 == 0;
    return (term->tabs[col / 8] & (1U << (col % 8))) != 0;
}

static void cmosh_set_tab(struct cmosh_terminal *term, unsigned int col)
{
    if (col < sizeof(term->tabs) * 8)
        term->tabs[col / 8] |= (unsigned char)(1U << (col % 8));
}

static void cmosh_clear_tab(struct cmosh_terminal *term, unsigned int col)
{
    if (col < sizeof(term->tabs) * 8)
        term->tabs[col / 8] &= (unsigned char)~(1U << (col % 8));
}

static void cmosh_reset_tabstops(struct cmosh_terminal *term)
{
    unsigned int col;

    cmosh_clear_tabstops(term);
    for (col = 8; col < term->cols; col += 8)
        cmosh_set_tab(term, col);
}

static void cmosh_horizontal_tab(struct cmosh_terminal *term)
{
    unsigned int col;

    term->pending_wrap = 0;
    for (col = term->cursor_col + 1; col < term->cols; col++) {
        if (cmosh_tab_is_set(term, col)) {
            term->cursor_col = col;
            return;
        }
    }
    if (term->cols)
        term->cursor_col = term->cols - 1;
}

static void cmosh_back_tab(struct cmosh_terminal *term)
{
    unsigned int col;

    term->pending_wrap = 0;
    if (!term->cursor_col)
        return;
    for (col = term->cursor_col; col-- > 0;) {
        if (cmosh_tab_is_set(term, col)) {
            term->cursor_col = col;
            return;
        }
    }
    term->cursor_col = 0;
}

struct cmosh_terminal *cmosh_terminal_new(unsigned int cols,
                                          unsigned int rows)
{
    struct cmosh_terminal *term;

    if (!cols)
        cols = 80;
    if (!rows)
        rows = 24;
    term = (struct cmosh_terminal *)calloc(1, sizeof(*term));
    if (!term)
        return NULL;
    term->cols = cols;
    term->rows = rows;
    term->primary_cells = (struct cmosh_cell *)calloc((size_t)cols * rows,
                                                      sizeof(*term->cells));
    term->alternate_cells = (struct cmosh_cell *)calloc((size_t)cols * rows,
                                                        sizeof(*term->cells));
    if (!term->primary_cells || !term->alternate_cells) {
        free(term->primary_cells);
        free(term->alternate_cells);
        free(term);
        return NULL;
    }
    term->cells = term->primary_cells;
    term->cursor_visible = 1;
    term->wrap_mode = 1;
    term->scroll_bottom = rows - 1;
    term->attr = cmosh_default_attr();
    cmosh_reset_tabstops(term);
    cmosh_reset_cell_buffer(term->primary_cells, cols, rows);
    cmosh_reset_cell_buffer(term->alternate_cells, cols, rows);
    return term;
}

void cmosh_terminal_free(struct cmosh_terminal *term)
{
    if (!term)
        return;
    free(term->primary_cells);
    free(term->alternate_cells);
    free(term);
}

static struct cmosh_cell *cmosh_resize_cell_buffer(struct cmosh_cell *oldcells,
                                                   unsigned int oldcols,
                                                   unsigned int oldrows,
                                                   unsigned int cols,
                                                   unsigned int rows)
{
    struct cmosh_cell *newcells;
    unsigned int copy_cols, copy_rows, r, c;
    struct cmosh_attr attr;

    newcells = (struct cmosh_cell *)calloc((size_t)cols * rows,
                                           sizeof(*newcells));
    if (!newcells)
        return NULL;
    attr = cmosh_default_attr();
    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++)
            cmosh_clear_cell(&newcells[(size_t)r * cols + c], &attr);
    copy_cols = cols < oldcols ? cols : oldcols;
    copy_rows = rows < oldrows ? rows : oldrows;
    for (r = 0; r < copy_rows; r++)
        memcpy(&newcells[(size_t)r * cols], &oldcells[(size_t)r * oldcols],
               (size_t)copy_cols * sizeof(*newcells));
    return newcells;
}

int cmosh_terminal_resize(struct cmosh_terminal *term, unsigned int cols,
                          unsigned int rows)
{
    struct cmosh_cell *new_primary, *new_alternate;

    if (!term)
        return -1;
    if (!cols)
        cols = 80;
    if (!rows)
        rows = 24;
    if (cols == term->cols && rows == term->rows)
        return 0;

    new_primary = cmosh_resize_cell_buffer(term->primary_cells, term->cols,
                                           term->rows, cols, rows);
    if (!new_primary)
        return -1;
    new_alternate = cmosh_resize_cell_buffer(term->alternate_cells,
                                             term->cols, term->rows, cols,
                                             rows);
    if (!new_alternate) {
        free(new_primary);
        return -1;
    }

    free(term->primary_cells);
    free(term->alternate_cells);
    term->primary_cells = new_primary;
    term->alternate_cells = new_alternate;
    term->cells = term->using_alternate ? term->alternate_cells :
                                      term->primary_cells;
    term->cols = cols;
    term->rows = rows;
    if (term->cursor_row >= rows)
        term->cursor_row = rows - 1;
    if (term->cursor_col >= cols)
        term->cursor_col = cols - 1;
    term->scroll_top = 0;
    term->scroll_bottom = rows - 1;
    term->pending_wrap = 0;
    cmosh_reset_tabstops(term);
    return 0;
}

static void cmosh_use_alternate_screen(struct cmosh_terminal *term, int use,
                                       int clear, int save_cursor)
{
    if (save_cursor) {
        term->saved_cursor_row = term->cursor_row;
        term->saved_cursor_col = term->cursor_col;
    }
    if (use) {
        term->using_alternate = 1;
        term->cells = term->alternate_cells;
        if (clear)
            cmosh_reset_cells(term);
        cmosh_move_cursor(term, 0, 0);
    } else {
        term->using_alternate = 0;
        term->cells = term->primary_cells;
        if (save_cursor)
            cmosh_move_cursor(term, term->saved_cursor_row,
                              term->saved_cursor_col);
    }
}

static void cmosh_scroll_up(struct cmosh_terminal *term)
{
    unsigned int r, c;

    if (term->scroll_top >= term->scroll_bottom)
        return;
    for (r = term->scroll_top; r < term->scroll_bottom; r++)
        memcpy(cmosh_cell_at(term, r, 0), cmosh_cell_at(term, r + 1, 0),
               (size_t)term->cols * sizeof(*term->cells));
    for (c = 0; c < term->cols; c++)
        cmosh_clear_cell(cmosh_cell_at(term, term->scroll_bottom, c),
                         &term->attr);
}

static void cmosh_scroll_down(struct cmosh_terminal *term)
{
    unsigned int r, c;

    if (term->scroll_top >= term->scroll_bottom)
        return;
    for (r = term->scroll_bottom; r > term->scroll_top; r--)
        memcpy(cmosh_cell_at(term, r, 0), cmosh_cell_at(term, r - 1, 0),
               (size_t)term->cols * sizeof(*term->cells));
    for (c = 0; c < term->cols; c++)
        cmosh_clear_cell(cmosh_cell_at(term, term->scroll_top, c),
                         &term->attr);
}

static void cmosh_insert_blank_chars(struct cmosh_terminal *term,
                                     unsigned int count)
{
    unsigned int col;

    if (!count)
        count = 1;
    if (count > term->cols - term->cursor_col)
        count = term->cols - term->cursor_col;
    for (col = term->cols; col-- > term->cursor_col + count;)
        *cmosh_cell_at(term, term->cursor_row, col) =
            *cmosh_cell_at(term, term->cursor_row, col - count);
    for (col = term->cursor_col; col < term->cursor_col + count; col++)
        cmosh_clear_cell(cmosh_cell_at(term, term->cursor_row, col),
                         &term->attr);
}

static void cmosh_delete_chars(struct cmosh_terminal *term,
                               unsigned int count)
{
    unsigned int col;

    if (!count)
        count = 1;
    if (count > term->cols - term->cursor_col)
        count = term->cols - term->cursor_col;
    for (col = term->cursor_col; col + count < term->cols; col++)
        *cmosh_cell_at(term, term->cursor_row, col) =
            *cmosh_cell_at(term, term->cursor_row, col + count);
    for (; col < term->cols; col++)
        cmosh_clear_cell(cmosh_cell_at(term, term->cursor_row, col),
                         &term->attr);
}

static void cmosh_insert_lines(struct cmosh_terminal *term,
                               unsigned int count)
{
    unsigned int r, c, bottom;

    if (term->cursor_row < term->scroll_top ||
        term->cursor_row > term->scroll_bottom)
        return;
    if (!count)
        count = 1;
    bottom = term->scroll_bottom;
    if (count > bottom - term->cursor_row + 1)
        count = bottom - term->cursor_row + 1;
    for (r = bottom + 1; r-- > term->cursor_row + count;) {
        memcpy(cmosh_cell_at(term, r, 0), cmosh_cell_at(term, r - count, 0),
               (size_t)term->cols * sizeof(*term->cells));
    }
    for (r = term->cursor_row; r < term->cursor_row + count; r++)
        for (c = 0; c < term->cols; c++)
            cmosh_clear_cell(cmosh_cell_at(term, r, c), &term->attr);
}

static void cmosh_delete_lines(struct cmosh_terminal *term,
                               unsigned int count)
{
    unsigned int r, c, bottom;

    if (term->cursor_row < term->scroll_top ||
        term->cursor_row > term->scroll_bottom)
        return;
    if (!count)
        count = 1;
    bottom = term->scroll_bottom;
    if (count > bottom - term->cursor_row + 1)
        count = bottom - term->cursor_row + 1;
    for (r = term->cursor_row; r + count <= bottom; r++)
        memcpy(cmosh_cell_at(term, r, 0), cmosh_cell_at(term, r + count, 0),
               (size_t)term->cols * sizeof(*term->cells));
    for (; r <= bottom; r++)
        for (c = 0; c < term->cols; c++)
            cmosh_clear_cell(cmosh_cell_at(term, r, c), &term->attr);
}

static void cmosh_lf(struct cmosh_terminal *term)
{
    term->pending_wrap = 0;
    if (term->cursor_row == term->scroll_bottom)
        cmosh_scroll_up(term);
    else if (term->cursor_row + 1 < term->rows)
        term->cursor_row++;
}

static void cmosh_put_codepoint(struct cmosh_terminal *term,
                                const unsigned char *utf8, unsigned int len,
                                int width)
{
    struct cmosh_cell *cell;

    if (!term->cols || !term->rows)
        return;
    if (width < 0)
        width = 1;
    if (width == 0) {
        unsigned int col = term->cursor_col ? term->cursor_col - 1 : 0;
        while (col > 0 &&
               cmosh_cell_at(term, term->cursor_row, col)->width == 0)
            col--;
        cell = cmosh_cell_at(term, term->cursor_row, col);
        if (cell->len + len < CMOSH_CELL_TEXT_MAX) {
            memcpy(cell->text + cell->len, utf8, len);
            cell->len = (unsigned char)(cell->len + len);
            cell->text[cell->len] = '\0';
            cell->dirty = 1;
        }
        return;
    }
    if (term->pending_wrap) {
        term->cursor_col = 0;
        cmosh_lf(term);
    }
    if (width > 1 && term->cursor_col + 1 >= term->cols) {
        if (term->wrap_mode) {
            term->cursor_col = 0;
            cmosh_lf(term);
        } else {
            width = 1;
        }
    }
    if (term->insert_mode)
        cmosh_insert_blank_chars(term, (unsigned int)width);

    cell = cmosh_cell_at(term, term->cursor_row, term->cursor_col);
    cmosh_clear_cell(cell, &term->attr);
    if (len >= CMOSH_CELL_TEXT_MAX)
        len = CMOSH_CELL_TEXT_MAX - 1;
    memcpy(cell->text, utf8, len);
    cell->text[len] = '\0';
    cell->len = (unsigned char)len;
    cell->width = (unsigned char)width;
    cell->attr = term->attr;
    cell->dirty = 1;
    if (utf8 != (const unsigned char *)term->last_text)
        memcpy(term->last_text, utf8, len);
    term->last_text[len] = '\0';
    term->last_len = (unsigned char)len;
    term->last_width = (unsigned char)width;
    if (width > 1 && term->cursor_col + 1 < term->cols) {
        cell = cmosh_cell_at(term, term->cursor_row, term->cursor_col + 1);
        cmosh_clear_cell(cell, &term->attr);
        cell->width = 0;
    }

    if (term->cursor_col + (unsigned int)width >= term->cols) {
        term->cursor_col = term->cols - 1;
        term->pending_wrap = term->wrap_mode;
    } else {
        term->cursor_col += (unsigned int)width;
        term->pending_wrap = 0;
    }
}

static int cmosh_is_combining(unsigned int cp)
{
    return (cp >= 0x0300 && cp <= 0x036f) ||
           (cp >= 0x1ab0 && cp <= 0x1aff) ||
           (cp >= 0x1dc0 && cp <= 0x1dff) ||
           (cp >= 0x20d0 && cp <= 0x20ff) ||
           (cp >= 0xfe20 && cp <= 0xfe2f);
}

static int cmosh_codepoint_width(unsigned int cp)
{
    if (cp == 0 || cmosh_is_combining(cp))
        return 0;
    if (cp < 32 || (cp >= 0x7f && cp < 0xa0))
        return -1;
    if (cp >= 0x1100 &&
        (cp <= 0x115f || cp == 0x2329 || cp == 0x232a ||
         (cp >= 0x2e80 && cp <= 0xa4cf && cp != 0x303f) ||
         (cp >= 0xac00 && cp <= 0xd7a3) ||
         (cp >= 0xf900 && cp <= 0xfaff) ||
         (cp >= 0xfe10 && cp <= 0xfe19) ||
         (cp >= 0xfe30 && cp <= 0xfe6f) ||
         (cp >= 0xff00 && cp <= 0xff60) ||
         (cp >= 0xffe0 && cp <= 0xffe6) ||
         (cp >= 0x1f300 && cp <= 0x1faff) ||
         (cp >= 0x20000 && cp <= 0x3fffd)))
        return 2;
    return 1;
}

static void cmosh_csi_reset(struct cmosh_terminal *term)
{
    term->csi_len = 0;
    term->csi[0] = '\0';
}

static void cmosh_parse_params(const char *s, int *params,
                               unsigned int maxparams,
                               unsigned int *nparams, int *priv)
{
    unsigned int n = 0;
    int value = -1;

    *priv = 0;
    if (*s == '?') {
        *priv = 1;
        s++;
    }
    while (*s && n < maxparams) {
        if (*s >= '0' && *s <= '9') {
            if (value < 0)
                value = 0;
            value = value * 10 + (*s - '0');
        } else if (*s == ';') {
            params[n++] = value;
            value = -1;
        }
        s++;
    }
    if (n < maxparams)
        params[n++] = value;
    *nparams = n;
}

static int cmosh_param(const int *params, unsigned int nparams,
                       unsigned int i, int def)
{
    if (i >= nparams || params[i] < 0)
        return def;
    return params[i];
}

static void cmosh_move_cursor(struct cmosh_terminal *term, int row, int col)
{
    if (row < 0)
        row = 0;
    if (col < 0)
        col = 0;
    if ((unsigned int)row >= term->rows)
        row = (int)term->rows - 1;
    if ((unsigned int)col >= term->cols)
        col = (int)term->cols - 1;
    term->cursor_row = (unsigned int)row;
    term->cursor_col = (unsigned int)col;
    term->pending_wrap = 0;
}

static void cmosh_move_cursor_cup(struct cmosh_terminal *term,
                                  int row, int col)
{
    if (term->origin_mode) {
        row += (int)term->scroll_top;
        if (row > (int)term->scroll_bottom)
            row = (int)term->scroll_bottom;
    }
    cmosh_move_cursor(term, row, col);
}

static void cmosh_apply_sgr(struct cmosh_terminal *term, const int *params,
                            unsigned int nparams)
{
    unsigned int i;

    if (nparams == 0)
        term->attr = cmosh_default_attr();
    for (i = 0; i < nparams; i++) {
        int p = params[i] < 0 ? 0 : params[i];
        switch (p) {
          case 0: term->attr = cmosh_default_attr(); break;
          case 1: term->attr.bold = 1; break;
          case 3: term->attr.italic = 1; break;
          case 4: term->attr.underline = 1; break;
          case 5: term->attr.blink = 1; break;
          case 7: term->attr.inverse = 1; break;
          case 8: term->attr.invisible = 1; break;
          case 22: term->attr.bold = 0; break;
          case 23: term->attr.italic = 0; break;
          case 24: term->attr.underline = 0; break;
          case 25: term->attr.blink = 0; break;
          case 27: term->attr.inverse = 0; break;
          case 28: term->attr.invisible = 0; break;
          case 39: term->attr.fg = cmosh_default_color(); break;
          case 49: term->attr.bg = cmosh_default_color(); break;
          default:
            if (p >= 30 && p <= 37) {
                term->attr.fg.kind = CMOSH_COLOR_ANSI;
                term->attr.fg.value = (unsigned int)(p - 30);
            } else if (p >= 40 && p <= 47) {
                term->attr.bg.kind = CMOSH_COLOR_ANSI;
                term->attr.bg.value = (unsigned int)(p - 40);
            } else if (p >= 90 && p <= 97) {
                term->attr.fg.kind = CMOSH_COLOR_ANSI;
                term->attr.fg.value = (unsigned int)(p - 90 + 8);
            } else if (p >= 100 && p <= 107) {
                term->attr.bg.kind = CMOSH_COLOR_ANSI;
                term->attr.bg.value = (unsigned int)(p - 100 + 8);
            } else if ((p == 38 || p == 48) && i + 2 < nparams) {
                struct cmosh_color *color =
                    p == 38 ? &term->attr.fg : &term->attr.bg;
                if (params[i + 1] == 5 && params[i + 2] >= 0) {
                    color->kind = CMOSH_COLOR_256;
                    color->value = (unsigned int)params[i + 2];
                    i += 2;
                } else if (params[i + 1] == 2 && i + 4 < nparams &&
                           params[i + 2] >= 0 && params[i + 3] >= 0 &&
                           params[i + 4] >= 0) {
                    color->kind = CMOSH_COLOR_TRUE;
                    color->r = (unsigned char)params[i + 2];
                    color->g = (unsigned char)params[i + 3];
                    color->b = (unsigned char)params[i + 4];
                    i += 4;
                }
            }
            break;
        }
    }
}

static void cmosh_apply_csi(struct cmosh_terminal *term, char final)
{
    int params[32], priv;
    unsigned int nparams = 0;
    int n;

    cmosh_parse_params(term->csi, params, 32, &nparams, &priv);
    switch (final) {
      case 'H':
      case 'f':
        cmosh_move_cursor_cup(term, cmosh_param(params, nparams, 0, 1) - 1,
                              cmosh_param(params, nparams, 1, 1) - 1);
        break;
      case 'A':
        n = cmosh_param(params, nparams, 0, 1);
        cmosh_move_cursor(term, (int)term->cursor_row - n,
                          (int)term->cursor_col);
        break;
      case 'B':
        n = cmosh_param(params, nparams, 0, 1);
        cmosh_move_cursor(term, (int)term->cursor_row + n,
                          (int)term->cursor_col);
        break;
      case 'C':
        n = cmosh_param(params, nparams, 0, 1);
        cmosh_move_cursor(term, (int)term->cursor_row,
                          (int)term->cursor_col + n);
        break;
      case 'D':
        n = cmosh_param(params, nparams, 0, 1);
        cmosh_move_cursor(term, (int)term->cursor_row,
                          (int)term->cursor_col - n);
        break;
      case 'E':
        n = cmosh_param(params, nparams, 0, 1);
        cmosh_move_cursor(term, (int)term->cursor_row + n, 0);
        break;
      case 'F':
        n = cmosh_param(params, nparams, 0, 1);
        cmosh_move_cursor(term, (int)term->cursor_row - n, 0);
        break;
      case 'G':
      case '`':
        cmosh_move_cursor(term, (int)term->cursor_row,
                          cmosh_param(params, nparams, 0, 1) - 1);
        break;
      case 'I':
        n = cmosh_param(params, nparams, 0, 1);
        while (n-- > 0)
            cmosh_horizontal_tab(term);
        break;
      case 'Z':
        n = cmosh_param(params, nparams, 0, 1);
        while (n-- > 0)
            cmosh_back_tab(term);
        break;
      case 'd':
        n = cmosh_param(params, nparams, 0, 1) - 1;
        if (term->origin_mode) {
            n += (int)term->scroll_top;
            if (n > (int)term->scroll_bottom)
                n = (int)term->scroll_bottom;
        }
        cmosh_move_cursor(term, n, (int)term->cursor_col);
        break;
      case 'b':
        n = cmosh_param(params, nparams, 0, 1);
        while (n-- > 0 && term->last_len)
            cmosh_put_codepoint(term, (const unsigned char *)term->last_text,
                                term->last_len, term->last_width);
        break;
      case 'J':
        n = cmosh_param(params, nparams, 0, 0);
        if (n == 2)
            cmosh_clear_region(term, 0, 0, term->rows - 1, term->cols - 1);
        else if (n == 1)
            cmosh_clear_region(term, 0, 0, term->cursor_row,
                               term->cursor_col);
        else
            cmosh_clear_region(term, term->cursor_row, term->cursor_col,
                               term->rows - 1, term->cols - 1);
        break;
      case 'K':
        n = cmosh_param(params, nparams, 0, 0);
        if (n == 2)
            cmosh_clear_region(term, term->cursor_row, 0, term->cursor_row,
                               term->cols - 1);
        else if (n == 1)
            cmosh_clear_region(term, term->cursor_row, 0, term->cursor_row,
                               term->cursor_col);
        else
            cmosh_clear_region(term, term->cursor_row, term->cursor_col,
                               term->cursor_row, term->cols - 1);
        break;
      case 'X':
        n = cmosh_param(params, nparams, 0, 1);
        if ((unsigned int)n > term->cols - term->cursor_col)
            n = (int)(term->cols - term->cursor_col);
        if (n > 0)
            cmosh_clear_region(term, term->cursor_row, term->cursor_col,
                               term->cursor_row,
                               term->cursor_col + (unsigned int)n - 1);
        break;
      case '@':
        cmosh_insert_blank_chars(term,
                                 (unsigned int)cmosh_param(params, nparams,
                                                           0, 1));
        break;
      case 'P':
        cmosh_delete_chars(term,
                           (unsigned int)cmosh_param(params, nparams, 0, 1));
        break;
      case 'L':
        cmosh_insert_lines(term,
                           (unsigned int)cmosh_param(params, nparams, 0, 1));
        break;
      case 'M':
        cmosh_delete_lines(term,
                           (unsigned int)cmosh_param(params, nparams, 0, 1));
        break;
      case 'S':
        n = cmosh_param(params, nparams, 0, 1);
        while (n-- > 0)
            cmosh_scroll_up(term);
        break;
      case 'T':
        n = cmosh_param(params, nparams, 0, 1);
        while (n-- > 0)
            cmosh_scroll_down(term);
        break;
      case 'g':
        n = cmosh_param(params, nparams, 0, 0);
        if (n == 0)
            cmosh_clear_tab(term, term->cursor_col);
        else if (n == 3)
            cmosh_clear_tabstops(term);
        break;
      case 'm':
        cmosh_apply_sgr(term, params, nparams);
        break;
      case 'r':
        if (!priv) {
            int top = cmosh_param(params, nparams, 0, 1);
            int bottom = cmosh_param(params, nparams, 1, (int)term->rows);
            if (top < 1)
                top = 1;
            if (bottom > (int)term->rows)
                bottom = (int)term->rows;
            if (top < bottom) {
                term->scroll_top = (unsigned int)(top - 1);
                term->scroll_bottom = (unsigned int)(bottom - 1);
                cmosh_move_cursor(term, 0, 0);
            }
        }
        break;
      case 's':
        term->saved_cursor_row = term->cursor_row;
        term->saved_cursor_col = term->cursor_col;
        break;
      case 'u':
        cmosh_move_cursor(term, term->saved_cursor_row,
                          term->saved_cursor_col);
        break;
      case 'h':
      case 'l':
        {
            int set = final == 'h';
            unsigned int i;
            for (i = 0; i < nparams; i++) {
                if (!priv && params[i] == 4)
                    term->insert_mode = set;
                else if (priv && params[i] == 1)
                    term->app_cursor_keys = set;
                else if (priv && params[i] == 25)
                    term->cursor_visible = set;
                else if (priv && params[i] == 5)
                    term->reverse_video = set;
                else if (priv && params[i] == 6) {
                    term->origin_mode = set;
                    cmosh_move_cursor(term, set ? (int)term->scroll_top : 0,
                                      0);
                } else if (priv && params[i] == 7)
                    term->wrap_mode = set;
                else if (priv && params[i] == 1047)
                    cmosh_use_alternate_screen(term, set, set, 0);
                else if (priv && params[i] == 1048) {
                    if (set) {
                        term->saved_cursor_row = term->cursor_row;
                        term->saved_cursor_col = term->cursor_col;
                    } else {
                        cmosh_move_cursor(term, term->saved_cursor_row,
                                          term->saved_cursor_col);
                    }
                } else if (priv && params[i] == 1049)
                    cmosh_use_alternate_screen(term, set, set, 1);
            }
        }
        break;
    }
}

static void cmosh_terminal_byte(struct cmosh_terminal *term, unsigned char ch);

static void cmosh_process_codepoint(struct cmosh_terminal *term,
                                    unsigned int cp,
                                    const unsigned char *utf8,
                                    unsigned int len)
{
    int width = cmosh_codepoint_width(cp);
    cmosh_put_codepoint(term, utf8, len, width);
}

static int cmosh_acs_utf8(unsigned char ch, const unsigned char **utf8,
                          unsigned int *len)
{
    switch (ch) {
      case '`': *utf8 = (const unsigned char *)"\xe2\x97\x86"; break;
      case 'a': *utf8 = (const unsigned char *)"\xe2\x96\x92"; break;
      case 'f': *utf8 = (const unsigned char *)"\xc2\xb0"; *len = 2; return 1;
      case 'g': *utf8 = (const unsigned char *)"\xc2\xb1"; *len = 2; return 1;
      case 'j': *utf8 = (const unsigned char *)"\xe2\x94\x98"; break;
      case 'k': *utf8 = (const unsigned char *)"\xe2\x94\x90"; break;
      case 'l': *utf8 = (const unsigned char *)"\xe2\x94\x8c"; break;
      case 'm': *utf8 = (const unsigned char *)"\xe2\x94\x94"; break;
      case 'n': *utf8 = (const unsigned char *)"\xe2\x94\xbc"; break;
      case 'q': *utf8 = (const unsigned char *)"\xe2\x94\x80"; break;
      case 't': *utf8 = (const unsigned char *)"\xe2\x94\x9c"; break;
      case 'u': *utf8 = (const unsigned char *)"\xe2\x94\xa4"; break;
      case 'v': *utf8 = (const unsigned char *)"\xe2\x94\xb4"; break;
      case 'w': *utf8 = (const unsigned char *)"\xe2\x94\xac"; break;
      case 'x': *utf8 = (const unsigned char *)"\xe2\x94\x82"; break;
      case 'y': *utf8 = (const unsigned char *)"\xe2\x89\xa4"; break;
      case 'z': *utf8 = (const unsigned char *)"\xe2\x89\xa5"; break;
      case '{': *utf8 = (const unsigned char *)"\xcf\x80"; *len = 2; return 1;
      case '|': *utf8 = (const unsigned char *)"\xe2\x89\xa0"; break;
      case '}': *utf8 = (const unsigned char *)"\xc2\xa3"; *len = 2; return 1;
      case '~': *utf8 = (const unsigned char *)"\xc2\xb7"; *len = 2; return 1;
      default:
        return 0;
    }
    *len = 3;
    return 1;
}

static void cmosh_terminal_normal_byte(struct cmosh_terminal *term,
                                       unsigned char ch)
{
    if (term->utf8_remaining) {
        if ((ch & 0xc0) != 0x80) {
            static const unsigned char repl[] = "\xef\xbf\xbd";
            cmosh_process_codepoint(term, 0xfffd, repl, 3);
            term->utf8_remaining = term->utf8_len = 0;
            cmosh_terminal_byte(term, ch);
            return;
        }
        term->utf8_codepoint = (term->utf8_codepoint << 6) | (ch & 0x3f);
        term->utf8_buf[term->utf8_len++] = ch;
        if (--term->utf8_remaining == 0)
            cmosh_process_codepoint(term, term->utf8_codepoint,
                                    term->utf8_buf, term->utf8_len);
        return;
    }

    if (ch < 0x80) {
        const unsigned char *acs;
        unsigned int acs_len;
        unsigned char b = ch;
        if (term->g0_acs && cmosh_acs_utf8(ch, &acs, &acs_len))
            cmosh_put_codepoint(term, acs, acs_len, 1);
        else
            cmosh_process_codepoint(term, ch, &b, 1);
    } else if ((ch & 0xe0) == 0xc0) {
        term->utf8_codepoint = ch & 0x1f;
        term->utf8_remaining = 1;
        term->utf8_len = 1;
        term->utf8_buf[0] = ch;
    } else if ((ch & 0xf0) == 0xe0) {
        term->utf8_codepoint = ch & 0x0f;
        term->utf8_remaining = 2;
        term->utf8_len = 1;
        term->utf8_buf[0] = ch;
    } else if ((ch & 0xf8) == 0xf0) {
        term->utf8_codepoint = ch & 0x07;
        term->utf8_remaining = 3;
        term->utf8_len = 1;
        term->utf8_buf[0] = ch;
    } else {
        static const unsigned char repl[] = "\xef\xbf\xbd";
        cmosh_process_codepoint(term, 0xfffd, repl, 3);
    }
}

static void cmosh_terminal_byte(struct cmosh_terminal *term, unsigned char ch)
{
    switch (term->state) {
      case CMOSH_PARSE_NORMAL:
        switch (ch) {
          case '\a': break;
          case '\b':
            if (term->cursor_col)
                term->cursor_col--;
            term->pending_wrap = 0;
            break;
          case '\t':
            cmosh_horizontal_tab(term);
            break;
          case 0x0e:
          case 0x0f:
            break;
          case '\r':
            term->cursor_col = 0;
            term->pending_wrap = 0;
            break;
          case '\n':
            cmosh_lf(term);
            break;
          case 0x1b:
            term->state = CMOSH_PARSE_ESC;
            term->utf8_remaining = term->utf8_len = 0;
            break;
          default:
            cmosh_terminal_normal_byte(term, ch);
            break;
        }
        break;
      case CMOSH_PARSE_ESC:
        if (ch == '[') {
            term->state = CMOSH_PARSE_CSI;
            cmosh_csi_reset(term);
        } else if (ch == ']') {
            term->state = CMOSH_PARSE_OSC;
        } else if (ch == '7') {
            term->saved_cursor_row = term->cursor_row;
            term->saved_cursor_col = term->cursor_col;
            term->state = CMOSH_PARSE_NORMAL;
        } else if (ch == '8') {
            cmosh_move_cursor(term, term->saved_cursor_row,
                              term->saved_cursor_col);
            term->state = CMOSH_PARSE_NORMAL;
        } else if (ch == 'H') {
            cmosh_set_tab(term, term->cursor_col);
            term->state = CMOSH_PARSE_NORMAL;
        } else if (ch == 'D') {
            cmosh_lf(term);
            term->state = CMOSH_PARSE_NORMAL;
        } else if (ch == 'E') {
            term->cursor_col = 0;
            cmosh_lf(term);
            term->state = CMOSH_PARSE_NORMAL;
        } else if (ch == 'M') {
            if (term->cursor_row == term->scroll_top)
                cmosh_scroll_down(term);
            else if (term->cursor_row)
                term->cursor_row--;
            term->pending_wrap = 0;
            term->state = CMOSH_PARSE_NORMAL;
        } else if (ch == '(' || ch == ')' || ch == '*' || ch == '+' ||
                   ch == '-' || ch == '.' || ch == '/') {
            term->charset_select = ch;
            term->state = CMOSH_PARSE_CHARSET;
        } else if (ch == '=') {
            term->app_keypad_keys = 1;
            term->state = CMOSH_PARSE_NORMAL;
        } else if (ch == '>') {
            term->app_keypad_keys = 0;
            term->state = CMOSH_PARSE_NORMAL;
        } else {
            term->state = CMOSH_PARSE_NORMAL;
        }
        break;
      case CMOSH_PARSE_CHARSET:
        if (term->charset_select == '(')
            term->g0_acs = (ch == '0');
        term->charset_select = 0;
        term->state = CMOSH_PARSE_NORMAL;
        break;
      case CMOSH_PARSE_CSI:
        if (ch >= 0x40 && ch <= 0x7e) {
            cmosh_apply_csi(term, (char)ch);
            term->state = CMOSH_PARSE_NORMAL;
        } else if (term->csi_len + 1 < sizeof(term->csi)) {
            term->csi[term->csi_len++] = (char)ch;
            term->csi[term->csi_len] = '\0';
        } else {
            term->state = CMOSH_PARSE_NORMAL;
        }
        break;
      case CMOSH_PARSE_OSC:
        if (ch == '\a') {
            term->state = CMOSH_PARSE_NORMAL;
        } else if (ch == 0x1b) {
            term->state = CMOSH_PARSE_OSC_ESC;
        }
        break;
      case CMOSH_PARSE_OSC_ESC:
        term->state = ch == '\\' ? CMOSH_PARSE_NORMAL : CMOSH_PARSE_OSC;
        break;
    }
}

int cmosh_terminal_apply_bytes(struct cmosh_terminal *term,
                               const unsigned char *data, size_t len)
{
    size_t i;

    if (!term || (!data && len))
        return -1;
    for (i = 0; i < len; i++)
        cmosh_terminal_byte(term, data[i]);
    return 0;
}

static int cmosh_out(cmosh_terminal_output_fn output, void *ctx,
                     const char *s)
{
    return output(ctx, (const unsigned char *)s, strlen(s));
}

static int cmosh_outf(cmosh_terminal_output_fn output, void *ctx,
                      const char *fmt, unsigned int a, unsigned int b,
                      unsigned int c)
{
    char buf[64];
    int len = snprintf(buf, sizeof(buf), fmt, a, b, c);
    if (len < 0 || (size_t)len >= sizeof(buf))
        return -1;
    return output(ctx, (const unsigned char *)buf, (size_t)len);
}

static int cmosh_emit_color(cmosh_terminal_output_fn output, void *ctx,
                            const struct cmosh_color *color, int fg)
{
    unsigned int base = fg ? 30 : 40;

    switch (color->kind) {
      case CMOSH_COLOR_DEFAULT:
        return cmosh_outf(output, ctx, "\033[%um", fg ? 39U : 49U, 0, 0);
      case CMOSH_COLOR_ANSI:
        if (color->value < 8)
            return cmosh_outf(output, ctx, "\033[%um", base + color->value,
                              0, 0);
        return cmosh_outf(output, ctx, "\033[%um",
                          (fg ? 90U : 100U) + color->value - 8, 0, 0);
      case CMOSH_COLOR_256:
        return cmosh_outf(output, ctx, fg ? "\033[38;5;%um" :
                          "\033[48;5;%um", color->value, 0, 0);
      case CMOSH_COLOR_TRUE:
        return cmosh_outf(output, ctx, fg ? "\033[38;2;%u;%u;%um" :
                          "\033[48;2;%u;%u;%um", color->r, color->g,
                          color->b);
    }
    return -1;
}

static int cmosh_emit_attr(cmosh_terminal_output_fn output, void *ctx,
                           const struct cmosh_attr *attr)
{
    if (cmosh_out(output, ctx, "\033[0m") != 0)
        return -1;
    if (attr->bold && cmosh_out(output, ctx, "\033[1m") != 0)
        return -1;
    if (attr->italic && cmosh_out(output, ctx, "\033[3m") != 0)
        return -1;
    if (attr->underline && cmosh_out(output, ctx, "\033[4m") != 0)
        return -1;
    if (attr->blink && cmosh_out(output, ctx, "\033[5m") != 0)
        return -1;
    if (attr->inverse && cmosh_out(output, ctx, "\033[7m") != 0)
        return -1;
    if (attr->invisible && cmosh_out(output, ctx, "\033[8m") != 0)
        return -1;
    if (cmosh_emit_color(output, ctx, &attr->fg, 1) != 0 ||
        cmosh_emit_color(output, ctx, &attr->bg, 0) != 0)
        return -1;
    return 0;
}

int cmosh_terminal_render_full(struct cmosh_terminal *term,
                               cmosh_terminal_output_fn output, void *ctx)
{
    unsigned int r, c;
    struct cmosh_attr default_attr = cmosh_default_attr();
    struct cmosh_attr current = default_attr;

    if (!term || !output)
        return -1;
    if (cmosh_out(output, ctx, "\033[?25l\033[?1l\033>\033[0m"
                            "\033[H\033[2J") != 0)
        return -1;
    if (term->app_cursor_keys &&
        cmosh_out(output, ctx, "\033[?1h") != 0)
        return -1;
    if (term->app_keypad_keys &&
        cmosh_out(output, ctx, "\033=") != 0)
        return -1;
    for (r = 0; r < term->rows; r++) {
        unsigned int last = 0;
        for (c = 0; c < term->cols; c++) {
            struct cmosh_cell *cell = cmosh_cell_at(term, r, c);
            if (cell->width &&
                (cell->len || !cmosh_attr_equal(&cell->attr, &default_attr)))
                last = c + cell->width;
        }
        if (r || cmosh_out(output, ctx, "\033[H") != 0) {
            if (r && cmosh_outf(output, ctx, "\033[%u;1H", r + 1, 0, 0) != 0)
                return -1;
        }
        for (c = 0; c < last && c < term->cols; c++) {
            struct cmosh_cell *cell = cmosh_cell_at(term, r, c);
            if (!cell->width)
                continue;
            if (!cmosh_attr_equal(&current, &cell->attr)) {
                if (cmosh_emit_attr(output, ctx, &cell->attr) != 0)
                    return -1;
                current = cell->attr;
            }
            if (cell->len) {
                if (output(ctx, (const unsigned char *)cell->text,
                           cell->len) != 0)
                    return -1;
            } else if (cmosh_out(output, ctx, " ") != 0) {
                return -1;
            }
        }
        if (!cmosh_attr_equal(&current, &default_attr)) {
            if (cmosh_out(output, ctx, "\033[0m") != 0)
                return -1;
            current = default_attr;
        }
        if (cmosh_out(output, ctx, "\033[K") != 0)
            return -1;
    }
    if (cmosh_out(output, ctx, "\033[0m") != 0 ||
        cmosh_outf(output, ctx, "\033[%u;%uH", term->cursor_row + 1,
                   term->cursor_col + 1, 0) != 0)
        return -1;
    if (term->cursor_visible &&
        cmosh_out(output, ctx, "\033[?25h") != 0)
        return -1;
    return 0;
}

struct cmosh_buffer_output_ctx {
    unsigned char *out;
    size_t outlen;
    size_t written;
};

static int cmosh_buffer_output(void *vctx, const unsigned char *data,
                               size_t len)
{
    struct cmosh_buffer_output_ctx *ctx =
        (struct cmosh_buffer_output_ctx *)vctx;

    if (ctx->written > ctx->outlen || len > ctx->outlen - ctx->written)
        return -1;
    if (len)
        memcpy(ctx->out + ctx->written, data, len);
    ctx->written += len;
    return 0;
}

int cmosh_terminal_render_full_to_buffer(struct cmosh_terminal *term,
                                         unsigned char *out, size_t outlen,
                                         size_t *written)
{
    struct cmosh_buffer_output_ctx ctx;
    int ret;

    if (!out || !written)
        return -1;
    ctx.out = out;
    ctx.outlen = outlen;
    ctx.written = 0;
    ret = cmosh_terminal_render_full(term, cmosh_buffer_output, &ctx);
    *written = ctx.written;
    return ret;
}

int cmosh_terminal_app_cursor_keys(const struct cmosh_terminal *term)
{
    return term ? term->app_cursor_keys : 0;
}
