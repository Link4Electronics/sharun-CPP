#define _GNU_SOURCE
#include "sharun.h"

// ── Stub when SETENV is off ────────────────────────────────────
#if !SHARUN_SETENV

strarr_t sharun_read_dotenv(const char *dotenv_dir) {
    (void)dotenv_dir;
    strarr_t out = strarr_init;
    return out;
}

void sharun_setup_environment(const char *a, const char *b,
                               const char *c, const char *d) {
    (void)a; (void)b; (void)c; (void)d;
}

#else

// ── Forward declarations of static helpers ─────────────────────
static void add_to_xdg_data_env(const char *xdg_data_dirs,
                                 const char *env, const char *path);
static void process_lib_path_dirs(const char *lib_path_data,
                                   const char *library_path);
static void process_share_dir(const char *sharun_dir);
static void process_etc_dir(const char *sharun_dir);
static void process_ssl_certs(void);

// ── add_to_xdg_data_env ────────────────────────────────────────
static void add_to_xdg_data_env(const char *xdg_data_dirs,
                                 const char *env, const char *path) {
    if (!xdg_data_dirs || !*xdg_data_dirs) return;

    char *copy = strdup(xdg_data_dirs);
    if (!copy) return;

    // Walk from right to left (reverse order)
    // Use strtok_r and collect all, then iterate in reverse
    size_t count = 0;
    for (const char *p = copy; *p; p++)
        if (*p == ':') count++;
    count++; // last entry

    char **dirs = calloc(count, sizeof(char *));
    if (!dirs) { free(copy); return; }

    size_t idx = 0;
    char *saveptr;
    for (char *tok = strtok_r(copy, ":", &saveptr); tok; tok = strtok_r(NULL, ":", &saveptr))
        dirs[idx++] = tok;

    for (size_t i = idx; i > 0; i--) {
        size_t dlen = strlen(dirs[i-1]);
        size_t plen = strlen(path);
        char *full = malloc(dlen + 1 + plen + 1);
        if (!full) continue;
        memcpy(full, dirs[i-1], dlen);
        full[dlen] = '/';
        memcpy(full + dlen + 1, path, plen + 1);
        if (sharun_is_dir(full))
            sharun_add_to_env(env, full);
        free(full);
    }

    free(dirs);
    free(copy);
}

// ── process_lib_path_dirs ──────────────────────────────────────
static void process_lib_path_dirs(const char *lib_path_data,
                                   const char *library_path) {
    if (!lib_path_data || !*lib_path_data) return;

    char *data = strdup(lib_path_data);
    if (!data) return;

    // First pass: collect unique first-level directory names
    size_t max_dirs = 64;
    char **dirs = calloc(max_dirs, sizeof(char *));
    size_t ndirs = 0;

    char *line;
    char *saveptr;
    for (line = strtok_r(data, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
        char *slash = strchr(line, '/');
        if (!slash) continue;
        char *rest = slash + 1;
        char *slash2 = strchr(rest, '/');
        size_t seg_len = slash2 ? (size_t)(slash2 - rest) : strlen(rest);

        // Check if already collected
        bool found = false;
        for (size_t i = 0; i < ndirs; i++) {
            if (strlen(dirs[i]) == seg_len && memcmp(dirs[i], rest, seg_len) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (ndirs >= max_dirs) {
                max_dirs *= 2;
                char **tmp = realloc(dirs, max_dirs * sizeof(char *));
                if (!tmp) { free(dirs); free(data); return; }
                dirs = tmp;
            }
            dirs[ndirs] = strndup(rest, seg_len);
            if (dirs[ndirs]) ndirs++;
        }
    }

    for (size_t i = 0; i < ndirs; i++) {
        size_t lp_len = strlen(library_path);
        size_t dlen = strlen(dirs[i]);
        char *dir_str = malloc(lp_len + 1 + dlen + 1);
        if (!dir_str) continue;
        memcpy(dir_str, library_path, lp_len);
        dir_str[lp_len] = '/';
        memcpy(dir_str + lp_len + 1, dirs[i], dlen + 1);

        if (strncmp(dirs[i], "python", 6) == 0) {
            // Check if parent is writable
            char parent[PATH_MAX];
            sharun_dirname(library_path, parent, sizeof(parent));
            if (!sharun_is_writable(parent))
                setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
        }

        if (strncmp(dirs[i], "perl", 4) == 0)
            sharun_add_to_env("PERLLIB", dir_str);

        if (strcmp(dirs[i], "gconv") == 0)
            sharun_add_to_env("GCONV_PATH", dir_str);

        if (strcmp(dirs[i], "gio") == 0) {
            char modules[PATH_MAX];
            snprintf(modules, sizeof(modules), "%s/modules", dir_str);
            if (sharun_is_dir(modules))
                setenv("GIO_MODULE_DIR", modules, 1);
        }

        if (strcmp(dirs[i], "dri") == 0) {
            setenv("LIBGL_DRIVERS_PATH", dir_str, 1);
            char *no_nvidia = sharun_get_env("SHARUN_NO_NVIDIA_EGL_PRIME");
            if (!(no_nvidia && strcmp(no_nvidia, "1") == 0) &&
                access("/sys/module/nvidia/version", F_OK) == 0) {
                sharun_add_to_env("LIBVA_DRIVERS_PATH", "/run/opengl-driver/lib/dri");
                sharun_add_to_env("LIBVA_DRIVERS_PATH", "/usr/lib/dri");
                sharun_add_to_env("LIBVA_DRIVERS_PATH", "/usr/lib64/dri");
#if defined(__x86_64__)
                sharun_add_to_env("LIBVA_DRIVERS_PATH", "/usr/lib/x86_64-linux-gnu/dri");
#elif defined(__aarch64__)
                sharun_add_to_env("LIBVA_DRIVERS_PATH", "/usr/lib/aarch64-linux-gnu/dri");
#endif
            }
            free(no_nvidia);
            sharun_add_to_env("LIBVA_DRIVERS_PATH", dir_str);
        }

        if (strcmp(dirs[i], "gbm") == 0) {
            sharun_add_to_env("GBM_BACKENDS_PATH", "/run/opengl-driver/lib/gbm");
            sharun_add_to_env("GBM_BACKENDS_PATH", "/usr/lib/gbm");
            sharun_add_to_env("GBM_BACKENDS_PATH", "/usr/lib64/gbm");
#if defined(__x86_64__)
            sharun_add_to_env("GBM_BACKENDS_PATH", "/usr/lib/x86_64-linux-gnu/gbm");
#elif defined(__aarch64__)
            sharun_add_to_env("GBM_BACKENDS_PATH", "/usr/lib/aarch64-linux-gnu/gbm");
#endif
            sharun_add_to_env("GBM_BACKENDS_PATH", dir_str);
        }

        if (strcmp(dirs[i], "libheif") == 0) {
            char plugins[PATH_MAX];
            snprintf(plugins, sizeof(plugins), "%s/plugins", dir_str);
            setenv("LIBHEIF_PLUGIN_PATH", sharun_is_dir(plugins) ? plugins : dir_str, 1);
        }

        if (strcmp(dirs[i], "xtables") == 0)
            setenv("XTABLES_LIBDIR", dir_str, 1);

        if (strncmp(dirs[i], "spa-", 4) == 0)
            setenv("SPA_PLUGIN_DIR", dir_str, 1);

        if (strncmp(dirs[i], "pipewire-", 9) == 0)
            setenv("PIPEWIRE_MODULE_DIR", dir_str, 1);

        if (strncmp(dirs[i], "gtk-", 4) == 0) {
            sharun_add_to_env("GTK_PATH", dir_str);
            char parent[PATH_MAX];
            sharun_dirname(library_path, parent, sizeof(parent));
            setenv("GTK_EXE_PREFIX", parent, 1);
            setenv("GTK_DATA_PREFIX", parent, 1);
            // Search for immodules.cache (one level deep)
            DIR *d = opendir(dir_str);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)) != NULL) {
                    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                    size_t dl = strlen(dir_str);
                    size_t nl = strlen(e->d_name);
                    char *sub = malloc(dl + 1 + nl + 1);
                    if (!sub) continue;
                    memcpy(sub, dir_str, dl);
                    sub[dl] = '/';
                    memcpy(sub + dl + 1, e->d_name, nl + 1);
                    if (sharun_is_dir(sub)) {
                        char cache[PATH_MAX];
                        snprintf(cache, sizeof(cache), "%s/immodules.cache", sub);
                        if (sharun_is_file(cache)) {
                            setenv("GTK_IM_MODULE_FILE", cache, 1);
                            free(sub);
                            break;
                        }
                    }
                    free(sub);
                }
                closedir(d);
            }
        }

        if (strcmp(dirs[i], "folks") == 0) {
            // Search for backends subdirectory (one level deep)
            DIR *d = opendir(dir_str);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)) != NULL) {
                    if (e->d_type != DT_DIR || strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                    size_t dl = strlen(dir_str);
                    size_t nl = strlen(e->d_name);
                    char *sub = malloc(dl + 1 + nl + 1 + 10);
                    if (!sub) continue;
                    snprintf(sub, dl + 1 + nl + 10, "%s/%s/backends", dir_str, e->d_name);
                    if (sharun_is_dir(sub)) {
                        setenv("FOLKS_BACKEND_PATH", sub, 1);
                        free(sub);
                        break;
                    }
                    free(sub);
                }
                closedir(d);
            }
        }

        if (strncmp(dirs[i], "qt", 2) == 0) {
            char parent[PATH_MAX], bin_dir[PATH_MAX], qt_conf[PATH_MAX], plugins[PATH_MAX];
            sharun_dirname(library_path, parent, sizeof(parent));
            snprintf(bin_dir, sizeof(bin_dir), "%s/bin", parent);
            snprintf(qt_conf, sizeof(qt_conf), "%s/qt.conf", bin_dir);
            snprintf(plugins, sizeof(plugins), "%s/plugins", dir_str);
            if (sharun_is_dir(plugins) && access(qt_conf, F_OK) != 0)
                sharun_add_to_env("QT_PLUGIN_PATH", plugins);
        }

        if (strcmp(dirs[i], "imlib2") == 0) {
            char loaders[PATH_MAX], filters[PATH_MAX];
            snprintf(loaders, sizeof(loaders), "%s/loaders", dir_str);
            snprintf(filters, sizeof(filters), "%s/filters", dir_str);
            if (sharun_is_dir(loaders)) setenv("IMLIB2_LOADER_PATH", loaders, 1);
            if (sharun_is_dir(filters)) setenv("IMLIB2_FILTER_PATH", filters, 1);
        }

        if (strncmp(dirs[i], "babl-", 5) == 0)
            setenv("BABL_PATH", dir_str, 1);

        if (strncmp(dirs[i], "gegl-", 5) == 0)
            setenv("GEGL_PATH", dir_str, 1);

        if (strcmp(dirs[i], "libdecor") == 0) {
            char plugins[PATH_MAX];
            snprintf(plugins, sizeof(plugins), "%s/plugins-1", dir_str);
            if (sharun_is_dir(plugins))
                setenv("LIBDECOR_PLUGIN_DIR", plugins, 1);
        }

        if (strncmp(dirs[i], "tcl", 3) == 0 && sharun_is_dir(dir_str)) {
            char msgs[PATH_MAX];
            snprintf(msgs, sizeof(msgs), "%s/msgs", dir_str);
            if (sharun_is_dir(msgs)) {
                sharun_add_to_env("TCL_LIBRARY", dir_str);
                // TK sibling
                if (strlen(dirs[i]) >= 4) {
                    char tkname[PATH_MAX];
                    snprintf(tkname, sizeof(tkname), "tk%s", dirs[i] + 3);
                    size_t lp_len = strlen(library_path);
                    size_t tk_len = strlen(tkname);
                    char *tk_dir = malloc(lp_len + 1 + tk_len + 1);
                    if (tk_dir) {
                        memcpy(tk_dir, library_path, lp_len);
                        tk_dir[lp_len] = '/';
                        memcpy(tk_dir + lp_len + 1, tkname, tk_len + 1);
                        if (sharun_is_dir(tk_dir))
                            sharun_add_to_env("TK_LIBRARY", tk_dir);
                        free(tk_dir);
                    }
                }
            }
        }

        if (strncmp(dirs[i], "gstreamer-", 10) == 0) {
            sharun_add_to_env("GST_PLUGIN_PATH", dir_str);
            sharun_add_to_env("GST_PLUGIN_SYSTEM_PATH", dir_str);
            setenv("GST_PLUGIN_SYSTEM_PATH_1_0", dir_str, 1);
            size_t dl = strlen(dir_str);
            char *scanner = malloc(dl + 19);
            if (scanner) {
                memcpy(scanner, dir_str, dl);
                memcpy(scanner + dl, "/gst-plugin-scanner", 20);
                if (sharun_is_file(scanner))
                    setenv("GST_PLUGIN_SCANNER", scanner, 1);
                free(scanner);
            }
        }

        if (strncmp(dirs[i], "gdk-pixbuf-", 11) == 0) {
            DIR *d = opendir(dir_str);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)) != NULL) {
                    if (e->d_name[0] == '.') continue;
                    size_t dl = strlen(dir_str);
                    size_t nl = strlen(e->d_name);
                    char *sub = malloc(dl + 1 + nl + 1);
                    if (!sub) continue;
                    memcpy(sub, dir_str, dl);
                    sub[dl] = '/';
                    memcpy(sub + dl + 1, e->d_name, nl + 1);
                    if (sharun_is_dir(sub)) {
                        snprintf(sub + dl + 1 + nl, PATH_MAX - dl - 1 - nl, "/loaders");
                        if (sharun_is_dir(sub))
                            setenv("GDK_PIXBUF_MODULEDIR", sub, 1);
                    }
                    free(sub);
                }
                closedir(d);
            }
            // Also look for loaders.cache
            char cache[PATH_MAX];
            snprintf(cache, sizeof(cache), "%s/loaders.cache", dir_str);
            if (sharun_is_file(cache))
                setenv("GDK_PIXBUF_MODULE_FILE", cache, 1);
        }

        free(dir_str);
    }

    for (size_t i = 0; i < ndirs; i++)
        free(dirs[i]);
    free(dirs);
    free(data);
}

// ── process_share_dir ──────────────────────────────────────────
static void process_share_dir(const char *sharun_dir) {
    char share_dir[PATH_MAX];
    snprintf(share_dir, sizeof(share_dir), "%s/share", sharun_dir);
    if (!sharun_is_dir(share_dir)) return;

    sharun_add_to_env("XDG_DATA_DIRS", "/run/current-system/sw/share");
    sharun_add_to_env("XDG_DATA_DIRS", "/run/opengl-driver/share");
    sharun_add_to_env("XDG_DATA_DIRS", "/usr/share");
    sharun_add_to_env("XDG_DATA_DIRS", "/usr/local/share");

    char *home = sharun_get_env("HOME");
    if (home) {
        size_t hlen = strlen(home);
        char *xdg_home = malloc(hlen + 13);
        if (xdg_home) {
            memcpy(xdg_home, home, hlen);
            memcpy(xdg_home + hlen, "/.local/share", 14);
            sharun_add_to_env("XDG_DATA_DIRS", xdg_home);
            free(xdg_home);
        }
        free(home);
    }

    sharun_add_to_env("XDG_DATA_DIRS", share_dir);

    char *xdg_data_dirs = sharun_get_env("XDG_DATA_DIRS");

    DIR *d = opendir(share_dir);
    if (!d) { free(xdg_data_dirs); return; }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;

        size_t dl = strlen(share_dir);
        size_t nl = strlen(e->d_name);
        char *full = malloc(dl + 1 + nl + 1);
        if (!full) continue;
        memcpy(full, share_dir, dl);
        full[dl] = '/';
        memcpy(full + dl + 1, e->d_name, nl + 1);
        if (!sharun_is_dir(full)) { free(full); continue; }

        char sub[PATH_MAX];

        if (strcmp(e->d_name, "glvnd") == 0) {
            char *no_nvidia = sharun_get_env("SHARUN_NO_NVIDIA_EGL_PRIME");
            bool has_nvidia = (access("/sys/module/nvidia/version", F_OK) == 0);
            char *egl_fnames = sharun_get_env("__EGL_VENDOR_LIBRARY_FILENAMES");
            if (!(no_nvidia && strcmp(no_nvidia, "1") == 0) && has_nvidia && !egl_fnames) {
                // Collect JSON files from XDG data dirs and prioritize nvidia
                if (xdg_data_dirs) {
                    char *xcopy = strdup(xdg_data_dirs);
                    if (xcopy) {
                        strarr_t all_jsons = strarr_init;
                        char *saveptr2;
                        for (char *dir = strtok_r(xcopy, ":", &saveptr2); dir; dir = strtok_r(NULL, ":", &saveptr2)) {
                            snprintf(sub, sizeof(sub), "%s/glvnd/egl_vendor.d", dir);
                            DIR *jd = opendir(sub);
                            if (jd) {
                                struct dirent *je;
                                while ((je = readdir(jd)) != NULL) {
                                    if (je->d_name[0] == '.') continue;
                                    char *jp = malloc(strlen(sub) + 1 + strlen(je->d_name) + 1);
                                    if (jp) {
                                        sprintf(jp, "%s/%s", sub, je->d_name);
                                        if (sharun_is_file(jp))
                                            strarr_push(&all_jsons, jp);
                                        free(jp);
                                    }
                                }
                                closedir(jd);
                            }
                        }
                        free(xcopy);

                        // Build ordered list: nvidia first, then others
                        strarr_t ordered = strarr_init;
                        for (size_t i = 0; i < all_jsons.len; i++) {
                            if (strstr(all_jsons.data[i], "nvidia"))
                                strarr_push(&ordered, all_jsons.data[i]);
                        }
                        for (size_t i = 0; i < all_jsons.len; i++) {
                            if (!strstr(all_jsons.data[i], "nvidia"))
                                strarr_push(&ordered, all_jsons.data[i]);
                        }
                        if (ordered.len > 0) {
                            size_t total = 0;
                            for (size_t i = 0; i < ordered.len; i++)
                                total += strlen(ordered.data[i]) + 1;
                            char *joined = malloc(total + 1);
                            if (joined) {
                                joined[0] = '\0';
                                for (size_t i = 0; i < ordered.len; i++) {
                                    if (i > 0) strcat(joined, ":");
                                    strcat(joined, ordered.data[i]);
                                }
                                setenv("__EGL_VENDOR_LIBRARY_FILENAMES", joined, 1);
                                free(joined);
                            }
                        }
                        strarr_free(&all_jsons);
                        strarr_free(&ordered);
                    }
                }
            }
            free(no_nvidia);
            free(egl_fnames);
            if (xdg_data_dirs)
                add_to_xdg_data_env(xdg_data_dirs, "__EGL_VENDOR_LIBRARY_DIRS", "glvnd/egl_vendor.d");
        }

        if (strcmp(e->d_name, "vulkan") == 0) {
            char *allow_sys = sharun_get_env("SHARUN_ALLOW_SYS_VKICD");
            if (allow_sys && strcmp(allow_sys, "1") == 0) {
                unsetenv("SHARUN_ALLOW_SYS_VKICD");
                if (xdg_data_dirs)
                    add_to_xdg_data_env(xdg_data_dirs, "VK_DRIVER_FILES", "vulkan/icd.d");
            } else {
                if (xdg_data_dirs) {
                    char *xcopy = strdup(xdg_data_dirs);
                    if (xcopy) {
                        char *saveptr2;
                        for (char *dir = strtok_r(xcopy, ":", &saveptr2); dir; dir = strtok_r(NULL, ":", &saveptr2)) {
                            snprintf(sub, sizeof(sub), "%s/vulkan/icd.d", dir);
                            if (sharun_is_dir(sub)) {
                                // Check if same as our share dir or if it has nvidia
                                if (strcmp(dir, share_dir) == 0 ||
                                    strncmp(dir, share_dir, strlen(share_dir)) == 0) {
                                    sharun_add_to_env("VK_DRIVER_FILES", sub);
                                } else {
                                    DIR *vd = opendir(sub);
                                    if (vd) {
                                        struct dirent *ve;
                                        while ((ve = readdir(vd)) != NULL) {
                                            if (strstr(ve->d_name, "nvidia")) {
                                                snprintf(sub, sizeof(sub), "%s/%s", dir, ve->d_name);
                                                sharun_add_to_env("VK_DRIVER_FILES", sub);
                                                break;
                                            }
                                        }
                                        closedir(vd);
                                    }
                                }
                            }
                        }
                        free(xcopy);
                    }
                }
            }
            free(allow_sys);
        }

        if (strcmp(e->d_name, "alsa") == 0) {
            snprintf(sub, sizeof(sub), "%s/%s/alsa.conf", share_dir, e->d_name);
            if (access("/usr/share/alsa/alsa.conf", F_OK) != 0 && sharun_is_file(sub))
                setenv("ALSA_CONFIG_PATH", sub, 1);
        }

        if (strcmp(e->d_name, "drirc.d") == 0) {
            if (access("/usr/share/drirc.d", F_OK) != 0)
                setenv("DRIRC_CONFIGDIR", full, 1);
        }

        if (strcmp(e->d_name, "X11") == 0) {
            snprintf(sub, sizeof(sub), "%s/%s/xkb", share_dir, e->d_name);
            if (access("/usr/share/X11/xkb", F_OK) != 0 && sharun_is_dir(sub))
                setenv("XKB_CONFIG_ROOT", sub, 1);
            snprintf(sub, sizeof(sub), "%s/%s/locale", share_dir, e->d_name);
            if (access("/usr/share/X11/locale", F_OK) != 0 && sharun_is_dir(sub))
                setenv("XLOCALEDIR", sub, 1);
        }

        if (strcmp(e->d_name, "libdrm") == 0) {
            sharun_add_to_env("AMDGPU_ASIC_ID_TABLE_PATHS", full);
            sharun_add_to_env("AMDGPU_ASIC_ID_TABLE_PATHS", "/usr/share/libdrm");
            sharun_add_to_env("AMDGPU_ASIC_ID_TABLE_PATHS", "/usr/local/share/libdrm");
        }

        if (strcmp(e->d_name, "libthai") == 0) {
            snprintf(sub, sizeof(sub), "%s/thbrk.tri", full);
            if (sharun_is_file(sub))
                setenv("LIBTHAI_DICTDIR", full, 1);
        }

        if (strcmp(e->d_name, "glib-2.0") == 0 && xdg_data_dirs)
            add_to_xdg_data_env(xdg_data_dirs, "GSETTINGS_SCHEMA_DIR", "glib-2.0/schemas");

        if (strcmp(e->d_name, "terminfo") == 0)
            setenv("TERMINFO", full, 1);

        if (strcmp(e->d_name, "locale") == 0)
            setenv("TEXTDOMAINDIR", full, 1);

        if (strcmp(e->d_name, "file") == 0) {
            snprintf(sub, sizeof(sub), "%s/misc/magic.mgc", full);
            if (sharun_is_file(sub))
                setenv("MAGIC", sub, 1);
        }

        free(full);
    }
    closedir(d);
    free(xdg_data_dirs);
}

// ── process_etc_dir ────────────────────────────────────────────
static void process_etc_dir(const char *sharun_dir) {
    char etc_dir[PATH_MAX];
    snprintf(etc_dir, sizeof(etc_dir), "%s/etc", sharun_dir);
    if (!sharun_is_dir(etc_dir)) return;

    DIR *d = opendir(etc_dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        size_t dl = strlen(etc_dir);
        size_t nl = strlen(e->d_name);
        char *full = malloc(dl + 1 + nl + 1);
        if (!full) continue;
        memcpy(full, etc_dir, dl);
        full[dl] = '/';
        memcpy(full + dl + 1, e->d_name, nl + 1);
        if (!sharun_is_dir(full)) { free(full); continue; }

        if (strcmp(e->d_name, "fonts") == 0) {
            char fonts_conf[PATH_MAX];
            snprintf(fonts_conf, sizeof(fonts_conf), "%s/fonts.conf", full);
            if (access("/etc/fonts/fonts.conf", F_OK) != 0 && sharun_is_file(fonts_conf))
                setenv("FONTCONFIG_FILE", fonts_conf, 1);
        }

        free(full);
    }
    closedir(d);
}

// ── process_ssl_certs ──────────────────────────────────────────
static void process_ssl_certs(void) {
    if (access("/etc/ssl/certs/ca-certificates.crt", F_OK) == 0) return;

    static const char *certs[] = {
        "/etc/pki/tls/cert.pem",
        "/etc/pki/tls/cacert.pem",
        "/etc/ssl/cert.pem",
        "/var/lib/ca-certificates/ca-bundle.pem",
        NULL
    };
    const char *found = NULL;
    for (int i = 0; certs[i]; i++) {
        if (access(certs[i], F_OK) == 0) { found = certs[i]; break; }
    }
    if (found) {
        static const char *vars[] = {"REQUESTS_CA_BUNDLE", "CURL_CA_BUNDLE", "SSL_CERT_FILE", NULL};
        for (int i = 0; vars[i]; i++) {
            if (!getenv(vars[i]))
                setenv(vars[i], found, 1);
        }
    } else {
        fprintf(stderr, "WARNING: Cannot find CA Certificates in host!\n");
    }
}

// ── Public API ─────────────────────────────────────────────────

strarr_t sharun_read_dotenv(const char *dotenv_dir) {
    strarr_t out = strarr_init;
    if (!dotenv_dir) return out;

    char dotenv_path[PATH_MAX];
    snprintf(dotenv_path, sizeof(dotenv_path), "%s/.env", dotenv_dir);
    if (access(dotenv_path, F_OK) != 0) return out;

    FILE *f = fopen(dotenv_path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to read .env file: %s\n", dotenv_path);
        exit(1);
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, f)) > 0) {
        char *start = line;
        while (*start == ' ' || *start == '\t' || *start == '\r') start++;
        if (strncmp(start, "unset ", 6) == 0) {
            char *v = start + 6;
            while (*v == ' ') v++;
            while (*v && *v != ' ' && *v != '\n' && *v != '\r') {
                char *vend = v;
                while (*vend && *vend != ' ' && *vend != '\n' && *vend != '\r') vend++;
                char saved = *vend;
                *vend = '\0';
                strarr_push(&out, v);
                *vend = saved;
                v = vend;
                while (*v == ' ') v++;
            }
        }
    }
    free(line);
    fclose(f);
    return out;
}

void sharun_setup_environment(const char *sharun_dir,
                               const char *bin_dir,
                               const char *library_path,
                               const char *lib_path_data) {

    // GIO_LAUNCH_DESKTOP
    char gio_path[PATH_MAX];
    snprintf(gio_path, sizeof(gio_path), "%s/gio-launch-desktop", bin_dir);
    if (sharun_is_exe(gio_path))
        setenv("GIO_LAUNCH_DESKTOP", gio_path, 1);

    // GI_TYPELIB_PATH
    if (sharun_is_dir(library_path)) {
        DIR *d = opendir(library_path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') continue;
                if (strncmp(e->d_name, "girepository-", 13) == 0) {
                    char *gi = malloc(strlen(library_path) + 1 + strlen(e->d_name) + 1);
                    if (gi) {
                        sprintf(gi, "%s/%s", library_path, e->d_name);
                        setenv("GI_TYPELIB_PATH", gi, 1);
                        free(gi);
                    }
                    break;
                }
            }
            closedir(d);
        }
    }

    // Per-directory env vars from lib.path
    process_lib_path_dirs(lib_path_data, library_path);

    // Share directory
    process_share_dir(sharun_dir);

    // Etc directory
    process_etc_dir(sharun_dir);

    // SSL certificates
    process_ssl_certs();
}

#endif // SHARUN_SETENV
