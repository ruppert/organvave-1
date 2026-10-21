/* SPDX-License-Identifier: GPL-3.0-only
 * Link stubs for sbf_ref: the parts of setBfree the tone generator's init references but the reference does not use. */
#include <stddef.h>
typedef struct _cc ConfigContext;
typedef struct _cd ConfigDoc;
int ampConfig (ConfigContext* c) { (void)c; return 0; }
int reverbConfig (ConfigContext* c) { (void)c; return 0; }
int whirlConfig (void* w, ConfigContext* c) { (void)w; (void)c; return 0; }
const ConfigDoc* ampDoc (void) { return NULL; }
const ConfigDoc* reverbDoc (void) { return NULL; }
const ConfigDoc* whirlDoc (void) { return NULL; }
const char* filterTypeNames[] = { "", "", "", "", "", "", "", "", "", "" };
