#include "save/sha256.h"

#include <assert.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    CpSaveSha256 hash;
    unsigned char input[113], digest[32];
    size_t count;
    unsigned i;
    FILE *file;
    assert(argc == 2);
    file = fopen(argv[1], "rb");
    assert(file != NULL);
    CpSaveSha256_Init(&hash);
    while ((count = fread(input, 1, sizeof(input), file)) != 0)
        CpSaveSha256_Update(&hash, input, count);
    assert(!ferror(file) && fclose(file) == 0);
    CpSaveSha256_Finish(&hash, digest);
    for (i = 0; i < sizeof(digest); ++i) printf("%02x", digest[i]);
    putchar('\n');
    return 0;
}
