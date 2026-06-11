#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void die(const char *msg) {
    fprintf(stderr, "embed_compress: %s\n", msg);
    exit(1);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <input> <output> <varname>\n", argv[0]);
        return 1;
    }
    const char *inp  = argv[1];
    const char *out  = argv[2];
    const char *var  = argv[3];

    // Read input
    FILE *fi = fopen(inp, "rb");
    if (!fi) die("cannot open input");
    fseek(fi, 0, SEEK_END);
    long raw_len_l = ftell(fi);
    if (raw_len_l <= 0) die("empty input");
    uLong raw_len = (uLong)raw_len_l;
    fseek(fi, 0, SEEK_SET);
    unsigned char *raw = (unsigned char *)malloc(raw_len);
    if (!raw) die("malloc");
    if (fread(raw, 1, raw_len, fi) != raw_len) die("fread");
    fclose(fi);

    // Compress (worst-case bound per zlib docs)
    uLong bound = compressBound(raw_len);
    unsigned char *comp = (unsigned char *)malloc(bound);
    if (!comp) die("malloc");
    uLong comp_len = bound;
    if (compress2(comp, &comp_len, raw, raw_len, 9) != Z_OK)
        die("compress2 failed");

    // Write output header
    FILE *fo = fopen(out, "w");
    if (!fo) die("cannot open output");
    const char *basename = strrchr(inp, '/');
    basename = basename ? basename + 1 : inp;

    // Generate C-compatible header
    fprintf(fo, "// Auto-generated from %s. Do not edit.\n", basename);
    fprintf(fo, "#pragma once\n");
    fprintf(fo, "#include <stddef.h>\n");
    fprintf(fo, "const unsigned char %s[] = {\n", var);
    for (uLong i = 0; i < comp_len; ++i) {
        if (i % 16 == 0) fprintf(fo, "    ");
        fprintf(fo, "0x%02x", comp[i]);
        if ((i + 1) % 16 == 0) {
            fprintf(fo, ",\n");
        } else if (i + 1 < comp_len) {
            fprintf(fo, ", ");
        }
    }
    if (comp_len % 16 != 0) fprintf(fo, "\n");
    fprintf(fo, "};\n");
    fprintf(fo, "const size_t %s_size = %zu;\n",
                var, (size_t)comp_len);
    fprintf(fo, "const size_t %s_uncompressed_size = %zu;\n",
                var, (size_t)raw_len);

    fclose(fo);
    fprintf(stderr, "Generated %s: %lu -> %lu bytes (%.1fx)\n",
                 out, (unsigned long)raw_len,
                 (unsigned long)comp_len,
                 (double)raw_len / comp_len);
    free(raw);
    free(comp);
    return 0;
}
