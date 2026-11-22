/* usbh_direct -- patience for a USB-MIDI device straight on the Lab's front
 * USB-C (TETHER 0.3.1).
 *
 * Behind a hub, Belt's hub driver sets the device up itself: 20 ms after the
 * port reset, 5 ms after it gives the device an address, and a device that
 * fails is tried again on the next scan. Straight on the port, ST's host core
 * does it: 2 ms after the address, three quick tries a request (the HAL's own
 * three under each), and on the third miss it reports an unrecoverable error
 * and starts the port again from wherever its enumeration had got to. A
 * device that services its USB from a timer rather than an interrupt (the
 * FM-1 and SMK-37 look at it every 0.5 ms, later while their audio is busy)
 * can miss all of that, and 0.3.0 showed red for good.
 *
 * Watch sits around the core's Process, on the module and in the native tests
 * (test/usbh_sim), and only while a device is being set up:
 *   - after the address, 10 ms before the next request;
 *   - a request that got no answer waits 5 ms (10, 15, 20 in later rounds)
 *     before each try, and gets three more rounds of the core's three tries;
 *   - a failed try (the core gave up, aborted, or nothing moved for 4 s)
 *     starts again cleanly from the bus reset, as a replug would;
 *   - red (Failed) only after three failed tries in a row, still trying;
 *   - Where says how far the last failed try got, for B3's blinks.
 * Once a class runs (HOST_CLASS: the MIDI class, or the hub driver) it leaves
 * the core alone, so the hub path is as it was.
 *
 * It also moves the MIDI interface to where the core's MIDI class looks
 * (place_midi, 0.1.1), so all of the direct path is in one place. */
#pragma once
#include <stdint.h>
#include <string.h>

#include "usbh_core.h"
#include "midi_itf_pick.h"

namespace usbdirect
{
constexpr uint32_t kAddrSettleMs = 10u;     /* after SET_ADDRESS (the core waits 2 ms itself) */
constexpr uint32_t kRetryGapMs   = 5u;      /* a request with no answer: wait, then try again */
constexpr uint8_t  kMoreRounds   = 3u;      /* rounds of the core's own tries, after its first */
constexpr uint32_t kStuckMs      = 4000u;   /* setting up, nothing moved this long: start again */
constexpr uint8_t  kRedAfter     = 3u;      /* failed tries in a row before B3 shows red */

/* How far a failed try got (B3 blinks this many times while red). */
enum Where : uint8_t
{
    kNowhere      = 0,
    kNoAnswer     = 1,   /* the device never answered its first request */
    kAfterAddress = 2,   /* it answered, then was lost around taking its address */
    kSetup        = 3,   /* it gave its descriptors; configuring it or opening MIDI failed */
};

inline Where where_of(uint8_t g, uint8_t e)
{
    if (g == HOST_IDLE || g == HOST_DEV_WAIT_FOR_ATTACHMENT || g == HOST_DEV_ATTACHED) return kNoAnswer;
    if (g == HOST_ENUMERATION)
        return (e == ENUM_IDLE || e == ENUM_GET_FULL_DEV_DESC) ? kNoAnswer : kAfterAddress;
    return kSetup;
}

/* The core runs control requests in these states while a device is set up. */
inline bool setting_up(uint8_t g)
{
    return g == HOST_ENUMERATION || g == HOST_SET_CONFIGURATION || g == HOST_SET_WAKEUP_FEATURE
           || g == HOST_CLASS_REQUEST;
}

/* 0.1.1: the core keeps the first USBH_MAX_NUM_INTERFACES interface entries
 * only, and the MIDI class looks for MIDI streaming there alone. A device
 * that lists audio streaming first (the FM-1: audio control, audio in, audio
 * out, then MIDI as interface 3) never had its MIDI opened. Before the core
 * picks a class, put the MIDI interface found in the raw descriptor where the
 * class looks: slot 1 behind an audio control interface (so slot 0 still
 * names the audio class), slot 0 otherwise. A hub is left to the hub class.
 * Returns the interface number + 1 when it moved one, else 0. */
inline uint8_t place_midi(USBH_HandleTypeDef* ph)
{
    USBH_CfgDescTypeDef& cfg = ph->device.CfgDesc;
    if (cfg.Itf_Desc[0].bInterfaceClass == 0x09) return 0;   /* a hub */
    for (unsigned i = 0; i < USBH_MAX_NUM_INTERFACES; i++)
        if (cfg.Itf_Desc[i].bInterfaceClass == 1 && cfg.Itf_Desc[i].bInterfaceSubClass == 3) return 0;
    uint16_t len = cfg.wTotalLength;
    if (len > USBH_MAX_SIZE_CONFIGURATION) len = USBH_MAX_SIZE_CONFIGURATION;
    midi_pick_t m;
    if (!midi_itf_pick(ph->device.CfgDesc_Raw, len, &m)) return 0;
    const int slot = (cfg.Itf_Desc[0].bInterfaceClass == 1 && USBH_MAX_NUM_INTERFACES > 1) ? 1 : 0;
    USBH_InterfaceDescTypeDef& d = cfg.Itf_Desc[slot];
    memset(&d, 0, sizeof d);
    d.bLength            = m.itf[0];
    d.bDescriptorType    = m.itf[1];
    d.bInterfaceNumber   = m.itf[2];
    d.bAlternateSetting  = m.itf[3];
    d.bNumEndpoints      = m.neps < USBH_MAX_NUM_ENDPOINTS ? m.neps : USBH_MAX_NUM_ENDPOINTS;
    d.bInterfaceClass    = m.itf[5];
    d.bInterfaceSubClass = m.itf[6];
    d.bInterfaceProtocol = m.itf[7];
    d.iInterface         = m.itf[8];
    for (int e = 0; e < d.bNumEndpoints; e++)
    {
        d.Ep_Desc[e].bLength          = m.ep[e][0];
        d.Ep_Desc[e].bDescriptorType  = m.ep[e][1];
        d.Ep_Desc[e].bEndpointAddress = m.ep[e][2];
        d.Ep_Desc[e].bmAttributes     = m.ep[e][3];
        d.Ep_Desc[e].wMaxPacketSize   = (uint16_t)(m.ep[e][4] | m.ep[e][5] << 8);
        d.Ep_Desc[e].bInterval        = m.ep[e][6];
    }
    return (uint8_t)(d.bInterfaceNumber + 1u);
}

struct Watch
{
    uint8_t  g = 0xFF, e = 0xFF;   /* gState / EnumState as last seen */
    uint8_t  pre_g = 0, pre_e = 0; /* ... before this Process */
    uint32_t since     = 0;        /* when they last changed */
    uint32_t hold_till = 0;
    bool     holding   = false;
    bool     in_err    = false;    /* the core has a request that got no answer */
    bool     errored   = false;    /* the core reported an unrecoverable error */
    uint8_t  rounds    = 0;        /* extra rounds given to this request */
    uint8_t  fails     = 0;        /* failed tries in a row */
    uint8_t  where     = kNowhere; /* how far the last failed try got */
    uint8_t  moved     = 0;        /* place_midi's answer, for the log */
    uint32_t tries     = 0;        /* failed tries since power-up, for the log */
    uint32_t restarts  = 0;        /* clean starts made here, for the log */

    /* The device went away: a new one starts from nothing. */
    void Unplugged()
    {
        fails = 0;
        where = kNowhere;
        moved = 0;
        holding = in_err = errored = false;
        rounds = 0;
    }
    /* The core's HOST_USER_UNRECOVERED_ERROR (from inside Process). */
    void OnError() { errored = true; }
    bool Failed() const { return fails >= kRedAfter; }

    void hold(uint32_t now, uint32_t ms)
    {
        holding   = true;
        hold_till = now + ms;
    }

    /* Call before the core's Process. False: leave Process out this time. */
    bool Before(USBH_HandleTypeDef* ph, uint32_t now)
    {
        pre_g = ph->gState;
        pre_e = ph->EnumState;
        if (holding)
        {
            if ((int32_t)(now - hold_till) < 0) return false;
            holding = false;
        }
        if (setting_up(ph->gState) && ph->Control.state == CTRL_ERROR)
        {
            if (!in_err)
            {
                in_err = true;   /* the first look at this miss: give the device a moment, */
                hold(now, kRetryGapMs * (1u + rounds));   /* longer each round */
                return false;
            }
            /* The core gives up on the next miss: three more of its tries, a few times. */
            if (ph->Control.errorcount >= USBH_MAX_ERROR_COUNT && rounds < kMoreRounds)
            {
                ph->Control.errorcount = 0;
                rounds++;
            }
        }
        return true;
    }

    /* Call after it (or in its place, when Before said no). */
    void After(USBH_HandleTypeDef* ph, uint32_t now)
    {
        if (ph->gState == HOST_CHECK_CLASS && !moved) moved = place_midi(ph);
        if (ph->Control.state != CTRL_ERROR) in_err = false;
        if (pre_e == ENUM_SET_ADDR && ph->EnumState == ENUM_GET_CFG_DESC && ph->gState == HOST_ENUMERATION)
            hold(now, kAddrSettleMs);
        if (ph->gState != g || ph->EnumState != e)
        {
            g      = ph->gState;
            e      = ph->EnumState;
            since  = now;
            rounds = 0;
        }
        if (ph->gState == HOST_CLASS)
        {
            fails = 0;   /* a class runs: it worked */
            where = kNowhere;
            return;
        }
        if (errored)
        {
            errored = false;
            failed(where_of(pre_g, pre_e));
            restart(ph, now);
        }
        else if (ph->gState == HOST_ABORT_STATE && pre_g != HOST_ABORT_STATE)
            failed(where_of(pre_g, pre_e));   /* libDaisy's Process starts the host again */
        else if (setting_up(ph->gState) && (uint32_t)(now - since) > kStuckMs)
        {
            failed(where_of(ph->gState, ph->EnumState));
            restart(ph, now);
        }
    }

    void failed(Where w)
    {
        where = w;
        tries++;
        if (fails < 255u) fails++;
    }

    /* Start again from the bus reset with nothing left over: the core, after
     * an unrecoverable error, goes back to HOST_IDLE but keeps its place in
     * the enumeration, the address it gave and any class it started. */
    void restart(USBH_HandleTypeDef* ph, uint32_t now)
    {
        if (ph->pActiveClass)
        {
            ph->pActiveClass->DeInit(ph);
            ph->pActiveClass = nullptr;
        }
        (void)USBH_ClosePipe(ph, ph->Control.pipe_in);   /* halt anything still in flight */
        (void)USBH_ClosePipe(ph, ph->Control.pipe_out);
        (void)USBH_FreePipe(ph, ph->Control.pipe_in);
        (void)USBH_FreePipe(ph, ph->Control.pipe_out);
        ph->gState              = HOST_IDLE;
        ph->EnumState           = ENUM_IDLE;
        ph->RequestState        = CMD_SEND;
        ph->Control.state       = CTRL_SETUP;
        ph->Control.pipe_size   = 0x40u;   /* the core's USBH_MPS_DEFAULT */
        ph->Control.errorcount  = 0;
        ph->device.address      = USBH_DEVICE_ADDRESS_DEFAULT;
        ph->device.EnumCnt      = 0;
        ph->device.RstCnt       = 0;
        ph->Timeout             = 0;
        moved   = 0;
        rounds  = 0;
        in_err  = false;
        holding = false;
        g = e   = 0xFF;
        since   = now;
        restarts++;
    }
};
} // namespace usbdirect
