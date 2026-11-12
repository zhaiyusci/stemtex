#ifndef STEMTEX_CHECKPOINT_H
#define STEMTEX_CHECKPOINT_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

int stemtex_checkpoint_enabled(void);
unsigned long long stemtex_checkpoint_milliseconds(void);
int stemtex_checkpoint_begin(void);
int stemtex_checkpoint_restore(void);
void stemtex_checkpoint_set_output(FILE *stream);
void stemtex_checkpoint_set_recovery_pending(int value);
int stemtex_checkpoint_recovery_pending(void);
const char *stemtex_checkpoint_last_error(void);
int stemtex_checkpoint_add_region(void *address, size_t size);
int stemtex_checkpoint_add_code_region(void *address, size_t size);
void stemtex_checkpoint_invalidate(const char *reason);

#ifdef __cplusplus
}
#endif
#endif
