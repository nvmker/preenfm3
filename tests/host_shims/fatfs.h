/*
 * Host shim for the firmware's lib/Inc/fatfs.h — extended for Phase 4
 * (spec-test-coverage-phase4) from the FIL-forward-declaration-only version
 * that carried Targets #1-#4.
 *
 * WHY THIS EXISTS (see tests/SEAM.md "Contact with the code", Correction 1)
 * ----------------------------------------------------------------
 * The real lib/Inc/ff.h includes sd_diskio.h -> adafruit_802_sd.h ->
 * adafruit_802_conf.h -> stm32h7xx_nucleo_conf.h -> stm32h7xx_hal.h, which has
 * no host build. Phase 4 compiles the whole firmware/Src/filesystem/ tier
 * (banks, parsers, PPMImage), whose .cpp bodies call the f_* API by value and
 * by field (FIL.obj.objsize via the f_size macro, FIL.err, FILINFO.fname/
 * fattrib/fsize, DIR). A forward declaration is no longer sufficient: this
 * shim now owns a COMPLETE, minimal, in-memory FatFs surface.
 *
 * SHIM OWNS THE LAYOUT: the firmware treats FIL opaquely except for
 * (a) the f_size(fp) macro reading fp->obj.objsize and (b) the one
 * `file.err = 1` error marker in PreenFMFileType::createFile. Everything else
 * (the trailing shim_id / fptr bookkeeping) is shim-private.
 *
 * FIDELITY CONTRACT (spec Design Notes "Shim fidelity"): the f_* semantics are
 * implemented exactly as far as the firmware TUs exercise them —
 *   * f_open(FA_READ) on a missing file                -> FR_NO_FILE
 *   * f_open(FA_WRITE|FA_OPEN_ALWAYS) with a missing
 *     parent directory                                  -> FR_NO_PATH
 *   * f_open(FA_WRITE|FA_OPEN_ALWAYS) creates-or-opens, fptr = 0 (no
 *     append-on-open; firmware that appends does an explicit f_lseek past
 *     the end, which this shim honours by zero-extending the file).
 *   * f_lseek past EOF is allowed; reads clamp to objsize (short read ->
 *     firmware sees byteRead != size), writes zero-extend the gap.
 *   * f_opendir/f_readdir iterate SORTED keys (the firmware relies on the
 *     post-sortFiles order only via its own sort; determinism is what tests
 *     need). Sub-directories known to the shim are returned with AM_DIR.
 *   * f_stat on a missing file -> FR_NO_FILE (PPMImage::init loops until
 *     exactly FR_NO_FILE).
 *
 * This file intentionally shadows lib/Inc/ff.h by being placed FIRST on the
 * test include path. The firmware build never sees tests/host_shims/.
 */
#ifndef PFM3_HOST_SHIM_FATFS_H
#define PFM3_HOST_SHIM_FATFS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- FatFs base types (names match lib/Inc/ff.h) ------------------------- */
typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef uint32_t FSIZE_t;   /* FF_FS_READONLY/64-bit-off: 32-bit is enough   */
typedef unsigned int UINT;
typedef char     TCHAR;

/* --- File function return code (values 0..19, matches lib/Inc/ff.h) ------ */
typedef enum {
    FR_OK = 0, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE, FR_NO_PATH,
    FR_INVALID_NAME, FR_DENIED, FR_EXIST, FR_INVALID_OBJECT,
    FR_WRITE_PROTECTED, FR_INVALID_DRIVE, FR_NOT_ENABLED, FR_NO_FILESYSTEM,
    FR_MKFS_ABORTED, FR_TIMEOUT, FR_LOCKED, FR_NOT_ENOUGH_CORE,
    FR_TOO_MANY_OPEN_FILES, FR_INVALID_PARAMETER
} FRESULT;

/* --- File access mode / attribute flags (values match lib/Inc/ff.h) ------ */
#define FA_READ             0x01
#define FA_WRITE            0x02
#define FA_OPEN_EXISTING    0x00
#define FA_CREATE_NEW       0x04
#define FA_CREATE_ALWAYS    0x08
#define FA_OPEN_ALWAYS      0x10
#define FA_OPEN_APPEND      0x30

#define AM_RDO  0x01
#define AM_HID  0x02
#define AM_SYS  0x04
#define AM_DIR  0x10
#define AM_ARC  0x20

/* --- Object structures (SHIM-OWNED LAYOUT — see header comment) ---------- */
typedef struct FATFS FATFS;            /* opaque fs handle (never built)     */

typedef struct {
    FATFS*  fs;                        /* pointer to the owning fs (unused)  */
    WORD    id;                        /* fs mount ID (unused)               */
    BYTE    attr;                      /* attribute (unused)                 */
    BYTE    stat;                      /* object status (unused)             */
    DWORD   sclust;                    /* start cluster (unused)             */
    FSIZE_t objsize;                   /* OBJECT SIZE — read by f_size()/the
                                          firmware's direct obj.objsize use */
} FFOBJID;

typedef struct {
    FFOBJID obj;                       /* object id (objsize is the live one)*/
    BYTE    err;                       /* abort flag — firmware writes 1 on
                                          createFile() failure               */
    FSIZE_t fptr;                      /* file read/write pointer            */
    DWORD   clust;                     /* current cluster (unused)           */
    /* ---- shim-private bookkeeping (firmware never touches these) -------- */
    uint32_t shim_id;                  /* open-file handle; 0 == not open    */
} FIL;

typedef struct {
    FSIZE_t fsize;                     /* file size                          */
    WORD    fdate;                     /* modified date (0 in the shim)      */
    WORD    ftime;                     /* modified time (0 in the shim)      */
    BYTE    fattrib;                   /* attribute (AM_DIR for directories) */
    TCHAR   fname[256];                /* object name (basename, NUL-term.)  */
} FILINFO;

typedef struct {
    uint32_t shim_iter;                /* next child index (sorted)          */
    TCHAR   shim_path[256];            /* directory path this DIR walks      */
} DIR;

/* --- f_size() etc. — same macro shapes as lib/Inc/ff.h -------------------- */
#define f_size(fp) ((fp)->obj.objsize)
#define f_tell(fp) ((fp)->fptr)
#define f_eof(fp)  ((int)((fp)->fptr == (fp)->obj.objsize))

/* --- FatFs API used by the firmware TUs ---------------------------------- */
FRESULT f_open(FIL* fp, const TCHAR* path, BYTE mode);
FRESULT f_close(FIL* fp);
FRESULT f_read(FIL* fp, void* buff, UINT btr, UINT* br);
FRESULT f_write(FIL* fp, const void* buff, UINT btw, UINT* bw);
FRESULT f_lseek(FIL* fp, FSIZE_t ofs);
FRESULT f_opendir(DIR* dp, const TCHAR* path);
FRESULT f_closedir(DIR* dp);
FRESULT f_readdir(DIR* dp, FILINFO* fno);
FRESULT f_mkdir(const TCHAR* path);
FRESULT f_unlink(const TCHAR* path);
FRESULT f_rename(const TCHAR* path_old, const TCHAR* path_new);
FRESULT f_stat(const TCHAR* path, FILINFO* fno);

#ifdef __cplusplus
} /* extern "C" */

/* --- Test-fixture helpers (C++ only; defined in fatfs_impl.cpp) -----------
 * The in-memory filesystem: a path -> byte-buffer map plus a directory set.
 * fatfsShimReset() restores the pristine "SD just mounted" state (empty map;
 * the root "0:" always exists).
 */
#include <string>
#include <vector>

void        fatfsShimReset();
void        fatfsShimFailNext(const char* fn, FRESULT err);     /* one-shot: next call to fn returns err */
void        fatfsShimFailNextNth(const char* fn, FRESULT err, int nth); /* nth call to fn returns err */
void        fatfsShimMkdir(const char* path);                 /* recursive  */
void        fatfsShimInjectBytes(const char* path, const void* data, size_t len);
void        fatfsShimInjectString(const char* path, const char* content);
bool        fatfsShimFileExists(const char* path);
size_t      fatfsShimFileSize(const char* path);
bool        fatfsShimExtract(const char* path, std::vector<uint8_t>& out);
size_t      fatfsShimFileCount();
size_t      fatfsShimOpenFileCount();
std::vector<std::string> fatfsShimListDir(const char* path);  /* sorted direct children */
#endif /* __cplusplus */

#endif /* PFM3_HOST_SHIM_FATFS_H */
