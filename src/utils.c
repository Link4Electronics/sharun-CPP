#define _GNU_SOURCE

#include "sharun.h"

#ifndef PATH_MAX
#  define PATH_MAX 4096
#endif

extern char **environ;

char *sharun_realpath(const char *path) {
    if (!path) return NULL;
    char buf[PATH_MAX];
    if (!realpath(path, buf))
        return NULL;
    return strdup(buf);
}

const char *sharun_basename(const char *path) {
    if (!path || !*path) return path;
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

void sharun_dirname(const char *path, char *out, size_t outsz) {
    if (!path || !*path || !out || outsz == 0) {
        if (out && outsz > 0) *out = '\0';
        return;
    }
    const char *slash = strrchr(path, '/');
    if (!slash) {
        out[0] = (path[0] == '.' || path[0] == '~') ? '\0' : '.';
        if (out[0] == '.') out[1] = '\0';
        return;
    }
    size_t len = (size_t)(slash - path);
    if (len == 0 && path[1] != '\0') {
        if (outsz > 1) { out[0] = '/'; out[1] = '\0'; }
        else if (outsz > 0) *out = '\0';
        return;
    }
    if (len >= outsz) len = outsz - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

bool sharun_is_hardlink(const char *p1, const char *p2) {
    struct stat s1, s2;
    return stat(p1, &s1) == 0 && stat(p2, &s2) == 0 && s1.st_ino == s2.st_ino;
}

bool sharun_is_same_rootdir(const char *rootdir,
                            const char *p1, const char *p2) {
    char *r = sharun_realpath(rootdir);
    if (!r) return false;
    char *a = sharun_realpath(p1);
    if (!a) { free(r); return false; }
    char *b = sharun_realpath(p2);
    if (!b) { free(r); free(a); return false; }
    size_t rlen = strlen(r);
    bool ok = strncmp(a, r, rlen) == 0 && strncmp(b, r, rlen) == 0;
    free(r); free(a); free(b);
    return ok;
}

bool sharun_is_writable(const char *path) {
    return access(path, W_OK) == 0;
}

bool sharun_is_dir(const char *path) {
    struct stat s;
    return stat(path, &s) == 0 && S_ISDIR(s.st_mode);
}

bool sharun_is_file(const char *path) {
    struct stat s;
    return stat(path, &s) == 0 && S_ISREG(s.st_mode);
}

bool sharun_is_exe(const char *path) {
    struct stat s;
    return stat(path, &s) == 0 && S_ISREG(s.st_mode) && access(path, X_OK) == 0;
}

bool sharun_is_script(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char buf[2];
    size_t n = fread(buf, 1, 2, f);
    fclose(f);
    return n == 2 && buf[0] == '#' && buf[1] == '!';
}

bool sharun_which(const char *executable, char *out, size_t outsz) {
    if (!executable || !*executable || !out || outsz == 0) return false;
    const char *path_env = getenv("PATH");
    if (!path_env || !*path_env) return false;

    char *path_dup = strdup(path_env);
    if (!path_dup) return false;

    bool found = false;
    char *saveptr;
    char *dir = strtok_r(path_dup, ":", &saveptr);
    while (dir) {
        size_t dirlen = strlen(dir);
        size_t exelen = strlen(executable);
        char *full = malloc(dirlen + 1 + exelen + 1);
        if (!full) break;
        memcpy(full, dir, dirlen);
        full[dirlen] = '/';
        memcpy(full + dirlen + 1, executable, exelen + 1);

        if (sharun_is_exe(full)) {
            size_t flen = strlen(full);
            if (flen < outsz)
                memcpy(out, full, flen + 1);
            else {
                memcpy(out, full, outsz - 1);
                out[outsz - 1] = '\0';
            }
            free(full);
            found = true;
            break;
        }
        free(full);
        dir = strtok_r(NULL, ":", &saveptr);
    }

    free(path_dup);
    return found;
}

char *sharun_get_env(const char *key) {
    if (!key) return NULL;
    const char *val = getenv(key);
    return val ? strdup(val) : NULL;
}

void sharun_add_to_env(const char *key, const char *val) {
    if (!key || !val) return;
    const char *existing = getenv(key);
    if (!existing) {
        setenv(key, val, 1);
        return;
    }

    size_t vlen = strlen(val);
    size_t elen = strlen(existing);

    if (elen == vlen && memcmp(existing, val, vlen) == 0)
        return;
    if (elen > vlen && memcmp(existing, val, vlen) == 0 && existing[vlen] == ':')
        return;
    if (elen > vlen && existing[elen - vlen - 1] == ':'
        && memcmp(existing + elen - vlen, val, vlen) == 0)
        return;

    char *pat = malloc(vlen + 3);
    if (!pat) return;
    pat[0] = ':';
    memcpy(pat + 1, val, vlen);
    pat[vlen + 1] = ':';
    pat[vlen + 2] = '\0';
    bool found = (strstr(existing, pat) != NULL);
    free(pat);
    if (found) return;

    char *newv = malloc(vlen + 1 + elen + 1);
    if (!newv) return;
    memcpy(newv, val, vlen);
    newv[vlen] = ':';
    memcpy(newv + vlen + 1, existing, elen);
    newv[vlen + 1 + elen] = '\0';
    setenv(key, newv, 1);
    free(newv);
}

void sharun_print_env(void) {
    for (char **e = environ; *e; e++)
        puts(*e);
}

static char *read_first_line(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    char *buf = NULL;
    size_t cap = 0, len = 0;
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n') {
        if (len + 1 >= cap) {
            cap = cap ? cap * 2 : 128;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); fclose(f); return NULL; }
            buf = tmp;
        }
        buf[len++] = (char)c;
    }

    fclose(f);
    if (len == 0) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

void sharun_exec_script(const char *path, strarr_t *exec_args) {
    if (!path || !exec_args) {
        fprintf(stderr, "Error: Invalid arguments to exec_script\n");
        exit(1);
    }

    char *first_line = read_first_line(path);
    if (!first_line || first_line[0] != '#' || first_line[1] != '!') {
        fprintf(stderr, "Error: Script does not have a valid shebang!\n");
        exit(1);
    }

    char *shebang = first_line + 2;
    while (*shebang == ' ' || *shebang == '\t') shebang++;

    if (!*shebang) {
        fprintf(stderr, "Error: Invalid shebang: no interpreter specified!\n");
        exit(1);
    }

    char *shebang_dup = strdup(shebang);
    if (!shebang_dup) {
        fprintf(stderr, "Error: Out of memory\n");
        exit(1);
    }

    size_t num_parts = 0;
    {
        const char *s = shebang_dup;
        while (*s) {
            while (*s == ' ') s++;
            if (!*s) break;
            num_parts++;
            while (*s && *s != ' ') s++;
        }
    }

    if (num_parts == 0) {
        fprintf(stderr, "Error: Invalid shebang: no interpreter specified!\n");
        free(shebang_dup);
        free(first_line);
        exit(1);
    }

    char **parts = malloc(num_parts * sizeof(char *));
    if (!parts) {
        fprintf(stderr, "Error: Out of memory\n");
        free(shebang_dup);
        free(first_line);
        exit(1);
    }

    {
        char *s = shebang_dup;
        size_t pi = 0;
        while (*s) {
            while (*s == ' ') s++;
            if (!*s) break;
            parts[pi++] = s;
            while (*s && *s != ' ') s++;
            if (*s) *s++ = '\0';
        }
    }

    size_t argv_cap = num_parts + exec_args->len + 2;
    const char **argv = malloc(argv_cap * sizeof(char *));
    if (!argv) {
        fprintf(stderr, "Error: Out of memory\n");
        free(parts);
        free(shebang_dup);
        free(first_line);
        exit(1);
    }

    size_t argc = 0;
    const char *interp_path = parts[0];
    size_t interp_len = strlen(interp_path);

    if (interp_len >= 4 && memcmp(interp_path + interp_len - 4, "/env", 4) == 0) {
        if (num_parts < 2) {
            fprintf(stderr, "Error: No interpreter specified after env!\n");
            free(argv); free(parts); free(shebang_dup); free(first_line);
            exit(1);
        }
        char resolved[PATH_MAX];
        if (!sharun_which(parts[1], resolved, sizeof(resolved))) {
            fprintf(stderr, "Error: Interpreter '%s' not found in PATH\n", parts[1]);
            free(argv); free(parts); free(shebang_dup); free(first_line);
            exit(1);
        }
        argv[argc] = strdup(resolved);
        if (!argv[argc]) {
            fprintf(stderr, "Error: Out of memory\n");
            free(argv); free(parts); free(shebang_dup); free(first_line);
            exit(1);
        }
        argc++;
        for (size_t i = 2; i < num_parts; i++)
            argv[argc++] = parts[i];
    } else {
        const char *iname = sharun_basename(interp_path);
        char resolved[PATH_MAX];
        if (sharun_which(iname, resolved, sizeof(resolved))) {
            argv[argc] = strdup(resolved);
            if (!argv[argc]) {
                fprintf(stderr, "Error: Out of memory\n");
                free(argv); free(parts); free(shebang_dup); free(first_line);
                exit(1);
            }
            argc++;
        } else {
            struct stat st;
            if (stat(interp_path, &st) != 0) {
                fprintf(stderr, "Error: Interpreter '%s' not found\n", interp_path);
                free(argv); free(parts); free(shebang_dup); free(first_line);
                exit(1);
            }
            argv[argc++] = interp_path;
        }
        for (size_t i = 1; i < num_parts; i++)
            argv[argc++] = parts[i];
    }

    argv[argc++] = path;
    for (size_t i = 0; i < exec_args->len; i++)
        argv[argc++] = exec_args->data[i];
    argv[argc] = NULL;

    execve(argv[0], (char *const *)argv, environ);
    fprintf(stderr, "Error executing script: %s\n", strerror(errno));
    free(argv); free(parts); free(shebang_dup); free(first_line);
    exit(1);
}

// ── String array ───────────────────────────────────────────────

int strarr_push(strarr_t *a, const char *s) {
    if (!a || !s) return -1;
    if (a->len + 1 >= a->cap) {
        size_t new_cap = a->cap ? a->cap * 2 : 8;
        char **tmp = realloc(a->data, new_cap * sizeof(char *));
        if (!tmp) return -1;
        a->data = tmp;
        a->cap = new_cap;
    }
    a->data[a->len] = strdup(s);
    if (!a->data[a->len]) return -1;
    a->len++;
    return 0;
}

void strarr_free(strarr_t *a) {
    if (!a) return;
    for (size_t i = 0; i < a->len; i++)
        free(a->data[i]);
    free(a->data);
    a->data = NULL;
    a->len = a->cap = 0;
}

// ── Byte array ─────────────────────────────────────────────────

int bytearr_push(bytearr_t *a, const unsigned char *d, size_t n) {
    if (!a || !d || n == 0) return -1;
    if (a->len + n > a->cap) {
        size_t new_cap = a->cap ? a->cap : 256;
        while (new_cap < a->len + n)
            new_cap *= 2;
        unsigned char *tmp = realloc(a->data, new_cap);
        if (!tmp) return -1;
        a->data = tmp;
        a->cap = new_cap;
    }
    memcpy(a->data + a->len, d, n);
    a->len += n;
    return 0;
}

void bytearr_free(bytearr_t *a) {
    if (!a) return;
    free(a->data);
    a->data = NULL;
    a->len = a->cap = 0;
}

bool sharun_write_file(const char *path, const unsigned char *data, size_t len) {
    if (!path || !data) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t written = fwrite(data, 1, len, f);
    int saved = ferror(f);
    fclose(f);
    return written == len && saved == 0;
}

char *sharun_read_file(const char *path) {
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}
