/*
 * This file is part of the MicroPython project, http://micropython.org/
 * The MIT License (MIT)
 * Copyright (c) 2019 Damien P. George
 *
 * STM32C0 HAL configuration base (modeled on stm32g0xx_hal_conf_base.h).
 */

#ifndef MICROPY_INCLUDED_STM32C0XX_HAL_CONF_BASE_H
#define MICROPY_INCLUDED_STM32C0XX_HAL_CONF_BASE_H

// Enable various HAL modules
#define HAL_MODULE_ENABLED
#define HAL_ADC_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_EXTI_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_I2C_MODULE_ENABLED
#define HAL_PCD_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_RTC_MODULE_ENABLED
#define HAL_SPI_MODULE_ENABLED
#define HAL_TIM_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED
#define HAL_USART_MODULE_ENABLED

// Oscillator values in Hz
#define HSI_VALUE       (48000000UL)
#define HSI48_VALUE     (48000000UL)
#define LSI_VALUE       (32000UL)
#define LSI_STARTUP_TIME (130UL)

// SysTick has the highest priority
#define TICK_INT_PRIORITY (0x00)

// Miscellaneous HAL settings
#define  USE_RTOS                     0
#define  PREFETCH_ENABLE              1
#define  INSTRUCTION_CACHE_ENABLE     1
#define  USE_SPI_CRC                  1

// Include HAL modules for convenience
#include "stm32c0xx_hal_rcc.h"
#include "stm32c0xx_hal_gpio.h"
#include "stm32c0xx_hal_dma.h"
#include "stm32c0xx_hal_cortex.h"
#include "stm32c0xx_hal_adc.h"
#include "stm32c0xx_hal_adc_ex.h"
#include "stm32c0xx_hal_crc.h"
#include "stm32c0xx_hal_exti.h"
#include "stm32c0xx_hal_flash.h"
#include "stm32c0xx_hal_i2c.h"
#include "stm32c0xx_hal_i2s.h"
#include "stm32c0xx_hal_irda.h"
#include "stm32c0xx_hal_iwdg.h"
#include "stm32c0xx_hal_pcd.h"
#include "stm32c0xx_hal_hcd.h"
#include "stm32c0xx_hal_pwr.h"
#include "stm32c0xx_hal_rtc.h"
#include "stm32c0xx_hal_smartcard.h"
#include "stm32c0xx_hal_smbus.h"
#include "stm32c0xx_hal_spi.h"
#include "stm32c0xx_hal_tim.h"
#include "stm32c0xx_hal_uart.h"
#include "stm32c0xx_hal_usart.h"
#include "stm32c0xx_hal_wwdg.h"
#include "stm32c0xx_ll_bus.h"
#include "stm32c0xx_ll_rtc.h"
#include "stm32c0xx_ll_usart.h"

// HAL parameter assertions are disabled
#define assert_param(expr) ((void)0)

#endif // MICROPY_INCLUDED_STM32C0XX_HAL_CONF_BASE_H
