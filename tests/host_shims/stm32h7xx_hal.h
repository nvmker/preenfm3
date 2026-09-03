/*
 * Host shim for the firmware's stm32h7xx_hal.h — scoped to the B1 TFT
 * display-corruption tier (spec-b1-tft-display-corruption-hardening).
 *
 * WHY THIS EXISTS (fatfs.h-precedent exception, see tests/SEAM.md
 * "Correction 1"): the real firmware/Drivers/.../stm32h7xx_hal.h is an
 * UNGUARDED chain into CMSIS device headers, compiler intrinsics and the
 * whole HAL driver surface — unhostable. But unlike the fatfs case, exactly
 * ONE host-compiled firmware header needs it: lib/Inc/TftDisplay.h (for the
 * DMA2D register macros + handle types used by the TFT action machine).
 * That makes this the second justified fake header: it provides ONLY the
 * symbols lib/Inc/TftDisplay.h and firmware/Src/hardware/
 * FirmwareTftDisplay.cpp reference (grep-verified):
 *   - HAL_StatusTypeDef enum (values match stm32h7xx_hal_def.h)
 *   - WRITE_REG / READ_REG / MODIFY_REG / SET_BIT / CLEAR_BIT (same macro
 *     shapes as stm32h7xx_hal_def.h)
 *   - POSITION_VAL: the real CMSIS definition is __CLZ(__RBIT(VAL)) which
 *     computes the bit position of a single-field mask; TftDisplay.h uses it
 *     in exactly one place, POSITION_VAL(DMA2D_NLR_PL) == 16. The shim
 *     special-cases that one input (16) and 0 otherwise.
 *   - DMA2D_TypeDef: dummy register block with only the registers the
 *     display code touches (a plain struct — no absolute addresses are
 *     dereferenced, only member accesses; tests point hdma2d.Instance at a
 *     static zero-initialized dummy, which satisfies PFM_IS_DMA2D_READY()
 *     and absorbs every register program the action steps perform).
 *   - DMA2D_HandleTypeDef / RNG_HandleTypeDef (handle shapes)
 *   - the DMA2D bit definitions used by the two TUs (values mirror
 *     CMSIS stm32h753xx.h / stm32h7xx_hal_dma2d.h)
 *
 * Nothing else in the host build resolves to this file (TftAlgo.h maps the
 * include to <stdint.h> under PFM3_HOST; Synth.cpp guards it out). The
 * firmware build never sees tests/host_shims/.
 */
#ifndef PFM3_HOST_SHIM_STM32H7XX_HAL_H
#define PFM3_HOST_SHIM_STM32H7XX_HAL_H

#include <stdint.h>

/* --- HAL status (values match stm32h7xx_hal_def.h) ------------------------ */
typedef enum {
    HAL_OK      = 0x00,
    HAL_ERROR   = 0x01,
    HAL_BUSY    = 0x02,
    HAL_TIMEOUT = 0x03
} HAL_StatusTypeDef;

/* --- Register access macros (same shapes as stm32h7xx_hal_def.h) ---------- */
#define WRITE_REG(REG, VAL)   ((REG) = (VAL))
#define READ_REG(REG)         ((REG))
#define MODIFY_REG(REG, CLEARMASK, SETMASK)  \
        WRITE_REG((REG), (((READ_REG(REG)) & (~(CLEARMASK))) | (SETMASK)))
#define SET_BIT(REG, BIT)     ((REG) = (REG) | (BIT))
#define CLEAR_BIT(REG, BIT)   ((REG) = (REG) & ~(BIT))

/* Only use is POSITION_VAL(DMA2D_NLR_PL) in TftDisplay.h's
 * DMA2D_POSITION_NLR_PL; the CMSIS original (__CLZ(__RBIT(x))) yields 16
 * for that mask. */
#define POSITION_VAL(VAL)     ((VAL) == DMA2D_NLR_PL ? 16 : 0)

/* --- DMA2D bit definitions (values mirror CMSIS stm32h753xx.h) ------------ */
#define DMA2D_CR_START         ((uint32_t)0x00000001)  /* CR bit 0            */
#define DMA2D_CR_MODE_1        ((uint32_t)0x00020000)  /* CR bit 17           */
#define DMA2D_CR_MODE          ((uint32_t)0x00030000)  /* CR bits 17:16       */
#define DMA2D_OOR_LO           ((uint32_t)0x00003FFF)  /* OOR bits 13:0       */
#define DMA2D_NLR_NL           ((uint32_t)0x0000FFFF)  /* NLR bits 15:0       */
#define DMA2D_NLR_PL           ((uint32_t)0x3FFF0000)  /* NLR bits 29:16      */
/* stm32h7xx_hal_dma2d.h: R2M == DMA2D_CR_MODE, M2M_BLEND == DMA2D_CR_MODE_1  */
#define DMA2D_R2M              DMA2D_CR_MODE
#define DMA2D_M2M_BLEND        DMA2D_CR_MODE_1

/* --- DMA2D register block: only the registers the display code touches ----
 * No absolute addresses are dereferenced — the host fixture backs
 * hdma2d.Instance with a static zero-initialized dummy (CR == 0 means
 * PFM_IS_DMA2D_READY() is true; tests reset CR between action pumps).      */
typedef struct {
    volatile uint32_t CR;      /* control: MODE + START                     */
    volatile uint32_t OOR;     /* output offset                            */
    volatile uint32_t NLR;     /* number of lines / pixels per line        */
    volatile uint32_t OMAR;    /* output memory address (truncated ptr on
                                  64-bit hosts — never dereferenced)       */
    volatile uint32_t OCOLR;   /* output color                             */
    volatile uint32_t FGCOLR;  /* foreground color (blend mode)            */
    volatile uint32_t FGMAR;   /* foreground memory address                */
    volatile uint32_t BGMAR;   /* background memory address                */
    volatile uint32_t BGOR;    /* background offset                        */
} DMA2D_TypeDef;

typedef struct {
    DMA2D_TypeDef *Instance;   /* points at the register block             */
} DMA2D_HandleTypeDef;

/* RNG handle: FirmwareTftDisplay.cpp declares `extern RNG_HandleTypeDef
 * hrng;` (the symbol itself is never referenced on host — Synth.cpp guards
 * its use out under PFM3_HOST). Shape-only. */
typedef struct {
    uint32_t unused;
} RNG_HandleTypeDef;

#endif /* PFM3_HOST_SHIM_STM32H7XX_HAL_H */
