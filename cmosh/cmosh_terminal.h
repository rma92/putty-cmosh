#ifndef CMOSH_TERMINAL_H
#define CMOSH_TERMINAL_H

#include <stddef.h>

struct cmosh_terminal;

typedef int (*cmosh_terminal_output_fn)(void *ctx, const unsigned char *data,
                                        size_t len);

struct cmosh_terminal *cmosh_terminal_new(unsigned int cols,
                                          unsigned int rows);
void cmosh_terminal_free(struct cmosh_terminal *term);
int cmosh_terminal_resize(struct cmosh_terminal *term, unsigned int cols,
                          unsigned int rows);
int cmosh_terminal_apply_bytes(struct cmosh_terminal *term,
                               const unsigned char *data, size_t len);
int cmosh_terminal_render_full(struct cmosh_terminal *term,
                               cmosh_terminal_output_fn output, void *ctx);
int cmosh_terminal_render_full_to_buffer(struct cmosh_terminal *term,
                                         unsigned char *out, size_t outlen,
                                         size_t *written);
int cmosh_terminal_app_cursor_keys(const struct cmosh_terminal *term);

#endif
