/*
 * Host shim for the firmware's lib/Inc/fatfs.h.
 *
 * WHY THIS EXISTS (see tests/SEAM.md "Contact with the code")
 * ----------------------------------------------------------------
 * The real lib/Inc/fatfs.h includes sd_diskio.h -> adafruit_802_sd.h ->
 * adafruit_802_conf.h -> stm32h7xx_nucleo_conf.h -> stm32h7xx_hal.h, which has
 * no host build. That chain is pulled in transitively whenever Sequencer.cpp's
 * header closure reaches Storage.h -> MixerBank.h -> PreenFMFileType.h, and
 * PreenFMFileType.h does `#include "fatfs.h"`.
 *
 * The firmware's header closure uses the FatFs `FIL` type ONLY in method
 * signatures (FIL*, FIL&, and one return-by-value declaration in
 * PreenFMFileType / MixerBank / SequenceBank). It is NEVER embedded by value in
 * any header member, and all the real by-value FIL objects live in .cpp files
 * that the host tests do not compile. Therefore a forward declaration of FIL is
 * sufficient to satisfy the closure, and we can omit the entire SD-disk/HAL
 * branch that the real fatfs.h drags in.
 *
 * This file intentionally shadows lib/Inc/fatfs.h by being placed FIRST on the
 * test include path. It contains NO logic and must not be used by the firmware
 * build (the firmware build never sees tests/host_shims/).
 */
#ifndef PFM3_HOST_SHIM_FATFS_H
#define PFM3_HOST_SHIM_FATFS_H

#ifdef __cplusplus
extern "C" {
#endif

/* The only FatFs type the firmware header closure references. Forward-
 * declaration is enough because every use is a method-signature declaration. */
struct FIL;

#ifdef __cplusplus
}
#endif

#endif /* PFM3_HOST_SHIM_FATFS_H */
