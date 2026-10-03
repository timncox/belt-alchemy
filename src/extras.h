/*
 * The values no control on the panel can express, carried in the preset
 * blob so they survive a power cycle:
 *
 *   cpu_peak  The worst-case audio-block load seen during the PREVIOUS
 *             session, replayed on a ring at boot. Seven TD-PSOLA voices on
 *             one Cortex-M7 is the open question of this port, and the live
 *             alarm on B2 can only be read by someone watching it, which you
 *             are not while singing.
 *   prog      The internal chord sequencer's progression (seq-alchemy's
 *             seq_prog_t: how many chords, each one's scale degree and
 *             bars), edited on the Launchpad's CHORDS page. Key, scale and
 *             triads / sevenths come from the panel and Settings instead.
 *   cv_off    The CV chord inputs' learned offsets, J4..J7, millivolts
 *             (Settings, Sources page, CV cal = Learn).
 *
 * Every Alchemy firmware in this family (Smack, Mark, Belt) has its own tag
 * here, so a slot 0 written by one of them fails the schema gate in the next
 * and lands in that firmware's first-boot defaults rather than being read as
 * its own settings.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include "alchemy/surface/serializable.h"

struct BeltExtras : public alchemy::Serializable
{
    float cpu_peak = 0.0f;   /* 0..1; 0 == no data yet */
    float cpu_avg  = 0.0f;   /* 0..1: worst smoothed load this session */

    /* seq_init()'s progression: i - VI - III - VII, a bar each */
    uint8_t prog_count     = 4;
    int8_t  prog_degree[8] = {0, 5, 2, 6, 0, 0, 0, 0};
    uint8_t prog_bars[8]   = {1, 1, 1, 1, 1, 1, 1, 1};
    int16_t cv_off_mv[4]   = {0, 0, 0, 0};

    size_t SerializedSize() const override { return 8u + 17u + 8u; }

    void Serialize(uint8_t* out) const override
    {
        std::memcpy(out, &cpu_peak, 4u);
        std::memcpy(out + 4u, &cpu_avg, 4u);
        out[8] = prog_count;
        std::memcpy(out + 9u, prog_degree, 8u);
        std::memcpy(out + 17u, prog_bars, 8u);
        std::memcpy(out + 25u, cv_off_mv, 8u);
    }

    bool Deserialize(const uint8_t* in) override
    {
        float p;
        std::memcpy(&p, in, 4u);
        /* Refuse values a stale or foreign blob could carry; the schema hash
         * catches layout changes, this catches garbage inside a valid one. */
        if (!(p >= 0.0f && p <= 1.0f)) p = 0.0f;
        cpu_peak = p;
        std::memcpy(&p, in + 4u, 4u);
        if (!(p >= 0.0f && p <= 1.0f)) p = 0.0f;
        cpu_avg = p;
        /* The ranges seq.c's state_sanitize keeps. */
        prog_count = in[8] < 1 ? 1 : (in[8] > 8 ? 8 : in[8]);
        std::memcpy(prog_degree, in + 9u, 8u);
        std::memcpy(prog_bars, in + 17u, 8u);
        for (int i = 0; i < 8; i++)
        {
            if (prog_degree[i] < -7) prog_degree[i] = -7;
            if (prog_degree[i] > 13) prog_degree[i] = 13;
            if (prog_bars[i] < 1) prog_bars[i] = 1;
            if (prog_bars[i] > 8) prog_bars[i] = 8;
        }
        std::memcpy(cv_off_mv, in + 25u, 8u);
        for (int i = 0; i < 4; i++)
            if (cv_off_mv[i] < -250 || cv_off_mv[i] > 250) cv_off_mv[i] = 0;
        return true;
    }

    /* 'BLT' + layout version. Bump the low byte when the layout changes.
     * 0x03: the chord sources' progression and CV offsets. */
    uint32_t SchemaHash() const override { return 0x424C5400u | 0x03u; }
};
