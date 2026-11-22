/* See sim.h. Stands in for libDaisy's usbh_conf.c: the USBH_LL_* calls the
 * core makes, answered by a modelled device (and hub) instead of the HAL. */
#include "sim.h"

#include <string.h>
#include <deque>
#include <array>

#include "stm32h7xx_hal.h"

namespace
{
struct Pipe
{
    uint8_t ep = 0, dev_addr = 0, type = 0;
    uint16_t mps = 0;
    USBH_URBStateTypeDef urb = USBH_URB_IDLE;
    uint32_t xfer = 0;
    uint8_t toggle = 0;
    uint8_t* pending = nullptr;   /* a bulk IN the device is NAKing: the HAL keeps it going */
    SimDev*  pend_dev = nullptr;
};

uint32_t g_now = 0;
Pipe     g_pipes[16];
SimDev*  g_dev    = nullptr;   /* what is plugged (straight, or behind the hub) */
bool     g_viahub = false;
std::deque<std::array<uint8_t, 4>> g_midi;

/* the device's view of the control transfer in progress */
struct Ctl
{
    SimDev*        who = nullptr;
    const uint8_t* reply = nullptr;
    uint16_t       reply_len = 0;
    bool           stall = false;
    bool           addr_after_status = false;
    uint8_t        new_addr = 0;
} g_ctl;

/* pending address changes */
struct Pend
{
    SimDev*  who = nullptr;
    uint8_t  addr = 0;
    uint32_t at = 0;
} g_pend;

uint32_t g_deaf_until = 0;    /* the plugged device, after a bus reset */

/* ---- the hub (when g_viahub) ---- */
SimDev  g_hub;
const uint8_t kHubDev[18] = {18, 1, 0x00, 0x02, 0x09, 0, 1, 64, 0x09, 0x21, 0x14, 0x28, 0x00, 0x01, 0, 0, 0, 1};
const uint8_t kHubCfg[25] = {9, 2, 25, 0, 1, 1, 0, 0xE0, 50, 9, 4, 0, 0, 1, 9, 0, 0, 0, 7, 5, 0x81, 3, 1, 0, 12};
const uint8_t kHubDesc[9] = {9, 0x29, 4, 0, 0, 50, 0, 0, 0xFF};
bool     hub_powered = false, port_enabled = false, port_reset = false;
uint16_t port_change = 0;
uint8_t  hub_status[4];

bool reachable(SimDev* d)
{
    if (!d) return false;
    if (d == &g_hub) return g_viahub;
    if (!g_viahub) return true;
    return port_enabled;
}

SimDev* who_at(uint8_t addr)
{
    if (g_pend.who && (int32_t)(g_now - g_pend.at) >= 0)
    {
        g_pend.who->addr = g_pend.addr;
        g_pend.who = nullptr;
    }
    if (g_viahub && g_hub.addr == addr) return &g_hub;
    if (g_dev && reachable(g_dev) && g_dev->addr == addr) return g_dev;
    return nullptr;
}

bool hears(SimDev* d)
{
    if (!d || d->dead) return false;
    if (d == g_dev && (int32_t)(g_now - g_deaf_until) < 0) return false;
    return true;
}

void bus_reset(SimDev* d)
{
    if (!d) return;
    d->addr   = 0;
    d->config = 0;
    d->wedged = false;
    d->resets++;
    if (g_pend.who == d) g_pend.who = nullptr;
    if (d == g_dev) g_deaf_until = g_now + d->deaf_after_reset_ms;
}

/* a SETUP to d: true when it is taken (false: STALL in the data/status stage) */
bool setup(SimDev* d, const uint8_t* s)
{
    const uint8_t  type = s[0], req = s[1];
    const uint16_t wv = (uint16_t)(s[2] | s[3] << 8), wi = (uint16_t)(s[4] | s[5] << 8),
                   wl = (uint16_t)(s[6] | s[7] << 8);
    g_ctl = Ctl{};
    g_ctl.who = d;
    auto reply = [&](const uint8_t* p, uint16_t n) {
        g_ctl.reply     = p;
        g_ctl.reply_len = n < wl ? n : wl;
    };
    if (type == 0x80 && req == 6)
    {
        const uint8_t dt = (uint8_t)(wv >> 8), ix = (uint8_t)wv;
        if (dt == 1) { reply(d->dev_desc, 18); return true; }
        if (dt == 2) { reply(d->cfg, d->cfg_len); return true; }
        if (dt == 3)
        {
            d->strings_asked++;
            if (ix < 4 && d->strings[ix]) { reply(d->strings[ix], d->strings[ix][0]); return true; }
            return false;
        }
        return false;
    }
    if (type == 0x00 && req == 5) { g_ctl.addr_after_status = true; g_ctl.new_addr = (uint8_t)(wv & 0x7F); return true; }
    if (type == 0x00 && req == 9) { d->config = (uint8_t)wv; d->sets_config++; return true; }
    if ((type == 0x02 || type == 0x00) && (req == 1 || req == 3)) return true;   /* clear/set feature */
    if (d == &g_hub)
    {
        if (type == 0xA0 && req == 6) { reply(kHubDesc, 9); return true; }
        if (type == 0x23 && wi == 1)
        {
            if (req == 3 && wv == 8) { hub_powered = true; return true; }          /* PORT_POWER */
            if (req == 3 && wv == 4)                                              /* PORT_RESET */
            {
                if (g_dev) bus_reset(g_dev);
                port_enabled = g_dev != nullptr;
                port_change |= 0x10;
                return true;
            }
            if (req == 1)
            {
                if (wv == 16) port_change &= (uint16_t)~0x01;   /* C_PORT_CONNECTION */
                if (wv == 20) port_change &= (uint16_t)~0x10;   /* C_PORT_RESET */
                if (wv == 1) port_enabled = false;              /* PORT_ENABLE */
                return true;
            }
            return true;
        }
        if (type == 0x23) return true;   /* other ports: power and so on */
        if (type == 0xA3 && req == 0)
        {
            uint16_t st = 0;
            if (wi == 1 && g_dev && hub_powered) st |= 0x01;
            if (wi == 1 && port_enabled) st |= 0x02;
            if (hub_powered) st |= 0x100;
            const uint16_t ch = wi == 1 ? port_change : 0;
            hub_status[0] = (uint8_t)st; hub_status[1] = (uint8_t)(st >> 8);
            hub_status[2] = (uint8_t)ch; hub_status[3] = (uint8_t)(ch >> 8);
            reply(hub_status, 4);
            return true;
        }
    }
    return false;
}
} // namespace

namespace sim
{
uint32_t Now() { return g_now; }
void Advance(USBH_HandleTypeDef* ph, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++)
    {
        g_now++;
        USBH_LL_IncTimer(ph);
    }
}
void Plug(USBH_HandleTypeDef* ph, SimDev* dev, bool via_hub)
{
    g_dev    = dev;
    g_viahub = via_hub;
    dev->addr = dev->config = 0;
    if (via_hub)
    {
        g_hub = SimDev{};
        g_hub.dev_desc = kHubDev;
        g_hub.cfg      = kHubCfg;
        g_hub.cfg_len  = sizeof kHubCfg;
        hub_powered = port_enabled = port_reset = false;
        port_change = 0x01;   /* C_PORT_CONNECTION */
    }
    USBH_LL_Connect(ph);
}
void Unplug(USBH_HandleTypeDef* ph)
{
    g_dev = nullptr;
    g_viahub = false;
    USBH_LL_Disconnect(ph);
}
void QueueMidi(const uint8_t pkt[4]) { g_midi.push_back({pkt[0], pkt[1], pkt[2], pkt[3]}); }
void Reset()
{
    g_now = 0;
    g_dev = nullptr;
    g_viahub = false;
    g_midi.clear();
    g_pend = Pend{};
    g_ctl = Ctl{};
    for (auto& p : g_pipes) p = Pipe{};
}
SimDev* Hub() { return &g_hub; }
} // namespace sim

/* ---- libDaisy's usbh_conf.c, as the core calls it ---- */
extern "C" {
uint32_t HAL_GetTick(void) { return g_now; }
void USBH_Delay(uint32_t ms) { g_now += ms; }

USBH_StatusTypeDef USBH_LL_Init(USBH_HandleTypeDef*) { return USBH_OK; }
USBH_StatusTypeDef USBH_LL_DeInit(USBH_HandleTypeDef*) { return USBH_OK; }
USBH_StatusTypeDef USBH_LL_Start(USBH_HandleTypeDef* ph)
{
    if (g_dev) USBH_LL_Connect(ph);   /* a device already on the port is seen at once */
    return USBH_OK;
}
USBH_StatusTypeDef USBH_LL_Stop(USBH_HandleTypeDef*) { return USBH_OK; }
USBH_SpeedTypeDef USBH_LL_GetSpeed(USBH_HandleTypeDef*) { return USBH_SPEED_FULL; }
USBH_StatusTypeDef USBH_LL_ResetPort(USBH_HandleTypeDef* ph)
{
    if (g_viahub) { bus_reset(&g_hub); hub_powered = port_enabled = false; port_change = g_dev ? 0x01 : 0; }
    else bus_reset(g_dev);
    if (g_dev) USBH_LL_PortEnabled(ph);
    return USBH_OK;
}
uint32_t USBH_LL_GetLastXferSize(USBH_HandleTypeDef*, uint8_t pipe) { return g_pipes[pipe].xfer; }
USBH_StatusTypeDef USBH_LL_DriverVBUS(USBH_HandleTypeDef*, uint8_t) { return USBH_OK; }
USBH_StatusTypeDef USBH_LL_OpenPipe(USBH_HandleTypeDef*, uint8_t pipe, uint8_t ep, uint8_t addr, uint8_t,
                                    uint8_t type, uint16_t mps)
{
    Pipe& p    = g_pipes[pipe];
    p.ep       = ep;
    p.dev_addr = addr;
    p.type     = type;
    p.mps      = mps;
    p.urb      = USBH_URB_IDLE;
    p.pending  = nullptr;
    return USBH_OK;
}
USBH_StatusTypeDef USBH_LL_ClosePipe(USBH_HandleTypeDef*, uint8_t pipe)
{
    g_pipes[pipe].pending = nullptr;   /* halted */
    return USBH_OK;
}
USBH_StatusTypeDef USBH_LL_SubmitURB(USBH_HandleTypeDef*, uint8_t pipe, uint8_t direction, uint8_t ep_type,
                                     uint8_t token, uint8_t* buf, uint16_t length, uint8_t)
{
    Pipe& p = g_pipes[pipe];
    p.xfer    = 0;
    p.pending = nullptr;
    SimDev* d = who_at(p.dev_addr);
    if (!hears(d)) { p.urb = USBH_URB_ERROR; return USBH_OK; }   /* no handshake (the HAL tried three times) */
    if (ep_type == USBH_EP_CONTROL)
    {
        if (token == 0)   /* SETUP */
        {
            d->setups++;
            if (d->setups <= d->drop_first || (d->drop_every && d->setups % d->drop_every == 0))
            {
                p.urb = USBH_URB_ERROR;
                return USBH_OK;
            }
            g_ctl.stall = !setup(d, buf);
            p.urb = USBH_URB_DONE;
            return USBH_OK;
        }
        if (d->wedged) { p.urb = USBH_URB_NOTREADY; return USBH_OK; }   /* NAK, NAK, ... */
        if (g_ctl.who != d || g_ctl.stall)
        {
            if (g_ctl.stall && d->wedge_after_stall) d->wedged = true;
            p.urb = USBH_URB_STALL;
            return USBH_OK;
        }
        if (direction == 1 && length && g_ctl.reply)   /* data IN */
        {
            const uint16_t n = length < g_ctl.reply_len ? length : g_ctl.reply_len;
            memcpy(buf, g_ctl.reply, n);
            p.xfer = n;
        }
        if (length == 0 && g_ctl.addr_after_status)   /* status of SET_ADDRESS */
        {
            g_pend.who  = d;
            g_pend.addr = g_ctl.new_addr;
            g_pend.at   = g_now + d->addr_lag_ms;
            g_ctl.addr_after_status = false;
        }
        p.urb = USBH_URB_DONE;
        return USBH_OK;
    }
    if (ep_type == USBH_EP_BULK && direction == 1)
    {
        p.pending  = buf;   /* NAKed until it has something: the URB stays idle */
        p.pend_dev = d;
        p.urb      = USBH_URB_IDLE;
        return USBH_OK;
    }
    if (ep_type == USBH_EP_BULK) { p.urb = USBH_URB_DONE; return USBH_OK; }
    p.urb = USBH_URB_NOTREADY;   /* interrupt IN (the hub's status change): nothing */
    return USBH_OK;
}
USBH_URBStateTypeDef USBH_LL_GetURBState(USBH_HandleTypeDef*, uint8_t pipe)
{
    Pipe& p = g_pipes[pipe];
    if (p.pending)
    {
        SimDev* d = p.pend_dev;
        if (d != g_dev || !reachable(d) || d->addr != p.dev_addr) { p.pending = nullptr; p.urb = USBH_URB_ERROR; }
        else if (d != &g_hub && d->config && p.ep == d->midi_in_ep && !g_midi.empty())
        {
            const auto pk = g_midi.front();
            g_midi.pop_front();
            memcpy(p.pending, pk.data(), 4);
            p.pending = nullptr;
            p.xfer    = 4;
            p.urb     = USBH_URB_DONE;
        }
    }
    return p.urb;
}
USBH_StatusTypeDef USBH_LL_SetToggle(USBH_HandleTypeDef*, uint8_t pipe, uint8_t t)
{
    g_pipes[pipe].toggle = t;
    return USBH_OK;
}
uint8_t USBH_LL_GetToggle(USBH_HandleTypeDef*, uint8_t pipe) { return g_pipes[pipe].toggle; }
}
