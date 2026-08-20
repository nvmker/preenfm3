/*
 * In-memory FatFs implementation backing tests/host_shims/fatfs.h.
 * (Phase 4 seam — see the header comment there + tests/SEAM.md.)
 *
 * Model: std::map<std::string, std::vector<uint8_t>> files + std::set dirs.
 * Open files are tracked by a shim_id stored INSIDE the FIL (so FIL copies —
 * e.g. PreenFMFileType::createFile returns FIL by value — stay valid), with
 * the open-file table keyed by that id.
 */
#include "fatfs.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

struct OpenFile {
    std::string path;
    bool read;
    bool write;
};

struct ShimState {
    std::map<std::string, std::vector<uint8_t>> files;
    std::set<std::string> dirs;
    std::map<uint32_t, OpenFile> open;
    uint32_t nextId = 1;
    /* One-shot failure injection (fatfsShimFailNext): fn name -> error the
     * NEXT call to that f_* function returns. Cleared on consumption and by
     * fatfsShimReset(). Test-fixture only — real FatFs never sees this. */
    std::map<std::string, FRESULT> failNext;
};

ShimState& st() {
    static ShimState s;
    return s;
}

/* One-shot injected failure for the next call to fn (see ShimState::failNext). */
bool consumeFail(const char* fn, FRESULT& out) {
    auto it = st().failNext.find(fn);
    if (it == st().failNext.end()) return false;
    out = it->second;
    st().failNext.erase(it);
    return true;
}

/* Hard size ceiling: the largest real firmware artifact is the sequence
 * bank (~2050 KB of zeros per save loop) — the firmware never seeks or
 * writes past a few hundred KB deliberately. A garbage offset (corrupt size
 * read from a truncated file) must fail closed, not bad_alloc the host. */
constexpr FSIZE_t kShimMaxFileSize = 4u * 1024u * 1024u;

/* Strip trailing slashes; empty string means the root. */
std::string norm(const TCHAR* p) {
    std::string s(p ? p : "");
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

std::string parentOf(const std::string& p) {
    size_t pos = p.find_last_of('/');
    if (pos == std::string::npos || pos == 0) return "";
    return p.substr(0, pos);
}

/* The volume root(s) always exist ("0:" and ""). */
bool dirExists(const std::string& p) {
    return p.empty() || p == "0:" || st().dirs.count(p) != 0;
}

bool fileExists(const std::string& p) {
    return st().files.count(p) != 0;
}

std::string baseName(const std::string& p) {
    size_t pos = p.find_last_of('/');
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

/* Direct children (files AND directories) of `dir`, sorted by basename. */
std::vector<std::string> childrenOf(const std::string& dir) {
    std::vector<std::string> out;
    std::string prefix = dir + "/";
    for (const auto& kv : st().files) {
        if (kv.first.compare(0, prefix.size(), prefix) == 0 &&
            kv.first.find('/', prefix.size()) == std::string::npos) {
            out.push_back(baseName(kv.first));
        }
    }
    for (const auto& d : st().dirs) {
        if (d.compare(0, prefix.size(), prefix) == 0 &&
            d.find('/', prefix.size()) == std::string::npos) {
            out.push_back(baseName(d));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

OpenFile* lookup(FIL* fp) {
    if (fp == nullptr || fp->shim_id == 0) return nullptr;
    auto it = st().open.find(fp->shim_id);
    return it == st().open.end() ? nullptr : &it->second;
}

} // namespace

/* --- f_* API --------------------------------------------------------------- */

extern "C" FRESULT f_open(FIL* fp, const TCHAR* path, BYTE mode) {
    if (fp == nullptr || path == nullptr) return FR_INVALID_PARAMETER;
    FRESULT injected;
    if (consumeFail("f_open", injected)) return injected;
    std::string p = norm(path);
    bool write = (mode & FA_WRITE) != 0;

    /* Volume roots are not openable as files (real FatFs: FR_DENIED). */
    if (p.empty() || p == "0:") return FR_DENIED;

    bool mayCreate = (mode & (FA_CREATE_NEW | FA_CREATE_ALWAYS |
                              FA_OPEN_ALWAYS | FA_OPEN_APPEND)) != 0;
    auto it = st().files.find(p);
    if (it == st().files.end()) {
        /* A DIRECTORY at this path must not be shadowed by a file. */
        if (st().dirs.count(p) != 0) return FR_DENIED;
        if (!mayCreate) return FR_NO_FILE;  /* e.g. bare FA_WRITE (MixerBank::
                                               saveMixer) requires existence */
        if (!dirExists(parentOf(p))) return FR_NO_PATH;
        it = st().files.emplace(p, std::vector<uint8_t>()).first;
    } else if (mode & FA_CREATE_NEW) {
        return FR_EXIST;  /* exclusive create on an existing file */
    } else if (mode & FA_CREATE_ALWAYS) {
        it->second.clear();  /* create-always truncates (real FatFs) */
    }

    uint32_t id = st().nextId++;
    st().open[id] = OpenFile{p, (mode & FA_READ) != 0, write};
    fp->shim_id = id;
    fp->err = 0;
    fp->fptr = ((mode & FA_OPEN_APPEND) == FA_OPEN_APPEND)
                    ? static_cast<FSIZE_t>(it->second.size())
                    : 0;
    fp->obj.objsize = static_cast<FSIZE_t>(it->second.size());
    return FR_OK;
}

extern "C" FRESULT f_close(FIL* fp) {
    if (fp == nullptr) return FR_INVALID_PARAMETER;
    uint32_t id = fp->shim_id;
    if (id == 0 || st().open.find(id) == st().open.end()) {
        return FR_INVALID_OBJECT;
    }
    /* Real FatFs leaves the FIL valid when f_sync fails during close, so an
     * injected close failure must keep the handle open for retry. */
    FRESULT injected;
    if (consumeFail("f_close", injected)) return injected;
    st().open.erase(id);
    fp->shim_id = 0;
    return FR_OK;
}

extern "C" FRESULT f_read(FIL* fp, void* buff, UINT btr, UINT* br) {
    OpenFile* of = lookup(fp);
    if (of == nullptr) return FR_INVALID_OBJECT;
    if (!of->read) {
        if (br) *br = 0;
        return FR_DENIED;
    }
    std::vector<uint8_t>& f = st().files[of->path];
    size_t avail = f.size() > fp->fptr ? f.size() - fp->fptr : 0;
    size_t n = std::min<size_t>(btr, avail);
    if (n && buff) std::copy(f.begin() + fp->fptr, f.begin() + fp->fptr + n,
                             static_cast<uint8_t*>(buff));
    if (br) *br = static_cast<UINT>(n);
    fp->fptr += static_cast<FSIZE_t>(n);
    return FR_OK;
}

extern "C" FRESULT f_write(FIL* fp, const void* buff, UINT btw, UINT* bw) {
    OpenFile* of = lookup(fp);
    if (of == nullptr) return FR_INVALID_OBJECT;
    FRESULT injected;
    if (consumeFail("f_write", injected)) {
        if (bw) *bw = 0;
        return injected;
    }
    if (!of->write) {
        if (bw) *bw = 0;
        return FR_DENIED;
    }
    if ((uint64_t)fp->fptr + btw > kShimMaxFileSize) {
        /* fail closed instead of bad_alloc/OOM on a garbage offset */
        if (bw) *bw = 0;
        return FR_DENIED;
    }
    std::vector<uint8_t>& f = st().files[of->path];
    if (fp->fptr + btw > f.size()) f.resize(fp->fptr + btw, 0);
    if (btw && buff) {
        std::copy(static_cast<const uint8_t*>(buff),
                  static_cast<const uint8_t*>(buff) + btw,
                  f.begin() + fp->fptr);
    }
    if (bw) *bw = btw;
    fp->fptr += btw;
    fp->obj.objsize = static_cast<FSIZE_t>(f.size());
    return FR_OK;
}

extern "C" FRESULT f_lseek(FIL* fp, FSIZE_t ofs) {
    OpenFile* of = lookup(fp);
    if (of == nullptr) return FR_INVALID_OBJECT;
    FRESULT injected;
    if (consumeFail("f_lseek", injected)) return injected;
    if (ofs > kShimMaxFileSize) return FR_INVALID_PARAMETER;
    fp->fptr = ofs; /* past-EOF seek allowed; reads clamp, writes extend */
    return FR_OK;
}

extern "C" FRESULT f_opendir(DIR* dp, const TCHAR* path) {
    if (dp == nullptr || path == nullptr) return FR_INVALID_PARAMETER;
    std::string p = norm(path);
    if (!dirExists(p)) return FR_NO_PATH;
    std::string copy = p;
    if (copy.size() >= sizeof(dp->shim_path)) return FR_INVALID_NAME;
    size_t i = 0;
    for (; i < copy.size() && i < sizeof(dp->shim_path) - 1; i++) {
        dp->shim_path[i] = copy[i];
    }
    dp->shim_path[i] = 0;
    dp->shim_iter = 0;
    return FR_OK;
}

extern "C" FRESULT f_closedir(DIR* dp) {
    if (dp == nullptr) return FR_INVALID_PARAMETER;
    dp->shim_iter = 0;
    dp->shim_path[0] = 0;
    return FR_OK;
}

extern "C" FRESULT f_readdir(DIR* dp, FILINFO* fno) {
    if (dp == nullptr) return FR_INVALID_OBJECT;
    if (fno == nullptr) return FR_INVALID_PARAMETER;
    std::vector<std::string> kids = childrenOf(dp->shim_path);
    if (dp->shim_iter >= kids.size()) {
        fno->fname[0] = 0; /* end-of-directory marker (firmware's stop test) */
        return FR_OK;
    }
    const std::string& name = kids[dp->shim_iter++];
    std::string full = std::string(dp->shim_path) + "/" + name;
    fno->fattrib = 0;
    if (dirExists(full) && !fileExists(full)) fno->fattrib = AM_DIR;
    fno->fsize = fileExists(full)
                     ? static_cast<FSIZE_t>(st().files[full].size())
                     : 0;
    fno->fdate = 0;
    fno->ftime = 0;
    size_t i = 0;
    for (; i < name.size() && i < sizeof(fno->fname) - 1; i++) {
        fno->fname[i] = name[i];
    }
    fno->fname[i] = 0;
    return FR_OK;
}

extern "C" FRESULT f_mkdir(const TCHAR* path) {
    if (path == nullptr) return FR_INVALID_PARAMETER;
    std::string p = norm(path);
    if (dirExists(p) && !p.empty() && p != "0:") {
        if (!fileExists(p)) return FR_EXIST;
    }
    if (fileExists(p)) return FR_EXIST;
    if (!dirExists(parentOf(p))) return FR_NO_PATH;
    st().dirs.insert(p);
    return FR_OK;
}

extern "C" FRESULT f_unlink(const TCHAR* path) {
    if (path == nullptr) return FR_INVALID_PARAMETER;
    FRESULT injected;
    if (consumeFail("f_unlink", injected)) return injected;
    std::string p = norm(path);
    if (fileExists(p)) {
        /* real FatFs refuses to unlink a file with an open handle */
        for (const auto& kv : st().open) {
            if (kv.second.path == p) return FR_DENIED;
        }
        st().files.erase(p);
        return FR_OK;
    }
    return FR_NO_FILE;
}

extern "C" FRESULT f_rename(const TCHAR* path_old, const TCHAR* path_new) {
    if (path_old == nullptr || path_new == nullptr) return FR_INVALID_PARAMETER;
    FRESULT injected;
    if (consumeFail("f_rename", injected)) return injected;
    std::string from = norm(path_old);
    std::string to = norm(path_new);
    if (!fileExists(from)) return FR_NO_FILE;
    /* Real FatFs requires the destination name to be unused. It returns
     * FR_EXIST for both file and directory collisions; callers that need
     * replacement must rotate or unlink the destination explicitly. */
    if (fileExists(to) || dirExists(to)) return FR_EXIST;
    if (!dirExists(parentOf(to))) return FR_NO_PATH;
    st().files[to] = std::move(st().files[from]);
    st().files.erase(from);
    return FR_OK;
}

extern "C" FRESULT f_stat(const TCHAR* path, FILINFO* fno) {
    if (path == nullptr) return FR_INVALID_PARAMETER;
    std::string p = norm(path);
    if (!fileExists(p)) return FR_NO_FILE;
    if (fno) {
        fno->fsize = static_cast<FSIZE_t>(st().files[p].size());
        fno->fattrib = 0;
        fno->fdate = 0;
        fno->ftime = 0;
        std::string name = baseName(p);
        size_t i = 0;
        for (; i < name.size() && i < sizeof(fno->fname) - 1; i++) {
            fno->fname[i] = name[i];
        }
        fno->fname[i] = 0;
    }
    return FR_OK;
}

/* --- Test-fixture helpers --------------------------------------------------- */

void fatfsShimReset() {
    st().files.clear();
    st().dirs.clear();
    st().open.clear();
    st().nextId = 1;
    st().failNext.clear();
}

void fatfsShimFailNext(const char* fn, FRESULT err) {
    if (fn == nullptr) return;
    st().failNext[fn] = err;
}

void fatfsShimMkdir(const char* path) {
    if (path == nullptr) return;
    std::string p = norm(path);
    /* Create every missing ancestor (fixture convenience) — e.g.
     * fatfsShimMkdir("a/b/c") must create "a" and "a/b" too. */
    for (size_t i = 1; i <= p.size(); i++) {
        if (i == p.size() || p[i] == '/') {
            std::string anc = p.substr(0, i);
            if (!anc.empty() && anc != "0:") st().dirs.insert(anc);
        }
    }
}

void fatfsShimInjectBytes(const char* path, const void* data, size_t len) {
    std::string p = norm(path);
    fatfsShimMkdir(parentOf(p).c_str());
    if (data == nullptr || len == 0) {
        st().files[p] = {};  /* null/empty payload injects an empty file */
        return;
    }
    const uint8_t* b = static_cast<const uint8_t*>(data);
    st().files[p] = std::vector<uint8_t>(b, b + len);
}

void fatfsShimInjectString(const char* path, const char* content) {
    fatfsShimInjectBytes(path, content, std::string(content).size());
}

bool fatfsShimFileExists(const char* path) { return fileExists(norm(path)); }

size_t fatfsShimFileSize(const char* path) {
    std::string p = norm(path);
    return fileExists(p) ? st().files[p].size() : 0;
}

bool fatfsShimExtract(const char* path, std::vector<uint8_t>& out) {
    std::string p = norm(path);
    if (!fileExists(p)) return false;
    out = st().files[p];
    return true;
}

size_t fatfsShimFileCount() { return st().files.size(); }

std::vector<std::string> fatfsShimListDir(const char* path) {
    return childrenOf(norm(path));
}
