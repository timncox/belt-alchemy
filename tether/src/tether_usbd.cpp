/* tether_usbd: see tether_usbd.h.
 *
 * libDaisy's device core is built for one interface (USBD_MAX_NUM_INTERFACES
 * 1: a request to interface 2 or 3 is refused before any class sees it), and
 * is a library this repo doesn't change. So the link wraps the core's
 * interface-request handler (-Wl,--wrap=USBD_StdItfReq in the Makefile):
 * while this class is the one registered, requests to interfaces 0..3 come
 * here; otherwise, as in Mac mode, the core's own handler runs unchanged.
 *
 * The FIFO RAM (OTG_HS, 4 KB = 1024 words) is laid out again after
 * USBD_Init, before USBD_Start: RX 512 words (as libDaisy), TX0 64, TX1 128
 * (MIDI in), TX2 16 (no IN endpoint 2; the minimum, as the RM asks of an
 * unused FIFO below a used one), TX3 128 (audio in: 192 bytes = 48 words
 * a packet). 848 of 1024 words.
 *
 * The iso endpoints: the HAL picks the frame parity when an endpoint is
 * armed and aborts a packet whose frame passed (incomplete-iso interrupts,
 * then the callbacks below re-arm). The SOF interrupt (on here; libDaisy
 * leaves it off) re-arms either endpoint should neither path have. */
#include "tether_usbd.h"

#include <stdio.h>
#include <string.h>

#include "daisy_core.h"
#include "sys/system.h"
#include "usbd_def.h"
#include "usbd_core.h"
#include "usbd_ctlreq.h"
#include "usbd_ioreq.h"
#include "usbd_desc.h"
#include "stm32h7xx_hal.h"

#include "usb_audio.h"
#include "usb_audio_desc.h"

extern "C" USBD_HandleTypeDef hUsbDeviceHS;      /* libDaisy's (hid/usb.cpp) */
extern "C" PCD_HandleTypeDef  hpcd_USB_OTG_HS;   /* libDaisy's (usbd_conf.c) */

namespace
{
using namespace usbaudio;

usbaudio::Link g_link;
tusbd::RxFn    g_rx = nullptr;
USBD_ClassTypeDef g_class;
bool           g_up = false;
uint8_t*     (*g_saved_dev_desc)(USBD_SpeedTypeDef, uint16_t*) = nullptr;

/* the class's state (pClassData points here) */
struct State
{
    uint8_t  alt_out = 0, alt_in = 0;
    bool     out_armed = false, in_armed = false;
    volatile bool midi_busy = false;
    bool     midi_zlp = false;
    uint32_t out_ok = 0, out_short = 0, out_incomplete = 0;
    uint32_t in_ok = 0, in_incomplete = 0, rearm_sof = 0, sets = 0;
} g_st;

alignas(4) uint8_t g_midi_rx[kMidiPacket];
alignas(4) uint8_t g_midi_tx[kMidiPacket];
alignas(4) uint8_t g_audio_rx[kMaxPacketBytes];
alignas(4) uint8_t g_audio_tx[kMaxPacketBytes];
alignas(4) uint8_t g_ctl[8];      /* class requests' data, read and dropped */
alignas(4) uint8_t g_reply[4];

alignas(4) uint8_t g_qualifier[10] = {10, 0x06, 0x00, 0x02, 0x00, 0x00, 0x00, 64, 0x01, 0x00};

uint8_t* device_desc(USBD_SpeedTypeDef, uint16_t* len)
{
    *len = sizeof kDeviceDesc;
    return const_cast<uint8_t*>(kDeviceDesc);
}
uint8_t* cfg_desc(uint16_t* len)
{
    *len = sizeof kConfigDesc;
    return const_cast<uint8_t*>(kConfigDesc);
}
uint8_t* qualifier_desc(uint16_t* len)
{
    *len = sizeof g_qualifier;
    return g_qualifier;
}

void arm_out(USBD_HandleTypeDef* pdev)
{
    if (g_st.alt_out != 1 || g_st.out_armed) return;
    g_st.out_armed = true;
    USBD_LL_PrepareReceive(pdev, kEpAudioOut, g_audio_rx, kMaxPacketBytes);
}
void arm_in(USBD_HandleTypeDef* pdev)
{
    if (g_st.alt_in != 1 || g_st.in_armed) return;
    const int bytes = g_link.InPacket(g_audio_tx, daisy::System::GetUs());
    g_st.in_armed = true;
    USBD_LL_Transmit(pdev, kEpAudioIn, g_audio_tx, (uint32_t)bytes);
}

void set_out(USBD_HandleTypeDef* pdev, uint8_t alt)
{
    if (g_st.alt_out == 1)
    {
        USBD_LL_FlushEP(pdev, kEpAudioOut);
        USBD_LL_CloseEP(pdev, kEpAudioOut);
        pdev->ep_out[kEpAudioOut & 0xF].is_used = 0;
    }
    g_st.alt_out   = alt;
    g_st.out_armed = false;
    g_link.OpenOut(alt == 1);
    if (alt == 1)
    {
        USBD_LL_OpenEP(pdev, kEpAudioOut, USBD_EP_TYPE_ISOC, kMaxPacketBytes);
        pdev->ep_out[kEpAudioOut & 0xF].is_used = 1;
        pdev->ep_out[kEpAudioOut & 0xF].maxpacket = kMaxPacketBytes;
        arm_out(pdev);
    }
}
void set_in(USBD_HandleTypeDef* pdev, uint8_t alt)
{
    if (g_st.alt_in == 1)
    {
        USBD_LL_FlushEP(pdev, kEpAudioIn);
        USBD_LL_CloseEP(pdev, kEpAudioIn);
        pdev->ep_in[kEpAudioIn & 0xF].is_used = 0;
    }
    g_st.alt_in   = alt;
    g_st.in_armed = false;
    g_link.OpenIn(alt == 1);
    if (alt == 1)
    {
        USBD_LL_OpenEP(pdev, kEpAudioIn, USBD_EP_TYPE_ISOC, kMaxPacketBytes);
        pdev->ep_in[kEpAudioIn & 0xF].is_used = 1;
        pdev->ep_in[kEpAudioIn & 0xF].maxpacket = kMaxPacketBytes;
        arm_in(pdev);
    }
}

/* ---- the class ---- */
uint8_t cls_init(USBD_HandleTypeDef* pdev, uint8_t)
{
    USBD_LL_OpenEP(pdev, kEpMidiOut, USBD_EP_TYPE_BULK, kMidiPacket);
    pdev->ep_out[kEpMidiOut & 0xF].is_used = 1;
    USBD_LL_OpenEP(pdev, kEpMidiIn, USBD_EP_TYPE_BULK, kMidiPacket);
    pdev->ep_in[kEpMidiIn & 0xF].is_used = 1;
    g_st.alt_out = g_st.alt_in = 0;
    g_st.out_armed = g_st.in_armed = false;
    g_st.midi_busy = false;
    g_st.midi_zlp  = false;
    g_link.OpenOut(false);
    g_link.OpenIn(false);
    pdev->pClassData                  = &g_st;
    pdev->pClassDataCmsit[pdev->classId] = &g_st;
    USBD_LL_PrepareReceive(pdev, kEpMidiOut, g_midi_rx, kMidiPacket);
    return (uint8_t)USBD_OK;
}

uint8_t cls_deinit(USBD_HandleTypeDef* pdev, uint8_t)
{
    if (g_st.alt_out) set_out(pdev, 0);
    if (g_st.alt_in) set_in(pdev, 0);
    USBD_LL_CloseEP(pdev, kEpMidiOut);
    pdev->ep_out[kEpMidiOut & 0xF].is_used = 0;
    USBD_LL_CloseEP(pdev, kEpMidiIn);
    pdev->ep_in[kEpMidiIn & 0xF].is_used = 0;
    g_st.midi_busy = false;
    g_link.OpenOut(false);
    g_link.OpenIn(false);
    pdev->pClassData                  = nullptr;
    pdev->pClassDataCmsit[pdev->classId] = nullptr;
    return (uint8_t)USBD_OK;
}

uint8_t cls_setup(USBD_HandleTypeDef* pdev, USBD_SetupReqTypedef* req)
{
    const uint8_t type  = req->bmRequest & USB_REQ_TYPE_MASK;
    const uint8_t recip = req->bmRequest & USB_REQ_RECIPIENT_MASK;
    const uint8_t itf   = LOBYTE(req->wIndex);

    if (type == USB_REQ_TYPE_STANDARD && recip == USB_REQ_RECIPIENT_INTERFACE)
    {
        switch (req->bRequest)
        {
            case USB_REQ_GET_STATUS:
                g_reply[0] = g_reply[1] = 0;
                USBD_CtlSendData(pdev, g_reply, 2);
                return (uint8_t)USBD_OK;
            case USB_REQ_GET_INTERFACE:
                if (pdev->dev_state != USBD_STATE_CONFIGURED || itf >= kNumItfs) break;
                g_reply[0] = itf == kItfOut ? g_st.alt_out : itf == kItfIn ? g_st.alt_in : 0;
                USBD_CtlSendData(pdev, g_reply, 1);
                return (uint8_t)USBD_OK;
            case USB_REQ_SET_INTERFACE:
            {
                if (pdev->dev_state != USBD_STATE_CONFIGURED) break;
                const uint8_t alt = LOBYTE(req->wValue);
                g_st.sets++;
                if ((itf == kItfOut || itf == kItfIn) && alt <= 1)
                {
                    if (itf == kItfOut) set_out(pdev, alt);
                    else set_in(pdev, alt);
                    return (uint8_t)USBD_OK;   /* the core sends the status stage */
                }
                if ((itf == kItfControl || itf == kItfMidi) && alt == 0) return (uint8_t)USBD_OK;
                break;
            }
            case USB_REQ_CLEAR_FEATURE: return (uint8_t)USBD_OK;
            default: break;
        }
        USBD_CtlError(pdev, req);
        return (uint8_t)USBD_FAIL;
    }

    if (type == USB_REQ_TYPE_CLASS && recip == USB_REQ_RECIPIENT_ENDPOINT)
    {
        /* audio endpoint controls: none are offered (bmAttributes 0), but a
         * host that sets the sampling frequency anyway is answered:
         * SET_CUR's three bytes are taken and dropped, GET_CUR says 44100. */
        if ((req->bmRequest & 0x80) != 0)
        {
            if (HIBYTE(req->wValue) == 0x01 && req->wLength >= 3)
            {
                g_reply[0] = 0x44;
                g_reply[1] = 0xAC;
                g_reply[2] = 0x00;
                USBD_CtlSendData(pdev, g_reply, 3);
                return (uint8_t)USBD_OK;
            }
        }
        else if (req->wLength == 0)
        {
            USBD_CtlSendStatus(pdev);
            return (uint8_t)USBD_OK;
        }
        else if (req->wLength <= sizeof g_ctl)
        {
            USBD_CtlPrepareRx(pdev, g_ctl, req->wLength);
            return (uint8_t)USBD_OK;
        }
    }
    /* AudioControl has no units with controls, MIDIStreaming none either */
    USBD_CtlError(pdev, req);
    return (uint8_t)USBD_FAIL;
}

uint8_t cls_ep0_rx(USBD_HandleTypeDef*) { return (uint8_t)USBD_OK; }

uint8_t cls_data_in(USBD_HandleTypeDef* pdev, uint8_t epnum)
{
    if (epnum == (kEpMidiIn & 0xF))
    {
        if (g_st.midi_zlp)
        {
            /* a whole 64 went: end the transfer with an empty packet, as
             * libDaisy's class did */
            g_st.midi_zlp = false;
            USBD_LL_Transmit(pdev, kEpMidiIn, nullptr, 0);
            return (uint8_t)USBD_OK;
        }
        g_st.midi_busy = false;
    }
    else if (epnum == (kEpAudioIn & 0xF))
    {
        g_st.in_ok++;
        g_st.in_armed = false;
        arm_in(pdev);
    }
    return (uint8_t)USBD_OK;
}

uint8_t cls_data_out(USBD_HandleTypeDef* pdev, uint8_t epnum)
{
    if (epnum == (kEpMidiOut & 0xF))
    {
        const uint32_t n = USBD_LL_GetRxDataSize(pdev, epnum);
        if (g_rx && n) g_rx(g_midi_rx, n);
        USBD_LL_PrepareReceive(pdev, kEpMidiOut, g_midi_rx, kMidiPacket);
    }
    else if (epnum == (kEpAudioOut & 0xF))
    {
        const uint32_t n = USBD_LL_GetRxDataSize(pdev, epnum);
        g_st.out_armed = false;
        if (n) { g_link.OutPacket(g_audio_rx, (int)n, daisy::System::GetUs()); g_st.out_ok++; }
        else g_st.out_short++;
        arm_out(pdev);
    }
    return (uint8_t)USBD_OK;
}

uint8_t cls_sof(USBD_HandleTypeDef* pdev)
{
    /* the belt to the braces: an iso endpoint left unarmed is armed */
    const bool o = g_st.alt_out == 1 && !g_st.out_armed;
    const bool i = g_st.alt_in == 1 && !g_st.in_armed;
    if (o || i) g_st.rearm_sof++;
    if (o) arm_out(pdev);
    if (i) arm_in(pdev);
    return (uint8_t)USBD_OK;
}

uint8_t cls_iso_in_incomplete(USBD_HandleTypeDef* pdev, uint8_t epnum)
{
    if (epnum == (kEpAudioIn & 0xF))
    {
        g_st.in_incomplete++;
        g_st.in_armed = false;
        arm_in(pdev);
    }
    return (uint8_t)USBD_OK;
}

uint8_t cls_iso_out_incomplete(USBD_HandleTypeDef* pdev, uint8_t epnum)
{
    if (epnum == (kEpAudioOut & 0xF))
    {
        g_st.out_incomplete++;
        g_st.out_armed = false;
        arm_out(pdev);
    }
    return (uint8_t)USBD_OK;
}

void fifos()
{
    PCD_HandleTypeDef* h = &hpcd_USB_OTG_HS;
    HAL_PCDEx_SetRxFiFo(h, 0x200);
    HAL_PCDEx_SetTxFiFo(h, 0, 0x40);
    HAL_PCDEx_SetTxFiFo(h, 1, 0x80);
    HAL_PCDEx_SetTxFiFo(h, 2, 0x10);
    HAL_PCDEx_SetTxFiFo(h, 3, 0x80);
    static_assert(0x200 + 0x40 + 0x80 + 0x10 + 0x80 <= 1024, "OTG_HS has 4 KB of FIFO RAM");
    static_assert(0x80 * 4 >= kMaxPacketBytes, "a whole audio packet fits TX3");
}
} // namespace

/* the core's interface requests: ours while this class is registered */
extern "C" USBD_StatusTypeDef __real_USBD_StdItfReq(USBD_HandleTypeDef* pdev, USBD_SetupReqTypedef* req);
extern "C" USBD_StatusTypeDef __wrap_USBD_StdItfReq(USBD_HandleTypeDef* pdev, USBD_SetupReqTypedef* req)
{
    if (pdev != &hUsbDeviceHS || pdev->pClass[0] != &g_class || !g_up) return __real_USBD_StdItfReq(pdev, req);
    const uint8_t type = req->bmRequest & USB_REQ_TYPE_MASK;
    if (type == USB_REQ_TYPE_STANDARD || type == USB_REQ_TYPE_CLASS || type == USB_REQ_TYPE_VENDOR)
    {
        if ((pdev->dev_state == USBD_STATE_DEFAULT || pdev->dev_state == USBD_STATE_ADDRESSED
             || pdev->dev_state == USBD_STATE_CONFIGURED)
            && LOBYTE(req->wIndex) < kNumItfs)
        {
            pdev->classId = 0;
            const USBD_StatusTypeDef ret = (USBD_StatusTypeDef)cls_setup(pdev, req);
            if (req->wLength == 0U && ret == USBD_OK && req->bRequest != USB_REQ_GET_STATUS) USBD_CtlSendStatus(pdev);
            return ret;
        }
    }
    USBD_CtlError(pdev, req);
    return USBD_FAIL;
}

namespace tusbd
{
void Start(RxFn rx)
{
    g_rx = rx;
    memset(&g_class, 0, sizeof g_class);
    g_class.Init                          = cls_init;
    g_class.DeInit                        = cls_deinit;
    g_class.Setup                         = cls_setup;
    g_class.EP0_RxReady                   = cls_ep0_rx;
    g_class.DataIn                        = cls_data_in;
    g_class.DataOut                       = cls_data_out;
    g_class.SOF                           = cls_sof;
    g_class.IsoINIncomplete               = cls_iso_in_incomplete;
    g_class.IsoOUTIncomplete              = cls_iso_out_incomplete;
    g_class.GetHSConfigDescriptor         = cfg_desc;
    g_class.GetFSConfigDescriptor         = cfg_desc;
    g_class.GetOtherSpeedConfigDescriptor = cfg_desc;
    g_class.GetDeviceQualifierDescriptor  = qualifier_desc;

    g_saved_dev_desc         = HS_Desc.GetDeviceDescriptor;
    HS_Desc.GetDeviceDescriptor = device_desc;
    g_st = State{};
    if (USBD_Init(&hUsbDeviceHS, &HS_Desc, DEVICE_HS) != USBD_OK) return;
    if (USBD_RegisterClass(&hUsbDeviceHS, &g_class) != USBD_OK) return;
    fifos();
    hpcd_USB_OTG_HS.Instance->GINTMSK |= USB_OTG_GINTMSK_SOFM;
    g_up = true;
    USBD_Start(&hUsbDeviceHS);
    HAL_PWREx_EnableUSBVoltageDetector();
}

void Stop()
{
    g_up = false;
    USBD_DeInit(&hUsbDeviceHS);
    HAL_PWREx_DisableUSBVoltageDetector();
    if (g_saved_dev_desc) HS_Desc.GetDeviceDescriptor = g_saved_dev_desc;
    g_link.OpenOut(false);
    g_link.OpenIn(false);
}

bool Configured() { return g_up && hUsbDeviceHS.dev_state == USBD_STATE_CONFIGURED; }
bool Addressed()
{
    return g_up && (hUsbDeviceHS.dev_state == USBD_STATE_ADDRESSED || hUsbDeviceHS.dev_state == USBD_STATE_CONFIGURED);
}

bool MidiSend(const uint8_t* p, int len)
{
    if (!Configured() || g_st.midi_busy || len <= 0 || len > (int)kMidiPacket) return false;
    memcpy(g_midi_tx, p, (size_t)len);
    g_st.midi_busy = true;
    g_st.midi_zlp  = len == (int)kMidiPacket;
    if (USBD_LL_Transmit(&hUsbDeviceHS, kEpMidiIn, g_midi_tx, (uint32_t)len) != USBD_OK)
    {
        g_st.midi_busy = false;
        g_st.midi_zlp  = false;
        return false;
    }
    return true;
}

usbaudio::Link& Link() { return g_link; }

int Report(char* b, int cap)
{
    const int n = snprintf(b, (size_t)cap,
                           "usb audio: out alt %u (%lu packets, %lu empty, %lu missed), in alt %u (%lu sent, %lu "
                           "missed), %lu re-armed at SOF, %lu SET_INTERFACE, in underruns %lu\n",
                           g_st.alt_out, (unsigned long)g_st.out_ok, (unsigned long)g_st.out_short,
                           (unsigned long)g_st.out_incomplete, g_st.alt_in, (unsigned long)g_st.in_ok,
                           (unsigned long)g_st.in_incomplete, (unsigned long)g_st.rearm_sof,
                           (unsigned long)g_st.sets, (unsigned long)g_link.sz_underruns);
    return n < cap ? n : cap;
}
} // namespace tusbd
