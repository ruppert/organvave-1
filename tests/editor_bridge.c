/* SPDX-License-Identifier: GPL-3.0-only
 * The firmware's real editor handler behind stdin / stdout, for web/test_editor_organ.mjs: each input line is the hex of a
 * SysEx request (F0 .. F7); the answer is the hex of the reply frame (or an empty line when there is none).
 * The organ is the instrument's engine, on its first factory patch, as after a power-on. */
#include "editor_host.h"
#include <stdio.h>
int main(void)
{
    char line[2048];
    reset();
    setvbuf(stdout, 0, _IOLBF, 0);
    while (fgets(line, sizeof line, stdin)) {
        uint8_t a[1024];
        uint32_t n = 0, i, got;
        char *p = line;
        unsigned v;
        int k;
        while (sscanf(p, "%2x%n", &v, &k) == 1 && n < sizeof a) { a[n++] = (uint8_t)v; p += k; }
        if (n < 6u) { puts(""); continue; }
        got = request(a[4], a + 5, n - 6u);                  /* (F0 7D 46 4C cmd args F7) */
        for (i = 0; i < got; i++)
            printf("%02x", host_wire[i]);
        puts("");
    }
    return 0;
}
