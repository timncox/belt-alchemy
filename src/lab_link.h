/*
 * lab_link -- two Alchemy Labs sharing one set of controllers over USB.
 *
 * Lab A is the USB host: the Launchpad Mini MK3, Launch Control XL (and an
 * XInput pad) sit on its hub as usual. Lab B is plugged into the same hub as
 * a USB-MIDI *device* (its front port in "Link" mode). Lab A relays the
 * controllers to whichever Lab has FOCUS and shows that Lab's LEDs; pressing
 * the Launchpad's top-left and top-right buttons together switches focus.
 * (The rear-header UART would be simpler, but it is not usable yet.)
 *
 * Hardware-free (no libDaisy, no ST headers) so both ends can be tested
 * against each other on the laptop: launchpad.cpp is the thin glue.
 *
 * Wire format: plain MIDI bytes. Cable numbers are NOT used -- libDaisy's
 * device-side MIDI drops them -- so traffic is told apart by channel and
 * SysEx header:
 *   A -> B  Launchpad events exactly as the Mini sends them in Programmer
 *           mode (channel 1: 9n/8n notes 11..88, Bn CCs 19..99).
 *           Launch Control XL events exactly as the XL sends them in factory
 *           template 1 (channel 9: notes, CCs).
 *           F0 7D 4C 01 F7          hello: "are you a Lab?" + resend every LED
 *           F0 7D 4C 02 F7          release everything held (focus left B)
 *           F0 7D 4C 03 bits F7     controllers present: bit0 Mini, bit1 XL, bit2 pad
 *           F0 7D 4C 04 b0 b1 b2 F7 pad buttons (pad:: mask, 7 bits per byte)
 *   B -> A  Mini LEDs as the Mini takes them: channel 1, 9n note (grid) or
 *           Bn CC (side, top, logo), value = palette colour.
 *           XL LEDs as the XL takes them: F0 00 20 29 02 11 78 08 idx val F7.
 *           F0 7D 4C 41 F7          hello reply: "I am a Lab"
 * 7D is the MIDI non-commercial/educational manufacturer id; 4C = 'L'.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LLK_MINI_CELLS 81 /* grid 0-63 (y*8+x, y=0 top), side 64-71, top 72-79, logo 80 */
#define LLK_XL_LEDS    40 /* 24 knobs, then 2 rows of 8 buttons */
#define LLK_LOGO       80
#define LLK_UNKNOWN    0xFF

#define LLK_HAS_MINI 0x01
#define LLK_HAS_XL   0x02
#define LLK_HAS_PAD  0x04

/* Logo colours Lab A shows while a Lab is linked: whose controllers they are. */
#define LLK_LOGO_A 21 /* green */
#define LLK_LOGO_B 45 /* blue */

/* ---------------------------------------------------------------- mapping */

/* Mini cell <-> the message that lights / reports it. Returns -1 if none. */
int  llk_mini_cell(uint8_t status, uint8_t d1);
void llk_mini_msg(int cell, uint8_t* status, uint8_t* d1);

/* ------------------------------------------------------------ USB packets */

/* MIDI bytes (whole messages) -> USB-MIDI event packets on cable 0. Returns
 * bytes of packets written; stops before a message that would not fit. */
int llk_to_usb(const uint8_t* bytes, int n, uint8_t* pk, int cap, int* used);
/* USB-MIDI event packets -> MIDI bytes (cable ignored). Returns bytes. */
int llk_from_usb(const uint8_t* pk, int len, uint8_t* bytes, int cap);

/* ----------------------------------------------------------------- parser */

typedef struct
{
    uint8_t msg[24];
    int     n;
    int     want; /* bytes a channel message needs; 0 = in SysEx */
    int     in_sx;
} llk_parser_t;

/* Feeds bytes; calls cb with each complete message (channel 3-byte or SysEx). */
typedef void (*llk_msg_cb)(const uint8_t* msg, int n, void* ctx);
void llk_parse(llk_parser_t* p, const uint8_t* bytes, int n, llk_msg_cb cb, void* ctx);

/* ------------------------------------------------------------------ encode */

int llk_enc_hello(uint8_t* out);
int llk_enc_hello_reply(uint8_t* out);
int llk_enc_release(uint8_t* out);
int llk_enc_presence(uint8_t* out, uint8_t bits);
int llk_enc_pad(uint8_t* out, uint32_t mask);
int llk_enc_mini_led(uint8_t* out, int cell, uint8_t colour);
int llk_enc_xl_led(uint8_t* out, int idx, uint8_t colour);

/* ================================================================== Lab A */

typedef enum { LLK_NONE = 0, LLK_CANDIDATE = 1, LLK_LINKED = 2 } llk_state_t;

/* What happened to a controller event on Lab A. */
typedef enum
{
    LLK_LOCAL     = 0, /* Lab A has focus: handle it as usual */
    LLK_FORWARDED = 1, /* sent to Lab B (bytes appended to the out buffer) */
    LLK_SWALLOWED = 2, /* part of the focus combo: nobody gets it */
    LLK_TOGGLED   = 3, /* completed the focus combo: focus switched */
} llk_route_t;

typedef struct
{
    llk_state_t  state;
    int          focus_b;               /* 1 = Lab B owns the controllers */
    uint8_t      b_mini[LLK_MINI_CELLS]; /* Lab B's LEDs, kept even while A has focus */
    uint8_t      b_xl[LLK_XL_LEDS];
    uint8_t      presence;               /* last bits sent to B */
    uint32_t     pad_sent;
    uint8_t      combo_down;             /* bit0 top-left, bit1 top-right */
    uint8_t      combo_swallow;          /* releases still to swallow, same bits */
    llk_parser_t parser;
    uint32_t     hellos;                 /* hellos sent */
    uint32_t     replies;                /* hello replies seen */
} llk_host_t;

void llk_host_init(llk_host_t* h);
/* A Lab-shaped device appeared (VID/PID 0483:5740 MIDI) or went away. */
void llk_host_attach(llk_host_t* h);
void llk_host_detach(llk_host_t* h);
/* Bytes from Lab B: LED state, hello replies. */
void llk_host_from_b(llk_host_t* h, const uint8_t* bytes, int n);

/* A Mini / XL message (status, d1, d2) arriving at Lab A. When it should go to
 * Lab B its 3 bytes are appended to out (*outn advanced; out must have room). */
llk_route_t llk_host_mini_in(llk_host_t* h, uint8_t st, uint8_t d1, uint8_t d2, uint8_t* out, int* outn);
llk_route_t llk_host_xl_in(llk_host_t* h, uint8_t st, uint8_t d1, uint8_t d2, uint8_t* out, int* outn);

/* Switch focus. Appends what Lab B must be told (release / presence). */
void llk_host_set_focus(llk_host_t* h, int focus_b, uint8_t* out, int* outn);
/* Controllers present changed, or pad buttons changed: appends a message
 * only when Lab B needs it (linked, value changed; pad only with focus). */
void llk_host_presence(llk_host_t* h, uint8_t bits, uint8_t* out, int* outn);
void llk_host_pad(llk_host_t* h, uint32_t mask, uint8_t* out, int* outn);

/* The colour Lab A should show on a Mini cell / XL LED, given its own. */
uint8_t llk_host_mini_led(const llk_host_t* h, int cell, uint8_t own);
uint8_t llk_host_xl_led(const llk_host_t* h, int idx, uint8_t own);
/* True while Lab B, not Lab A, owns input. */
int llk_host_b_owns(const llk_host_t* h);

/* ================================================================== Lab B */

typedef struct
{
    void (*mini)(uint8_t st, uint8_t d1, uint8_t d2, void* ctx);
    void (*xl)(uint8_t st, uint8_t d1, uint8_t d2, void* ctx);
    void (*hello)(void* ctx);              /* reply, then resend every LED */
    void (*release)(void* ctx);
    void (*presence)(uint8_t bits, void* ctx);
    void (*pad)(uint32_t mask, void* ctx);
    void* ctx;
} llk_client_cb_t;

typedef struct
{
    llk_parser_t    parser;
    llk_client_cb_t cb;
    uint8_t         presence;
    uint32_t        pad;
    uint32_t        msgs;
} llk_client_t;

void llk_client_init(llk_client_t* c, const llk_client_cb_t* cb);
void llk_client_feed(llk_client_t* c, const uint8_t* bytes, int n);

#ifdef __cplusplus
}
#endif
