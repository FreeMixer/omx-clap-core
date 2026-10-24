/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* A driver of a harness without libFuzzer (make fuzz-static): each file named runs through LLVMFuzzerTestOneInput once,
 * as libFuzzer's -runs=0 runs a corpus, so a harness and its reader link as a fully static executable with no library
 * beyond libc and libm, the build a verifier that brings its own driver takes. A property that fails aborts, as under
 * libFuzzer. Usage: fuzz_<harness> <file>... */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++)
    {
        FILE *f = fopen(argv[i], "rb");
        uint8_t *b = NULL;
        size_t n = 0, cap = 0, got;

        if (!f)
        {
            perror(argv[i]);
            return 2;
        }
        do
        {
            if (n + 65536 > cap && !(b = realloc(b, cap = (n + 65536) * 2)))
                abort();
            got = fread(b + n, 1, 65536, f);
            n += got;
        } while (got == 65536);
        if (ferror(f))
        {
            perror(argv[i]);
            return 2;
        }
        fclose(f);
        // an exact-size copy, as libFuzzer hands its input
        {
            uint8_t *exact = malloc(n ? n : 1);

            if (!exact)
                abort();
            for (got = 0; got < n; got++)
                exact[got] = b[got];
            LLVMFuzzerTestOneInput(exact, n);
            free(exact);
        }
        free(b);
    }
    printf("%d inputs\n", argc - 1);
    return 0;
}
