#define _GNU_SOURCE

#include "sharun.h"

#ifndef PATH_MAX
#  define PATH_MAX 4096
#endif

#ifndef SHARUN_ELF32
#  define SHARUN_ELF32 0
#endif
#ifndef SHARUN_SETENV
#  define SHARUN_SETENV 0
#endif
#ifndef SHARUN_LIB4BIN
#  define SHARUN_LIB4BIN 0
#endif
#ifndef SHARUN_PYINSTALLER
#  define SHARUN_PYINSTALLER 0
#endif

extern char **environ;

#if SHARUN_PYINSTALLER
#  include <ftw.h>
#endif

// ── PT_INTERP constant ────────────────────────────────────────────
#define PT_INTERP 3

// ── File-scope internal helpers ──────────────────────────────────

#if SHARUN_ELF32
static bool is_elf32(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char hdr[5];
    size_t n = fread(hdr, 1, 5, f);
    fclose(f);
    if (n != 5) return false;
    return hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F' && hdr[4] == 1;
}
#else
static bool is_elf32(const char *path) {
    (void)path;
    return false;
}
#endif

bool sharun_is_elf32(const char *path) {
    return is_elf32(path);
}

#if SHARUN_PYINSTALLER
static bytearr_t get_elf(const char *path, bool elf32_bin) {
    bytearr_t result = bytearr_init;
    FILE *f = fopen(path, "rb");
    if (!f) return result;
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    if (file_size < 0) { fclose(f); return result; }
    rewind(f);

    if (elf32_bin) {
        result.data = malloc((size_t)file_size);
        if (!result.data) { fclose(f); return result; }
        result.len = fread(result.data, 1, (size_t)file_size, f);
        result.cap = result.len;
        fclose(f);
        return result;
    }

    unsigned char raw[64];
    if (fread(raw, 1, 64, f) != 64) { fclose(f); return result; }
    bool elf_be = (raw[5] == 2);
    uint64_t shoff = read_u64(raw + 40, elf_be);
    uint16_t shnum = read_u16(raw + 60, elf_be);

    uint64_t need = shoff + (uint64_t)shnum * 64;
    if ((uint64_t)file_size < need) { fclose(f); return result; }

    result.data = malloc((size_t)need);
    if (!result.data) { fclose(f); return result; }
    rewind(f);
    result.len = fread(result.data, 1, (size_t)need, f);
    result.cap = result.len;
    fclose(f);
    return result;
}

static bool is_elf_section(const bytearr_t *elf, const char *section_name) {
    if (!elf->data || elf->len < 64) return false;
    const unsigned char *raw = elf->data;
    size_t size = elf->len;

    bool is_64 = (raw[4] == 2);
    bool elf_be = (raw[5] == 2);

    unsigned e_shoff, e_shentsize, e_shnum, e_shstrndx;
    if (is_64) {
        e_shoff     = (unsigned)read_u64(raw + 40, elf_be);
        e_shentsize = read_u16(raw + 58, elf_be);
        e_shnum     = read_u16(raw + 60, elf_be);
        e_shstrndx  = read_u16(raw + 62, elf_be);
    } else {
        e_shoff     = read_u32(raw + 32, elf_be);
        e_shentsize = read_u16(raw + 46, elf_be);
        e_shnum     = read_u16(raw + 48, elf_be);
        e_shstrndx  = read_u16(raw + 50, elf_be);
    }

    if (e_shoff == 0 || e_shentsize == 0 || e_shnum == 0) return false;
    if (e_shstrndx >= e_shnum) return false;

    unsigned shstrtab_off = e_shoff + e_shstrndx * e_shentsize;
    unsigned shstrtab_sec_off = is_64
        ? read_u32(raw + shstrtab_off + 24, elf_be)
        : read_u32(raw + shstrtab_off + 16, elf_be);
    unsigned shstrtab_sec_size = is_64
        ? read_u32(raw + shstrtab_off + 32, elf_be)
        : read_u32(raw + shstrtab_off + 20, elf_be);

    if (shstrtab_sec_off + shstrtab_sec_size > size) return false;
    const unsigned char *strtab = raw + shstrtab_sec_off;

    for (unsigned i = 0; i < e_shnum; ++i) {
        unsigned sec_off = e_shoff + i * e_shentsize;
        if (sec_off + 8 > size) break;
        unsigned name_off = read_u32(raw + sec_off, elf_be);
        if (name_off < shstrtab_sec_size) {
            const char *name = (const char *)(strtab + name_off);
            if (strcmp(section_name, name) == 0) return true;
        }
    }
    return false;
}
#else
static bytearr_t get_elf(const char *path, bool elf32_bin) {
    (void)path;
    (void)elf32_bin;
    return (bytearr_t){0};
}
static bool is_elf_section(const bytearr_t *elf, const char *section_name) {
    (void)elf;
    (void)section_name;
    return false;
}
#endif


// ── PT_INTERP patching ──────────────────────────────────────────

static bool set_interp(bytearr_t *elf_bytes, const char *elf_path, const char *new_interp) {
    const unsigned char *raw = elf_bytes->data;
    size_t size = elf_bytes->len;
    if (size < 64) return false;

    bool is_64  = (raw[4] == 2);
    bool elf_be = (raw[5] == 2);
    unsigned e_phoff, e_phentsize, e_phnum;

    if (is_64) {
        e_phoff     = (unsigned)read_u64(raw + 28, elf_be);
        e_phentsize = read_u16(raw + 54, elf_be);
        e_phnum     = read_u16(raw + 56, elf_be);
    } else {
        e_phoff     = read_u32(raw + 28, elf_be);
        e_phentsize = read_u16(raw + 42, elf_be);
        e_phnum     = read_u16(raw + 44, elf_be);
    }

    unsigned p_offset_off = is_64 ? 8 : 4;
    unsigned p_filesz_off = is_64 ? 32 : 16;

    for (unsigned i = 0; i < e_phnum; ++i) {
        unsigned ph_off = e_phoff + i * e_phentsize;
        if (ph_off + 8 > size) break;

        if (read_u32(raw + ph_off, elf_be) != PT_INTERP) continue;

        uint64_t p_offset = is_64
            ? read_u64(raw + ph_off + p_offset_off, elf_be)
            : read_u32(raw + ph_off + p_offset_off, elf_be);
        uint64_t p_filesz = is_64
            ? read_u64(raw + ph_off + p_filesz_off, elf_be)
            : read_u32(raw + ph_off + p_filesz_off, elf_be);

        size_t start = (size_t)p_offset;
        size_t end   = start + (size_t)p_filesz;
        if (end > size) return false;
        if (elf_bytes->data[end - 1] != 0) return false;

        size_t new_len = strlen(new_interp);
        if (new_len > p_filesz - 1) return false;

        memcpy(&elf_bytes->data[start], new_interp, new_len);
        for (size_t j = start + new_len; j < end - 1; ++j)
            elf_bytes->data[j] = 0;
        elf_bytes->data[end - 1] = 0;

        return sharun_write_file(elf_path, elf_bytes->data, elf_bytes->len);
    }
    return false;
}

bool sharun_needs_interp(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    unsigned char ehdr[64];
    ssize_t n = read(fd, ehdr, 64);
    if (n != 64) { close(fd); return false; }
    bool is64 = (ehdr[4] == 2);
    bool be   = (ehdr[5] == 2);
    uint64_t e_phoff;
    uint16_t e_phentsize, e_phnum;
    if (is64) {
        e_phoff     = read_u64(ehdr + 32, be);
        e_phentsize = read_u16(ehdr + 54, be);
        e_phnum     = read_u16(ehdr + 56, be);
    } else {
        e_phoff     = read_u32(ehdr + 28, be);
        e_phentsize = read_u16(ehdr + 42, be);
        e_phnum     = read_u16(ehdr + 44, be);
    }
    size_t phdr_sz = (size_t)e_phnum * e_phentsize;
    unsigned char *phdr = malloc(phdr_sz);
    if (!phdr) { close(fd); return false; }
    ssize_t pr = pread(fd, phdr, phdr_sz, (off_t)e_phoff);
    bool found = false;
    if (pr == (ssize_t)phdr_sz) {
        for (uint16_t i = 0; i < e_phnum; ++i) {
            const unsigned char *ph = phdr + i * e_phentsize;
            if (read_u32(ph, be) == PT_INTERP) { found = true; break; }
        }
    }
    free(phdr);
    close(fd);
    return found;
}

// ── Interpreter lookup ──────────────────────────────────────────

void sharun_get_interpreter(const char *library_path, char *out, size_t outsz) {
    char *ldname = sharun_get_env("SHARUN_LDNAME");
    if (ldname) {
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/%s", library_path, ldname);
        if (sharun_is_file(p)) {
            size_t plen = strlen(p);
            if (plen < outsz) { memcpy(out, p, plen + 1); }
            else { memcpy(out, p, outsz - 1); out[outsz - 1] = '\0'; }
            free(ldname);
            return;
        }
        free(ldname);
    }

    static const char *candidates[] = {
#if defined(__x86_64__)
        "ld-linux-x86-64.so.2", "ld-musl-x86_64.so.1", "ld-linux.so.2"
#elif defined(__aarch64__)
        "ld-linux-aarch64.so.1", "ld-musl-aarch64.so.1"
#elif defined(__powerpc64__) && defined(__LITTLE_ENDIAN__)
        "ld64.so.2", "ld-linux-ppc64le.so.1", "ld-musl-ppc64le.so.1"
#elif defined(__powerpc64__)
        "ld64.so.2", "ld-linux-ppc64.so.1"
#elif defined(__PPC__)
        "ld.so.1", "ld-linux-ppc.so.1"
#elif defined(__riscv)
        "ld-musl-riscv64-lp64d.so.1", "ld-linux-riscv64-lp64d.so.1"
#elif defined(__loongarch64)
        "ld-linux-loongarch64-lp64d.so.1", "ld-musl-loongarch64-lp64d.so.1"
#elif defined(__s390x__)
        "ld-linux-s390x.so.2", "ld-musl-s390x.so.1"
#else
        "ld-linux-x86-64.so.2", "ld-musl-x86_64.so.1", "ld-linux.so.2"
#endif
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/%s", library_path, candidates[i]);
        if (sharun_is_file(p)) {
            size_t plen = strlen(p);
            if (plen < outsz) { memcpy(out, p, plen + 1); }
            else { memcpy(out, p, outsz - 1); out[outsz - 1] = '\0'; }
            return;
        }
    }
    fprintf(stderr, "Interpreter not found!\n");
    exit(1);
}

// ── lib.path generation ──────────────────────────────────────────

#if SHARUN_PYINSTALLER
struct glp_ctx {
    const char *library_path;
    size_t library_path_len;
    const char **dirs;
    size_t ndirs;
    size_t dirs_cap;
};

static struct glp_ctx g_glp_ctx;

static int glp_callback(const char *fpath, const struct stat *sb, int tflag, struct FTW *ftwbuf) {
    (void)sb;
    if (tflag != FTW_F) return 0;

    const char *name = fpath + ftwbuf->base;
    size_t nlen = strlen(name);

    if (!(nlen >= 3 && memcmp(name + nlen - 3, ".so", 3) == 0)) {
        if (!strstr(name, ".so.")) return 0;
    }

    char parent[PATH_MAX];
    sharun_dirname(fpath, parent, sizeof(parent));

    if (strcmp(parent, g_glp_ctx.library_path) == 0) return 0;

    const char *parent_base = sharun_basename(parent);
    if (strcmp(parent_base, "lib-dynload") == 0) return 0;

    for (size_t i = 0; i < g_glp_ctx.ndirs; ++i)
        if (strcmp(g_glp_ctx.dirs[i], parent) == 0) return 0;

    if (g_glp_ctx.ndirs >= g_glp_ctx.dirs_cap) {
        size_t newcap = g_glp_ctx.dirs_cap ? g_glp_ctx.dirs_cap * 2 : 64;
        const char **tmp = realloc(g_glp_ctx.dirs, newcap * sizeof(const char *));
        if (!tmp) return 0;
        g_glp_ctx.dirs = tmp;
        g_glp_ctx.dirs_cap = newcap;
    }
    g_glp_ctx.dirs[g_glp_ctx.ndirs++] = strdup(parent);
    return 0;
}
#endif

void sharun_gen_lib_path(const char *library_path, const char *lib_path_file) {
#if SHARUN_PYINSTALLER
    g_glp_ctx.library_path = library_path;
    g_glp_ctx.library_path_len = strlen(library_path);
    g_glp_ctx.dirs = NULL;
    g_glp_ctx.ndirs = 0;
    g_glp_ctx.dirs_cap = 0;

    nftw(library_path, glp_callback, 64, FTW_PHYS);

    size_t content_cap = 256;
    size_t content_len = 0;
    char *content = malloc(content_cap);
    if (!content) { fprintf(stderr, "Out of memory\n"); exit(1); }

    content[content_len++] = '+';
    content[content_len++] = ':';

    for (size_t i = 0; i < g_glp_ctx.ndirs; ++i) {
        if (i > 0) {
            if (content_len + 1 >= content_cap) {
                content_cap *= 2;
                content = realloc(content, content_cap);
                if (!content) { fprintf(stderr, "Out of memory\n"); exit(1); }
            }
            content[content_len++] = ':';
        }
        size_t dlen = strlen(g_glp_ctx.dirs[i]);
        if (content_len + dlen >= content_cap) {
            while (content_len + dlen >= content_cap) content_cap *= 2;
            content = realloc(content, content_cap);
            if (!content) { fprintf(stderr, "Out of memory\n"); exit(1); }
        }
        memcpy(content + content_len, g_glp_ctx.dirs[i], dlen);
        content_len += dlen;
    }

    for (size_t i = 0; i < content_len; ++i)
        if (content[i] == ':') content[i] = '\n';

    char *replace_pos = content;
    while ((replace_pos = strstr(replace_pos, library_path)) != NULL) {
        size_t off = (size_t)(replace_pos - content);
        size_t remaining = content_len - off;
        if (remaining >= g_glp_ctx.library_path_len) {
            content[off] = '+';
            memmove(content + off + 1, content + off + g_glp_ctx.library_path_len,
                    remaining - g_glp_ctx.library_path_len + 1);
            content_len -= g_glp_ctx.library_path_len - 1;
            replace_pos = content + off + 1;
        } else {
            break;
        }
    }

    FILE *f = fopen(lib_path_file, "wb");
    if (!f) {
        fprintf(stderr, "Failed to write lib.path: %s\n", lib_path_file);
        exit(1);
    }
    fwrite(content, 1, content_len, f);
    fclose(f);
    fprintf(stderr, "Write lib.path: %s\n", lib_path_file);

    for (size_t i = 0; i < g_glp_ctx.ndirs; ++i)
        free((void *)g_glp_ctx.dirs[i]);
    free(g_glp_ctx.dirs);
    free(content);
#else
    (void)library_path;
    (void)lib_path_file;
#endif
}

// ── Decompress lib4bin ──────────────────────────────────────────

unsigned char *sharun_decompress_lib4bin(size_t *out_len) {
#if SHARUN_LIB4BIN
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, MAX_WBITS) != Z_OK) {
        fprintf(stderr, "Failed to init zlib stream!\n");
        exit(1);
    }

    strm.next_in  = (unsigned char *)lib4bin_data;
    strm.avail_in = lib4bin_data_size;

    unsigned char *result = malloc(lib4bin_data_uncompressed_size);
    if (!result) { fprintf(stderr, "Out of memory\n"); exit(1); }
    strm.next_out  = result;
    strm.avail_out = lib4bin_data_uncompressed_size;

    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    if (ret != Z_STREAM_END) {
        fprintf(stderr, "Failed to decompress lib4bin!\n");
        exit(1);
    }
    if (out_len) *out_len = lib4bin_data_uncompressed_size;
    return result;
#else
    (void)out_len;
    return NULL;
#endif
}

// ── Userland execve (avoids /proc/self/exe -> ld) ────────────────

LoadedElf sharun_map_elf(const char *path, uint64_t fixed_base) {
    uint64_t ps  = (uint64_t)sysconf(_SC_PAGESIZE);
    uint64_t psm = ps - 1;

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror("open"); exit(1); }

    unsigned char ehdr[64];
    memset(ehdr, 0, sizeof(ehdr));
    if (read(fd, ehdr, 64) != 64) { perror("read ehdr"); close(fd); exit(1); }

    bool is64 = (ehdr[4] == 2);
    bool be   = (ehdr[5] == 2);
    bool dyn  = (read_u16(ehdr + 16, be) == 3); // ET_DYN

    uint64_t e_phoff;
    uint16_t e_phentsize, e_phnum;
    if (is64) {
        e_phoff     = read_u64(ehdr + 32, be);
        e_phentsize = read_u16(ehdr + 54, be);
        e_phnum     = read_u16(ehdr + 56, be);
    } else {
        e_phoff     = read_u32(ehdr + 28, be);
        e_phentsize = read_u16(ehdr + 42, be);
        e_phnum     = read_u16(ehdr + 44, be);
    }

    size_t phdr_sz = (size_t)e_phnum * e_phentsize;
    unsigned char *phdr = malloc(phdr_sz);
    if (!phdr) { fprintf(stderr, "Out of memory\n"); close(fd); exit(1); }
    if (pread(fd, phdr, phdr_sz, (off_t)e_phoff) != (ssize_t)phdr_sz)
        { perror("pread phdrs"); close(fd); free(phdr); exit(1); }

    uint64_t min_vaddr = UINT64_MAX, max_end = 0;
    uint64_t first_load_vaddr = 0, first_load_offset = 0;
    for (uint16_t i = 0; i < e_phnum; ++i) {
        const unsigned char *ph = phdr + i * e_phentsize;
        if (read_u32(ph, be) != 1) continue;
        uint64_t vaddr = is64 ? read_u64(ph + 16, be) : read_u32(ph + 8, be);
        uint64_t memsz = is64 ? read_u64(ph + 40, be) : read_u32(ph + 20, be);
        if (vaddr < min_vaddr) {
            min_vaddr = vaddr;
            first_load_vaddr  = vaddr;
            first_load_offset = is64 ? read_u64(ph + 8, be) : read_u32(ph + 4, be);
        }
        uint64_t end = vaddr + memsz;
        if (end > max_end) max_end = end;
    }

    uint64_t base = fixed_base;
    if (dyn && base == 0) {
        size_t total = (size_t)(((max_end - min_vaddr) + psm) & ~psm);
        void *area = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (area == MAP_FAILED) { perror("mmap reserve"); close(fd); free(phdr); exit(1); }
        base = (uint64_t)(uintptr_t)area - min_vaddr;
    }

    for (uint16_t i = 0; i < e_phnum; ++i) {
        const unsigned char *ph = phdr + i * e_phentsize;
        if (read_u32(ph, be) != 1) continue;
        uint64_t vaddr  = is64 ? read_u64(ph + 16, be) : read_u32(ph + 8, be);
        uint64_t offset = is64 ? read_u64(ph + 8, be)  : read_u32(ph + 4, be);
        uint64_t filesz = is64 ? read_u64(ph + 32, be) : read_u32(ph + 16, be);
        uint64_t memsz  = is64 ? read_u64(ph + 40, be) : read_u32(ph + 20, be);
        uint32_t flags  = is64 ? read_u32(ph + 4, be)  : read_u32(ph + 24, be);
        if (memsz == 0) continue;

        uint64_t map_at  = (base + vaddr) & ~psm;
        uint64_t map_end = ((base + vaddr + memsz) + psm) & ~psm;
        uint64_t map_off = offset & ~psm;
        int prot = ((flags & 4) ? PROT_READ : 0)
                 | ((flags & 2) ? PROT_WRITE : 0)
                 | ((flags & 1) ? PROT_EXEC : 0);

        void *addr = mmap((void *)(uintptr_t)map_at,
                          (size_t)(map_end - map_at), prot,
                          MAP_PRIVATE | MAP_FIXED, fd,
                          (off_t)map_off);
        if (addr == MAP_FAILED) {
            fprintf(stderr, "mmap 0x%lx %s\n",
                    (unsigned long)vaddr, strerror(errno));
            close(fd); free(phdr); exit(1);
        }

        uint64_t file_end = base + vaddr + filesz;
        if (memsz > filesz) {
            uint64_t page_end = (file_end + psm) & ~psm;
            if (page_end > file_end)
                memset((void *)(uintptr_t)file_end, 0,
                       (size_t)(page_end - file_end));
            if (map_end > page_end)
                mmap((void *)(uintptr_t)page_end,
                     (size_t)(map_end - page_end),
                     prot, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
        }
    }
    close(fd);

    uint64_t tls_vaddr = 0, tls_filesz = 0, tls_memsz = 0, tls_align = 0;
    for (uint16_t i = 0; i < e_phnum; ++i) {
        const unsigned char *ph = phdr + i * e_phentsize;
        if (read_u32(ph, be) == 7) { // PT_TLS
            tls_vaddr  = is64 ? read_u64(ph + 16, be) : read_u32(ph + 8, be);
            tls_filesz = is64 ? read_u64(ph + 32, be) : read_u32(ph + 16, be);
            tls_memsz  = is64 ? read_u64(ph + 40, be) : read_u32(ph + 20, be);
            tls_align  = is64 ? read_u64(ph + 48, be) : read_u32(ph + 32, be);
            break;
        }
    }

    uint64_t entry     = is64 ? read_u64(ehdr + 24, be) : read_u32(ehdr + 24, be);
    uint64_t abs_entry = base + entry;
    uint64_t phdr_vaddr = base + first_load_vaddr + (e_phoff - first_load_offset);

    free(phdr);

    LoadedElf result;
    result.base       = base;
    result.entry      = abs_entry;
    result.phdr_vaddr = phdr_vaddr;
    result.phnum      = e_phnum;
    result.phent      = e_phentsize;
    result.tls_vaddr  = tls_vaddr;
    result.tls_filesz = tls_filesz;
    result.tls_memsz  = tls_memsz;
    result.tls_align  = tls_align;
    return result;
}

#define ALIGN_UP(n) (((n) + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1))

static void push_bytearr(bytearr_t *buf, const void *data, size_t len) {
    size_t pos = buf->len;
    size_t alen = ALIGN_UP(len);
    if (pos + alen > buf->cap) {
        size_t newcap = buf->cap ? buf->cap * 2 : 4096;
        while (newcap < pos + alen) newcap *= 2;
        unsigned char *tmp = realloc(buf->data, newcap);
        if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
        buf->data = tmp;
        buf->cap = newcap;
    }
    if (len) memcpy(buf->data + pos, data, len);
    buf->len = pos + alen;
}

static void push_str_bytearr(bytearr_t *buf, const char *s) {
    push_bytearr(buf, s, strlen(s) + 1);
}

static void push_aux(bytearr_t *buf, uint64_t type, uint64_t val) {
    push_bytearr(buf, &type, sizeof(type));
    push_bytearr(buf, &val, sizeof(val));
}

typedef struct {
    uint64_t type;
    uint64_t val;
} AuxE;

typedef struct {
    size_t off;
} StrRef;

_Noreturn void sharun_userland_execve(const char *bin_path,
                                       const char *interp_path,
                                       strarr_t *exec_args,
                                       const char *library_path,
                                       const char *argv0) {
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

    bool has_interp = interp_path && interp_path[0];
    LoadedElf bin_elf    = sharun_map_elf(bin_path, 0);
    LoadedElf interp_elf;
    if (has_interp)
        interp_elf = sharun_map_elf(interp_path, 0);
    else
        memset(&interp_elf, 0, sizeof(interp_elf));

    if (library_path && library_path[0])
        setenv("LD_LIBRARY_PATH", library_path, 1);

    strarr_t argv_vec = strarr_init;
    strarr_push(&argv_vec, argv0);
    for (size_t i = 0; i < exec_args->len; ++i)
        strarr_push(&argv_vec, exec_args->data[i]);

    strarr_t env_vec = strarr_init;
    for (char **e = environ; *e; ++e)
        strarr_push(&env_vec, *e);

    uint64_t hwcap = 0, clktck = 100;
    {
        FILE *af = fopen("/proc/self/auxv", "rb");
        if (af) {
            uint64_t at, av;
            while (fread(&at, sizeof(at), 1, af) == 1 &&
                   fread(&av, sizeof(av), 1, af) == 1) {
                if (at == AT_HWCAP)  hwcap  = av;
                if (at == AT_CLKTCK) clktck = av;
                if (at == AT_NULL) break;
            }
            fclose(af);
        }
    }

    const char *platform_str =
#if defined(__x86_64__)
        "x86_64";
#elif defined(__i386__)
        "i386";
#elif defined(__aarch64__)
        "aarch64";
#elif defined(__arm__)
        "armv7l";
#elif defined(__powerpc64__) && defined(__LITTLE_ENDIAN__)
        "powerpc64le";
#elif defined(__powerpc64__)
        "powerpc64";
#elif defined(__PPC__)
        "powerpc";
#elif defined(__riscv)
#  if __riscv_xlen == 64
        "riscv64";
#  else
        "riscv32";
#  endif
#elif defined(__loongarch64)
        "loongarch64";
#elif defined(__s390x__)
        "s390x";
#else
        "unknown";
#endif

    unsigned char rand16[16];
    int rfd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (rfd >= 0) {
        read(rfd, rand16, 16);
        close(rfd);
    } else {
        for (int i = 0; i < 16; ++i)
            rand16[i] = (unsigned char)rand();
    }

    bytearr_t stk = bytearr_init;

    uint64_t zero = 0;

    size_t argc_offset = stk.len;
    uint64_t argc_val = argv_vec.len;
    push_bytearr(&stk, &argc_val, sizeof(argc_val));

    size_t argv_start = stk.len;
    for (size_t i = 0; i <= argv_vec.len; ++i)
        push_bytearr(&stk, &zero, sizeof(zero));

    size_t envp_start = stk.len;
    for (size_t i = 0; i <= env_vec.len; ++i)
        push_bytearr(&stk, &zero, sizeof(zero));

    size_t auxv_start = stk.len;
    AuxE aux_entries[] = {
        {AT_PHDR,     bin_elf.phdr_vaddr},
        {AT_PHENT,    bin_elf.phent},
        {AT_PHNUM,    bin_elf.phnum},
        {AT_PAGESZ,   page_size},
        {AT_BASE,     has_interp ? interp_elf.base : 0},
        {AT_FLAGS,    0},
        {AT_ENTRY,    bin_elf.entry},
        {AT_UID,      (uint64_t)getuid()},
        {AT_EUID,     (uint64_t)geteuid()},
        {AT_GID,      (uint64_t)getgid()},
        {AT_EGID,     (uint64_t)getegid()},
        {AT_SECURE,   0},
        {AT_CLKTCK,   clktck},
        {AT_PLATFORM, 0},
        {AT_HWCAP,    hwcap},
        {AT_RANDOM,   0},
        {AT_EXECFN,   0},
        {AT_NULL,     0},
    };
    size_t auxv_count = sizeof(aux_entries) / sizeof(aux_entries[0]);
    for (size_t i = 0; i < auxv_count; ++i)
        push_aux(&stk, aux_entries[i].type, aux_entries[i].val);

    size_t *arg_off = malloc(argv_vec.len * sizeof(size_t));
    size_t *env_off = malloc(env_vec.len * sizeof(size_t));
    size_t plat_off = 0, execfn_off = 0, str_start = stk.len;

    for (size_t i = 0; i < argv_vec.len; ++i) {
        arg_off[i] = stk.len - str_start;
        push_str_bytearr(&stk, argv_vec.data[i]);
    }
    for (size_t i = 0; i < env_vec.len; ++i) {
        env_off[i] = stk.len - str_start;
        push_str_bytearr(&stk, env_vec.data[i]);
    }
    plat_off = stk.len - str_start;
    push_str_bytearr(&stk, platform_str);
    execfn_off = stk.len - str_start;
    push_str_bytearr(&stk, bin_path);
    {
        size_t cur = stk.len;
        size_t aligned = ALIGN_UP(cur);
        if (aligned > stk.cap) {
            unsigned char *tmp = realloc(stk.data, aligned);
            if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
            stk.data = tmp;
            stk.cap = aligned;
        }
        memset(stk.data + stk.len, 0, aligned - stk.len);
        stk.len = aligned;
    }

    size_t rand_offset = stk.len;
    push_bytearr(&stk, rand16, 16);

    size_t stack_gap = 2 * 1024 * 1024;
    size_t alloc_size = stk.len + stack_gap;
    void *stack_mem = mmap(NULL, alloc_size,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack_mem == MAP_FAILED) {
        perror("mmap stack");
        exit(1);
    }
    memset(stack_mem, 0, alloc_size);

    uint64_t stack_base = (uint64_t)(uintptr_t)stack_mem;
    uint64_t data_top   = stack_base + alloc_size - stk.len;

    memcpy((void *)(uintptr_t)data_top, stk.data, stk.len);

    unsigned char *ptr = (unsigned char *)(uintptr_t)data_top;

    uint64_t *argv_out = (uint64_t *)(ptr + argv_start);
    for (size_t i = 0; i < argv_vec.len; ++i)
        argv_out[i] = data_top + str_start + arg_off[i];
    argv_out[argv_vec.len] = 0;

    uint64_t *envp_out = (uint64_t *)(ptr + envp_start);
    for (size_t i = 0; i < env_vec.len; ++i)
        envp_out[i] = data_top + str_start + env_off[i];
    envp_out[env_vec.len] = 0;

    uint64_t *auxv_out = (uint64_t *)(ptr + auxv_start);
    uint64_t platform_addr = data_top + str_start + plat_off;
    uint64_t execfn_addr   = data_top + str_start + execfn_off;
    uint64_t random_addr   = data_top + rand_offset;
    for (size_t i = 0; i < auxv_count; ++i) {
        switch (auxv_out[i * 2]) {
        case AT_PLATFORM: auxv_out[i * 2 + 1] = platform_addr; break;
        case AT_RANDOM:   auxv_out[i * 2 + 1] = random_addr;   break;
        case AT_EXECFN:   auxv_out[i * 2 + 1] = execfn_addr;   break;
        }
    }

    void *sp = ptr + argc_offset;

    void *entry = (void *)(uintptr_t)(has_interp ? interp_elf.entry : bin_elf.entry);

    if (!has_interp && bin_elf.tls_memsz > 0) {
        size_t tls_size = (size_t)((bin_elf.tls_memsz + 7) & ~7ULL);
        size_t tcb_size = 0x70;
        size_t total = tls_size + tcb_size;

        void *tls_mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (tls_mem == MAP_FAILED) {
            perror("mmap TLS");
            exit(1);
        }
        memset(tls_mem, 0, total);

        size_t filesz = (size_t)bin_elf.tls_filesz;
        if (filesz > 0) {
            void *src = (void *)(uintptr_t)(bin_elf.base + bin_elf.tls_vaddr);
            size_t cp = filesz < tls_size ? filesz : tls_size;
            memcpy(tls_mem, src, cp);
        }

        uint64_t *tcb = (uint64_t *)((unsigned char *)tls_mem + tls_size);
        uint64_t tcb_addr = (uint64_t)(uintptr_t)tcb;

        uint64_t *dtv = malloc(2 * sizeof(uint64_t));
        if (!dtv) { fprintf(stderr, "Out of memory\n"); exit(1); }
        dtv[0] = (uint64_t)(uintptr_t)tls_mem;
        dtv[1] = 0;

        tcb[0] = tcb_addr;
        tcb[1] = (uint64_t)(uintptr_t)dtv;
        tcb[2] = tcb_addr;

#ifdef SYS_arch_prctl
        long fs_ret = syscall(SYS_arch_prctl, ARCH_SET_FS, tcb_addr);
        if (fs_ret != 0) {
            fprintf(stderr, "arch_prctl(ARCH_SET_FS) failed: %s\n",
                    strerror(errno));
            exit(1);
        }
#else
        fprintf(stderr, "userland TLS setup unsupported on this arch\n");
        exit(1);
#endif
    }

    free(arg_off);
    free(env_off);
    bytearr_free(&stk);
    strarr_free(&argv_vec);
    strarr_free(&env_vec);

    __sync_synchronize();

#if defined(__x86_64__)
    __asm__ __volatile__("mov %[entry], %%r11\n\t"
                         "mov %[sp], %%rsp\n\t"
                         "xor %%ebp, %%ebp\n\t"
                         "jmp *%%r11\n\t"
                         : : [sp]"r"(sp), [entry]"r"(entry) : "memory", "r11");
#elif defined(__i386__)
    __asm__ __volatile__("mov %[entry], %%eax\n\t"
                         "mov %[sp], %%esp\n\t"
                         "xor %%ebp, %%ebp\n\t"
                         "jmp *%%eax\n\t"
                         : : [sp]"r"(sp), [entry]"r"(entry) : "memory", "eax");
#elif defined(__aarch64__)
    __asm__ __volatile__("mov sp, %[sp]\n\tmov x29, #0\n\tmov x30, #0\n\tbr %[entry]\n\t"
                         : : [sp]"r"(sp), [entry]"r"(entry) : "memory", "x29", "x30");
#elif defined(__arm__)
    __asm__ __volatile__("mov sp, %[sp]\n\tmov fp, #0\n\tbx %[entry]\n\t"
                         : : [sp]"r"(sp), [entry]"r"(entry) : "memory", "fp");
#elif defined(__powerpc64__) || defined(__PPC__)
    __asm__ __volatile__("mr 1, %[sp]\n\tli 31, 0\n\tmtctr %[entry]\n\tbctr\n\t"
                         : : [sp]"r"(sp), [entry]"r"(entry) : "memory", "r31");
#elif defined(__riscv)
    __asm__ __volatile__("mv sp, %[sp]\n\tmv s0, zero\n\tjr %[entry]\n\t"
                         : : [sp]"r"(sp), [entry]"r"(entry) : "memory", "s0");
#elif defined(__loongarch64)
    __asm__ __volatile__("move $sp, %[sp]\n\tmove $fp, $zero\n\tjr %[entry]\n\t"
                         : : [sp]"r"(sp), [entry]"r"(entry) : "memory", "$fp");
#elif defined(__s390x__)
    __asm__ __volatile__("lgr %%r15, %[sp]\n\tlghi %%r11, 0\n\tbr %[entry]\n\t"
                         : : [sp]"r"(sp), [entry]"r"(entry) : "memory", "r11");
#else
#  error unsupported arch
#endif
    __builtin_unreachable();
}

// ── Usage ───────────────────────────────────────────────────────

static void print_usage(void) {
    fprintf(stderr,
        "[ %s ]\n"
        "\n"
        "[ Usage ]: %s [OPTIONS] [EXEC ARGS]...\n",
        SHARUN_NAME, SHARUN_NAME);
#if SHARUN_LIB4BIN
    fprintf(stderr, "     Use lib4bin for create 'bin' and 'shared' dirs\n");
#endif
    fprintf(stderr,
"\n"
"[ Arguments ]:\n"
"    [EXEC ARGS]...              Command line arguments for execution\n"
"\n"
"[ Options ]:\n");
#if SHARUN_LIB4BIN
    fprintf(stderr, "     l,  lib4bin [ARGS]         Launch the built-in lib4bin\n");
#endif
    fprintf(stderr,
"    -g,  --gen-lib-path         Generate a lib.path file\n"
"    -v,  --version              Print version\n"
"    -h,  --help                 Print help\n"
"\n"
"[ Environments ]:\n"
"    SHARUN_WORKING_DIR=/path       Specifies the path to the working directory\n"
"    SHARUN_ALLOW_SYS_VKICD=1       Enables breaking system vulkan/icd.d for vulkan loader\n"
"    SHARUN_ALLOW_LD_PRELOAD=1      Enables breaking LD_PRELOAD env variable\n"
"    SHARUN_ALLOW_QT_PLUGIN_PATH=1  Enables breaking QT_PLUGIN_PATH env variable\n"
"    SHARUN_NO_NVIDIA_EGL_PRIME=1   Disables NVIDIA EGL prime logic\n"
"    SHARUN_PRINTENV=1              Print environment variables to stderr\n"
"    SHARUN_LDNAME=ld.so            Specifies the name of the interpreter\n"
"    SHARUN_EXTRA_LIBRARY_PATH      Extra library directories with highest priority\n"
"    SHARUN_FALLBACK_LIBRARY_PATH   Fallback library directories with lowest priority\n"
"    SHARUN_DIR                     Sharun directory\n");
}

// ═════════════════════════════════════════════════════════════════
//  main
// ═════════════════════════════════════════════════════════════════

int main(int argc, char **argv) {
    // ── Current exe path ────────────────────────────────────────
    char *sharun_path = sharun_realpath("/proc/self/exe");
    if (!sharun_path) sharun_path = sharun_realpath(argv[0]);
    if (!sharun_path) {
        fprintf(stderr, "Cannot resolve executable path\n");
        return 1;
    }

    // ── Build exec_args from argv; save original argv[0] ────────
    strarr_t exec_args = strarr_init;
    for (int i = 0; i < argc; ++i)
        strarr_push(&exec_args, argv[i]);
    char *saved_argv0 = strdup(exec_args.data[0]);

    // ── Determine sharun_dir ────────────────────────────────────
    char *sharun_dir = sharun_get_env("SHARUN_DIR");

    if (sharun_dir && sharun_is_dir(sharun_dir)) {
        char sp[PATH_MAX];
        snprintf(sp, sizeof(sp), "%s/%s", sharun_dir, SHARUN_NAME);
        char shared_path[PATH_MAX];
        snprintf(shared_path, sizeof(shared_path), "%s/shared", sharun_dir);
        bool valid = sharun_is_dir(shared_path) && sharun_is_exe(sp) &&
                     sharun_is_same_rootdir(sharun_dir, sharun_path, sp);
        if (!valid) { free(sharun_dir); sharun_dir = NULL; }
    }

    if (!sharun_dir) {
        char sharun_dir_buf[PATH_MAX];
        sharun_dirname(sharun_path, sharun_dir_buf, sizeof(sharun_dir_buf));
        sharun_dir = strdup(sharun_dir_buf);

        const char *base = sharun_basename(sharun_dir);
        if (strcmp(base, "bin") == 0) {
            char lower[PATH_MAX];
            sharun_dirname(sharun_dir, lower, sizeof(lower));
            char shared_check[PATH_MAX];
            snprintf(shared_check, sizeof(shared_check), "%s/shared", lower);
            if (sharun_is_dir(shared_check)) {
                char *tmp = sharun_realpath(lower);
                if (tmp) { free(sharun_dir); sharun_dir = tmp; }
            }
        }
        setenv("SHARUN_DIR", sharun_dir, 1);
    }

    // ── Derived paths ───────────────────────────────────────────
    char bin_dir[PATH_MAX];
    char shared_dir[PATH_MAX];
    char shared_bin[PATH_MAX];
    char shared_lib[PATH_MAX];
    char shared_lib32[PATH_MAX];

    snprintf(bin_dir, sizeof(bin_dir), "%s/bin", sharun_dir);
    snprintf(shared_dir, sizeof(shared_dir), "%s/shared", sharun_dir);
    snprintf(shared_bin, sizeof(shared_bin), "%s/bin", shared_dir);
    snprintf(shared_lib, sizeof(shared_lib), "%s/lib", shared_dir);
    snprintf(shared_lib32, sizeof(shared_lib32), "%s/lib32", shared_dir);

    // ── Determine bin_name from argv[0] / symlinks ──────────────
    char *arg0_path = strdup(exec_args.data[0]);
    const char *arg0_name = sharun_basename(arg0_path);

    char arg0_dir_buf[PATH_MAX];
    {
        char arg0_parent[PATH_MAX];
        sharun_dirname(arg0_path, arg0_parent, sizeof(arg0_parent));
        if (!realpath(arg0_parent, arg0_dir_buf)) {
            char found_buf[PATH_MAX];
            if (sharun_which(arg0_name, found_buf, sizeof(found_buf))) {
                sharun_dirname(found_buf, arg0_dir_buf, sizeof(arg0_dir_buf));
            } else {
                fprintf(stderr, "Failed to find ARG0 dir!\n");
                exit(1);
            }
        }
    }

    char arg0_full[PATH_MAX];
    snprintf(arg0_full, sizeof(arg0_full), "%s/%s", arg0_dir_buf, arg0_name);

    char arg0_full_canon[PATH_MAX];
    char *arg0_full_name;
    if (!realpath(arg0_full, arg0_full_canon)) {
        arg0_full_name = strdup(arg0_name);
    } else {
        arg0_full_name = strdup(sharun_basename(arg0_full_canon));
    }

    // Remove argv[0] from exec_args
    if (exec_args.len > 0) {
        free(exec_args.data[0]);
        memmove(&exec_args.data[0], &exec_args.data[1],
                (exec_args.len - 1) * sizeof(char *));
        exec_args.len--;
    }

    // Determine bin_name
    struct stat st;
    bool is_sym = (lstat(arg0_path, &st) == 0 && S_ISLNK(st.st_mode));

    char sharun_name_path[PATH_MAX];
    snprintf(sharun_name_path, sizeof(sharun_name_path), "%s/%s", sharun_dir, SHARUN_NAME);

    char *bin_name;
    if (is_sym && strcmp(arg0_full_canon, sharun_name_path) == 0) {
        bin_name = strdup(arg0_name);
    } else if (is_sym) {
        char shared_bin_test[PATH_MAX];
        snprintf(shared_bin_test, sizeof(shared_bin_test), "%s/%s", shared_bin, arg0_full_name);
        if (sharun_is_file(shared_bin_test))
            bin_name = strdup(arg0_full_name);
        else
            bin_name = strdup(sharun_basename(sharun_path));
    } else {
        bin_name = strdup(sharun_basename(sharun_path));
    }

    // ── AppRun handling ─────────────────────────────────────────
    if (strcmp(bin_name, "AppRun") == 0) {
        char appname_file[PATH_MAX];
        snprintf(appname_file, sizeof(appname_file), "%s/%s", sharun_dir, ".app");
        char *appname = NULL;

        if (!sharun_is_file(appname_file)) {
            DIR *d = opendir(sharun_dir);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) != NULL) {
                    size_t nlen = strlen(de->d_name);
                    if (nlen < 9) continue;
                    if (strcmp(de->d_name + nlen - 8, ".desktop") != 0) continue;
                    char df_path[PATH_MAX];
                    snprintf(df_path, sizeof(df_path), "%s/%s", sharun_dir, de->d_name);
                    FILE *df = fopen(df_path, "rb");
                    if (df) {
                        char line[4096];
                        while (fgets(line, sizeof(line), df)) {
                            size_t llen = strlen(line);
                            while (llen > 0 && (line[llen-1] == '\n' || line[llen-1] == '\r'))
                                line[--llen] = '\0';
                            if (strncmp(line, "Exec=", 5) == 0) {
                                const char *rest = line + 5;
                                const char *sp = strchr(rest, ' ');
                                if (sp) {
                                    appname = strndup(rest, (size_t)(sp - rest));
                                } else {
                                    appname = strdup(rest);
                                }
                                break;
                            }
                        }
                        fclose(df);
                    }
                    if (appname) break;
                }
                closedir(d);
            }
        }

        if (!appname) {
            FILE *af = fopen(appname_file, "rb");
            if (!af) {
                fprintf(stderr, "Failed to read .app file: %s\n", appname_file);
                exit(1);
            }
            char buf[4096];
            if (fgets(buf, sizeof(buf), af)) {
                size_t l = strlen(buf);
                while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r')) buf[--l] = '\0';
                appname = strdup(buf);
            }
            fclose(af);
        }

        if (!appname || !appname[0]) {
            fprintf(stderr, "Failed to get app name: %s\n", appname_file);
            exit(1);
        }

        {
            const char *base = sharun_basename(appname);
            char *tmp = strdup(base);
            free(appname);
            appname = tmp;
        }

        // Remove quotes
        char *dst = appname;
        for (char *src = appname; *src; ++src) {
            if (*src != '\'' && *src != '"')
                *dst++ = *src;
        }
        *dst = '\0';

        char app_path[PATH_MAX];
        snprintf(app_path, sizeof(app_path), "%s/%s", bin_dir, appname);

        sharun_add_to_env("PATH", bin_dir);

        char *argv0_val = sharun_get_env("ARGV0");
        if (!argv0_val) {
            setenv("ARGV0", saved_argv0, 1);
        } else {
            free(argv0_val);
        }
        char *appdir_val = sharun_get_env("APPDIR");
        if (!appdir_val) {
            setenv("APPDIR", sharun_dir, 1);
        } else {
            free(appdir_val);
        }

        const char **av = malloc((exec_args.len + 2) * sizeof(const char *));
        if (!av) { fprintf(stderr, "Out of memory\n"); exit(1); }
        size_t avi = 0;
        av[avi++] = app_path;
        for (size_t i = 0; i < exec_args.len; ++i)
            av[avi++] = exec_args.data[i];
        av[avi++] = NULL;

        execve(app_path, (char *const *)av, environ);
        fprintf(stderr, "Failed to run App: %s: %s\n",
                app_path, strerror(errno));
        exit(1);
    }

    // ── Subcommand dispatch (works regardless of binary name) ───
    if (exec_args.len > 0) {
        const char *arg = exec_args.data[0];
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--version") == 0) {
            fprintf(stdout, "v%s\n", SHARUN_VERSION);
            return 0;
        }
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage();
            return 0;
        }
        if (strcmp(arg, "-g") == 0 || strcmp(arg, "--gen-lib-path") == 0) {
            const char *lib_paths[] = {shared_lib, shared_lib32};
            for (size_t i = 0; i < 2; ++i) {
                if (sharun_is_dir(lib_paths[i])) {
                    char lpfile[PATH_MAX];
                    snprintf(lpfile, sizeof(lpfile), "%s/lib.path", lib_paths[i]);
                    sharun_gen_lib_path(lib_paths[i], lpfile);
                }
            }
            return 0;
        }

#if SHARUN_LIB4BIN
        if (strcmp(arg, "l") == 0 || strcmp(arg, "lib4bin") == 0) {
            size_t script_len;
            unsigned char *lib4bin_script = sharun_decompress_lib4bin(&script_len);

            // Remove "l"/"lib4bin" from exec_args
            if (exec_args.len > 0) {
                free(exec_args.data[0]);
                memmove(&exec_args.data[0], &exec_args.data[1],
                        (exec_args.len - 1) * sizeof(char *));
                exec_args.len--;
            }

            sharun_add_to_env("PATH", bin_dir);

            int pd[2];
            if (pipe(pd) != 0) {
                fprintf(stderr, "pipe failed!\n");
                exit(1);
            }

            char bash_path[PATH_MAX];
            if (!sharun_which("bash", bash_path, sizeof(bash_path))) {
                fprintf(stderr, "bash not found!\n");
                exit(1);
            }

            pid_t pid = fork();
            if (pid == 0) {
                close(pd[1]);
                dup2(pd[0], STDIN_FILENO);
                close(pd[0]);

                setenv("SHARUN", sharun_path, 1);

                const char **bash_av = malloc((exec_args.len + 4) * sizeof(const char *));
                if (!bash_av) { fprintf(stderr, "Out of memory\n"); exit(1); }
                size_t bai = 0;
                bash_av[bai++] = bash_path;
                bash_av[bai++] = "-s";
                bash_av[bai++] = "--";
                for (size_t i = 0; i < exec_args.len; ++i)
                    bash_av[bai++] = exec_args.data[i];
                bash_av[bai++] = NULL;

                execve(bash_path, (char *const *)bash_av, environ);
                fprintf(stderr, "Failed to run bash: %s\n", strerror(errno));
                exit(1);
            } else if (pid < 0) {
                fprintf(stderr, "fork failed!\n");
                exit(1);
            }

            close(pd[0]);
            ssize_t w = write(pd[1], lib4bin_script, script_len);
            (void)w;
            close(pd[1]);

            int status;
            waitpid(pid, &status, 0);
            free(lib4bin_script);
            exit(WEXITSTATUS(status));
        }
#endif

    }

    // ── sharun itself invoked ───────────────────────────────────
    if (strcmp(bin_name, SHARUN_NAME) == 0) {
        if (exec_args.len == 0) {
            fprintf(stderr, "Specify the executable from: '%s'\n", bin_dir);
            DIR *d = opendir(bin_dir);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) != NULL) {
                    if (de->d_type == DT_REG || de->d_type == DT_LNK) {
                        char fpath[PATH_MAX];
                        snprintf(fpath, sizeof(fpath), "%s/%s", bin_dir, de->d_name);
                        if (sharun_is_exe(fpath))
                            fprintf(stdout, "%s\n", de->d_name);
                    }
                }
                closedir(d);
            }
            exit(1);
        }

        bin_name = strdup(exec_args.data[0]);
        char bin_path[PATH_MAX];
        snprintf(bin_path, sizeof(bin_path), "%s/%s", bin_dir, bin_name);

        char bin_full[PATH_MAX];
        if (realpath(bin_path, bin_full)) {
            const char *bin_full_name = sharun_basename(bin_full);
            char test_shared_bin[PATH_MAX];
            snprintf(test_shared_bin, sizeof(test_shared_bin), "%s/%s", shared_bin, bin_full_name);

            struct stat st_bt;
            bool bin_is_sym = (lstat(bin_path, &st_bt) == 0 && S_ISLNK(st_bt.st_mode));
            if (bin_is_sym && sharun_is_file(test_shared_bin))
                bin_name = strdup(bin_full_name);

            if (sharun_is_exe(bin_full) &&
                (sharun_is_hardlink(sharun_path, bin_full) ||
                 !sharun_is_file(test_shared_bin) ||
                 strcmp(bin_full, sharun_path) != 0))
            {
                if (exec_args.len > 0) {
                    free(exec_args.data[0]);
                    memmove(&exec_args.data[0], &exec_args.data[1],
                            (exec_args.len - 1) * sizeof(char *));
                    exec_args.len--;
                }

                sharun_add_to_env("PATH", bin_dir);
                if (sharun_is_script(bin_path)) {
                    sharun_exec_script(bin_path, &exec_args);
                } else {
                    const char **av = malloc((exec_args.len + 2) * sizeof(const char *));
                    if (!av) { fprintf(stderr, "Out of memory\n"); exit(1); }
                    size_t avi = 0;
                    av[avi++] = bin_full;
                    for (size_t i = 0; i < exec_args.len; ++i)
                        av[avi++] = exec_args.data[i];
                    av[avi++] = NULL;

                    execve(bin_full, (char *const *)av, environ);
                    fprintf(stderr, "Error executing file %s: %s\n",
                            bin_full, strerror(errno));
                    exit(1);
                }
            }
        }
    }

    // ── General execution path ──────────────────────────────────
    char bin[PATH_MAX];
    snprintf(bin, sizeof(bin), "%s/%s", bin_dir, bin_name);
    if (!sharun_is_file(bin) || sharun_is_hardlink(sharun_path, bin))
        snprintf(bin, sizeof(bin), "%s/%s", shared_bin, bin_name);
    const char *bin_str = bin;

    // ELF32 check
#if SHARUN_ELF32
    bool is_elf32_bin = is_elf32(bin_str);
#else
    bool is_elf32_bin = false;
#endif

    // PyInstaller check
#if SHARUN_PYINSTALLER
    bytearr_t elf_bytes = get_elf(bin_str, is_elf32_bin);
#else
    bytearr_t elf_bytes = bytearr_init;
#endif

    // Select library path based on ELF class
    const char *library_path_base = is_elf32_bin ? shared_lib32 : shared_lib;

    // ── Dotenv ──────────────────────────────────────────────────
    strarr_t unset_envs = strarr_init;
#if SHARUN_SETENV
    unset_envs = sharun_read_dotenv(sharun_dir);
#endif

    // ── LD_PRELOAD / QT_PLUGIN_PATH sanitization ────────────────
    {
        char *v = sharun_get_env("SHARUN_ALLOW_LD_PRELOAD");
        if (!v || strcmp(v, "1") != 0)
            unsetenv("LD_PRELOAD");
        free(v);
        unsetenv("SHARUN_ALLOW_LD_PRELOAD");
    }
    {
        char *v = sharun_get_env("SHARUN_ALLOW_QT_PLUGIN_PATH");
        if (!v || strcmp(v, "1") != 0)
            unsetenv("QT_PLUGIN_PATH");
        free(v);
        unsetenv("SHARUN_ALLOW_QT_PLUGIN_PATH");
    }

    // ── Find interpreter ────────────────────────────────────────
    bool needs_interp = false;
    {
        int fd = open(bin_str, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            unsigned char ehdr[64];
            memset(ehdr, 0, sizeof(ehdr));
            if (read(fd, ehdr, 64) == 64) {
                bool be   = (ehdr[5] == 2);
                bool is64 = (ehdr[4] == 2);
                uint64_t e_phoff;
                uint16_t e_phentsize, e_phnum;
                if (is64) {
                    e_phoff     = read_u64(ehdr + 32, be);
                    e_phentsize = read_u16(ehdr + 54, be);
                    e_phnum     = read_u16(ehdr + 56, be);
                } else {
                    e_phoff     = read_u32(ehdr + 28, be);
                    e_phentsize = read_u16(ehdr + 42, be);
                    e_phnum     = read_u16(ehdr + 44, be);
                }
                size_t phdr_sz = (size_t)e_phnum * e_phentsize;
                unsigned char *phdr = malloc(phdr_sz);
                if (phdr) {
                    if (pread(fd, phdr, phdr_sz, (off_t)e_phoff) == (ssize_t)phdr_sz) {
                        for (uint16_t i = 0; i < e_phnum; ++i) {
                            const unsigned char *ph = phdr + i * e_phentsize;
                            if (read_u32(ph, be) == PT_INTERP) {
                                needs_interp = true;
                                break;
                            }
                        }
                    }
                    free(phdr);
                }
            }
            close(fd);
        }
    }

    char interpreter[PATH_MAX] = "";
    if (needs_interp)
        sharun_get_interpreter(library_path_base, interpreter, sizeof(interpreter));

    // ── Working directory ───────────────────────────────────────
    {
        char *working_dir = sharun_get_env("SHARUN_WORKING_DIR");
        if (working_dir) {
            if (chdir(working_dir) != 0) {
                fprintf(stderr, "Failed to change working directory: %s\n",
                        working_dir);
                exit(1);
            }
            unsetenv("SHARUN_WORKING_DIR");
            free(working_dir);
        }
    }

    // ── Gen / read lib.path ────────────────────────────────────
    char lib_path_file[PATH_MAX];
    snprintf(lib_path_file, sizeof(lib_path_file), "%s/lib.path", library_path_base);
    if (!sharun_is_file(lib_path_file) && sharun_is_writable(library_path_base))
        sharun_gen_lib_path(library_path_base, lib_path_file);

    sharun_add_to_env("PATH", bin_dir);

    // ── Read lib.path data ──────────────────────────────────────
    char *lib_path_data = sharun_read_file(lib_path_file);

    // ── Environment setup (setenv feature) ──────────────────────
#if SHARUN_SETENV
    sharun_setup_environment(sharun_dir, bin_dir, library_path_base,
                             lib_path_data ? lib_path_data : "");
#endif

    // ── Build LD_LIBRARY_PATH ───────────────────────────────────
    char *library_path = strdup(library_path_base);

    if (lib_path_data && lib_path_data[0]) {
        char *lp_data = lib_path_data;
        // Trim trailing whitespace
        char *end = lp_data + strlen(lp_data);
        while (end > lp_data && strchr(" \t\n\r", end[-1])) end--;
        *end = '\0';

        // Replace \n with :, + with library_path_base
        size_t lp_base_len = strlen(library_path_base);
        size_t lpdata_len = strlen(lp_data);

        // Estimate max size: worst case all chars are +, each expands to lp_base_len
        size_t est = lpdata_len * (lp_base_len > 1 ? lp_base_len : 1) + 1;
        char *lp_new = malloc(est);
        if (!lp_new) { fprintf(stderr, "Out of memory\n"); exit(1); }
        size_t lpn = 0;

        for (const char *p = lp_data; *p; ++p) {
            if (*p == '\n') {
                lp_new[lpn++] = ':';
            } else if (*p == '+') {
                if (lpn + lp_base_len >= est) {
                    est *= 2;
                    char *tmp = realloc(lp_new, est);
                    if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
                    lp_new = tmp;
                }
                memcpy(lp_new + lpn, library_path_base, lp_base_len);
                lpn += lp_base_len;
            } else {
                lp_new[lpn++] = *p;
            }
        }
        lp_new[lpn] = '\0';
        free(library_path);
        library_path = lp_new;
    }

    {
        char *ld_lib = sharun_get_env("LD_LIBRARY_PATH");
        if (ld_lib) {
            size_t cur = strlen(library_path);
            size_t add = strlen(ld_lib);
            char *tmp = realloc(library_path, cur + 1 + add + 1);
            if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
            library_path = tmp;
            library_path[cur] = ':';
            memcpy(library_path + cur + 1, ld_lib, add + 1);
            free(ld_lib);
        }
    }

    {
        char *extra = sharun_get_env("SHARUN_EXTRA_LIBRARY_PATH");
        if (extra) {
            size_t e_len = strlen(extra);
            size_t cur = strlen(library_path);
            char *tmp = realloc(library_path, e_len + 1 + cur + 1);
            if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
            library_path = tmp;
            memmove(library_path + e_len + 1, library_path, cur + 1);
            memcpy(library_path, extra, e_len);
            library_path[e_len] = ':';
            unsetenv("SHARUN_EXTRA_LIBRARY_PATH");
            free(extra);
        }
    }

    {
        size_t cur = strlen(library_path);
        const char *suffixes[] = {":/usr/lib:/lib", NULL};
        if (is_elf32_bin) {
            static const char *suffix_32 = ":/usr/lib32:/lib32";
            suffixes[0] = suffix_32;
        } else {
            static const char *suffix_64 = ":/usr/lib64:/lib64";
            suffixes[0] = suffix_64;
        }
        size_t s_len = strlen(suffixes[0]);
        char *tmp = realloc(library_path, cur + s_len + 1);
        if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
        library_path = tmp;
        memcpy(library_path + cur, suffixes[0], s_len + 1);
    }

    // Arch-specific library paths
    if (is_elf32_bin) {
#if defined(__x86_64__)
        size_t cur = strlen(library_path);
        const char *more = ":/usr/lib/i386-linux-gnu";
        size_t mlen = strlen(more);
        char *tmp = realloc(library_path, cur + mlen + 1);
        if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
        library_path = tmp;
        memcpy(library_path + cur, more, mlen + 1);
#elif defined(__powerpc64__)
        size_t cur = strlen(library_path);
        const char *more = ":/usr/lib/powerpc-linux-gnu";
        size_t mlen = strlen(more);
        char *tmp = realloc(library_path, cur + mlen + 1);
        if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
        library_path = tmp;
        memcpy(library_path + cur, more, mlen + 1);
#elif defined(__riscv) && __riscv_xlen == 64
        size_t cur = strlen(library_path);
        const char *more = ":/usr/lib/riscv32-linux-gnu";
        size_t mlen = strlen(more);
        char *tmp = realloc(library_path, cur + mlen + 1);
        if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
        library_path = tmp;
        memcpy(library_path + cur, more, mlen + 1);
#endif
    } else {
#if defined(__x86_64__)
        size_t cur = strlen(library_path);
        const char *more = ":/usr/lib/x86_64-linux-gnu";
        size_t mlen = strlen(more);
        char *tmp = realloc(library_path, cur + mlen + 1);
        if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
        library_path = tmp;
        memcpy(library_path + cur, more, mlen + 1);
#elif defined(__aarch64__)
        size_t cur = strlen(library_path);
        const char *more = ":/usr/lib/aarch64-linux-gnu";
        size_t mlen = strlen(more);
        char *tmp = realloc(library_path, cur + mlen + 1);
        if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
        library_path = tmp;
        memcpy(library_path + cur, more, mlen + 1);
#elif defined(__powerpc64__) && defined(__LITTLE_ENDIAN__)
        size_t cur = strlen(library_path);
        const char *more = ":/usr/lib/powerpc64le-linux-gnu";
        size_t mlen = strlen(more);
        char *tmp = realloc(library_path, cur + mlen + 1);
        if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
        library_path = tmp;
        memcpy(library_path + cur, more, mlen + 1);
#elif defined(__powerpc64__)
        size_t cur = strlen(library_path);
        const char *more = ":/usr/lib/powerpc64-linux-gnu";
        size_t mlen = strlen(more);
        char *tmp = realloc(library_path, cur + mlen + 1);
        if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
        library_path = tmp;
        memcpy(library_path + cur, more, mlen + 1);
#elif defined(__PPC__)
        size_t cur = strlen(library_path);
        const char *more = ":/usr/lib/powerpc-linux-gnu";
        size_t mlen = strlen(more);
        char *tmp = realloc(library_path, cur + mlen + 1);
        if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
        library_path = tmp;
        memcpy(library_path + cur, more, mlen + 1);
#elif defined(__riscv)
        size_t cur = strlen(library_path);
        const char *more = ":/usr/lib/riscv64-linux-gnu";
        size_t mlen = strlen(more);
        char *tmp = realloc(library_path, cur + mlen + 1);
        if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
        library_path = tmp;
        memcpy(library_path + cur, more, mlen + 1);
#elif defined(__loongarch64)
        size_t cur = strlen(library_path);
        const char *more = ":/usr/lib/loongarch64-linux-gnu";
        size_t mlen = strlen(more);
        char *tmp = realloc(library_path, cur + mlen + 1);
        if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
        library_path = tmp;
        memcpy(library_path + cur, more, mlen + 1);
#elif defined(__s390x__)
        size_t cur = strlen(library_path);
        const char *more = ":/usr/lib/s390x-linux-gnu";
        size_t mlen = strlen(more);
        char *tmp = realloc(library_path, cur + mlen + 1);
        if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
        library_path = tmp;
        memcpy(library_path + cur, more, mlen + 1);
#endif
    }

    {
        size_t cur = strlen(library_path);
        const char *extra = ":/run/opengl-driver/lib:/run/current-system/sw/lib";
        size_t mlen = strlen(extra);
        char *tmp = realloc(library_path, cur + mlen + 1);
        if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
        library_path = tmp;
        memcpy(library_path + cur, extra, mlen + 1);
    }

    {
        char *fallback = sharun_get_env("SHARUN_FALLBACK_LIBRARY_PATH");
        if (fallback) {
            size_t cur = strlen(library_path);
            size_t flen = strlen(fallback);
            char *tmp = realloc(library_path, cur + 1 + flen + 1);
            if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
            library_path = tmp;
            library_path[cur] = ':';
            memcpy(library_path + cur + 1, fallback, flen + 1);
            unsetenv("SHARUN_FALLBACK_LIBRARY_PATH");
            free(fallback);
        }
    }

    // ── Unset collected vars ────────────────────────────────────
    for (size_t i = 0; i < unset_envs.len; ++i)
        unsetenv(unset_envs.data[i]);

    // ── Debug print ─────────────────────────────────────────────
    {
        char *printenv = sharun_get_env("SHARUN_PRINTENV");
        if (printenv && strcmp(printenv, "1") == 0) {
            unsetenv("SHARUN_PRINTENV");
            for (char **e = environ; *e; ++e)
                fprintf(stderr, "%s\n", *e);
        }
        free(printenv);
    }

    // ── PyInstaller / ELF32 check ───────────────────────────────
#if SHARUN_PYINSTALLER
    bool is_pyinstaller_elf = is_elf_section(&elf_bytes, "pydata");
    char pyinstaller_dir_path[PATH_MAX];
    snprintf(pyinstaller_dir_path, sizeof(pyinstaller_dir_path), "%s/_internal", shared_bin);
    bool is_pyinstaller_dir = sharun_is_dir(pyinstaller_dir_path);
#else
    bool is_pyinstaller_elf = false;
    bool is_pyinstaller_dir = false;
#endif

    // ── .preload -> LD_PRELOAD ──────────────────────────────────
    {
        char preload_path[PATH_MAX];
        snprintf(preload_path, sizeof(preload_path), "%s/.preload", sharun_dir);
        if (sharun_is_file(preload_path)) {
            FILE *pf = fopen(preload_path, "rb");
            if (!pf) {
                fprintf(stderr, "Failed to read .preload file: %s\n", preload_path);
                exit(1);
            }
            size_t joined_cap = 256;
            size_t joined_len = 0;
            char *joined = malloc(joined_cap);
            if (!joined) { fprintf(stderr, "Out of memory\n"); exit(1); }
            joined[0] = '\0';

            char line[4096];
            while (fgets(line, sizeof(line), pf)) {
                size_t llen = strlen(line);
                while (llen > 0 && (line[llen-1] == '\n' || line[llen-1] == '\r'))
                    line[--llen] = '\0';
                char *trimmed = line;
                while (*trimmed == ' ' || *trimmed == '\t' || *trimmed == '\r')
                    trimmed++;
                if (!*trimmed) continue;

                size_t tlen = strlen(trimmed);
                if (joined_len) {
                    if (joined_len + 1 + tlen + 1 >= joined_cap) {
                        while (joined_len + 1 + tlen + 1 >= joined_cap) joined_cap *= 2;
                        joined = realloc(joined, joined_cap);
                        if (!joined) { fprintf(stderr, "Out of memory\n"); exit(1); }
                    }
                    joined[joined_len++] = ':';
                } else {
                    if (tlen + 1 >= joined_cap) {
                        joined_cap = tlen + 1;
                        joined = realloc(joined, joined_cap);
                        if (!joined) { fprintf(stderr, "Out of memory\n"); exit(1); }
                    }
                }
                memcpy(joined + joined_len, trimmed, tlen + 1);
                joined_len += tlen;
            }
            fclose(pf);

            if (joined_len > 0)
                setenv("LD_PRELOAD", joined, 1);
            free(joined);
        }
    }


    // ── Determine argv0 for the target binary ─────────────────
    char *argv0_for_target;
    if (is_pyinstaller_elf || is_elf32_bin)
        argv0_for_target = strdup(bin_str);
    else
        argv0_for_target = strdup(arg0_path);

    // ── Execute ─────────────────────────────────────────────────
    if (is_pyinstaller_elf || is_elf32_bin) {
        if (!is_pyinstaller_dir && is_pyinstaller_elf) {
            // PyInstaller, no .dir > patch interpreter in-place, then exec directly
            if (set_interp(&elf_bytes, bin_str, interpreter)) {
                const char **av = malloc((exec_args.len + 2) * sizeof(const char *));
                if (!av) { fprintf(stderr, "Out of memory\n"); exit(1); }
                size_t avi = 0;
                av[avi++] = bin_str;
                for (size_t i = 0; i < exec_args.len; ++i)
                    av[avi++] = exec_args.data[i];
                av[avi++] = NULL;
                execve(bin_str, (char *const *)av, environ);
            } else {
                fprintf(stderr, "Failed to set ELF interpreter: %s\n", bin_str);
                exit(1);
            }
        } else {
            // PyInstaller with .dir, or ELF32 -> use interpreter
            bytearr_free(&elf_bytes);
            sharun_userland_execve(bin_str, interpreter, &exec_args,
                                   library_path, argv0_for_target);
        }
        fprintf(stderr, "Failed to exec: %s: %s\n",
                bin_str, strerror(errno));
        exit(1);
    }

    // ── Non-pyinstaller, non-elf32: userland execve with interpreter ──
    bytearr_free(&elf_bytes);

    sharun_userland_execve(bin_str, interpreter, &exec_args,
                           library_path, argv0_for_target);
}
