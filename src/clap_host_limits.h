#ifndef CLAP_HOST_LIMITS_H
#define CLAP_HOST_LIMITS_H
/*
 * GENERATED — DO NOT EDIT BY HAND.
 * Produced by openmixer's `node harness/contract-limits-gen.mjs --clap-host-limits <file>` from
 * HOSTED_STAGE_LIMITS (packages/core/src/hosted-stage-limits.ts) and CLAP_CORE_REFUSALS over
 * HOSTING_CODE (packages/plugin-qualify/src/hosting-suitability.ts).
 * Committed here because this repository builds without that tree; openmixer's
 * contract-limits-generated ratchet requires it byte-identical to a fresh render. Change a number
 * there, regenerate, commit the result here.
 */

#include <stddef.h>

/* the stage and the host: every number the core reads */
#define CLAP_HOST_WARMUP_BLOCKS 64u
#define CLAP_HOST_WARMUP_LEVEL_DBFS -6.0f
#define CLAP_HOST_NON_FINITE_STRIKES 3u
#define CLAP_HOST_CLAMP_DBFS 24.0f
#define CLAP_HOST_PARAM_QUEUE_DEPTH 256u
#define CLAP_HOST_EVENTS_PER_BLOCK 64u
#define CLAP_HOST_UNPUBLISH_POLL_US 100u
#define CLAP_HOST_UNPUBLISH_TIMEOUT_US 2000000u
#define CLAP_HOST_NOTES_PER_BLOCK 256u
#define CLAP_HOST_MAIN_PORT_CHANNELS 2u
#define CLAP_HOST_AUX_OUTPUTS 8u
#define CLAP_HOST_STATE_MAX_BYTES 1048576u
#define CLAP_HOST_LOG_BYTES 256u
#define CLAP_HOST_ROLE_POLL_US 1000u
#define CLAP_HOST_ROLE_TIMEOUT_US 200000u
#define CLAP_HOST_PARAM_COUNT_MAX 2048u

/* the refusals the core returns, as the hosting codes the console's verdicts use */
#define CLAP_HOST_CODE_HEADLESS_FAILED "hosting.clap.headless-failed"
#define CLAP_HOST_CODE_NOT_AUDIO_EFFECT "hosting.clap.not-audio-effect"
#define CLAP_HOST_CODE_NOTE_INPUT "hosting.clap.note-input"
#define CLAP_HOST_CODE_NO_AUDIO_INPUT "hosting.topology.no-audio-input"
#define CLAP_HOST_CODE_NO_AUDIO_OUTPUT "hosting.topology.no-audio-output"
#define CLAP_HOST_CODE_WIDER_THAN_STRIP "hosting.topology.wider-than-strip"
#define CLAP_HOST_CODE_EXTRA_INPUTS "hosting.topology.extra-inputs-fed-silence"
#define CLAP_HOST_CODE_CRASHED_LIVE "hosting.stability.crashed-live-on-this-rig"
#define CLAP_HOST_CODE_OUTPUT_NON_FINITE "hosting.output.non-finite"

#endif /* CLAP_HOST_LIMITS_H */
