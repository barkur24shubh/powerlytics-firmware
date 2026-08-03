  .syntax unified
  .cpu cortex-m4
  .fpu softvfp
  .thumb

  .global g_pfnVectors
  .global Reset_Handler
  .global Default_Handler

  .word _sidata
  .word _sdata
  .word _edata
  .word _sbss
  .word _ebss

  .section .text.Reset_Handler
  .type Reset_Handler, %function
Reset_Handler:
  ldr sp, =_estack

  ldr r0, =_sdata
  ldr r1, =_edata
  ldr r2, =_sidata
  movs r3, #0
  b copy_data_check
copy_data:
  ldr r4, [r2, r3]
  str r4, [r0, r3]
  adds r3, r3, #4
copy_data_check:
  adds r4, r0, r3
  cmp r4, r1
  bcc copy_data

  ldr r2, =_sbss
  ldr r4, =_ebss
  movs r3, #0
  b zero_bss_check
zero_bss:
  str r3, [r2]
  adds r2, r2, #4
zero_bss_check:
  cmp r2, r4
  bcc zero_bss

  bl boot_main

hang:
  b hang

  .section .text.Default_Handler
  .type Default_Handler, %function
Default_Handler:
  b Default_Handler

  .section .isr_vector,"a",%progbits
  .type g_pfnVectors, %object
g_pfnVectors:
  .word _estack
  .word Reset_Handler
  .word Default_Handler
  .word Default_Handler
  .word Default_Handler
  .word Default_Handler
  .word Default_Handler
  .word 0
  .word 0
  .word 0
  .word 0
  .word Default_Handler
  .word Default_Handler
  .word 0
  .word Default_Handler
  .word Default_Handler

  .rept 82
  .word Default_Handler
  .endr
  .size g_pfnVectors, .-g_pfnVectors
