@******************************************************************************
@ File Name    : startup_stm32f40_41xxx_gcc.S
@ Description : STM32F40xxx/41xxx vector table + reset for arm-none-eabi-gcc
@               (GNU assembler syntax). Vector order matches the ST official
@               MDK template startup_stm32f40_41xxx.s (V1.8.1).
@               - Sets initial SP
@               - Sets initial PC == Reset_Handler
@               - Provides weak defaults for every peripheral IRQHandler
@               - Calls SystemInit() then __libc_init_array(), branches to main()
@ Target       : STM32F407VGT6
@******************************************************************************
  .syntax unified
  .cpu    cortex-m4
  .fpu    fpv4-sp-d16
  .thumb

@******************************************************************************
@ Vector Table (must land in .isr_vector, mapped at 0x08000000 by linker)
@******************************************************************************
  .section .isr_vector,"a",%progbits
  .align  2
  .globl  g_pfnVectors
  .type   g_pfnVectors, %object
g_pfnVectors:
  .word   _estack
  .word   Reset_Handler
  .word   NMI_Handler
  .word   HardFault_Handler
  .word   MemManage_Handler
  .word   BusFault_Handler
  .word   UsageFault_Handler
  .word   0
  .word   0
  .word   0
  .word   0
  .word   SVC_Handler
  .word   DebugMon_Handler
  .word   0
  .word   PendSV_Handler
  .word   SysTick_Handler

@ External Interrupts (STM32F40x/41x order)
  .word   WWDG_IRQHandler
  .word   PVD_IRQHandler
  .word   TAMP_STAMP_IRQHandler
  .word   RTC_WKUP_IRQHandler
  .word   FLASH_IRQHandler
  .word   RCC_IRQHandler
  .word   EXTI0_IRQHandler
  .word   EXTI1_IRQHandler
  .word   EXTI2_IRQHandler
  .word   EXTI3_IRQHandler
  .word   EXTI4_IRQHandler
  .word   DMA1_Stream0_IRQHandler
  .word   DMA1_Stream1_IRQHandler
  .word   DMA1_Stream2_IRQHandler
  .word   DMA1_Stream3_IRQHandler
  .word   DMA1_Stream4_IRQHandler
  .word   DMA1_Stream5_IRQHandler
  .word   DMA1_Stream6_IRQHandler
  .word   ADC_IRQHandler
  .word   CAN1_TX_IRQHandler
  .word   CAN1_RX0_IRQHandler
  .word   CAN1_RX1_IRQHandler
  .word   CAN1_SCE_IRQHandler
  .word   EXTI9_5_IRQHandler
  .word   TIM1_BRK_TIM9_IRQHandler
  .word   TIM1_UP_TIM10_IRQHandler
  .word   TIM1_TRG_COM_TIM11_IRQHandler
  .word   TIM1_CC_IRQHandler
  .word   TIM2_IRQHandler
  .word   TIM3_IRQHandler
  .word   TIM4_IRQHandler
  .word   I2C1_EV_IRQHandler
  .word   I2C1_ER_IRQHandler
  .word   I2C2_EV_IRQHandler
  .word   I2C2_ER_IRQHandler
  .word   SPI1_IRQHandler
  .word   SPI2_IRQHandler
  .word   USART1_IRQHandler
  .word   USART2_IRQHandler
  .word   USART3_IRQHandler
  .word   EXTI15_10_IRQHandler
  .word   RTC_Alarm_IRQHandler
  .word   OTG_FS_WKUP_IRQHandler
  .word   TIM8_BRK_TIM12_IRQHandler
  .word   TIM8_UP_TIM13_IRQHandler
  .word   TIM8_TRG_COM_TIM14_IRQHandler
  .word   TIM8_CC_IRQHandler
  .word   DMA1_Stream7_IRQHandler
  .word   FSMC_IRQHandler
  .word   SDIO_IRQHandler
  .word   TIM5_IRQHandler
  .word   SPI3_IRQHandler
  .word   UART4_IRQHandler
  .word   UART5_IRQHandler
  .word   TIM6_DAC_IRQHandler
  .word   TIM7_IRQHandler
  .word   DMA2_Stream0_IRQHandler
  .word   DMA2_Stream1_IRQHandler
  .word   DMA2_Stream2_IRQHandler
  .word   DMA2_Stream3_IRQHandler
  .word   DMA2_Stream4_IRQHandler
  .word   ETH_IRQHandler
  .word   ETH_WKUP_IRQHandler
  .word   CAN2_TX_IRQHandler
  .word   CAN2_RX0_IRQHandler
  .word   CAN2_RX1_IRQHandler
  .word   CAN2_SCE_IRQHandler
  .word   OTG_FS_IRQHandler
  .word   DMA2_Stream5_IRQHandler
  .word   DMA2_Stream6_IRQHandler
  .word   DMA2_Stream7_IRQHandler
  .word   USART6_IRQHandler
  .word   I2C3_EV_IRQHandler
  .word   I2C3_ER_IRQHandler
  .word   OTG_HS_EP1_OUT_IRQHandler
  .word   OTG_HS_EP1_IN_IRQHandler
  .word   OTG_HS_WKUP_IRQHandler
  .word   OTG_HS_IRQHandler
  .word   DCMI_IRQHandler
  .word   CRYP_IRQHandler
  .word   HASH_RNG_IRQHandler
  .word   FPU_IRQHandler
g_pfnVectors_End:
  .size   g_pfnVectors, .-g_pfnVectors


@******************************************************************************
@ Reset_Handler : entry after reset.
@   1) init MSP to _estack
@   2) copy .data from FLASH (_sidata) to RAM (_sdata .. _edata)
@   3) zero-fill .bss (_sbss .. _ebss)
@   4) call SystemInit()
@   5) call __libc_init_array() (C runtime init)
@   6) branch to main()
@******************************************************************************
  .section .text.Reset_Handler,"ax",%progbits
  .align  2
  .globl  Reset_Handler
  .type   Reset_Handler, %function
Reset_Handler:
  ldr   sp, =_estack          @ set stack pointer

  @ ---- Copy .data section (LMA _sidata -> VMA _sdata.._edata) ----
  ldr   r0, =_sdata
  ldr   r1, =_edata
  ldr   r2, =_sidata
  movs  r3, #0
LoopCopyData:
  cmp   r0, r1
  bcs   LoopFillZerobss
  ldr   r3, [r2], #4
  str   r3, [r0], #4
  b     LoopCopyData

  @ ---- Zero fill .bss section (_sbss .. _ebss) ----
LoopFillZerobss:
  ldr   r0, =_sbss
  ldr   r1, =_ebss
  movs  r2, #0
  b     FillZerobss
LoopFillZerobssNext:
  str   r2, [r0], #4
FillZerobss:
  cmp   r0, r1
  bcc   LoopFillZerobssNext

  @ ---- Call SystemInit / C runtime init / main ----
  ldr   r0, =SystemInit
  blx   r0
  ldr   r0, =__libc_init_array
  blx   r0
  ldr   r0, =main
  blx   r0

LoopForever:
  b     LoopForever
  .size Reset_Handler, .-Reset_Handler

@******************************************************************************
@ Default_Handler : fallback for any unimplemented exception / IRQ.
@******************************************************************************
  .section .text.Default_Handler,"ax",%progbits
  .align  1
  .globl  Default_Handler
  .type   Default_Handler, %function
Default_Handler:
  b       .
  .size   Default_Handler, .-Default_Handler

@******************************************************************************
@ Weak aliases : if the application does not provide a strong IRQHandler
@ (e.g. in stm32f4xx_it.c), the vector entry falls back to Default_Handler.
@******************************************************************************
  .weak   WWDG_IRQHandler
  .thumb_set WWDG_IRQHandler, Default_Handler
  .weak   PVD_IRQHandler
  .thumb_set PVD_IRQHandler, Default_Handler
  .weak   TAMP_STAMP_IRQHandler
  .thumb_set TAMP_STAMP_IRQHandler, Default_Handler
  .weak   RTC_WKUP_IRQHandler
  .thumb_set RTC_WKUP_IRQHandler, Default_Handler
  .weak   FLASH_IRQHandler
  .thumb_set FLASH_IRQHandler, Default_Handler
  .weak   RCC_IRQHandler
  .thumb_set RCC_IRQHandler, Default_Handler
  .weak   EXTI0_IRQHandler
  .thumb_set EXTI0_IRQHandler, Default_Handler
  .weak   EXTI1_IRQHandler
  .thumb_set EXTI1_IRQHandler, Default_Handler
  .weak   EXTI2_IRQHandler
  .thumb_set EXTI2_IRQHandler, Default_Handler
  .weak   EXTI3_IRQHandler
  .thumb_set EXTI3_IRQHandler, Default_Handler
  .weak   EXTI4_IRQHandler
  .thumb_set EXTI4_IRQHandler, Default_Handler
  .weak   DMA1_Stream0_IRQHandler
  .thumb_set DMA1_Stream0_IRQHandler, Default_Handler
  .weak   DMA1_Stream1_IRQHandler
  .thumb_set DMA1_Stream1_IRQHandler, Default_Handler
  .weak   DMA1_Stream2_IRQHandler
  .thumb_set DMA1_Stream2_IRQHandler, Default_Handler
  .weak   DMA1_Stream3_IRQHandler
  .thumb_set DMA1_Stream3_IRQHandler, Default_Handler
  .weak   DMA1_Stream4_IRQHandler
  .thumb_set DMA1_Stream4_IRQHandler, Default_Handler
  .weak   DMA1_Stream5_IRQHandler
  .thumb_set DMA1_Stream5_IRQHandler, Default_Handler
  .weak   DMA1_Stream6_IRQHandler
  .thumb_set DMA1_Stream6_IRQHandler, Default_Handler
  .weak   ADC_IRQHandler
  .thumb_set ADC_IRQHandler, Default_Handler
  .weak   CAN1_TX_IRQHandler
  .thumb_set CAN1_TX_IRQHandler, Default_Handler
  .weak   CAN1_RX0_IRQHandler
  .thumb_set CAN1_RX0_IRQHandler, Default_Handler
  .weak   CAN1_RX1_IRQHandler
  .thumb_set CAN1_RX1_IRQHandler, Default_Handler
  .weak   CAN1_SCE_IRQHandler
  .thumb_set CAN1_SCE_IRQHandler, Default_Handler
  .weak   EXTI9_5_IRQHandler
  .thumb_set EXTI9_5_IRQHandler, Default_Handler
  .weak   TIM1_BRK_TIM9_IRQHandler
  .thumb_set TIM1_BRK_TIM9_IRQHandler, Default_Handler
  .weak   TIM1_UP_TIM10_IRQHandler
  .thumb_set TIM1_UP_TIM10_IRQHandler, Default_Handler
  .weak   TIM1_TRG_COM_TIM11_IRQHandler
  .thumb_set TIM1_TRG_COM_TIM11_IRQHandler, Default_Handler
  .weak   TIM1_CC_IRQHandler
  .thumb_set TIM1_CC_IRQHandler, Default_Handler
  .weak   TIM2_IRQHandler
  .thumb_set TIM2_IRQHandler, Default_Handler
  .weak   TIM3_IRQHandler
  .thumb_set TIM3_IRQHandler, Default_Handler
  .weak   TIM4_IRQHandler
  .thumb_set TIM4_IRQHandler, Default_Handler
  .weak   I2C1_EV_IRQHandler
  .thumb_set I2C1_EV_IRQHandler, Default_Handler
  .weak   I2C1_ER_IRQHandler
  .thumb_set I2C1_ER_IRQHandler, Default_Handler
  .weak   I2C2_EV_IRQHandler
  .thumb_set I2C2_EV_IRQHandler, Default_Handler
  .weak   I2C2_ER_IRQHandler
  .thumb_set I2C2_ER_IRQHandler, Default_Handler
  .weak   SPI1_IRQHandler
  .thumb_set SPI1_IRQHandler, Default_Handler
  .weak   SPI2_IRQHandler
  .thumb_set SPI2_IRQHandler, Default_Handler
  .weak   USART1_IRQHandler
  .thumb_set USART1_IRQHandler, Default_Handler
  .weak   USART2_IRQHandler
  .thumb_set USART2_IRQHandler, Default_Handler
  .weak   USART3_IRQHandler
  .thumb_set USART3_IRQHandler, Default_Handler
  .weak   EXTI15_10_IRQHandler
  .thumb_set EXTI15_10_IRQHandler, Default_Handler
  .weak   RTC_Alarm_IRQHandler
  .thumb_set RTC_Alarm_IRQHandler, Default_Handler
  .weak   OTG_FS_WKUP_IRQHandler
  .thumb_set OTG_FS_WKUP_IRQHandler, Default_Handler

  .weak   TIM8_BRK_TIM12_IRQHandler
  .thumb_set TIM8_BRK_TIM12_IRQHandler, Default_Handler
  .weak   TIM8_UP_TIM13_IRQHandler
  .thumb_set TIM8_UP_TIM13_IRQHandler, Default_Handler
  .weak   TIM8_TRG_COM_TIM14_IRQHandler
  .thumb_set TIM8_TRG_COM_TIM14_IRQHandler, Default_Handler
  .weak   TIM8_CC_IRQHandler
  .thumb_set TIM8_CC_IRQHandler, Default_Handler
  .weak   DMA1_Stream7_IRQHandler
  .thumb_set DMA1_Stream7_IRQHandler, Default_Handler
  .weak   FSMC_IRQHandler
  .thumb_set FSMC_IRQHandler, Default_Handler
  .weak   SDIO_IRQHandler
  .thumb_set SDIO_IRQHandler, Default_Handler
  .weak   TIM5_IRQHandler
  .thumb_set TIM5_IRQHandler, Default_Handler
  .weak   SPI3_IRQHandler
  .thumb_set SPI3_IRQHandler, Default_Handler
  .weak   UART4_IRQHandler
  .thumb_set UART4_IRQHandler, Default_Handler
  .weak   UART5_IRQHandler
  .thumb_set UART5_IRQHandler, Default_Handler
  .weak   TIM6_DAC_IRQHandler
  .thumb_set TIM6_DAC_IRQHandler, Default_Handler
  .weak   TIM7_IRQHandler
  .thumb_set TIM7_IRQHandler, Default_Handler
  .weak   DMA2_Stream0_IRQHandler
  .thumb_set DMA2_Stream0_IRQHandler, Default_Handler
  .weak   DMA2_Stream1_IRQHandler
  .thumb_set DMA2_Stream1_IRQHandler, Default_Handler
  .weak   DMA2_Stream2_IRQHandler
  .thumb_set DMA2_Stream2_IRQHandler, Default_Handler
  .weak   DMA2_Stream3_IRQHandler
  .thumb_set DMA2_Stream3_IRQHandler, Default_Handler
  .weak   DMA2_Stream4_IRQHandler
  .thumb_set DMA2_Stream4_IRQHandler, Default_Handler
  .weak   ETH_IRQHandler
  .thumb_set ETH_IRQHandler, Default_Handler
  .weak   ETH_WKUP_IRQHandler
  .thumb_set ETH_WKUP_IRQHandler, Default_Handler
  .weak   CAN2_TX_IRQHandler
  .thumb_set CAN2_TX_IRQHandler, Default_Handler
  .weak   CAN2_RX0_IRQHandler
  .thumb_set CAN2_RX0_IRQHandler, Default_Handler
  .weak   CAN2_RX1_IRQHandler
  .thumb_set CAN2_RX1_IRQHandler, Default_Handler
  .weak   CAN2_SCE_IRQHandler
  .thumb_set CAN2_SCE_IRQHandler, Default_Handler
  .weak   OTG_FS_IRQHandler
  .thumb_set OTG_FS_IRQHandler, Default_Handler
  .weak   DMA2_Stream5_IRQHandler
  .thumb_set DMA2_Stream5_IRQHandler, Default_Handler
  .weak   DMA2_Stream6_IRQHandler
  .thumb_set DMA2_Stream6_IRQHandler, Default_Handler
  .weak   DMA2_Stream7_IRQHandler
  .thumb_set DMA2_Stream7_IRQHandler, Default_Handler
  .weak   USART6_IRQHandler
  .thumb_set USART6_IRQHandler, Default_Handler
  .weak   I2C3_EV_IRQHandler
  .thumb_set I2C3_EV_IRQHandler, Default_Handler
  .weak   I2C3_ER_IRQHandler
  .thumb_set I2C3_ER_IRQHandler, Default_Handler
  .weak   OTG_HS_EP1_OUT_IRQHandler
  .thumb_set OTG_HS_EP1_OUT_IRQHandler, Default_Handler
  .weak   OTG_HS_EP1_IN_IRQHandler
  .thumb_set OTG_HS_EP1_IN_IRQHandler, Default_Handler
  .weak   OTG_HS_WKUP_IRQHandler
  .thumb_set OTG_HS_WKUP_IRQHandler, Default_Handler
  .weak   OTG_HS_IRQHandler
  .thumb_set OTG_HS_IRQHandler, Default_Handler
  .weak   DCMI_IRQHandler
  .thumb_set DCMI_IRQHandler, Default_Handler
  .weak   CRYP_IRQHandler
  .thumb_set CRYP_IRQHandler, Default_Handler
  .weak   HASH_RNG_IRQHandler
  .thumb_set HASH_RNG_IRQHandler, Default_Handler
  .weak   FPU_IRQHandler
  .thumb_set FPU_IRQHandler, Default_Handler

  .end

