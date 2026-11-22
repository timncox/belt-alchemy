/*
 * A stand-in for the ST USB Host Library's headers, just big enough to
 * compile src/usbh_hub_midi.c on the laptop against test/test_hub_unplug.c's
 * simulated hub. Names and values are the library's (libDaisy's copy,
 * Middlewares/ST/STM32_USB_Host_Library/Core/Inc); the handle keeps only the
 * fields the hub driver touches.
 */
#ifndef FAKE_USBH_CORE_H
#define FAKE_USBH_CORE_H

#include <stddef.h>
#include <stdint.h>

#define USB_H2D                  0x00U
#define USB_D2H                  0x80U
#define USB_REQ_TYPE_STANDARD    0x00U
#define USB_REQ_TYPE_CLASS       0x20U
#define USB_REQ_RECIPIENT_DEVICE 0x00U
#define USB_REQ_RECIPIENT_OTHER  0x03U
#define USB_REQ_CLEAR_FEATURE    0x01U
#define USB_REQ_SET_FEATURE      0x03U
#define USB_REQ_SET_ADDRESS      0x05U
#define USB_REQ_GET_DESCRIPTOR   0x06U
#define USB_REQ_SET_CONFIGURATION 0x09U
#define USB_DESC_DEVICE          0x0100U
#define USB_DESC_CONFIGURATION   0x0200U
#define USBH_DEVICE_ADDRESS      0x01U
#define USBH_EP_CONTROL          0U
#define USBH_EP_BULK             2U
#define USBH_EP_INTERRUPT        3U

typedef enum
{
    USBH_OK = 0,
    USBH_BUSY,
    USBH_FAIL,
    USBH_NOT_SUPPORTED,
    USBH_UNRECOVERED_ERROR,
    USBH_ERROR_SPEED_UNKNOWN,
} USBH_StatusTypeDef;

typedef enum
{
    USBH_URB_IDLE = 0U,
    USBH_URB_DONE,
    USBH_URB_NOTREADY,
    USBH_URB_NYET,
    USBH_URB_ERROR,
    USBH_URB_STALL
} USBH_URBStateTypeDef;

typedef enum { CMD_IDLE = 0U, CMD_SEND, CMD_WAIT } CMD_StateTypeDef;

struct _USBH_HandleTypeDef;

typedef struct
{
    const char *Name;
    uint8_t     ClassCode;
    USBH_StatusTypeDef (*Init)(struct _USBH_HandleTypeDef *phost);
    USBH_StatusTypeDef (*DeInit)(struct _USBH_HandleTypeDef *phost);
    USBH_StatusTypeDef (*Requests)(struct _USBH_HandleTypeDef *phost);
    USBH_StatusTypeDef (*BgndProcess)(struct _USBH_HandleTypeDef *phost);
    USBH_StatusTypeDef (*SOFProcess)(struct _USBH_HandleTypeDef *phost);
    void *pData;
} USBH_ClassTypeDef;

typedef union { uint16_t w; struct { uint8_t lsb, msb; } bw; } uint16_t_uint8_t;

typedef struct _USBH_HandleTypeDef
{
    CMD_StateTypeDef RequestState;
    struct
    {
        uint8_t  pipe_in, pipe_out;
        uint8_t  pipe_size;
        uint8_t *buff;
        uint16_t length;
        union
        {
            struct
            {
                uint8_t           bmRequestType;
                uint8_t           bRequest;
                uint16_t_uint8_t  wValue, wIndex, wLength;
            } b;
        } setup;
    } Control;
    struct { uint8_t speed, address; } device;
    USBH_ClassTypeDef *pActiveClass;
} USBH_HandleTypeDef;

USBH_StatusTypeDef USBH_CtlReq(USBH_HandleTypeDef *phost, uint8_t *buff, uint16_t length);
USBH_StatusTypeDef USBH_GetDescriptor(USBH_HandleTypeDef *phost, uint8_t req_type,
                                      uint16_t value_idx, uint8_t *buff, uint16_t length);
USBH_StatusTypeDef USBH_SetAddress(USBH_HandleTypeDef *phost, uint8_t DeviceAddress);
USBH_StatusTypeDef USBH_SetCfg(USBH_HandleTypeDef *phost, uint16_t cfg_idx);

USBH_StatusTypeDef USBH_OpenPipe(USBH_HandleTypeDef *phost, uint8_t pipe_num, uint8_t epnum,
                                 uint8_t dev_address, uint8_t speed, uint8_t ep_type,
                                 uint16_t mps);
USBH_StatusTypeDef USBH_ClosePipe(USBH_HandleTypeDef *phost, uint8_t pipe_num);
uint8_t            USBH_AllocPipe(USBH_HandleTypeDef *phost, uint8_t ep_addr);
USBH_StatusTypeDef USBH_FreePipe(USBH_HandleTypeDef *phost, uint8_t idx);

USBH_StatusTypeDef USBH_BulkSendData(USBH_HandleTypeDef *phost, uint8_t *buff, uint16_t length,
                                     uint8_t pipe_num, uint8_t do_ping);
USBH_StatusTypeDef USBH_BulkReceiveData(USBH_HandleTypeDef *phost, uint8_t *buff,
                                        uint16_t length, uint8_t pipe_num);
USBH_StatusTypeDef USBH_InterruptSendData(USBH_HandleTypeDef *phost, uint8_t *buff,
                                          uint8_t length, uint8_t pipe_num);
USBH_StatusTypeDef USBH_InterruptReceiveData(USBH_HandleTypeDef *phost, uint8_t *buff,
                                             uint8_t length, uint8_t pipe_num);

USBH_URBStateTypeDef USBH_LL_GetURBState(USBH_HandleTypeDef *phost, uint8_t pipe);
uint32_t             USBH_LL_GetLastXferSize(USBH_HandleTypeDef *phost, uint8_t pipe);
USBH_StatusTypeDef   USBH_LL_SetToggle(USBH_HandleTypeDef *phost, uint8_t pipe, uint8_t toggle);

#endif
