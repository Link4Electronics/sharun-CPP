#ifndef SHARUN_H
#define SHARUN_H

#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

// ── Feature compile-time checks ─────────────────────────────────
#ifndef SHARUN_NAME
#  define SHARUN_NAME "sharun"
#endif
#ifndef SHARUN_VERSION
#  define SHARUN_VERSION "0.8.1"
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
// ── String array (dynamic) ──────────────────────────────────────
typedef struct {
    char **data;
    size_t len;
    size_t cap;
} strarr_t;

#define strarr_init {NULL,0,0}

int  strarr_push(strarr_t *a, const char *s);
void strarr_free(strarr_t *a);

// ── Byte array (dynamic) ────────────────────────────────────────
typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} bytearr_t;

#define bytearr_init {NULL,0,0}

int  bytearr_push(bytearr_t *a, const unsigned char *d, size_t n);
void bytearr_free(bytearr_t *a);

// ── Endian-aware ELF field readers ──────────────────────────────
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define SHARUN_HOST_BE true
#else
#  define SHARUN_HOST_BE false
#endif

static inline uint16_t read_u16(const uint8_t *p, bool elf_be) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    if (elf_be != SHARUN_HOST_BE)
        v = __builtin_bswap16(v);
    return v;
}
static inline uint32_t read_u32(const uint8_t *p, bool elf_be) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    if (elf_be != SHARUN_HOST_BE)
        v = __builtin_bswap32(v);
    return v;
}
static inline uint64_t read_u64(const uint8_t *p, bool elf_be) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    if (elf_be != SHARUN_HOST_BE)
        v = __builtin_bswap64(v);
    return v;
}

// ── LoadedElf struct (C version) ────────────────────────────────
typedef struct {
    uint64_t base;
    uint64_t entry;
    uint64_t phdr_vaddr;
    uint16_t phnum;
    uint16_t phent;
    uint64_t tls_vaddr;
    uint64_t tls_filesz;
    uint64_t tls_memsz;
    uint64_t tls_align;
} LoadedElf;

// ── AT_* constants (in case headers don't define them) ──────────
#ifndef AT_NULL
#  define AT_NULL      0
#  define AT_IGNORE    1
#  define AT_EXECFD    2
#  define AT_PHDR      3
#  define AT_PHENT     4
#  define AT_PHNUM     5
#  define AT_PAGESZ    6
#  define AT_BASE      7
#  define AT_FLAGS     8
#  define AT_ENTRY     9
#  define AT_NOTELF   10
#  define AT_UID      11
#  define AT_EUID     12
#  define AT_GID      13
#  define AT_EGID     14
#  define AT_PLATFORM 15
#  define AT_HWCAP    16
#  define AT_CLKTCK   17
#  define AT_SECURE   23
#  define AT_RANDOM   25
#  define AT_EXECFN   31
#endif
#ifndef ARCH_SET_FS
#  define ARCH_SET_FS 0x1002
#endif
#ifndef ARCH_GET_FS
#  define ARCH_GET_FS 0x1003
#endif
#ifndef SYS_arch_prctl
#  if defined(__x86_64__)
#    define SYS_arch_prctl 158
#  elif defined(__i386__)
#    define SYS_arch_prctl 374
#  endif
#endif

// ── Decompression function types ────────────────────────────────
// These expand to the actual decompress functions using embedded data.
// They are defined in main.c where the embedded data headers are included.

// ── Utility function declarations ───────────────────────────────

// Path utilities
char *sharun_realpath(const char *path);    // malloc'd result
const char *sharun_basename(const char *path); // returns pointer into path
void sharun_dirname(const char *path, char *out, size_t outsz);

// File system checks
bool sharun_is_hardlink(const char *p1, const char *p2);
bool sharun_is_same_rootdir(const char *rootdir,
                            const char *p1, const char *p2);
bool sharun_is_writable(const char *path);
bool sharun_is_dir(const char *path);
bool sharun_is_file(const char *path);
bool sharun_is_exe(const char *path);
bool sharun_is_script(const char *path);

// PATH lookup
bool sharun_which(const char *executable, char *out, size_t outsz);

// Script execution
void sharun_exec_script(const char *path, strarr_t *exec_args);

// Environment helpers
char *sharun_get_env(const char *key); // returns malloc'd value or NULL
void  sharun_add_to_env(const char *key, const char *val);
void  sharun_print_env(void);

// File write (binary)
bool sharun_write_file(const char *path, const unsigned char *data, size_t len);

// File read helpers
char *sharun_read_file(const char *path); // malloc'd content

// ELF helpers
bool sharun_is_elf32(const char *path);
bool sharun_needs_interp(const char *path);

// Library path generation
void sharun_gen_lib_path(const char *library_path, const char *lib_path_file);

// Interpreter lookup
void sharun_get_interpreter(const char *library_path, char *out, size_t outsz);

// ── Environment setup (setenv feature) ──────────────────────────
#if SHARUN_SETENV
strarr_t sharun_read_dotenv(const char *dotenv_dir);
void sharun_setup_environment(const char *sharun_dir,
                               const char *bin_dir,
                               const char *library_path,
                               const char *lib_path_data);
#endif

// ── Userland execve ─────────────────────────────────────────────
LoadedElf sharun_map_elf(const char *path, uint64_t fixed_base);
_Noreturn void sharun_userland_execve(const char *bin_path,
                                       const char *interp_path,
                                       strarr_t *exec_args,
                                       const char *library_path,
                                       const char *argv0);

// ── Decompress embedded data ────────────────────────────────────
// These return malloc'd buffers; caller must free.
unsigned char *sharun_decompress_lib4bin(size_t *out_len);
// ── Embedded data references ────────────────────────────────────
extern const unsigned char lib4bin_data[];
extern const size_t lib4bin_data_size;
extern const size_t lib4bin_data_uncompressed_size;

#endif // SHARUN_H
