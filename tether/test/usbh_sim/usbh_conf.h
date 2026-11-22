/* usbh_conf.h for the native host simulation (test/usbh_sim): libDaisy's
 * settings (src/usbh/usbh_conf.h) without the STM32 HAL. */
#ifndef __USBH_CONF__H__
#define __USBH_CONF__H__
#ifdef __cplusplus
extern "C" {
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef __IO
#define __IO volatile
#endif

#define USBH_MAX_NUM_ENDPOINTS 2U
#define USBH_MAX_NUM_INTERFACES 2U
#define USBH_MAX_NUM_CONFIGURATION 1U
#define USBH_KEEP_CFG_DESCRIPTOR 1U
#define USBH_MAX_NUM_SUPPORTED_CLASS 2U
#define USBH_MAX_SIZE_CONFIGURATION 256U
#define USBH_MAX_DATA_BUFFER 512U
#define USBH_DEBUG_LEVEL 0U
#define USBH_USE_OS 0U
#define HOST_HS 0
#define HOST_FS 1

#define USBH_malloc malloc
#define USBH_free free
#define USBH_memset memset
#define USBH_memcpy memcpy

#define USBH_UsrLog(...) do { } while (0)
#define USBH_ErrLog(...) do { } while (0)
#define USBH_DbgLog(...) do { } while (0)

#ifdef __cplusplus
}
#endif
#endif
