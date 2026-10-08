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

/** @file
 * @brief The limits and refusal codes of the hosting code. */

#include <stddef.h>

/** the stage and the host: every number the core reads */
#define CLAP_HOST_WARMUP_BLOCKS 64u
#define CLAP_HOST_WARMUP_LEVEL_DBFS -6.0f  ///< the level of the warm-up signal, in dBFS
#define CLAP_HOST_NON_FINITE_STRIKES 3u  ///< the blocks of non-finite output that strike a stage
#define CLAP_HOST_CLAMP_DBFS 24.0f  ///< the level the output is clamped at, in dBFS
#define CLAP_HOST_PARAM_QUEUE_DEPTH 256u  ///< the records the parameter queue holds
#define CLAP_HOST_EVENTS_PER_BLOCK 64u  ///< the parameter events delivered in one block
#define CLAP_HOST_UNPUBLISH_POLL_US 100u  ///< the poll interval while an unpublish waits for the audio thread, in microseconds
#define CLAP_HOST_UNPUBLISH_TIMEOUT_US 2000000u  ///< the longest an unpublish waits for the audio thread, in microseconds
#define CLAP_HOST_NOTES_PER_BLOCK 256u  ///< the note events delivered in one block
#define CLAP_HOST_MAIN_PORT_CHANNELS 2u  ///< the channels the main port is limited to
#define CLAP_HOST_AUX_OUTPUTS 8u  ///< the auxiliary output ports the core accepts
#define CLAP_HOST_STATE_MAX_BYTES 1048576u  ///< the largest plugin state the core saves or loads, in bytes
#define CLAP_HOST_LOG_BYTES 256u  ///< the size of a plugin log message, in bytes
#define CLAP_HOST_ROLE_POLL_US 1000u  ///< the poll interval while the audio role is taken, in microseconds
#define CLAP_HOST_ROLE_TIMEOUT_US 200000u  ///< the longest the audio role is waited for, in microseconds
#define CLAP_HOST_PARAM_COUNT_MAX 2048u  ///< the widest parameter count the core trusts

/** the refusals the core returns, as the hosting codes the console's verdicts use */
#define CLAP_HOST_CODE_HEADLESS_FAILED "hosting.clap.headless-failed"
#define CLAP_HOST_CODE_NOT_AUDIO_EFFECT "hosting.clap.not-audio-effect"  ///< the plugin is not an audio effect
#define CLAP_HOST_CODE_NOTE_INPUT "hosting.clap.note-input"  ///< the plugin declares a note input the configuration refuses
#define CLAP_HOST_CODE_NO_AUDIO_INPUT "hosting.topology.no-audio-input"  ///< the plugin has no audio input
#define CLAP_HOST_CODE_NO_AUDIO_OUTPUT "hosting.topology.no-audio-output"  ///< the plugin has no audio output
#define CLAP_HOST_CODE_WIDER_THAN_STRIP "hosting.topology.wider-than-strip"  ///< the plugin is wider than the strip
#define CLAP_HOST_CODE_EXTRA_INPUTS "hosting.topology.extra-inputs-fed-silence"  ///< the plugin has extra inputs, fed silence
#define CLAP_HOST_CODE_CRASHED_LIVE "hosting.stability.crashed-live-on-this-rig"  ///< the plugin crashed live on this rig
#define CLAP_HOST_CODE_OUTPUT_NON_FINITE "hosting.output.non-finite"  ///< the plugin produced non-finite output

#endif /* CLAP_HOST_LIMITS_H */
