#include "darwin_abi.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <ctime>
#include <cerrno>
#include <cstdarg>
#include <dirent.h>
#include <dlfcn.h>
#include <unordered_map>
#include <vector>
#include "macho_loader.h"
#include "foundation_shim.h"
#include "eagl_shim.h"
#include <semaphore.h>
#include <pthread.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <android/log.h>
#include <atomic>
#include <algorithm>
#include <zlib.h>

#define TAG "FTL_DARWIN_ABI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Apple Blocks Runtime & CoreFoundation globals
void *_NSConcreteGlobalBlock[32] = {0};
void *_NSConcreteStackBlock[32] = {0};
void *___CFConstantStringClassReference[32] = {0};
void *kCFAllocatorNull = (void *)0xDEADCF00ULL;
double kCFCoreFoundationVersionNumber = 1348.0;
uint32_t darwin_mach_task_self = 1;
struct DarwinRuneLocale g_darwin_default_rune_locale;

__attribute__((constructor))
static void init_darwin_rune_locale() {
    memset(&g_darwin_default_rune_locale, 0, sizeof(g_darwin_default_rune_locale));
    for (int c = 0; c < 256; c++) {
        uint32_t flags = 0;
        if (c < 128) {
            if (isalpha(c))  flags |= 0x00000100U;
            if (iscntrl(c))  flags |= 0x00000200U;
            if (isdigit(c))  flags |= 0x00000400U;
            if (isgraph(c))  flags |= 0x00000800U;
            if (islower(c))  flags |= 0x00001000U;
            if (ispunct(c))  flags |= 0x00002000U;
            if (isspace(c))  flags |= 0x00004000U;
            if (isupper(c))  flags |= 0x00008000U;
            if (isxdigit(c)) flags |= 0x00010000U;
            if (isblank(c))  flags |= 0x00020000U;
            if (isprint(c))  flags |= 0x00040000U;
            g_darwin_default_rune_locale.maplower[c] = tolower(c);
            g_darwin_default_rune_locale.mapupper[c] = toupper(c);
        } else {
            g_darwin_default_rune_locale.maplower[c] = c;
            g_darwin_default_rune_locale.mapupper[c] = c;
        }
        g_darwin_default_rune_locale.runetype[c] = flags;
    }
}

static std::string g_internal_storage = "/data/data/com.subsetgames.ftl/files";
static std::string g_external_storage = "/sdcard/Android/data/com.subsetgames.ftl/files";

extern "C" {

// ============================================================================
// VFS & Filesystem Translation
// ============================================================================

void darwin_vfs_init(const char *internal_storage_path, const char *external_storage_path) {
    if (internal_storage_path) g_internal_storage = internal_storage_path;
    if (external_storage_path) g_external_storage = external_storage_path;
    LOGI("Darwin VFS initialized. Internal: %s, External: %s", g_internal_storage.c_str(), g_external_storage.c_str());
}

const char *darwin_vfs_resolve(const char *ios_path, char *out_buf, size_t out_len) {
    if (!ios_path || !out_buf || out_len == 0) return ios_path;

    // Check if path is relative or points directly to ftl.dat / resource.dat
    if (strcmp(ios_path, "ftl.dat") == 0 || strcmp(ios_path, "./ftl.dat") == 0) {
        snprintf(out_buf, out_len, "%s/ftl.dat", g_external_storage.c_str());
        return out_buf;
    }

    if (strstr(ios_path, ".app/") != nullptr) {
        const char *rel = strstr(ios_path, ".app/") + 5;
        snprintf(out_buf, out_len, "%s/%s", g_external_storage.c_str(), rel);
        return out_buf;
    }

    if (strstr(ios_path, "/Documents/") != nullptr) {
        const char *rel = strstr(ios_path, "/Documents/") + 11;
        snprintf(out_buf, out_len, "%s/save/%s", g_internal_storage.c_str(), rel);
        return out_buf;
    }

    // Default: pass through
    strncpy(out_buf, ios_path, out_len - 1);
    out_buf[out_len - 1] = '\0';
    return out_buf;
}

static int darwin_oflags_to_bionic(int dflags) {
    int bflags = dflags & 3; // O_RDONLY(0), O_WRONLY(1), O_RDWR(2)
    if (dflags & 0x0004) bflags |= O_NONBLOCK;
    if (dflags & 0x0008) bflags |= O_APPEND;
    if (dflags & 0x0200) bflags |= O_CREAT;
    if (dflags & 0x0400) bflags |= O_TRUNC;
    if (dflags & 0x0800) bflags |= O_EXCL;
    if (dflags & 0x1000000) bflags |= O_CLOEXEC;
    return bflags;
}

static int bionic_oflags_to_darwin(int bflags) {
    int dflags = bflags & 3;
    if (bflags & O_NONBLOCK) dflags |= 0x0004;
    if (bflags & O_APPEND) dflags |= 0x0008;
    if (bflags & O_CREAT) dflags |= 0x0200;
    if (bflags & O_TRUNC) dflags |= 0x0400;
    if (bflags & O_EXCL) dflags |= 0x0800;
    if (bflags & O_CLOEXEC) dflags |= 0x1000000;
    return dflags;
}

static int g_vfs_log_count = 0;

// ---------------------------------------------------------------------------
// Asset tagging for the diorama: parse ftl.dat's PKG index when FTL opens it and
// remember which image entry ("atlas/people/human.tex", "img/ship/kestrel_base.png",
// ...) was read last. Texture uploads are tagged with that name (see register_tex_plane_meta).
// Loose files opened by path (mods) set the name directly.
// ---------------------------------------------------------------------------
struct PkgEntry { uint32_t off, size; uint8_t flags; std::string name; };
static std::vector<PkgEntry> g_pkg_entries; // sorted by offset
static std::atomic<int> g_pkg_fd{-1};
static FILE *g_pkg_fp = nullptr;
static std::mutex g_asset_mu;
static int g_asset_logs = 0;
// Memory ranges that hold (compressed or inflated) bytes of a named asset. Newest wins.
struct AssetRange { uintptr_t lo, hi; const std::string *name; int w, h; };
static AssetRange g_asset_ranges[1024];
static size_t g_asset_range_next = 0;
static std::unordered_map<std::string, std::string *> g_asset_names;
static thread_local const std::string *t_last_read_asset = nullptr;
static const std::string *intern_asset_unlocked_fwd(const std::string &n);
// .tex entries read from ftl.dat, in read order, with the dimensions from their TEX header.
// GL uploads (on the render thread) claim the oldest pending entry with matching size.
struct PendingTex { const std::string *name; int w, h; };
static std::vector<PendingTex> g_pending_tex;

static void pending_tex_push(const std::string &n, const uint8_t *head, size_t len) {
    if (len < 12 || memcmp(head, "TEX\n", 4) != 0) return;
    int w = head[8] << 8 | head[9], h = head[10] << 8 | head[11];
    std::lock_guard<std::mutex> lk(g_asset_mu);
    g_pending_tex.push_back({intern_asset_unlocked_fwd(n), w, h});
    if (g_pending_tex.size() > 512) g_pending_tex.erase(g_pending_tex.begin(), g_pending_tex.begin() + 128);
}

static std::string pending_tex_claim(int w, int h) {
    std::lock_guard<std::mutex> lk(g_asset_mu);
    for (size_t i = 0; i < g_pending_tex.size(); i++) {
        if (g_pending_tex[i].w == w && g_pending_tex[i].h == h) {
            std::string r = *g_pending_tex[i].name;
            g_pending_tex.erase(g_pending_tex.begin() + (long)i);
            return r;
        }
    }
    return std::string();
}

static bool is_image_asset(const std::string &n) {
    size_t L = n.size();
    auto ends = [&](const char *e) { size_t l = strlen(e); return L >= l && strcasecmp(n.c_str() + L - l, e) == 0; };
    return ends(".tex") || ends(".png") || ends(".jpg") || ends(".pvr") || ends(".pvrtc");
}

static const std::string *intern_asset(const std::string &n) {
    auto it = g_asset_names.find(n);
    if (it != g_asset_names.end()) return it->second;
    auto *p = new std::string(n);
    g_asset_names[n] = p;
    return p;
}

static const std::string *intern_asset_unlocked_fwd(const std::string &n) { return intern_asset(n); }

static void asset_add_range(const void *buf, size_t n, const std::string *name, int w = 0, int h = 0) {
    if (!buf || !n || !name) return;
    g_asset_ranges[g_asset_range_next % 1024] = {(uintptr_t)buf, (uintptr_t)buf + n, name, w, h};
    g_asset_range_next++;
}

// w/h > 0: only accept a range whose TEX header size matches (freed buffers get reused, and a
// generated texture must not inherit a stale file name — power bars were tagged as a hull cloak).
static const std::string *asset_find_locked(const void *p, int w = 0, int h = 0) {
    uintptr_t a = (uintptr_t)p;
    size_t n = std::min<size_t>(g_asset_range_next, 1024);
    for (size_t i = 0; i < n; i++) {
        const AssetRange &r = g_asset_ranges[(g_asset_range_next - 1 - i) % 1024];
        if (a >= r.lo && a < r.hi) {
            if (w > 0 && (r.w != w || r.h != h)) return nullptr;
            return r.name;
        }
    }
    return nullptr;
}

static std::string asset_for_pointer(const void *p, int w, int h) {
    if (!p) return std::string();
    std::lock_guard<std::mutex> lk(g_asset_mu);
    const std::string *n = asset_find_locked(p, w, h);
    return n ? *n : std::string();
}

static void note_asset_read(const std::string &n, const void *buf, size_t len) {
    if (!is_image_asset(n)) { t_last_read_asset = nullptr; return; }
    int w = 0, h = 0;
    const uint8_t *b = (const uint8_t *)buf;
    if (len >= 12 && memcmp(b, "TEX\n", 4) == 0) { w = b[8] << 8 | b[9]; h = b[10] << 8 | b[11]; }
    std::lock_guard<std::mutex> lk(g_asset_mu);
    const std::string *name = intern_asset(n);
    t_last_read_asset = name;
    asset_add_range(buf, len, name, w, h);
}

int darwin_inflate(void *strm_v, int flush) {
    z_stream *strm = (z_stream *)strm_v;
    const Bytef *in0 = strm->next_in;
    Bytef *out0 = strm->next_out;
    int r = inflate(strm, flush);
    static int s_inf_logs = 0;
    if (s_inf_logs < 12) {
        s_inf_logs++;
        LOGI("Diorama inflate call in=%p out=%p produced=%ld last_read=%s", in0, out0,
             (long)(strm->next_out - out0), t_last_read_asset ? t_last_read_asset->c_str() : "-");
    }
    if (out0 && strm->next_out > out0) {
        std::lock_guard<std::mutex> lk(g_asset_mu);
        const std::string *name = asset_find_locked(in0);
        if (!name) name = t_last_read_asset;
        if (name) {
            asset_add_range(out0, (size_t)(strm->next_out - out0), name);
            if (g_asset_logs < 40) { g_asset_logs++; LOGI("Diorama inflate %s -> %p +%zu", name->c_str(), out0, (size_t)(strm->next_out - out0)); }
        }
    }
    return r;
}

static void pkg_parse_index(const char *path) {
    if (!g_pkg_entries.empty()) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t hdr[16];
    if (fread(hdr, 1, 16, f) == 16 && memcmp(hdr, "PKG\n", 4) == 0) {
        auto be16 = [](const uint8_t *p) { return (uint32_t)(p[0] << 8 | p[1]); };
        auto be32 = [](const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; };
        uint32_t hsz = be16(hdr + 4), esz = be16(hdr + 6), n = be32(hdr + 8), psz = be32(hdr + 12);
        std::vector<uint8_t> ents((size_t)esz * n), paths(psz + 1, 0);
        fseek(f, hsz, SEEK_SET);
        if (fread(ents.data(), 1, ents.size(), f) == ents.size() && fread(paths.data(), 1, psz, f) == psz) {
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t *e = &ents[(size_t)i * esz];
                uint32_t po = be32(e + 4) & 0xFFFFFF;
                if (po >= psz) continue;
                g_pkg_entries.push_back({be32(e + 8), be32(e + 12), (uint8_t)(be32(e + 4) >> 24), std::string((const char *)&paths[po])});
            }
            std::sort(g_pkg_entries.begin(), g_pkg_entries.end(), [](const PkgEntry &a, const PkgEntry &b) { return a.off < b.off; });
        }
    }
    fclose(f);
    LOGI("Diorama: parsed ftl.dat index (%zu entries) from %s", g_pkg_entries.size(), path);
}

static void pkg_note_read(off_t off, const void *buf, ssize_t got) {
    static int s_rd_logs = 0;
    if (s_rd_logs < 12) { s_rd_logs++; LOGI("Diorama pkg read off=%lld buf=%p n=%zd", (long long)off, buf, got); }
    if (g_pkg_entries.empty() || off < 0 || got <= 0) return;
    auto it = std::upper_bound(g_pkg_entries.begin(), g_pkg_entries.end(), (uint32_t)off,
                               [](uint32_t v, const PkgEntry &e) { return v < e.off; });
    if (it == g_pkg_entries.begin()) return;
    --it;
    if ((uint32_t)off >= it->off + std::max<uint32_t>(it->size, 1)) return;
    note_asset_read(it->name, buf, (size_t)got);
    size_t L = it->name.size();
    if ((uint32_t)off == it->off && L > 4 && strcasecmp(it->name.c_str() + L - 4, ".tex") == 0) {
        if (it->flags & 1) {
            uint8_t head[32];
            z_stream zs; memset(&zs, 0, sizeof(zs));
            if (inflateInit(&zs) == Z_OK) {
                zs.next_in = (Bytef *)buf; zs.avail_in = (uInt)got;
                zs.next_out = head; zs.avail_out = sizeof(head);
                inflate(&zs, Z_SYNC_FLUSH);
                size_t produced = sizeof(head) - zs.avail_out;
                inflateEnd(&zs);
                pending_tex_push(it->name, head, produced);
            }
        } else {
            pending_tex_push(it->name, (const uint8_t *)buf, (size_t)got);
        }
    }
}

static void asset_note_open(const char *path, int fd, FILE *fp) {
    if (!path) return;
    size_t L = strlen(path);
    if (L >= 7 && strcmp(path + L - 7, "ftl.dat") == 0) {
        pkg_parse_index(path);
        if (fd >= 0) g_pkg_fd.store(fd);
        if (fp) g_pkg_fp = fp;
        return;
    }
    (void)path; // loose files: tagged on read below when FTL reads them (not tracked yet)
}

ssize_t darwin_read(int fd, void *buf, size_t n) {
    if (fd != g_pkg_fd.load(std::memory_order_relaxed)) return read(fd, buf, n);
    off_t off = lseek(fd, 0, SEEK_CUR);
    ssize_t got = read(fd, buf, n);
    pkg_note_read(off, buf, got);
    return got;
}

ssize_t darwin_pread(int fd, void *buf, size_t n, off_t off) {
    ssize_t got = pread(fd, buf, n, off);
    if (fd == g_pkg_fd.load(std::memory_order_relaxed)) pkg_note_read(off, buf, got);
    return got;
}

size_t darwin_fread(void *buf, size_t sz, size_t n, FILE *fp) {
    if (!fp || fp != g_pkg_fp) return fread(buf, sz, n, fp);
    off_t off = ftello(fp);
    size_t got = fread(buf, sz, n, fp);
    pkg_note_read(off, buf, (ssize_t)(got * sz));
    return got;
}

extern "C" int darwin_open_va(const char *path, int oflag, mode_t mode) {
    char resolved[1024];
    darwin_vfs_resolve(path, resolved, sizeof(resolved));
    int boflag = darwin_oflags_to_bionic(oflag);
    int fd;
    if (oflag & 0x0200) {
        fd = open(resolved, boflag, mode);
    } else {
        fd = open(resolved, boflag);
    }
    if (g_vfs_log_count < 30 || (oflag & 3) != 0) {
        g_vfs_log_count++;
        LOGI("darwin_open('%s' -> '%s', dflags=0x%x mode=0%o) = %d errno=%d", path ? path : "", resolved, oflag,
             (unsigned)mode, fd, fd < 0 ? errno : 0);
    }
    if (fd >= 0) asset_note_open(resolved, fd, nullptr);
    return fd;
}

extern "C" int darwin_fcntl_va(int fd, int cmd, long arg) {
    if (cmd == 3 /* F_GETFL */) {
        int res = fcntl(fd, F_GETFL);
        return (res < 0) ? res : bionic_oflags_to_darwin(res);
    } else if (cmd == 4 /* F_SETFL */) {
        return fcntl(fd, F_SETFL, darwin_oflags_to_bionic((int)arg));
    }
    switch (cmd) {
        case 0: case 1: case 2: // F_DUPFD, F_GETFD, F_SETFD: same numbers
            return fcntl(fd, cmd, arg);
        case 51: { // F_FULLFSYNC: FTL's safe-save flushes the temp file with this before renaming it;
                   // bionic rejected cmd 51, so FTL deleted every save. Map to fsync.
            int r = fsync(fd);
            static int s_logs = 0;
            if (s_logs++ < 4) LOGI("darwin_fcntl F_FULLFSYNC(fd=%d) -> fsync = %d", fd, r);
            return r;
        }
        case 7: return fcntl(fd, F_GETLK, arg);  // Darwin F_GETLK
        case 8: return fcntl(fd, F_SETLK, arg);  // Darwin F_SETLK (struct flock layout matches on arm64)
        case 9: return fcntl(fd, F_SETLKW, arg); // Darwin F_SETLKW
        case 67: return fcntl(fd, F_DUPFD_CLOEXEC, arg); // Darwin F_DUPFD_CLOEXEC
        case 45: case 48: case 82: case 85: // F_RDAHEAD, F_NOCACHE, F_BARRIERFSYNC(85), F_SINGLE_WRITER(82): hints
            if (cmd == 85) return fsync(fd);
            return 0;
        default: {
            static int s_unk = 0;
            if (s_unk++ < 16) LOGI("darwin_fcntl unknown Darwin cmd %d (fd=%d)", cmd, fd);
            return fcntl(fd, cmd, arg);
        }
    }
}

struct aarch64_va_list {
    void *__stack;
    void *__gr_top;
    void *__vr_top;
    int __gr_offs;
    int __vr_offs;
};

extern "C" int darwin_sprintf_chk_va(char *str, int flag, size_t slen, const char *format, void *va_stack) {
    (void)flag;
    aarch64_va_list raw_va = { va_stack, nullptr, nullptr, 0, 0 };
    va_list ap;
    memcpy(&ap, &raw_va, sizeof(ap));
    if (slen == (size_t)-1) {
        return vsprintf(str, format, ap);
    }
    return vsnprintf(str, slen, format, ap);
}

extern "C" int darwin_asprintf_va(char **ret, const char *format, void *va_stack) {
    aarch64_va_list raw_va = { va_stack, nullptr, nullptr, 0, 0 };
    va_list ap;
    memcpy(&ap, &raw_va, sizeof(ap));
    return vasprintf(ret, format, ap);
}

static char darwin_ctype_do_widen_char(const void *, char c) {
    return c;
}

const void *apple_locale_use_facet(const void *loc, void *id) {
    typedef const void *(*use_facet_fn)(const void *, void *);
    static use_facet_fn real_use_facet = nullptr;
    static void *ctype_char_id = nullptr;
    static uintptr_t s_fallback_vtable[16] = {0};
    static uintptr_t s_darwin_ctype_facet[4] = {0};

    if (!real_use_facet) {
        real_use_facet = (use_facet_fn)dlsym(RTLD_DEFAULT, "_ZNKSt6__ndk16locale9use_facetERNS0_2idE");
        if (!real_use_facet) {
            real_use_facet = (use_facet_fn)dlsym(RTLD_DEFAULT, "_ZNKSt3__16locale9use_facetERNS0_2idE");
        }
        ctype_char_id = dlsym(RTLD_DEFAULT, "_ZNSt6__ndk15ctypeIcE2idE");
        if (!ctype_char_id) {
            ctype_char_id = dlsym(RTLD_DEFAULT, "_ZNSt3__15ctypeIcE2idE");
        }
        for (int i = 0; i < 16; i++) {
            s_fallback_vtable[i] = (uintptr_t)darwin_ctype_do_widen_char;
        }
    }
    const void *real_facet = (real_use_facet && loc && id) ? real_use_facet(loc, id) : nullptr;
    if (!ctype_char_id || id == ctype_char_id) {
        const uintptr_t *rf = (const uintptr_t *)real_facet;
        s_darwin_ctype_facet[0] = rf ? rf[0] : (uintptr_t)s_fallback_vtable;
        s_darwin_ctype_facet[1] = rf ? rf[1] : 1;
        s_darwin_ctype_facet[2] = (uintptr_t)&g_darwin_default_rune_locale.runetype[0];
        s_darwin_ctype_facet[3] = 0;
        return (const void *)s_darwin_ctype_facet;
    }
    return real_facet;
}

int darwin_stat(const char *pathname, struct darwin_stat64 *buf) {
    char resolved[1024];
    darwin_vfs_resolve(pathname, resolved, sizeof(resolved));

    struct stat st;
    int res = stat(resolved, &st);
    if (res != 0) return res;

    memset(buf, 0, sizeof(*buf));
    buf->st_dev = st.st_dev;
    buf->st_mode = st.st_mode;
    buf->st_nlink = st.st_nlink;
    buf->st_ino = st.st_ino;
    buf->st_uid = st.st_uid;
    buf->st_gid = st.st_gid;
    buf->st_rdev = st.st_rdev;
    buf->st_size = st.st_size;
    buf->st_blksize = st.st_blksize;
    buf->st_blocks = st.st_blocks;
    buf->darwin_st_atime = st.st_atim.tv_sec;
    buf->darwin_st_atimensec = st.st_atim.tv_nsec;
    buf->darwin_st_mtime = st.st_mtim.tv_sec;
    buf->darwin_st_mtimensec = st.st_mtim.tv_nsec;
    buf->darwin_st_ctime = st.st_ctim.tv_sec;
    buf->darwin_st_ctimensec = st.st_ctim.tv_nsec;
    return 0;
}

int darwin_fstat(int fd, struct darwin_stat64 *buf) {
    struct stat st;
    int res = fstat(fd, &st);
    if (res != 0) return res;

    memset(buf, 0, sizeof(*buf));
    buf->st_dev = st.st_dev;
    buf->st_mode = st.st_mode;
    buf->st_nlink = st.st_nlink;
    buf->st_ino = st.st_ino;
    buf->st_uid = st.st_uid;
    buf->st_gid = st.st_gid;
    buf->st_rdev = st.st_rdev;
    buf->st_size = st.st_size;
    buf->st_blksize = st.st_blksize;
    buf->st_blocks = st.st_blocks;
    buf->darwin_st_atime = st.st_atim.tv_sec;
    buf->darwin_st_atimensec = st.st_atim.tv_nsec;
    buf->darwin_st_mtime = st.st_mtim.tv_sec;
    buf->darwin_st_mtimensec = st.st_mtim.tv_nsec;
    buf->darwin_st_ctime = st.st_ctim.tv_sec;
    buf->darwin_st_ctimensec = st.st_ctim.tv_nsec;
    return 0;
}

void *darwin_fopen(const char *pathname, const char *mode) {
    char resolved[1024];
    darwin_vfs_resolve(pathname, resolved, sizeof(resolved));
    FILE *fp = fopen(resolved, mode);
    if (g_vfs_log_count < 30 || (mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+')))) {
        g_vfs_log_count++;
        LOGI("darwin_fopen('%s' -> '%s', '%s') = %p errno=%d", pathname ? pathname : "", resolved, mode ? mode : "", fp,
             fp ? 0 : errno);
    }
    if (fp) asset_note_open(resolved, -1, fp);
    return fp;
}

int darwin_access(const char *pathname, int mode) {
    char resolved[1024];
    darwin_vfs_resolve(pathname, resolved, sizeof(resolved));
    return access(resolved, mode);
}

int darwin_mkdir(const char *pathname, unsigned int mode) {
    char resolved[1024];
    darwin_vfs_resolve(pathname, resolved, sizeof(resolved));
    int r = mkdir(resolved, (mode_t)mode);
    LOGI("darwin_mkdir('%s' -> '%s', 0%o) = %d errno=%d", pathname ? pathname : "", resolved, mode, r, r ? errno : 0);
    return r;
}

int darwin_unlink(const char *pathname) {
    char resolved[1024];
    darwin_vfs_resolve(pathname, resolved, sizeof(resolved));
    int r = unlink(resolved);
    if (!strstr(resolved, "/.")) LOGI("darwin_unlink('%s') = %d errno=%d", resolved, r, r ? errno : 0);
    return r;
}

int darwin_rename(const char *oldpath, const char *newpath) {
    char res_old[1024], res_new[1024];
    darwin_vfs_resolve(oldpath, res_old, sizeof(res_old));
    darwin_vfs_resolve(newpath, res_new, sizeof(res_new));
    int r = rename(res_old, res_new);
    LOGI("darwin_rename('%s' -> '%s') = %d errno=%d", res_old, res_new, r, r ? errno : 0);
    return r;
}

#pragma pack(push, 1)
struct darwin_dirent64 {
    uint64_t d_ino;
    uint64_t d_seekoff;
    uint16_t d_reclen;
    uint16_t d_namlen;
    uint8_t d_type;
    char d_name[1024];
};
#pragma pack(pop)

struct DarwinDIR {
    DIR *dir;
    struct darwin_dirent64 entry;
};

void *darwin_opendir(const char *name) {
    char resolved[1024];
    darwin_vfs_resolve(name, resolved, sizeof(resolved));
    DIR *d = opendir(resolved);
    if (!d) return nullptr;
    auto *wrap = new DarwinDIR();
    wrap->dir = d;
    memset(&wrap->entry, 0, sizeof(wrap->entry));
    return wrap;
}

void *darwin_readdir(void *dirp) {
    if (!dirp) return nullptr;
    auto *wrap = (DarwinDIR *)dirp;
    struct dirent *de = readdir(wrap->dir);
    if (!de) return nullptr;
    wrap->entry.d_ino = de->d_ino;
    wrap->entry.d_seekoff = (uint64_t)de->d_off;
    wrap->entry.d_type = de->d_type;
    size_t nlen = strlen(de->d_name);
    if (nlen >= sizeof(wrap->entry.d_name)) nlen = sizeof(wrap->entry.d_name) - 1;
    wrap->entry.d_namlen = (uint16_t)nlen;
    memcpy(wrap->entry.d_name, de->d_name, nlen);
    wrap->entry.d_name[nlen] = '\0';
    wrap->entry.d_reclen = (uint16_t)((21 + nlen + 8) & ~7);
    return &wrap->entry;
}

int darwin_closedir(void *dirp) {
    if (!dirp) return -1;
    auto *wrap = (DarwinDIR *)dirp;
    int r = closedir(wrap->dir);
    delete wrap;
    return r;
}

static int g_gl_shader_logs = 0;
static int g_gl_draw_logs = 0;
static int g_gl_tex_logs = 0;
static int g_gl_vp_logs = 0;

static void diag_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length) {
    if (g_gl_shader_logs < 4 && count > 0 && string && string[0]) {
        LOGI("GL glShaderSource(shader=%u, count=%d): %.180s", shader, count, string[0]);
        g_gl_shader_logs++;
    }
    glShaderSource(shader, count, string, length);
}

static void diag_glCompileShader(GLuint shader) {
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512] = {0};
        glGetShaderInfoLog(shader, sizeof(buf) - 1, nullptr, buf);
        LOGE("GL SHADER COMPILE FAILED (shader=%u): %s", shader, buf);
    }
}

// Programs with no sampler uniform draw solid colour (boxes, bars, beams).
static uint8_t g_prog_has_sampler[4096];

static void diag_glLinkProgram(GLuint program) {
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok && program < 4096) {
        GLint n = 0;
        glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &n);
        uint8_t has = 0;
        for (GLint i = 0; i < n; i++) {
            GLint sz = 0; GLenum ty = 0; char nm[64];
            glGetActiveUniform(program, (GLuint)i, sizeof(nm), nullptr, &sz, &ty, nm);
            if (ty == GL_SAMPLER_2D || ty == GL_SAMPLER_CUBE || ty == GL_SAMPLER_3D || ty == GL_SAMPLER_2D_ARRAY) has = 1;
        }
        g_prog_has_sampler[program] = has ? 1 : 2;
        LOGI("Diorama program %u linked: sampler=%d uniforms=%d", program, has, n);
    }
    if (!ok) {
        char buf[512] = {0};
        glGetProgramInfoLog(program, sizeof(buf) - 1, nullptr, buf);
        LOGE("GL PROGRAM LINK FAILED (prog=%u): %s", program, buf);
    }
}

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef GL_BGRA8_EXT
#define GL_BGRA8_EXT 0x93A1
#endif

extern "C++" {
#include "pvrtc_decode.h"
}

static void reset_unpack_state(GLint &saved_row_len, GLint &saved_skip_rows, GLint &saved_skip_pix) {
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &saved_row_len);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &saved_skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &saved_skip_pix);
    if (saved_row_len) glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (saved_skip_rows) glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    if (saved_skip_pix) glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
}

static void convert_bgra_to_rgba(const void *pixels, GLsizei width, GLsizei height,
                                 std::vector<uint8_t> &out,
                                 GLint &saved_row_len, GLint &saved_skip_rows, GLint &saved_skip_pix) {
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &saved_row_len);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &saved_skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &saved_skip_pix);
    GLint align = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    if (align <= 0) align = 1;

    GLsizei eff_row_pixels = (saved_row_len > 0) ? saved_row_len : width;
    size_t row_bytes = (size_t)eff_row_pixels * 4;
    size_t stride = (row_bytes + (size_t)align - 1) & ~((size_t)align - 1);

    out.resize((size_t)width * (size_t)height * 4);
    const uint8_t *src_base = (const uint8_t *)pixels +
                              (size_t)saved_skip_rows * stride +
                              (size_t)saved_skip_pix * 4;
    for (GLsizei y = 0; y < height; y++) {
        const uint8_t *srow = src_base + (size_t)y * stride;
        uint8_t *drow = out.data() + (size_t)y * (size_t)width * 4;
        for (GLsizei x = 0; x < width; x++) {
            drow[x * 4 + 0] = srow[x * 4 + 2]; // R <- B
            drow[x * 4 + 1] = srow[x * 4 + 1]; // G <- G
            drow[x * 4 + 2] = srow[x * 4 + 0]; // B <- R
            drow[x * 4 + 3] = srow[x * 4 + 3]; // A <- A
        }
    }
    if (saved_row_len) glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (saved_skip_rows) glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    if (saved_skip_pix) glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
}

static void restore_unpack_state(GLint saved_row_len, GLint saved_skip_rows, GLint saved_skip_pix) {
    if (saved_row_len) glPixelStorei(GL_UNPACK_ROW_LENGTH, saved_row_len);
    if (saved_skip_rows) glPixelStorei(GL_UNPACK_SKIP_ROWS, saved_skip_rows);
    if (saved_skip_pix) glPixelStorei(GL_UNPACK_SKIP_PIXELS, saved_skip_pix);
}

#ifndef GL_UNSIGNED_SHORT_4_4_4_4_REV
#define GL_UNSIGNED_SHORT_4_4_4_4_REV 0x8365
#endif
#ifndef GL_UNSIGNED_SHORT_1_5_5_5_REV
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#endif

static void convert_bgra16_to_rgba8(const void *pixels, GLsizei width, GLsizei height, GLenum type,
                                    std::vector<uint8_t> &out,
                                    GLint &saved_row_len, GLint &saved_skip_rows, GLint &saved_skip_pix) {
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &saved_row_len);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &saved_skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &saved_skip_pix);
    GLint align = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    if (align <= 0) align = 1;

    GLsizei eff_row_pixels = (saved_row_len > 0) ? saved_row_len : width;
    size_t row_bytes = (size_t)eff_row_pixels * 2;
    size_t stride = (row_bytes + (size_t)align - 1) & ~((size_t)align - 1);

    out.resize((size_t)width * (size_t)height * 4);
    const uint8_t *src_base = (const uint8_t *)pixels +
                              (size_t)saved_skip_rows * stride +
                              (size_t)saved_skip_pix * 2;
    const bool is_4444_rev = (type == GL_UNSIGNED_SHORT_4_4_4_4_REV);
    const bool is_1555_rev = (type == GL_UNSIGNED_SHORT_1_5_5_5_REV);
    const bool is_4444 = (type == GL_UNSIGNED_SHORT_4_4_4_4);
    for (GLsizei y = 0; y < height; y++) {
        const uint16_t *srow = (const uint16_t *)(src_base + (size_t)y * stride);
        uint8_t *drow = out.data() + (size_t)y * (size_t)width * 4;
        if (is_4444_rev) {
            for (GLsizei x = 0; x < width; x++) {
                uint16_t p = srow[x];
                drow[x * 4 + 0] = (uint8_t)(((p >> 8) & 0xF) * 17);
                drow[x * 4 + 1] = (uint8_t)(((p >> 4) & 0xF) * 17);
                drow[x * 4 + 2] = (uint8_t)((p & 0xF) * 17);
                drow[x * 4 + 3] = (uint8_t)(((p >> 12) & 0xF) * 17);
            }
        } else if (is_1555_rev) {
            for (GLsizei x = 0; x < width; x++) {
                uint16_t p = srow[x];
                uint8_t r5 = (uint8_t)((p >> 10) & 0x1F);
                uint8_t g5 = (uint8_t)((p >> 5) & 0x1F);
                uint8_t b5 = (uint8_t)(p & 0x1F);
                drow[x * 4 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
                drow[x * 4 + 1] = (uint8_t)((g5 << 3) | (g5 >> 2));
                drow[x * 4 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
                drow[x * 4 + 3] = (p & 0x8000) ? 255 : 0;
            }
        } else if (is_4444) {
            for (GLsizei x = 0; x < width; x++) {
                uint16_t p = srow[x];
                drow[x * 4 + 0] = (uint8_t)(((p >> 12) & 0xF) * 17);
                drow[x * 4 + 1] = (uint8_t)(((p >> 8) & 0xF) * 17);
                drow[x * 4 + 2] = (uint8_t)(((p >> 4) & 0xF) * 17);
                drow[x * 4 + 3] = (uint8_t)((p & 0xF) * 17);
            }
        } else {
            // GL_UNSIGNED_SHORT_5_5_5_1 (0x8034)
            for (GLsizei x = 0; x < width; x++) {
                uint16_t p = srow[x];
                uint8_t r5 = (uint8_t)((p >> 11) & 0x1F);
                uint8_t g5 = (uint8_t)((p >> 6) & 0x1F);
                uint8_t b5 = (uint8_t)((p >> 1) & 0x1F);
                drow[x * 4 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
                drow[x * 4 + 1] = (uint8_t)((g5 << 3) | (g5 >> 2));
                drow[x * 4 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
                drow[x * 4 + 3] = (p & 1) ? 255 : 0;
            }
        }
    }
    if (saved_row_len) glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (saved_skip_rows) glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    if (saved_skip_pix) glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
}

static bool maybe_convert_tex_pixels(GLenum &format, GLenum &type, const void *&pixels,
                                     GLsizei width, GLsizei height, std::vector<uint8_t> &tmp,
                                     GLint &rlen, GLint &srows, GLint &spix) {
    if (format == GL_RGBA &&
        (type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1)) {
        if (!pixels || width <= 0 || height <= 0) return false;
        convert_bgra16_to_rgba8(pixels, width, height, type, tmp, rlen, srows, spix);
        pixels = tmp.data();
        type = GL_UNSIGNED_BYTE;
        return true;
    }
    if (format != GL_BGRA_EXT) return false;
    format = GL_RGBA;
    if (!pixels || width <= 0 || height <= 0) return false;
    if (type == GL_UNSIGNED_BYTE) {
        convert_bgra_to_rgba(pixels, width, height, tmp, rlen, srows, spix);
        pixels = tmp.data();
        return true;
    }
    if (type == GL_UNSIGNED_SHORT_4_4_4_4_REV || type == GL_UNSIGNED_SHORT_1_5_5_5_REV) {
        convert_bgra16_to_rgba8(pixels, width, height, type, tmp, rlen, srows, spix);
        pixels = tmp.data();
        type = GL_UNSIGNED_BYTE;
        return true;
    }
    return false;
}

static int g_tex_stor_logs = 0;
static int g_pvrtc_logs = 0;

struct TexPlaneMeta {
    int width = 0;
    int height = 0;
    GLenum orig_fmt = 0;
    uint8_t base_plane = 1;
    bool writes_ui_stencil = false;
    std::string asset;
};

static TexPlaneMeta g_tex_meta[8192];
static GLuint g_plane_fbo[5] = {0, 0, 0, 0, 0};
static GLuint g_plane_tex[5] = {0, 0, 0, 0, 0};
static GLuint g_shared_stencil_rbo = 0;
static int g_plane_w = 0;
static int g_plane_h = 0;
static int g_last_world_plane = 1;
static bool g_after_floor = false;

static std::string asset_for_pointer(const void *p, int w, int h);

// Plane for an ftl.dat / mod asset path. -1 = unknown.
static int plane_for_asset(const std::string &a) {
    auto has = [&](const char *k) { return a.find(k) != std::string::npos; };
    auto starts = [&](const char *k) { return a.compare(0, strlen(k), k) == 0; };
    if (a.empty()) return -1;
    // 0: far space, planets, nebulae, menu backdrop
    if (has("img/stars/") || has("/stars/") || has("atlas/planet_") || has("img/effects/pulsar") ||
        has("img/asteroids/") || has("atlas/fleet") || has("main_menus/main_base") ||
        has("customizeUI/custom_background"))
        return 0;
    // Window panes (enemy panel, event dialogs) belong to their window on the UI plane.
    // They write the UI stencil, so sprites drawn on top (enemy ship) stay on the card.
    if (has("img/window_base") || has("img/window_mask") || has("img/window_outline")) return 4;
    // 3: crew & drones
    if (has("atlas/people/") || has("img/people/")) return 3;
    // 2: interiors, systems, effects, weapons
    if (has("atlas/ship_effects") || has("atlas/weapons") || has("img/misc/beam") || has("img/effects/") ||
        (has("img/ship/") && has("_floor")))
        return 2;
    // 1: hulls (player img/ship/*_base, enemy img/ships_glow|ships_noglow/*_base), gibs, cloaks, shields
    if (has("img/ship/") || has("img/ships_glow/") || has("img/ships_noglow/")) return 1;
    (void)starts;
    // 4: everything else is interface
    return 4;
}

static void register_tex_plane_meta(GLsizei width, GLsizei height, GLenum orig_fmt, const void *data) {
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    if (bound <= 0 || bound >= 8192) return;
    TexPlaneMeta &m = g_tex_meta[bound];
    m.width = width;
    m.height = height;
    m.orig_fmt = orig_fmt;
    m.writes_ui_stencil = false;
    m.asset = asset_for_pointer(data, width, height);
    if (m.asset.empty() && orig_fmt != 0x8229) m.asset = pending_tex_claim(width, height);

    int p = -1;
    if (orig_fmt == 0x8229 /* GL_R8 font glyph cache */) p = 4;
    else if (!m.asset.empty()) p = plane_for_asset(m.asset);
    if (p < 0) p = pvrtc_is_supported_format(orig_fmt) ? 0 : 4;
    m.base_plane = (uint8_t)p;
    m.writes_ui_stencil = m.asset.find("img/window_base") != std::string::npos ||
                          m.asset.find("img/window_mask") != std::string::npos;
    static int s_logs = 0;
    if (s_logs < 600) {
        s_logs++;
        LOGI("Diorama tex %d %dx%d fmt=0x%x asset='%s' -> plane %d", bound, width, height, orig_fmt, m.asset.c_str(), p);
    }
}

// glTexStorage2D carries no data; tag on the first sub-image upload instead.
static void tex_meta_tag_late(const void *data) {
    if (!data) return;
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    if (bound <= 0 || bound >= 8192) return;
    TexPlaneMeta &m = g_tex_meta[bound];
    if (!m.asset.empty() || m.width == 0 || m.orig_fmt == 0x8229) return;
    m.asset = asset_for_pointer(data, m.width, m.height);
    if (m.asset.empty()) return;
    int p = plane_for_asset(m.asset);
    if (p >= 0) m.base_plane = (uint8_t)p;
    LOGI("Diorama tex %d late tag asset='%s' -> plane %d", bound, m.asset.c_str(), p);
}

static void ensure_plane_fbos(int w, int h) {
    if (g_plane_fbo[0] != 0 && g_plane_w == w && g_plane_h == h) return;
    g_plane_w = w;
    g_plane_h = h;

    GLint prev_tex = 0, prev_fbo = 0, prev_rbo = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rbo);

    if (!g_shared_stencil_rbo) glGenRenderbuffers(1, &g_shared_stencil_rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, g_shared_stencil_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, w, h);

    if (!g_plane_fbo[0]) {
        glGenFramebuffers(5, g_plane_fbo);
        glGenTextures(5, g_plane_tex);
    }
    for (int i = 0; i < 5; i++) {
        glBindTexture(GL_TEXTURE_2D, g_plane_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_plane_fbo[i]);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_plane_tex[i], 0);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_shared_stencil_rbo);
    }
    LOGI("Initialized 5-Plane Diorama FBOs (%dx%d): fbos=[%u,%u,%u,%u,%u] texs=[%u,%u,%u,%u,%u]",
         w, h, g_plane_fbo[0], g_plane_fbo[1], g_plane_fbo[2], g_plane_fbo[3], g_plane_fbo[4],
         g_plane_tex[0], g_plane_tex[1], g_plane_tex[2], g_plane_tex[3], g_plane_tex[4]);

    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)prev_rbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_fbo);
}

uint32_t darwin_get_plane_tex(int plane) {
    return (plane >= 0 && plane < 5) ? g_plane_tex[plane] : 0;
}

uint32_t darwin_get_plane_fbo(int plane) {
    return (plane >= 0 && plane < 5) ? g_plane_fbo[plane] : 0;
}

static void diag_glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) {
    if (target == GL_TEXTURE_2D && width > 0 && height > 0) {
        register_tex_plane_meta(width, height, internalformat, nullptr);
    }
    GLenum mapped = internalformat;
    if (mapped == GL_BGRA_EXT || mapped == GL_BGRA8_EXT || mapped == GL_RGBA ||
        mapped == GL_RGBA4 || mapped == GL_RGB5_A1 || pvrtc_is_supported_format(mapped)) {
        mapped = GL_RGBA8;
    } else if (mapped == GL_RGB || mapped == GL_RGB565) {
        mapped = GL_RGB8;
    }
    if (g_tex_stor_logs < 80) {
        LOGI("GL glTexStorage2D(t=0x%x, lvls=%d, intFmt=0x%x->0x%x, %dx%d)",
             target, levels, internalformat, mapped, width, height);
        g_tex_stor_logs++;
    }
    glTexStorage2D(target, levels, mapped, width, height);
}

static void diag_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data) {
    if (target == GL_TEXTURE_2D && level == 0 && width > 0 && height > 0) {
        register_tex_plane_meta(width, height, internalformat, data);
    }
    if (pvrtc_is_supported_format(internalformat)) {
        std::vector<uint8_t> rgba;
        if (pvrtc_decode_to_rgba8(data, (size_t)std::max(0, imageSize), width, height, internalformat, rgba)) {
            GLint rlen = 0, srows = 0, spix = 0;
            reset_unpack_state(rlen, srows, spix);
            glTexImage2D(target, level, GL_RGBA8, width, height, border, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            restore_unpack_state(rlen, srows, spix);
            if (g_pvrtc_logs < 16) {
                LOGI("GL PVRTC->RGBA8 glCompressedTexImage2D(lvl=%d, fmt=0x%x, %dx%d, bytes=%d)",
                     level, internalformat, width, height, imageSize);
                g_pvrtc_logs++;
            }
            return;
        }
    }
    glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
}

static void diag_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void *data) {
    if (level == 0) tex_meta_tag_late(data);
    if (pvrtc_is_supported_format(format)) {
        std::vector<uint8_t> rgba;
        if (pvrtc_decode_to_rgba8(data, (size_t)std::max(0, imageSize), width, height, format, rgba)) {
            GLint rlen = 0, srows = 0, spix = 0;
            reset_unpack_state(rlen, srows, spix);
            glTexSubImage2D(target, level, xoffset, yoffset, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            restore_unpack_state(rlen, srows, spix);
            if (g_pvrtc_logs < 16) {
                LOGI("GL PVRTC->RGBA8 glCompressedTexSubImage2D(lvl=%d, %d,%d %dx%d, fmt=0x%x, bytes=%d)",
                     level, xoffset, yoffset, width, height, format, imageSize);
                g_pvrtc_logs++;
            }
            return;
        }
    }
    glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, imageSize, data);
}

static void diag_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels) {
    if (target == GL_TEXTURE_2D && level == 0 && width > 0 && height > 0) {
        register_tex_plane_meta(width, height, (GLenum)internalformat, pixels);
    }
    if (internalformat == GL_BGRA_EXT || internalformat == GL_BGRA8_EXT ||
        internalformat == GL_RGBA4 || internalformat == GL_RGB5_A1) {
        internalformat = GL_RGBA8;
    }
    std::vector<uint8_t> tmp;
    GLint rlen = 0, srows = 0, spix = 0;
    bool converted = maybe_convert_tex_pixels(format, type, pixels, width, height, tmp, rlen, srows, spix);
    if (converted && internalformat == GL_RGBA) {
        internalformat = GL_RGBA8;
    }
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    if (converted) restore_unpack_state(rlen, srows, spix);
    if (g_gl_tex_logs < 16) {
        LOGI("GL glTexImage2D(t=0x%x, lvl=%d, intFmt=0x%x, %dx%d, fmt=0x%x, type=0x%x)",
             target, level, internalformat, width, height, format, type);
        g_gl_tex_logs++;
    }
}

static void diag_glTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels) {
    if (level == 0) tex_meta_tag_late(pixels);
    std::vector<uint8_t> tmp;
    GLint rlen = 0, srows = 0, spix = 0;
    bool converted = maybe_convert_tex_pixels(format, type, pixels, width, height, tmp, rlen, srows, spix);
    glTexSubImage2D(target, level, xoff, yoff, width, height, format, type, pixels);
    if (converted) restore_unpack_state(rlen, srows, spix);
    if (type != GL_UNSIGNED_BYTE || g_gl_tex_logs < 32) {
        LOGI("GL glTexSubImage2D(lvl=%d, %d,%d %dx%d, fmt=0x%x, type=0x%x, conv=%d)",
             level, xoff, yoff, width, height, format, type, converted ? 1 : 0);
        if (type == GL_UNSIGNED_BYTE) g_gl_tex_logs++;
    }
}

static void trace_event(const char *fmt, ...);
static void trace_frame_boundary(int frame);
static bool trace_on();

static void diag_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    trace_event("VIEWPORT %d %d %d %d", x, y, w, h);
    if (g_gl_vp_logs < 8) {
        LOGI("GL glViewport(%d, %d, %d, %d)", x, y, w, h);
        g_gl_vp_logs++;
    }
    glViewport(x, y, w, h);
}

// ---------------------------------------------------------------------------
// 5-plane diorama routing (debug/experimental; OFF unless planes are enabled)
//
// Every draw into FTL's main FBO is executed normally (so the visible frame is
// unchanged) and then replayed into one of five offscreen plane FBOs:
//   0 = far space / planets   1 = hulls / hangar   2 = rooms / weapons / fx
//   3 = crew                  4 = UI / fonts
// Planes 1..4 are stored PREMULTIPLIED (cleared to 0,0,0,0) so additive glows
// survive compositing: rgb keeps the game's blend factors, alpha uses
// (ONE, ONE_MINUS_SRC_ALPHA) for "over" draws and (ZERO, ONE) for additive /
// multiplicative draws. All GL state touched here is saved and restored.
// ---------------------------------------------------------------------------
static std::atomic<bool> g_planes_enabled{false};
static std::atomic<bool> g_planes_pending{false};
static GLuint g_main_fbo = 1;
static int g_main_w = 1024;
static int g_main_h = 768;
static bool g_planes_frame_active = false;
static int g_plane_draws[5] = {0, 0, 0, 0, 0};
// FTL renders the scene into its own FBO (fbo 2, colour tex 1228x768 on Quest) and then
// composites it into the drawable FBO with one quad. The scene FBO is discovered from that
// composite draw (main FBO sampling an FBO's colour texture) and becomes the routing target.
static GLuint g_scene_fbo = 0;
static int g_scene_w = 0, g_scene_h = 0;
static std::unordered_map<GLuint, GLuint> g_fbo_color_tex; // fbo -> color texture attachment
static std::unordered_map<GLuint, GLuint> g_fbo_color_rb;  // fbo -> color renderbuffer attachment
static GLuint route_fbo() { return g_scene_fbo ? g_scene_fbo : g_main_fbo; }

void darwin_set_main_fbo(uint32_t fbo, int w, int h) {
    if (fbo) g_main_fbo = fbo;
    if (w > 0 && h > 0) { g_main_w = w; g_main_h = h; }
    LOGI("Diorama: main FBO=%u (%dx%d)", g_main_fbo, g_main_w, g_main_h);
}

// Applied only at frame boundaries (called from present) so a frame is never half-routed.
void darwin_planes_request(bool enabled) { g_planes_pending.store(enabled); }
void darwin_planes_frame_boundary() {
    bool want = g_planes_pending.load();
    if (want != g_planes_enabled.load()) {
        g_planes_enabled.store(want);
        LOGI("Diorama plane routing %s", want ? "ENABLED" : "DISABLED");
    }
    g_planes_frame_active = false;
    static int s_frame = 0;
    trace_frame_boundary(s_frame++);
}
bool darwin_planes_enabled() { return g_planes_enabled.load(); }

static void diag_glClear(GLbitfield mask) {
    if (trace_on()) {
        GLint tf = 0; GLfloat cc[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &tf);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
        trace_event("CLEAR fbo=%d mask=0x%x color=%.2f,%.2f,%.2f,%.2f scissor=%d", tf, mask, cc[0], cc[1], cc[2], cc[3],
                    glIsEnabled(GL_SCISSOR_TEST) ? 1 : 0);
    }
    if (!g_planes_enabled.load(std::memory_order_relaxed)) { glClear(mask); return; }
    GLint fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glClear(mask);
    if ((GLuint)fbo != route_fbo() || !(mask & GL_COLOR_BUFFER_BIT)) return;

    if (g_scene_fbo) ensure_plane_fbos(g_scene_w, g_scene_h);
    else ensure_plane_fbos(g_main_w, g_main_h);
    GLfloat saved_cc[4] = {0, 0, 0, 0};
    GLint saved_cs = 0;
    GLboolean saved_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean saved_cmask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLint saved_smask = 0xFF;
    glGetFloatv(GL_COLOR_CLEAR_VALUE, saved_cc);
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &saved_cs);
    glGetBooleanv(GL_COLOR_WRITEMASK, saved_cmask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &saved_smask);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xFF);

    // Plane 0 is the opaque backdrop; it starts as the game's clear colour.
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_plane_fbo[0]);
    glClearColor(saved_cc[0], saved_cc[1], saved_cc[2], 1.0f);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 1; i < 5; i++) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_plane_fbo[i]);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glClearColor(saved_cc[0], saved_cc[1], saved_cc[2], saved_cc[3]);
    glClearStencil(saved_cs);
    glColorMask(saved_cmask[0], saved_cmask[1], saved_cmask[2], saved_cmask[3]);
    glStencilMask((GLuint)saved_smask);
    if (saved_scissor) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)fbo);
    g_last_world_plane = 0;
    g_after_floor = false;
    g_planes_frame_active = true;
    for (int i = 0; i < 5; i++) g_plane_draws[i] = 0;
}

struct SavedRouteState {
    GLint fbo;
    GLboolean blend, stencil;
    GLint src_rgb, dst_rgb, src_a, dst_a;
    GLint s_func, s_ref, s_vmask, s_fail, s_zfail, s_zpass, s_wmask;
};

static void save_route_state(SavedRouteState &s, GLint fbo) {
    s.fbo = fbo;
    s.blend = glIsEnabled(GL_BLEND);
    s.stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s.src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &s.dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.dst_a);
    glGetIntegerv(GL_STENCIL_FUNC, &s.s_func);
    glGetIntegerv(GL_STENCIL_REF, &s.s_ref);
    glGetIntegerv(GL_STENCIL_VALUE_MASK, &s.s_vmask);
    glGetIntegerv(GL_STENCIL_FAIL, &s.s_fail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &s.s_zfail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &s.s_zpass);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &s.s_wmask);
}

static void restore_route_state(const SavedRouteState &s) {
    if (s.blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFuncSeparate((GLenum)s.src_rgb, (GLenum)s.dst_rgb, (GLenum)s.src_a, (GLenum)s.dst_a);
    if (s.stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    glStencilFunc((GLenum)s.s_func, s.s_ref, (GLuint)s.s_vmask);
    glStencilOp((GLenum)s.s_fail, (GLenum)s.s_zfail, (GLenum)s.s_zpass);
    glStencilMask((GLuint)s.s_wmask);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)s.fbo);
}

extern "C++" {
template <typename DrawFn>
static void route_draw_to_planes(DrawFn draw, GLint orig_fbo) {
    if ((GLuint)orig_fbo == g_main_fbo && !g_scene_fbo) {
        // Composite of an offscreen scene FBO into the drawable? Adopt it as routing target.
        GLint t = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &t);
        for (auto &kv : g_fbo_color_tex) {
            if (kv.second == (GLuint)t && kv.first != g_main_fbo && t > 0 && t < 8192 &&
                g_tex_meta[t].width >= 512 && g_tex_meta[t].height >= 384) {
                bool ours = false;
                for (int i = 0; i < 5; i++) if (g_plane_fbo[i] == kv.first) ours = true;
                if (ours) continue;
                g_scene_fbo = kv.first;
                g_scene_w = g_tex_meta[t].width;
                g_scene_h = g_tex_meta[t].height;
                g_planes_frame_active = false; // start routing at the next clear of the scene FBO
                LOGI("Diorama: scene FBO=%u (tex %d, %dx%d) composited into main FBO %u", g_scene_fbo, t,
                     g_scene_w, g_scene_h, g_main_fbo);
                return;
            }
        }
    }
    if (!g_planes_frame_active || (GLuint)orig_fbo != route_fbo() || g_plane_fbo[0] == 0) return;

    GLint bound_tex = 0, prog = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound_tex);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);

    int plane = g_last_world_plane;
    bool writes_stencil = false;
    bool is_untextured = bound_tex == 0 || (prog > 0 && prog < 4096 && g_prog_has_sampler[prog] == 2);

    if (!is_untextured && bound_tex > 0 && bound_tex < 8192 && g_tex_meta[bound_tex].width > 0) {
        const TexPlaneMeta &m = g_tex_meta[bound_tex];
        plane = m.base_plane;
        writes_stencil = m.writes_ui_stencil;
        // Engine flames: ship_effects sprites drawn right after a ship's floor and before its
        // rooms (first untextured draw) belong to the hull, not to the interior effects.
        if (m.asset.find("_floor") != std::string::npos) g_after_floor = true;
        else if (g_after_floor && m.asset.find("atlas/ship_effects") != std::string::npos) plane = 1;
        else if (g_after_floor) g_after_floor = false;
        g_last_world_plane = plane;
    } else if (is_untextured) {
        g_after_floor = false;
        plane = (g_last_world_plane <= 1) ? 2 : g_last_world_plane;
        // Solid UI boxes (crew list, tooltips, panels) mark stencil=4 so world sprites drawn
        // on top of them (crew portraits, system icons) stay on the UI plane (pass B below).
        writes_stencil = (plane == 4);
    }

    SavedRouteState s;
    save_route_state(s, orig_fbo);

    // Premultiplied plane blending derived from the game's current rgb factors.
    if (s.blend) {
        GLenum a_src = GL_ONE, a_dst = GL_ONE_MINUS_SRC_ALPHA;
        if ((GLenum)s.dst_rgb == GL_ONE ||                       // additive glow
            (GLenum)s.src_rgb == GL_DST_COLOR || (GLenum)s.src_rgb == GL_ZERO) { // multiply
            a_src = GL_ZERO; a_dst = GL_ONE;
        }
        glBlendFuncSeparate((GLenum)s.src_rgb, (GLenum)s.dst_rgb, a_src, a_dst);
    }
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);

    if (plane == 4) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_plane_fbo[4]);
        glStencilFunc(GL_ALWAYS, 4, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, writes_stencil ? GL_REPLACE : GL_KEEP);
        draw();
    } else if (plane == 0) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_plane_fbo[0]);
        glStencilFunc(GL_ALWAYS, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        draw();
    } else {
        // Pass A: world space (outside any UI box stencilled by plane 4)
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_plane_fbo[plane]);
        glStencilFunc(GL_NOTEQUAL, 4, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        draw();
        // Pass B: icons / portraits drawn inside a UI box stay on the UI plane
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_plane_fbo[4]);
        glStencilFunc(GL_EQUAL, 4, 0xFF);
        draw();
    }
    g_plane_draws[plane]++;
    restore_route_state(s);
}
} // extern "C++"

// --- One-frame GL trace (touch files/.trace_frame) --------------------------
// Records FBO binds/attachments, clears, viewports, program switches and draws
// (run-length merged by fbo/tex/prog) for exactly one frame into files/trace.txt.
static std::atomic<bool> g_trace_pending{false};
static bool g_trace_active = false;
static FILE *g_trace_fp = nullptr;
static int g_trace_frame = 0;
struct TraceRun { GLint fbo = -1, tex = -1, prog = -1; GLenum mode = 0; int draws = 0; long verts = 0; int plane = -1; int st = 0; GLint bs = 0, bd = 0; };
static TraceRun g_trace_run;
void darwin_trace_request() { g_trace_pending.store(true); }
uint32_t darwin_get_scene_fbo(int *w, int *h) {
    if (g_scene_fbo) { if (w) *w = g_scene_w; if (h) *h = g_scene_h; return g_scene_fbo; }
    if (w) *w = g_main_w;
    if (h) *h = g_main_h;
    return g_main_fbo;
}
void darwin_get_scene_size(int *w, int *h) { darwin_get_scene_fbo(w, h); }
bool darwin_scene_known() { return g_scene_fbo != 0; }
void darwin_get_plane_size(int *w, int *h) { if (w) *w = g_plane_w; if (h) *h = g_plane_h; }
static bool trace_on() { return g_trace_active; }

static void trace_flush_run() {
    if (!g_trace_active || !g_trace_fp || g_trace_run.draws == 0) return;
    const TraceRun &r = g_trace_run;
    int tw = 0, th = 0;
    const char *an = "";
    if (r.tex > 0 && r.tex < 8192) { tw = g_tex_meta[r.tex].width; th = g_tex_meta[r.tex].height; an = g_tex_meta[r.tex].asset.c_str(); }
    fprintf(g_trace_fp, "  DRAW fbo=%d prog=%d tex=%d(%dx%d %s) mode=%u x%d verts=%ld plane=%d stencil=%d blend=%x/%x\n",
            r.fbo, r.prog, r.tex, tw, th, an, r.mode, r.draws, r.verts, r.plane, r.st, r.bs, r.bd);
    g_trace_run = TraceRun();
}

static void trace_event(const char *fmt, ...) {
    if (!g_trace_active || !g_trace_fp) return;
    trace_flush_run();
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_trace_fp, fmt, ap);
    va_end(ap);
    fputc('\n', g_trace_fp);
}

static void trace_draw(GLenum mode, long verts, int plane) {
    if (!g_trace_active) return;
    GLint fbo = 0, tex = 0, prog = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    int st = glIsEnabled(GL_STENCIL_TEST) ? 1 : 0;
    GLint bs = 0, bd = 0;
    if (glIsEnabled(GL_BLEND)) { glGetIntegerv(GL_BLEND_SRC_RGB, &bs); glGetIntegerv(GL_BLEND_DST_RGB, &bd); }
    TraceRun &r = g_trace_run;
    if (r.draws && (r.fbo != fbo || r.tex != tex || r.prog != prog || r.mode != mode || r.plane != plane ||
                    r.st != st || r.bs != bs || r.bd != bd)) trace_flush_run();
    r.fbo = fbo; r.tex = tex; r.prog = prog; r.mode = mode; r.plane = plane; r.st = st; r.bs = bs; r.bd = bd;
    r.draws++; r.verts += verts;
}

static void trace_frame_boundary(int frame) {
    if (g_trace_active) {
        trace_flush_run();
        fprintf(g_trace_fp, "== END frame %d\n", frame);
        fclose(g_trace_fp);
        g_trace_fp = nullptr;
        g_trace_active = false;
        LOGI("Diorama trace written (frame %d)", g_trace_frame);
    }
    if (g_trace_pending.exchange(false)) {
        g_trace_fp = fopen("/sdcard/Android/data/com.subsetgames.ftl/files/trace.txt", "w");
        if (g_trace_fp) {
            g_trace_active = true;
            g_trace_frame = frame;
            fprintf(g_trace_fp, "== BEGIN frame %d main_fbo=%u (%dx%d) planes=%d\n", frame, g_main_fbo, g_main_w, g_main_h,
                    g_planes_enabled.load() ? 1 : 0);
            for (auto &kv : g_fbo_color_tex) {
                GLuint t = kv.second;
                fprintf(g_trace_fp, "  fbo %u -> color tex %u (%dx%d)\n", kv.first, t,
                        t < 8192 ? g_tex_meta[t].width : 0, t < 8192 ? g_tex_meta[t].height : 0);
            }
            for (auto &kv : g_fbo_color_rb) fprintf(g_trace_fp, "  fbo %u -> color rb %u\n", kv.first, kv.second);
        }
    }
}

static void diag_glBindFramebuffer(GLenum target, GLuint fbo) {
    glBindFramebuffer(target, fbo);
    trace_event("BIND target=0x%x fbo=%u", target, fbo);
}

static void diag_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint tex, GLint level) {
    glFramebufferTexture2D(target, attachment, textarget, tex, level);
    GLint fbo = 0;
    glGetIntegerv(target == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER_BINDING : GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    if (attachment == GL_COLOR_ATTACHMENT0) g_fbo_color_tex[(GLuint)fbo] = tex;
    LOGI("Diorama FBO %d: attach tex %u (%dx%d) to 0x%x", fbo, tex,
         tex < 8192 ? g_tex_meta[tex].width : 0, tex < 8192 ? g_tex_meta[tex].height : 0, attachment);
    trace_event("ATTACH fbo=%d tex=%u att=0x%x", fbo, tex, attachment);
}

static void diag_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb) {
    glFramebufferRenderbuffer(target, attachment, rbtarget, rb);
    GLint fbo = 0;
    glGetIntegerv(target == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER_BINDING : GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    if (attachment == GL_COLOR_ATTACHMENT0) g_fbo_color_rb[(GLuint)fbo] = rb;
    LOGI("Diorama FBO %d: attach rb %u to 0x%x", fbo, rb, attachment);
    trace_event("ATTACH_RB fbo=%d rb=%u att=0x%x", fbo, rb, attachment);
}

static void diag_glUseProgram(GLuint prog) {
    glUseProgram(prog);
    // Program switches are captured per draw-run; no separate event.
}

static void diag_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    glScissor(x, y, w, h);
    trace_event("SCISSOR %d %d %d %d", x, y, w, h);
}

static void diag_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (!g_planes_enabled.load(std::memory_order_relaxed)) {
        glDrawArrays(mode, first, count);
        trace_draw(mode, count, -1);
        return;
    }
    GLint fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glDrawArrays(mode, first, count);
    int before[5]; memcpy(before, g_plane_draws, sizeof(before));
    route_draw_to_planes([&] { glDrawArrays(mode, first, count); }, fbo);
    int plane = -1;
    for (int i = 0; i < 5; i++) if (g_plane_draws[i] != before[i]) plane = i;
    trace_draw(mode, count, plane);
}

static void diag_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    if (!g_planes_enabled.load(std::memory_order_relaxed)) {
        glDrawElements(mode, count, type, indices);
        trace_draw(mode, count, -1);
        return;
    }
    GLint fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glDrawElements(mode, count, type, indices);
    int before[5]; memcpy(before, g_plane_draws, sizeof(before));
    route_draw_to_planes([&] { glDrawElements(mode, count, type, indices); }, fbo);
    int plane = -1;
    for (int i = 0; i < 5; i++) if (g_plane_draws[i] != before[i]) plane = i;
    trace_draw(mode, count, plane);
}

// --- Plane dump (PNG, un-premultiplied RGBA, top-down) ----------------------
static void png_chunk(FILE *fp, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t be[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    fwrite(be, 1, 4, fp);
    fwrite(type, 1, 4, fp);
    if (len) fwrite(data, 1, len, fp);
    uLong crc = crc32(0L, (const Bytef *)type, 4);
    if (len) crc = crc32(crc, data, len);
    uint8_t cb[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
    fwrite(cb, 1, 4, fp);
}

static bool write_png_rgba(const char *path, const uint8_t *rgba_bottom_up, int w, int h) {
    std::vector<uint8_t> raw((size_t)(w * 4 + 1) * h);
    for (int y = 0; y < h; y++) {
        uint8_t *dst = &raw[(size_t)(w * 4 + 1) * y];
        dst[0] = 0;
        memcpy(dst + 1, rgba_bottom_up + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
    }
    uLongf zlen = compressBound((uLong)raw.size());
    std::vector<uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), (uLong)raw.size(), 3) != Z_OK) return false;
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(sig, 1, 8, fp);
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                        8, 6, 0, 0, 0};
    png_chunk(fp, "IHDR", ihdr, 13);
    png_chunk(fp, "IDAT", z.data(), (uint32_t)zlen);
    png_chunk(fp, "IEND", nullptr, 0);
    fclose(fp);
    return true;
}

// Call on the GL thread after the frame is complete (before swap).
void darwin_dump_planes(const char *dir, int frame) {
    if (!g_planes_enabled.load() || g_plane_fbo[0] == 0) {
        LOGI("Diorama dump skipped (planes %s)", g_planes_enabled.load() ? "not allocated" : "disabled");
        return;
    }
    const int w = g_plane_w, h = g_plane_h;
    std::vector<uint8_t> px((size_t)w * h * 4);
    GLint prev_read = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    for (int p = 0; p < 5; p++) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_plane_fbo[p]);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        size_t covered = 0;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t *q = &px[i * 4];
            const uint8_t a = q[3];
            if (a) covered++;
            if (p > 0 && a > 0 && a < 255) { // un-premultiply for viewing
                q[0] = (uint8_t)std::min(255, q[0] * 255 / a);
                q[1] = (uint8_t)std::min(255, q[1] * 255 / a);
                q[2] = (uint8_t)std::min(255, q[2] * 255 / a);
            }
        }
        char path[256];
        snprintf(path, sizeof(path), "%s/plane%d.png", dir, p);
        bool ok = write_png_rgba(path, px.data(), w, h);
        LOGI("Diorama dump plane%d -> %s ok=%d coverage=%.1f%% draws=%d (frame=%d %dx%d)",
             p, path, ok ? 1 : 0, 100.0 * (double)covered / ((double)w * h), g_plane_draws[p], frame, w, h);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
}

void *darwin_dlsym(void *handle, const char *symbol) {
    if (!symbol) return nullptr;
    if (handle == (void *)(intptr_t)-2 || handle == (void *)(intptr_t)-1) {
        if (strcmp(symbol, "glShaderSource") == 0) return (void *)diag_glShaderSource;
        if (strcmp(symbol, "glCompileShader") == 0) return (void *)diag_glCompileShader;
        if (strcmp(symbol, "glLinkProgram") == 0) return (void *)diag_glLinkProgram;
        if (strcmp(symbol, "glTexStorage2D") == 0) return (void *)diag_glTexStorage2D;
        if (strcmp(symbol, "glCompressedTexImage2D") == 0 || strcmp(symbol, "glCompressedTexImage2DARB") == 0) {
            return (void *)diag_glCompressedTexImage2D;
        }
        if (strcmp(symbol, "glCompressedTexSubImage2D") == 0 || strcmp(symbol, "glCompressedTexSubImage2DARB") == 0) {
            return (void *)diag_glCompressedTexSubImage2D;
        }
        if (strcmp(symbol, "glTexImage2D") == 0) return (void *)diag_glTexImage2D;
        if (strcmp(symbol, "glTexSubImage2D") == 0) return (void *)diag_glTexSubImage2D;
        if (strcmp(symbol, "glViewport") == 0) return (void *)diag_glViewport;
        if (strcmp(symbol, "glClear") == 0) return (void *)diag_glClear;
        if (strcmp(symbol, "glDrawArrays") == 0) return (void *)diag_glDrawArrays;
        if (strcmp(symbol, "glDrawElements") == 0) return (void *)diag_glDrawElements;
        if (strcmp(symbol, "glBindFramebuffer") == 0 || strcmp(symbol, "glBindFramebufferOES") == 0) return (void *)diag_glBindFramebuffer;
        if (strcmp(symbol, "glFramebufferTexture2D") == 0 || strcmp(symbol, "glFramebufferTexture2DOES") == 0) return (void *)diag_glFramebufferTexture2D;
        if (strcmp(symbol, "glFramebufferRenderbuffer") == 0 || strcmp(symbol, "glFramebufferRenderbufferOES") == 0) return (void *)diag_glFramebufferRenderbuffer;
        if (strcmp(symbol, "glUseProgram") == 0) return (void *)diag_glUseProgram;
        if (strcmp(symbol, "glScissor") == 0) return (void *)diag_glScissor;
        if (strncmp(symbol, "gl", 2) == 0) {
            static std::mutex s_gl_names_mu;
            static std::vector<std::string> s_gl_names;
            std::lock_guard<std::mutex> lk(s_gl_names_mu);
            if (std::find(s_gl_names.begin(), s_gl_names.end(), symbol) == s_gl_names.end()) {
                s_gl_names.push_back(symbol);
                LOGI("Diorama dlsym GL: %s", symbol);
            }
        }
        // Darwin RTLD_DEFAULT / RTLD_NEXT (from mmap'd Mach-O caller): check shim + RTLD_DEFAULT
        std::string underscored = std::string("_") + symbol;
        void *s = macho_resolve_symbol(underscored.c_str());
        if (s) return s;
        return dlsym(RTLD_DEFAULT, symbol);
    }
    return dlsym(handle, symbol);
}

long darwin_sysconf(int name) {
    switch (name) {
        case 29: return sysconf(_SC_PAGESIZE);         // Darwin _SC_PAGESIZE
        case 57: return sysconf(_SC_NPROCESSORS_CONF); // Darwin _SC_NPROCESSORS_CONF
        case 58: return sysconf(_SC_NPROCESSORS_ONLN); // Darwin _SC_NPROCESSORS_ONLN
        default: return sysconf(name);
    }
}

// ============================================================================
// Darwin pthread / sync ABI bridges
// ============================================================================

static pthread_mutex_t g_darwin_sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_darwin_once_cv = PTHREAD_COND_INITIALIZER;
static std::unordered_map<void *, int> g_darwin_once_states;

int darwin_pthread_once(void *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine) return EINVAL;
    pthread_mutex_lock(&g_darwin_sync_lock);
    auto it = g_darwin_once_states.find(once_control);
    if (it != g_darwin_once_states.end()) {
        while (it->second == 1) {
            pthread_cond_wait(&g_darwin_once_cv, &g_darwin_sync_lock);
            it = g_darwin_once_states.find(once_control);
        }
        if (it != g_darwin_once_states.end() && it->second == 2) {
            pthread_mutex_unlock(&g_darwin_sync_lock);
            return 0;
        }
    }
    g_darwin_once_states[once_control] = 1;
    pthread_mutex_unlock(&g_darwin_sync_lock);

    init_routine();

    pthread_mutex_lock(&g_darwin_sync_lock);
    g_darwin_once_states[once_control] = 2;
    pthread_cond_broadcast(&g_darwin_once_cv);
    pthread_mutex_unlock(&g_darwin_sync_lock);
    return 0;
}

int darwin_pthread_setname_np(const char *name) {
    if (!name) return 0;
    char buf[16];
    strncpy(buf, name, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return pthread_setname_np(pthread_self(), buf);
}

int darwin_pthread_key_create(uint64_t *key, void (*destructor)(void *)) {
    if (!key) return EINVAL;
    *key = 0;
    pthread_key_t k = 0;
    int res = pthread_key_create(&k, destructor);
    if (res == 0) *key = (uint64_t)(uint32_t)k;
    return res;
}

int darwin_pthread_mutexattr_settype(void *attr, int type) {
    // Darwin: NORMAL=0, ERRORCHECK=1, RECURSIVE=2
    // Bionic: NORMAL=0, RECURSIVE=1, ERRORCHECK=2
    int btype = PTHREAD_MUTEX_NORMAL;
    if (type == 1) btype = PTHREAD_MUTEX_ERRORCHECK;
    else if (type == 2) btype = PTHREAD_MUTEX_RECURSIVE;
    return pthread_mutexattr_settype((pthread_mutexattr_t *)attr, btype);
}

static void ensure_darwin_mutex_init(void *mutex) {
    if (!mutex) return;
    uint64_t sig = *(volatile uint64_t *)mutex;
    if (sig == 0x32AAABA7ULL || sig == 0x32AAABA8ULL || sig == 0x32AAABA9ULL) {
        pthread_mutex_lock(&g_darwin_sync_lock);
        sig = *(volatile uint64_t *)mutex;
        if (sig == 0x32AAABA7ULL || sig == 0x32AAABA8ULL || sig == 0x32AAABA9ULL) {
            pthread_mutexattr_t attr;
            pthread_mutexattr_init(&attr);
            int kind = PTHREAD_MUTEX_NORMAL;
            if (sig == 0x32AAABA8ULL) kind = PTHREAD_MUTEX_RECURSIVE;
            else if (sig == 0x32AAABA9ULL) kind = PTHREAD_MUTEX_ERRORCHECK;
            pthread_mutexattr_settype(&attr, kind);
            memset(mutex, 0, 64);
            pthread_mutex_init((pthread_mutex_t *)mutex, &attr);
            pthread_mutexattr_destroy(&attr);
        }
        pthread_mutex_unlock(&g_darwin_sync_lock);
    }
}

static void ensure_darwin_cond_init(void *cond) {
    if (!cond) return;
    uint64_t sig = *(volatile uint64_t *)cond;
    if (sig == 0x3CB0B1BBULL) {
        pthread_mutex_lock(&g_darwin_sync_lock);
        sig = *(volatile uint64_t *)cond;
        if (sig == 0x3CB0B1BBULL) {
            memset(cond, 0, 48);
            pthread_cond_init((pthread_cond_t *)cond, nullptr);
        }
        pthread_mutex_unlock(&g_darwin_sync_lock);
    }
}

int darwin_pthread_mutex_init(void *mutex, const void *attr) {
    if (!mutex) return EINVAL;
    memset(mutex, 0, 64);
    return pthread_mutex_init((pthread_mutex_t *)mutex, (const pthread_mutexattr_t *)attr);
}

int darwin_pthread_mutex_destroy(void *mutex) {
    if (!mutex) return EINVAL;
    ensure_darwin_mutex_init(mutex);
    return pthread_mutex_destroy((pthread_mutex_t *)mutex);
}

int darwin_pthread_mutex_lock(void *mutex) {
    if (!mutex) return EINVAL;
    ensure_darwin_mutex_init(mutex);
    return pthread_mutex_lock((pthread_mutex_t *)mutex);
}

int darwin_pthread_mutex_trylock(void *mutex) {
    if (!mutex) return EINVAL;
    ensure_darwin_mutex_init(mutex);
    return pthread_mutex_trylock((pthread_mutex_t *)mutex);
}

int darwin_pthread_mutex_unlock(void *mutex) {
    if (!mutex) return EINVAL;
    ensure_darwin_mutex_init(mutex);
    return pthread_mutex_unlock((pthread_mutex_t *)mutex);
}

int darwin_pthread_cond_init(void *cond, const void *attr) {
    if (!cond) return EINVAL;
    memset(cond, 0, 48);
    return pthread_cond_init((pthread_cond_t *)cond, (const pthread_condattr_t *)attr);
}

int darwin_pthread_cond_destroy(void *cond) {
    if (!cond) return EINVAL;
    ensure_darwin_cond_init(cond);
    return pthread_cond_destroy((pthread_cond_t *)cond);
}

int darwin_pthread_cond_signal(void *cond) {
    if (!cond) return EINVAL;
    ensure_darwin_cond_init(cond);
    return pthread_cond_signal((pthread_cond_t *)cond);
}

int darwin_pthread_cond_broadcast(void *cond) {
    if (!cond) return EINVAL;
    ensure_darwin_cond_init(cond);
    return pthread_cond_broadcast((pthread_cond_t *)cond);
}

int darwin_pthread_cond_wait(void *cond, void *mutex) {
    if (!cond || !mutex) return EINVAL;
    ensure_darwin_cond_init(cond);
    ensure_darwin_mutex_init(mutex);
    return pthread_cond_wait((pthread_cond_t *)cond, (pthread_mutex_t *)mutex);
}

int darwin_pthread_cond_timedwait(void *cond, void *mutex, const struct timespec *abstime) {
    if (!cond || !mutex) return EINVAL;
    ensure_darwin_cond_init(cond);
    ensure_darwin_mutex_init(mutex);
    return pthread_cond_timedwait((pthread_cond_t *)cond, (pthread_mutex_t *)mutex, abstime);
}

void darwin_assert_rtn(const char *func, const char *file, int line, const char *msg) {
    LOGE("Assertion failed: (%s), function %s, file %s, line %d.", msg ? msg : "", func ? func : "", file ? file : "", line);
    abort();
}

int darwin_tolower(int c) {
    return tolower((unsigned char)c);
}

int darwin_toupper(int c) {
    return toupper((unsigned char)c);
}

void darwin_bzero(void *s, size_t n) {
    if (s && n > 0) memset(s, 0, n);
}

void darwin_memset_pattern16(void *b, const void *pattern16, size_t len) {
    if (!b || !pattern16 || len == 0) return;
    uint8_t *dst = (uint8_t *)b;
    const uint8_t *pat = (const uint8_t *)pattern16;
    size_t i = 0;
    while (i + 16 <= len) {
        memcpy(dst + i, pat, 16);
        i += 16;
    }
    if (i < len) {
        memcpy(dst + i, pat, len - i);
    }
}

void *darwin_hash_create(int count) {
    (void)count;
    return new std::unordered_map<std::string, void *>();
}

void *darwin_hash_search(void *table, const char *key, void *datum, void **replaced) {
    if (!table || !key) return nullptr;
    auto *map = static_cast<std::unordered_map<std::string, void *> *>(table);
    if (datum == nullptr) {
        auto it = map->find(key);
        return (it != map->end()) ? it->second : nullptr;
    }
    auto it = map->find(key);
    if (replaced) {
        *replaced = (it != map->end()) ? it->second : nullptr;
    }
    (*map)[key] = datum;
    return datum;
}

// ============================================================================
// Math & Architecture Register Shims
// ============================================================================

struct sincosf_res __sincosf_stret(float x) {
    struct sincosf_res r;
    sincosf(x, &r.s, &r.c);
    return r;
}

float ___exp10(float x) {
    return powf(10.0f, x);
}

int *___error(void) {
    return __errno();
}

void _OSMemoryBarrier(void) {
    __sync_synchronize();
}

// ============================================================================
// Mach Timing & Semaphore Subsystem
// ============================================================================

uint64_t _mach_absolute_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int _mach_timebase_info(struct mach_timebase_info_t *info) {
    if (info) {
        info->numer = 1;
        info->denom = 1;
    }
    return 0;
}

int _mach_wait_until(uint64_t deadline) {
    uint64_t now = _mach_absolute_time();
    if (deadline > now) {
        uint64_t diff_ns = deadline - now;
        struct timespec ts;
        ts.tv_sec = diff_ns / 1000000000ULL;
        ts.tv_nsec = diff_ns % 1000000000ULL;
        nanosleep(&ts, nullptr);
    }
    return 0;
}

struct MachSemaphore {
    sem_t sem;
};

static std::unordered_map<int, MachSemaphore*> g_mach_semaphores;
static int g_sem_id_counter = 100;

int _semaphore_create(int task, int *out_sem, int policy, int value) {
    MachSemaphore *s = new MachSemaphore();
    sem_init(&s->sem, 0, value);
    int id = ++g_sem_id_counter;
    g_mach_semaphores[id] = s;
    *out_sem = id;
    return 0;
}

int _semaphore_destroy(int task, int sem_id) {
    auto it = g_mach_semaphores.find(sem_id);
    if (it != g_mach_semaphores.end()) {
        sem_destroy(&it->second->sem);
        delete it->second;
        g_mach_semaphores.erase(it);
    }
    return 0;
}

int _semaphore_signal(int sem_id) {
    auto it = g_mach_semaphores.find(sem_id);
    if (it != g_mach_semaphores.end()) {
        return sem_post(&it->second->sem);
    }
    return -1;
}

int _semaphore_wait(int sem_id) {
    auto it = g_mach_semaphores.find(sem_id);
    if (it != g_mach_semaphores.end()) {
        return sem_wait(&it->second->sem);
    }
    return -1;
}

int _semaphore_timedwait(int sem_id, struct mach_timespec_t wait_time) {
    auto it = g_mach_semaphores.find(sem_id);
    if (it != g_mach_semaphores.end()) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += wait_time.tv_sec;
        ts.tv_nsec += wait_time.tv_nsec;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        return sem_timedwait(&it->second->sem, &ts);
    }
    return -1;
}

int _mach_host_self(void) {
    return 1;
}

int _host_page_size(int host, size_t *out_size) {
    if (out_size) *out_size = 4096;
    return 0;
}

int _host_statistics(int host, int type, void *out_info, unsigned int *count) {
    return 0;
}

int _task_info(int task, int flavor, void *task_info_out, unsigned int *count) {
    return 0;
}

int _sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    (void)newp; (void)newlen;
    if (!name) return -1;
    if (strcmp(name, "hw.machine") == 0) {
        const char *model = "iPad4,1";
        size_t need = strlen(model) + 1;
        if (oldp && oldlenp && *oldlenp > 0) {
            size_t copy_len = (*oldlenp < need) ? *oldlenp : need;
            memcpy(oldp, model, copy_len);
            if (*oldlenp < need) ((char *)oldp)[copy_len - 1] = '\0';
        }
        if (oldlenp) *oldlenp = need;
        return 0;
    }
    if (strcmp(name, "hw.ncpu") == 0) {
        int ncpu = 8;
        if (oldp && oldlenp && *oldlenp >= sizeof(int)) {
            *(int *)oldp = ncpu;
        }
        if (oldlenp) *oldlenp = sizeof(int);
        return 0;
    }
    if (oldlenp) *oldlenp = 0;
    return -1;
}

// ============================================================================
// CoreFoundation & CFString Emulation
// ============================================================================

struct CFStringInternal {
    void *isa;
    uintptr_t flags;
    const char *chars;
    size_t length;
};

void *CFStringCreateWithCStringNoCopy(void *alloc, const char *cStr, uint32_t encoding, void *contentsDeallocator) {
    (void)alloc; (void)encoding; (void)contentsDeallocator;
    CFStringInternal *s = (CFStringInternal *)malloc(sizeof(CFStringInternal));
    s->isa = ___CFConstantStringClassReference;
    s->flags = 0x7C8;
    s->chars = strdup(cStr ? cStr : "");
    s->length = cStr ? strlen(cStr) : 0;
    return s;
}

const char *CFStringGetCStringPtr(void *theString, uint32_t encoding) {
    if (!theString) return "";
    CFStringInternal *s = (CFStringInternal *)theString;
    return s->chars;
}

int CFStringGetCString(void *theString, char *buffer, size_t bufferSize, uint32_t encoding) {
    if (!theString || !buffer || bufferSize == 0) return 0;
    CFStringInternal *s = (CFStringInternal *)theString;
    strncpy(buffer, s->chars, bufferSize - 1);
    buffer[bufferSize - 1] = '\0';
    return 1;
}

void CFRelease(void *cf) {
    // Stub
}

void *CFBundleGetMainBundle(void) {
    return (void *)nsbundle_main_bundle();
}

void *CFBundleGetInfoDictionary(void *bundle) {
    (void)bundle;
    static std::mutex s_info_mutex;
    static id s_info_dict = nil;
    std::lock_guard<std::mutex> lock(s_info_mutex);
    if (!s_info_dict) {
        (void)nsbundle_main_bundle();
        Class dictCls = objc_getClass("NSMutableDictionary");
        if (dictCls) {
            s_info_dict = objc_alloc(dictCls);
            if (s_info_dict) {
                auto msg_init = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend);
                auto msg_set = reinterpret_cast<void (*)(id, SEL, id, id)>(objc_msgSend);
                s_info_dict = msg_init(s_info_dict, sel_registerName("init"));
                SEL setSel = sel_registerName("setObject:forKey:");
                msg_set(s_info_dict, setSel, nsstring_create_with_utf8("FTL"), nsstring_create_with_utf8("CFBundleDisplayName"));
                msg_set(s_info_dict, setSel, nsstring_create_with_utf8("FTL"), nsstring_create_with_utf8("CFBundleName"));
                msg_set(s_info_dict, setSel, nsstring_create_with_utf8("com.subsetgames.ftl"), nsstring_create_with_utf8("CFBundleIdentifier"));
                msg_set(s_info_dict, setSel, nsstring_create_with_utf8("1.6.21"), nsstring_create_with_utf8("CFBundleShortVersionString"));
                msg_set(s_info_dict, setSel, nsstring_create_with_utf8("1.6.21"), nsstring_create_with_utf8("CFBundleVersion"));
            }
        }
    }
    return (void *)s_info_dict;
}

void *CFBundleCopyLocalizedString(void *bundle, void *key, void *value, void *tableName) {
    return key;
}

void __dyld_register_func_for_add_image(void (*func)(const void *header, intptr_t slide)) {
    LOGI("Registered dyld add_image callback");
    // Call immediately with dummy Mach-O header
    static uint32_t dummy_header[8] = { 0xFEEDFACF, 0x0100000C, 0, 2, 0, 0, 0, 0 };
    if (func) {
        func((const void *)dummy_header, 0);
    }
}

// ============================================================================
// CoreAudio / AudioToolbox RemoteIO AudioUnit Bridge (backed by NDK AAudio)
// ============================================================================
#include <aaudio/AAudio.h>

typedef OSStatus (*AURenderCallback)(
    void *inRefCon,
    void *ioActionFlags,
    const void *inTimeStamp,
    uint32_t inBusNumber,
    uint32_t inNumberFrames,
    void *ioData
);

struct AURenderCallbackStruct {
    AURenderCallback inputProc;
    void *inputProcRefCon;
};

static AURenderCallback g_audio_callback = nullptr;
static void *g_audio_refcon = nullptr;
static pthread_t g_audio_thread = 0;
static bool g_audio_running = false;

struct AudioBuffer {
    uint32_t mNumberChannels;
    uint32_t mDataByteSize;
    void *mData;
};

struct AudioBufferList {
    uint32_t mNumberBuffers;
    struct AudioBuffer mBuffers[1];
};

static void *audio_worker_thread(void *arg) {
    (void)arg;
    LOGI("Starting AudioToolbox -> AAudio bridge worker thread...");
    const uint32_t kFrames = 512;
    const uint32_t kChannels = 2;
    const uint32_t kBytesPerSample = 2; // 16-bit PCM
    const uint32_t kBufSize = kFrames * kChannels * kBytesPerSample;

    AAudioStreamBuilder *builder = nullptr;
    AAudioStream *stream = nullptr;
    if (AAudio_createStreamBuilder(&builder) == AAUDIO_OK && builder) {
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSampleRate(builder, 44100);
        AAudioStreamBuilder_setChannelCount(builder, kChannels);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
        aaudio_result_t r = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);
        if (r == AAUDIO_OK && stream) {
            if (AAudioStream_requestStart(stream) == AAUDIO_OK) {
                LOGI("AAudio output stream started (44100Hz stereo PCM16).");
            } else {
                LOGE("AAudioStream_requestStart failed.");
                AAudioStream_close(stream);
                stream = nullptr;
            }
        } else {
            LOGE("AAudioStreamBuilder_openStream failed (%d).", (int)r);
            stream = nullptr;
        }
    }

    int16_t pcm_buffer[kFrames * kChannels];
    struct AudioBufferList bufList;
    bufList.mNumberBuffers = 1;
    bufList.mBuffers[0].mNumberChannels = kChannels;
    bufList.mBuffers[0].mDataByteSize = kBufSize;
    bufList.mBuffers[0].mData = pcm_buffer;

    while (g_audio_running) {
        if (g_audio_callback) {
            memset(pcm_buffer, 0, kBufSize);
            g_audio_callback(g_audio_refcon, nullptr, nullptr, 0, kFrames, &bufList);
        } else {
            memset(pcm_buffer, 0, kBufSize);
        }

        if (stream) {
            aaudio_result_t written = AAudioStream_write(stream, pcm_buffer, kFrames, 100000000LL);
            if (written < 0) {
                // Recover from disconnect or XRun cleanly without spinning
                usleep(11600);
            }
        } else {
            usleep(11600);
        }
    }

    if (stream) {
        AAudioStream_requestStop(stream);
        AAudioStream_close(stream);
    }
    LOGI("AudioToolbox bridge worker thread stopped.");
    return nullptr;
}

OSStatus _AudioComponentFindNext(void *inComponent, void *inDesc) {
    return 0;
}

OSStatus _AudioComponentInstanceNew(void *inComponent, void **outInstance) {
    *outInstance = (void *)0x8000;
    return 0;
}

OSStatus _AudioComponentInstanceDispose(void *inInstance) {
    return 0;
}

OSStatus _AudioUnitInitialize(void *inUnit) {
    LOGI("AudioUnitInitialize called.");
    return 0;
}

OSStatus _AudioUnitUninitialize(void *inUnit) {
    return 0;
}

OSStatus _AudioOutputUnitStart(void *inUnit) {
    LOGI("AudioOutputUnitStart called - starting audio worker.");
    if (!g_audio_running) {
        g_audio_running = true;
        pthread_create(&g_audio_thread, nullptr, audio_worker_thread, nullptr);
    }
    return 0;
}

OSStatus _AudioOutputUnitStop(void *inUnit) {
    LOGI("AudioOutputUnitStop called.");
    g_audio_running = false;
    if (g_audio_thread) {
        pthread_join(g_audio_thread, nullptr);
        g_audio_thread = 0;
    }
    return 0;
}

OSStatus _AudioUnitSetProperty(
    void *inUnit,
    AudioUnitPropertyID inID,
    AudioUnitScope inScope,
    AudioUnitElement inElement,
    const void *inData,
    uint32_t inDataSize
) {
    if (inID == 23) { // kAudioUnitProperty_SetRenderCallback
        const struct AURenderCallbackStruct *cb = (const struct AURenderCallbackStruct *)inData;
        g_audio_callback = cb->inputProc;
        g_audio_refcon = cb->inputProcRefCon;
        LOGI("AudioUnitSetProperty: Registered AURenderCallback at %p (refcon=%p)", g_audio_callback, g_audio_refcon);
    }
    return 0;
}

}
