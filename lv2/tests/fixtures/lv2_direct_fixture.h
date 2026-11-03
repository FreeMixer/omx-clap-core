/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
#ifndef LV2_DIRECT_FIXTURE_H
#define LV2_DIRECT_FIXTURE_H

#include <stdint.h>

#define DIRECT_FIXTURE_URI "urn:openmixer:test:direct-fixture"

/* what the last instance saw: the memory its ports held at its last run, and how many times it was told of one. A host that
 * copies shows its own memory here, and one that connects shows the buffers it was handed */
struct lv2_direct_fixture_witness
{
    const float *in[2];
    const float *out[2];
    unsigned connects;      // connect_port calls on audio ports since the last reset of this structure
    unsigned runs;
    unsigned aliased;       // runs in which an input and an output were one buffer
    const float *deactivate_in; // where input 0 was when LV2 deactivate came
};
extern struct lv2_direct_fixture_witness lv2_direct_fixture_witness;

#endif
