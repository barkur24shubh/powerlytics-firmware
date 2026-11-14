# Secondary MCU firmware

Firmware in this directory belongs to the isolated acquisition MCU used across
Powerlytics PCB variants. It is intentionally independent of any main-board
profile.

`analog_acquisition_c031` samples ten analogue inputs every 50 ms and transmits
raw ADC scans to the main STM32F407. Scaling, calibration, aggregation, rules,
events, and cloud behavior remain the responsibility of the main MCU.

