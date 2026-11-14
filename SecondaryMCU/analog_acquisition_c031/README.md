# STM32C031 isolated analogue acquisition

Target: STM32C031F6Px, 32 KiB flash, 12 KiB RAM.

## PCB mapping

| Signal | MCU pin |
|---|---|
| AIN0..AIN8 | PA0..PA8 / ADC_IN0..ADC_IN8 |
| AIN9 (AI10 on main MCU) | PA11 / ADC_IN11 |
| Input power monitor | PA12 / ADC_IN12 |
| UART TX to main MCU | PC14 / USART1_TX AF0 |
| UART RX from main MCU | PB7 / USART1_RX AF0 |
| SWDIO | PA13 |
| SWCLK | PA14 |

The firmware samples all ten analogue inputs plus the input-power monitor
every 50 ms and sends the eleven raw
12-bit readings at 115200 baud. It performs no engineering-unit conversion or
rule evaluation.

The B1-V4-C board uses a 33 kOhm high-side resistor (`3302`) and a 2.4 kOhm
low-side resistor (`242`) for the power-monitor divider. The main MCU applies
the resulting 14.75:1 conversion; the secondary MCU continues to transmit the
raw ADC reading only.

## Build

Run `make`. The output is `build/analog_acquisition_c031.elf`.
