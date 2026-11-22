/* stm32h7xx_hal.h stand-in for the native host simulation: the hub driver's clock */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t HAL_GetTick(void);
#ifdef __cplusplus
}
#endif
