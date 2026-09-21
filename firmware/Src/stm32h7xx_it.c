/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32h7xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2019 STMicroelectronics.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by ST under BSD 3-Clause license,
  * the "License"; You may not use this file except in compliance with the
  * License. You may obtain a copy of the License at:
  *                        opensource.org/licenses/BSD-3-Clause
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "Common.h"
#include "stm32h7xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
// define in lib stm32h7xx_nucleo_bus.c
extern SPI_HandleTypeDef sd_spi2;

// define in lib adafruit_802_sd.c
void spi2TransferComplete();

extern DMA_HandleTypeDef hdma_spi2_rx;
extern DMA_HandleTypeDef hdma_spi2_tx;

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
 
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
extern uint8_t midiControllerMode;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
void preenfm3Tic();
void preenfm3MidiControllerTic();
#ifdef PFM3_DIAG_ENABLED
/* 8.1 diagnostics (8.8 SW5, gated): C-linkage entries defined in
 * pfm3_diag.cpp (see pfm3_diag.h). SysTick enter is called before
 * HAL_IncTick so a stall is visible even if preenfm3Tic early-returns.
 * Fault-hook ABI (8.8 SW5 review round): r0=faultId, r1=stacked-frame
 * pointer, r2=EXC_RETURN (LR) — matches pfm3DiagFaultHook's C signature
 * exactly. The old asm passed SP in r0 / faultId in r1 (the parked-code
 * bug this signature now pins down; the make ubsan-trap objdump guard
 * asserts it on every diagnostic build). */
void pfm3DiagSysTickEnter(void);
void pfm3DiagFaultHook(unsigned int faultId, unsigned int *stackedFrame,
                       unsigned int excReturn);
#endif /* PFM3_DIAG_ENABLED */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern PCD_HandleTypeDef hpcd_USB_OTG_FS;
extern DMA_HandleTypeDef hdma_sai1_a;
extern DMA_HandleTypeDef hdma_sai1_b;
extern DMA_HandleTypeDef hdma_sai2_a;
extern DMA_HandleTypeDef hdma_spi1_tx;
extern SPI_HandleTypeDef hspi1;
extern UART_HandleTypeDef huart1;
/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex Processor Interruption and Exception Handlers          */ 
/******************************************************************************/
/* 8.8 SW5 review round — REGENERATION HAZARD (read before regenerating).
 *
 * The five fault handlers below (NMI/Hard/MemManage/Bus/Usage) each have
 * EXACTLY ONE definition in this file:
 *   - PFM3_DIAG_ENABLED  -> a __attribute__((naked, noreturn)) shim whose body
 *     is ASM ONLY: select MSP/PSP into r1 via EXC_RETURN bit 2, faultId in
 *     r0, EXC_RETURN in r2, tail-branch to pfm3DiagFaultHook. `naked` is what
 *     makes this CORRECT, not an optimization: without it correctness would
 *     silently depend on -Ofast emitting no prologue (any push/stack-frame
 *     setup would relocate the stacked exception frame before the hook reads
 *     it). The `make ubsan-trap` self-verification disassembles each handler
 *     and fails the build if the first instructions are not exactly the shim
 *     sequence (no prologue, r0/r1/r2 ABI).
 *   - otherwise          -> the stock CubeMX handler (empty body / while(1)).
 *
 * The #ifdef/#ifndef wrappers live OUTSIDE the CubeMX USER CODE markers, so
 * re-running STM32CubeMX code generation WILL DISCARD them and restore the
 * stock handlers. After any regeneration, re-apply this block (git diff of
 * stm32h7xx_it.c against the branch shows the expected shape) — the ubsan-trap
 * objdump guard then proves the shims are back before any diagnostic flash.
 */
#ifdef PFM3_DIAG_ENABLED

/* Common shim sequence. Uses r0-r2 only (an AAPCS-compatible scratch set):
 *   r1 <- MSP or PSP per EXC_RETURN bit 2 (which stack the frame is on)
 *   r0 <- <faultId literal>, r2 <- EXC_RETURN, then tail-branch (the hook
 *   never returns). No other register is touched: the hook reads the stacked
 *   r0-r3/r12/lr/pc/psr from the frame pointer, never from live registers.
 * (The %0 substitution carries NO '#' — the assembler printer adds the
 * immediate prefix itself.) */
#define PFM3_DIAG_FAULT_SHIM(faultIdLit)                                    \
  __asm volatile (                                                          \
      "tst lr, #4         \n"  /* EXC_RETURN bit 2: 1=frame on PSP */        \
      "ite eq             \n"                                               \
      "mrseq r1, msp      \n"  /* r1 = stacked-frame pointer (basic)  */   \
      "mrsne r1, psp      \n"                                               \
      "mov r0, %0         \n"  /* r0 = faultId (1..5, see pfm3_diag.h) */  \
      "mov r2, lr         \n"  /* r2 = EXC_RETURN (extended-frame bit 4) */\
      "b pfm3DiagFaultHook\n"  /* never returns: capture + soft reset */   \
      : : "i" (faultIdLit))

/**
  * @brief This function handles Non maskable interrupt (faultId 5: the
  *        capture contract and h8_crashwatch.py reserve it for NMI).
  */
__attribute__((naked, noreturn)) void NMI_Handler(void)
{
  PFM3_DIAG_FAULT_SHIM(5);
}

/**
  * @brief This function handles Hard fault interrupt (faultId 1).
  */
__attribute__((naked, noreturn)) void HardFault_Handler(void)
{
  PFM3_DIAG_FAULT_SHIM(1);
}

/**
  * @brief This function handles Memory management fault (faultId 2).
  */
__attribute__((naked, noreturn)) void MemManage_Handler(void)
{
  PFM3_DIAG_FAULT_SHIM(2);
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault
  *        (faultId 3).
  */
__attribute__((naked, noreturn)) void BusFault_Handler(void)
{
  PFM3_DIAG_FAULT_SHIM(3);
}

/**
  * @brief This function handles Undefined instruction or illegal state
  *        (faultId 4; a trap-mode UBSan UDF lands here when USGFAULTENA
  *        is set by pfm3DiagInit).
  */
__attribute__((naked, noreturn)) void UsageFault_Handler(void)
{
  PFM3_DIAG_FAULT_SHIM(4);
}

#else /* !PFM3_DIAG_ENABLED — stock CubeMX handlers (byte-identical release) */

/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */

  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */

  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */

  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

#endif /* PFM3_DIAG_ENABLED */

/**
  * @brief This function handles System service call via SWI instruction.
  */
void SVC_Handler(void)
{
  /* USER CODE BEGIN SVCall_IRQn 0 */

  /* USER CODE END SVCall_IRQn 0 */
  /* USER CODE BEGIN SVCall_IRQn 1 */

  /* USER CODE END SVCall_IRQn 1 */
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles Pendable request for system service.
  */
void PendSV_Handler(void)
{
  /* USER CODE BEGIN PendSV_IRQn 0 */

  /* USER CODE END PendSV_IRQn 0 */
  /* USER CODE BEGIN PendSV_IRQn 1 */

  /* USER CODE END PendSV_IRQn 1 */
}

/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */

  /* USER CODE END SysTick_IRQn 0 */
  HAL_IncTick();
  /* USER CODE BEGIN SysTick_IRQn 1 */
#ifdef PFM3_DIAG_ENABLED
  pfm3DiagSysTickEnter();
#endif /* PFM3_DIAG_ENABLED */
  switch (midiControllerMode) {
  case 0:
      preenfm3Tic();
      break;
  case 1:
      preenfm3MidiControllerTic();
      break;
  case 2:
  // Do nothing
      break;
  }
  /* USER CODE END SysTick_IRQn 1 */
}

/******************************************************************************/
/* STM32H7xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32h7xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles DMA1 stream0 global interrupt.
  */
void DMA1_Stream0_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Stream0_IRQn 0 */
  /* USER CODE END DMA1_Stream0_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_sai1_a);
  /* USER CODE BEGIN DMA1_Stream0_IRQn 1 */
  /* USER CODE END DMA1_Stream0_IRQn 1 */
}

/**
  * @brief This function handles DMA1 stream1 global interrupt.
  */
void DMA1_Stream1_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Stream1_IRQn 0 */

  /* USER CODE END DMA1_Stream1_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_sai1_b);
  /* USER CODE BEGIN DMA1_Stream1_IRQn 1 */

  /* USER CODE END DMA1_Stream1_IRQn 1 */
}

/**
  * @brief This function handles DMA1 stream2 global interrupt.
  */
void DMA1_Stream2_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Stream2_IRQn 0 */

  /* USER CODE END DMA1_Stream2_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_sai2_a);
  /* USER CODE BEGIN DMA1_Stream2_IRQn 1 */

  /* USER CODE END DMA1_Stream2_IRQn 1 */
}

/**
  * @brief This function handles SPI1 global interrupt.
  */
void SPI1_IRQHandler(void)
{
  /* USER CODE BEGIN SPI1_IRQn 0 */

  /* USER CODE END SPI1_IRQn 0 */
  HAL_SPI_IRQHandler(&hspi1);
  /* USER CODE BEGIN SPI1_IRQn 1 */

  /* USER CODE END SPI1_IRQn 1 */
}

/**
  * @brief This function handles USART1 global interrupt.
  */
void USART1_IRQHandler(void)
{
  /* USER CODE BEGIN USART1_IRQn 0 */
#ifdef PFM_DONT_NEED_HAL_IRQHandler
  /* USER CODE END USART1_IRQn 0 */
  HAL_UART_IRQHandler(&huart1);
  /* USER CODE BEGIN USART1_IRQn 1 */
#endif

  preenfm3_USART1_IRQHandler();

  /* USER CODE END USART1_IRQn 1 */
}

/**
  * @brief This function handles DMA2 stream0 global interrupt.
  */
void DMA2_Stream0_IRQHandler(void)
{
  /* USER CODE BEGIN DMA2_Stream0_IRQn 0 */

  /* USER CODE END DMA2_Stream0_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_spi1_tx);
  /* USER CODE BEGIN DMA2_Stream0_IRQn 1 */

  /* USER CODE END DMA2_Stream0_IRQn 1 */
}


/**
  * @brief This function handles USB On The Go FS global interrupt.
  */
void OTG_FS_IRQHandler(void)
{
  /* USER CODE BEGIN OTG_FS_IRQn 0 */

  /* USER CODE END OTG_FS_IRQn 0 */
  HAL_PCD_IRQHandler(&hpcd_USB_OTG_FS);
  /* USER CODE BEGIN OTG_FS_IRQn 1 */

  /* USER CODE END OTG_FS_IRQn 1 */
}

/* USER CODE BEGIN 1 */



/**
  * @brief This function handles SPI2 global interrupt.
  */
void SPI2_IRQHandler(void)
{
  /* USER CODE BEGIN SPI2_IRQn 0 */

  /* USER CODE END SPI2_IRQn 0 */
  HAL_SPI_IRQHandler(&sd_spi2);
  /* USER CODE BEGIN SPI2_IRQn 1 */

  /* USER CODE END SPI2_IRQn 1 */
}

/**
  * @brief This function handles DMA2 stream1 global interrupt.
  */
void DMA2_Stream1_IRQHandler(void)
{
  /* USER CODE BEGIN DMA2_Stream1_IRQn 0 */

  /* USER CODE END DMA2_Stream1_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_spi2_rx);
  /* USER CODE BEGIN DMA2_Stream1_IRQn 1 */

  /* USER CODE END DMA2_Stream1_IRQn 1 */
}

/**
  * @brief This function handles DMA2 stream2 global interrupt.
  */
void DMA2_Stream2_IRQHandler(void)
{
  /* USER CODE BEGIN DMA2_Stream2_IRQn 0 */

  /* USER CODE END DMA2_Stream2_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_spi2_tx);
  /* USER CODE BEGIN DMA2_Stream2_IRQn 1 */

  /* USER CODE END DMA2_Stream2_IRQn 1 */
}




void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi) {

    if (hspi == &sd_spi2) {
        spi2TransferComplete();
    }

}

/**
  * @brief Tx and Rx Transfer completed callback.
  * @param  hspi: pointer to a SPI_HandleTypeDef structure that contains
  *               the configuration information for SPI module.
  * @retval None
  */
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &sd_spi2) {
      /* Prevent unused argument(s) compilation warning */
      spi2TransferComplete();
    }
}



/* USER CODE END 1 */
/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
