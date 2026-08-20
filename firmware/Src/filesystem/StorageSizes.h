/*
 * Shared size constants for the storage parse buffers.
 *
 * The real definitions live in Storage.cpp; every TU that externs these
 * arrays must reference the same constant so a wrong-size extern (the
 * historical 512/64 mismatches vs the real 1024) cannot compile silently.
 */

#ifndef __STORAGE_SIZES_H__
#define __STORAGE_SIZES_H__

/* C-compatible: usable from C-style translation units. */
#define PFM3_LINE_BUFFER_SIZE 1024

#endif /* __STORAGE_SIZES_H__ */
