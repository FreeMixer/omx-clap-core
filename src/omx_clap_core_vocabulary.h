// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#ifndef OMX_CLAP_CORE_VOCABULARY_H
#define OMX_CLAP_CORE_VOCABULARY_H
/*
 * GENERATED — DO NOT EDIT BY HAND.
 * Produced by `omx-contract render --target c` from FreeMixer/omx-clap-core's data/: the vocabulary
 * (docs/data-layout.md). A code is OMX_ and the code in upper snake case, and a reply code that carries a resp has it as that name and _RESP; a word is OMX_<ITEM>_<NAME>, each words
 * item with its _COUNT and an _INIT of its words in their order. A consumer writes and compares these, never a
 * literal of its own.
 */

/* HOSTING_CLAP_CODES (data/hosting/clap.json) */
#define OMX_HOSTING_CLAP_EXTENSIONS_NOT_COVERED "hosting.clap.extensions-not-covered"
#define OMX_HOSTING_CLAP_FORMAT_NOT_HOSTED "hosting.clap.format-not-hosted"
#define OMX_HOSTING_CLAP_HEADLESS_FAILED "hosting.clap.headless-failed"
#define OMX_HOSTING_CLAP_LATENCY_MISMATCH "hosting.clap.latency-mismatch"
#define OMX_HOSTING_CLAP_NOTE_INPUT "hosting.clap.note-input" ///< the plugin declares a note input the configuration refuses
#define OMX_HOSTING_CLAP_PROCESS_ERROR "hosting.clap.process-error"
#define OMX_HOSTING_CLAP_THREAD_VIOLATION "hosting.clap.thread-violation"
#define OMX_HOSTING_CLAP_UNQUALIFIED "hosting.clap.unqualified"

/* HOSTING_COST_CODES (data/hosting/cost.json) */
#define OMX_HOSTING_COST_ABOVE_CEILING "hosting.cost.above-ceiling"
#define OMX_HOSTING_COST_FIELDS_MISSING "hosting.cost.fields-missing"
#define OMX_HOSTING_COST_SPIKY_TAIL "hosting.cost.spiky-tail"
#define OMX_HOSTING_COST_UNMEASURED_AT_RATE_AND_QUANTUM "hosting.cost.unmeasured-at-rate-and-quantum"
#define OMX_HOSTING_COST_WITHIN_CEILING "hosting.cost.within-ceiling"

/* HOSTING_FEATURES_CODES (data/hosting/features.json) */
#define OMX_HOSTING_FEATURES_ATOM_IN_UNFED "hosting.features.atom-in-unfed" ///< a non-MIDI atom input no buffer here can feed
#define OMX_HOSTING_FEATURES_HOST_NOT_PROVIDED "hosting.features.host-not-provided"
#define OMX_HOSTING_FEATURES_MIDI_IN_FED_EMPTY "hosting.features.midi-in-fed-empty"
#define OMX_HOSTING_FEATURES_MISSING "hosting.features.missing"
#define OMX_HOSTING_FEATURES_PORTS_UNREADABLE "hosting.features.ports-unreadable" ///< the plugin declares ports its reader cannot read (a port node shared by several plugins of one bundle, a port with no symbol), so none reaches the host: the bundle's data is the fault, not the layout (omx-clap-core#255)
#define OMX_HOSTING_FEATURES_ALL_PROVIDED "hosting.features.all-provided"
#define OMX_HOSTING_FEATURES_SCAN_PREDATES_FIELD "hosting.features.scan-predates-field"

/* HOSTING_LATENCY_CODES (data/hosting/latency.json) */
#define OMX_HOSTING_LATENCY_NONCONFORMING "hosting.latency.nonconforming"
#define OMX_HOSTING_LATENCY_PARAMETER_DEPENDENT "hosting.latency.parameter-dependent"
#define OMX_HOSTING_LATENCY_RESOLVED "hosting.latency.resolved"
#define OMX_HOSTING_LATENCY_UNMEASURED "hosting.latency.unmeasured"

/* HOSTING_OUTPUT_CODES (data/hosting/output.json) */
#define OMX_HOSTING_OUTPUT_NON_FINITE "hosting.output.non-finite" ///< the plugin produced non-finite output

/* HOSTING_OVERRIDE_CODES (data/hosting/override.json) */
#define OMX_HOSTING_OVERRIDE_CURATOR_ISOLATED "hosting.override.curator-isolated"

/* HOSTING_PATCH_CODES (data/hosting/patch.json) */
#define OMX_HOSTING_PATCH_BUSY "hosting.patch.busy"
#define OMX_HOSTING_PATCH_NOT_WRITABLE "hosting.patch.not-writable"
#define OMX_HOSTING_PATCH_TOO_LARGE "hosting.patch.too-large"

/* HOSTING_PRESET_CODES (data/hosting/preset.json) */
#define OMX_HOSTING_PRESET_NOT_FOUND "hosting.preset.not-found"

/* HOSTING_PROVENANCE_CODES (data/hosting/provenance.json) */
#define OMX_HOSTING_PROVENANCE_HOST_DIFFERS "hosting.provenance.host-differs"
#define OMX_HOSTING_PROVENANCE_SWEEP_CONTROL_DID_NOT_FIRE "hosting.provenance.sweep-control-did-not-fire"

/* HOSTING_REALISED_CODES (data/hosting/realised.json) */
#define OMX_HOSTING_DISABLED "hosting.disabled"
#define OMX_HOSTING_LINKED "hosting.linked"
#define OMX_HOSTING_NO_REALISATION "hosting.no-realisation"
#define OMX_HOSTING_REALISED_IN_PROCESS "hosting.realised.in-process"
#define OMX_HOSTING_UNJUDGED "hosting.unjudged"

/* HOSTING_RESOURCE_HINTS_CODES (data/hosting/resource-hints.json) */
#define OMX_HOSTING_RESOURCE_HINTS_OUT_OF_RANGE "hosting.resource-hints.out-of-range" ///< a figure below zero, or past what its hint's member holds
#define OMX_HOSTING_RESOURCE_HINTS_UNKNOWN_HINT "hosting.resource-hints.unknown-hint" ///< the declared mask sets a bit RESOURCE_HINTS has no hint for
#define OMX_HOSTING_RESOURCE_HINTS_WRONG_UNIT "hosting.resource-hints.wrong-unit" ///< a figure not in its hint's unit: a flag that is not a boolean, a count, a time or a size that is not an integer

/* HOSTING_RT_SAFETY_CODES (data/hosting/rt-safety.json) */
#define OMX_HOSTING_RT_SAFETY_CLEAN_UNDER_SWEEP "hosting.rt-safety.clean-under-sweep"
#define OMX_HOSTING_RT_SAFETY_NOT_REPRODUCIBLE "hosting.rt-safety.not-reproducible"
#define OMX_HOSTING_RT_SAFETY_CLEAN_UNDER_NOTES "hosting.rt-safety.clean-under-notes"
#define OMX_HOSTING_RT_SAFETY_NOTES_UNMEASURED "hosting.rt-safety.notes-unmeasured"
#define OMX_HOSTING_RT_SAFETY_VIOLATIONS_UNDER_NOTES "hosting.rt-safety.violations-under-notes"
#define OMX_HOSTING_RT_SAFETY_UNMEASURED "hosting.rt-safety.unmeasured"
#define OMX_HOSTING_RT_SAFETY_VIOLATIONS_IN_RUN "hosting.rt-safety.violations-in-run"

/* HOSTING_STABILITY_CODES (data/hosting/stability.json) */
#define OMX_HOSTING_STABILITY_CYCLES_BELOW_FLOOR "hosting.stability.cycles-below-floor"
#define OMX_HOSTING_STABILITY_CRASH_UNATTRIBUTED "hosting.stability.crash-unattributed"
#define OMX_HOSTING_STABILITY_CRASHED_ATTRIBUTED_PLUGIN "hosting.stability.crashed-attributed-plugin"
#define OMX_HOSTING_STABILITY_CRASHED_LIVE_ON_THIS_RIG "hosting.stability.crashed-live-on-this-rig" ///< the plugin crashed live on this rig
#define OMX_HOSTING_STABILITY_LINKS_GUI_TOOLKIT "hosting.stability.links-gui-toolkit"
#define OMX_HOSTING_STABILITY_SOAK_FAILED_AT_RATE "hosting.stability.soak-failed-at-rate"
#define OMX_HOSTING_STABILITY_SOAKED "hosting.stability.soaked"
#define OMX_HOSTING_STABILITY_SPAWNS_THREADS "hosting.stability.spawns-threads"
#define OMX_HOSTING_STABILITY_UNMEASURED "hosting.stability.unmeasured"
#define OMX_HOSTING_STABILITY_UNSOAKED "hosting.stability.unsoaked"

/* HOSTING_STATE_CODES (data/hosting/state.json) */
#define OMX_HOSTING_STATE_NOT_A_COPY "hosting.state.not-a-copy" ///< not a save's copy directory of the root
#define OMX_HOSTING_STATE_PATH_ON_AUDIO "hosting.state.path-on-audio" ///< makePath: called on the audio role
#define OMX_HOSTING_STATE_PATH_OUTSIDE "hosting.state.path-outside" ///< makePath: empty, absolute, or a `..` component
#define OMX_HOSTING_STATE_PATH_SYMLINK "hosting.state.path-symlink" ///< makePath: a component is a symbolic link
#define OMX_HOSTING_STATE_PATH_TOO_LONG "hosting.state.path-too-long" ///< makePath: PATH_MAX or more, or a long component
#define OMX_HOSTING_STATE_PATH_UNMAKEABLE "hosting.state.path-unmakeable" ///< makePath: a directory could not be made
#define OMX_HOSTING_STATE_UNREADABLE "hosting.state.unreadable"

/* HOSTING_TOPOLOGY_CODES (data/hosting/topology.json) */
#define OMX_HOSTING_TOPOLOGY_AMBISONIC_CONFIG "hosting.topology.ambisonic-config" ///< an ambisonic port refuses the carrier's ordering or normalisation
#define OMX_HOSTING_TOPOLOGY_CHANNEL_MAP_UNKNOWN "hosting.topology.channel-map-unknown" ///< a surround port names a channel id the host does not know, or refuses the carrier's channel mask
#define OMX_HOSTING_TOPOLOGY_INSTRUMENT "hosting.topology.instrument"
#define OMX_HOSTING_TOPOLOGY_MATCHED "hosting.topology.matched"
#define OMX_HOSTING_TOPOLOGY_NO_AUDIO_INPUT "hosting.topology.no-audio-input" ///< the plugin has no audio input
#define OMX_HOSTING_TOPOLOGY_NO_AUDIO_OUTPUT "hosting.topology.no-audio-output" ///< the plugin has no audio output
#define OMX_HOSTING_TOPOLOGY_PORT_TOO_WIDE "hosting.topology.port-too-wide" ///< a port of more channels than the limit, or an ambisonic port whose channels are not (order+1)²
#define OMX_HOSTING_TOPOLOGY_PORT_WITHOUT_CHANNELS "hosting.topology.port-without-channels" ///< a port of zero channels
#define OMX_HOSTING_TOPOLOGY_TOO_MANY_PORTS "hosting.topology.too-many-ports" ///< more than the limit of auxiliary ports on one side, or more than one main port on a side
#define OMX_HOSTING_TOPOLOGY_WIDER_THAN_STRIP "hosting.topology.wider-than-strip" ///< a main port is wider than the lane the caller runs the plugin on

/* HOSTING_WORKER_CODES (data/hosting/worker.json) */
#define OMX_HOSTING_WORKER_NONCONFORMING "hosting.worker.nonconforming"

/* HOST_REPLY_CODES (data/verbs.json) */
#define OMX_REPLY_HOST_INSTANCE_INVALID "reply.host.instance-invalid" ///< mod-host's ERR_INSTANCE_INVALID: an instance number outside the host's table
#define OMX_REPLY_HOST_INSTANCE_INVALID_RESP (-1)
#define OMX_REPLY_HOST_INSTANCE_NON_EXISTS "reply.host.instance-non-exists" ///< mod-host's ERR_INSTANCE_NON_EXISTS: an instance no add made
#define OMX_REPLY_HOST_INSTANCE_NON_EXISTS_RESP (-3)
#define OMX_REPLY_HOST_PARAM_SYMBOL_INVALID "reply.host.param-symbol-invalid" ///< mod-host's ERR_LV2_INVALID_PARAM_SYMBOL: a symbol that is not a parameter param_set takes, the plugin's own bypass included
#define OMX_REPLY_HOST_PARAM_SYMBOL_INVALID_RESP (-103)
#define OMX_REPLY_HOST_MEMORY_ALLOCATION "reply.host.memory-allocation" ///< mod-host's ERR_MEMORY_ALLOCATION: the reply could not be built
#define OMX_REPLY_HOST_MEMORY_ALLOCATION_RESP (-901)
#define OMX_REPLY_HOST_INVALID_OPERATION "reply.host.invalid-operation" ///< mod-host's ERR_INVALID_OPERATION: a word outside the verb's grammar, or a verb the host refuses on purpose
#define OMX_REPLY_HOST_INVALID_OPERATION_RESP (-902)
#define OMX_REPLY_HOST_PIN_ABSENT "reply.host.pin-absent" ///< a layout pin in a scheme this host does not know, refused as no pin at all
#define OMX_REPLY_HOST_PIN_ABSENT_RESP (-508)
#define OMX_REPLY_HOST_PIN_LAYOUT_MISMATCH "reply.host.pin-layout-mismatch" ///< the add of an instance whose layout after init is not the one pin_expect named: the plugin is not activated
#define OMX_REPLY_HOST_PIN_LAYOUT_MISMATCH_RESP (-510)
#define OMX_REPLY_HOST_NO_PARAM_CONTRACT "reply.host.no-param-contract" ///< the host holds no checked unit and scale for this parameter: the controller falls back to the layout the scan gives
#define OMX_REPLY_HOST_NO_PARAM_CONTRACT_RESP (-511)
#define OMX_REPLY_HOST_NO_NOTE_INPUT "reply.host.no-note-input" ///< the instance's plugin takes no notes
#define OMX_REPLY_HOST_NO_NOTE_INPUT_RESP (-513)
#define OMX_REPLY_HOST_NOTES_BY_JACK "reply.host.notes-by-jack" ///< the instance's worker takes no notes on its socket (mod-host): the plugin is played through its jack MIDI input
#define OMX_REPLY_HOST_NOTES_BY_JACK_RESP (-514)
#define OMX_REPLY_HOST_NOTE_QUEUE_FULL "reply.host.note-queue-full" ///< the worker has no room for the line's notes before its next block: nothing of it is kept, and the line may be sent again
#define OMX_REPLY_HOST_NOTE_QUEUE_FULL_RESP (-517)

/* HOST_TRACK_KINDS (data/verbs.json) */
#define OMX_HOST_TRACK_KINDS_BUS "bus"
#define OMX_HOST_TRACK_KINDS_RETURN "return"
#define OMX_HOST_TRACK_KINDS_MASTER "master"
#define OMX_HOST_TRACK_KINDS_COUNT 3u
#define OMX_HOST_TRACK_KINDS_INIT { "bus", "return", "master" }

/* HOST_EVENTS (data/verbs.json) */
#define OMX_HOST_EVENTS_REMOTE_PAGES_CHANGED "remote_pages_changed" ///< the plugin changed its remote pages: a controller reads remote_pages and remote_page_get again
#define OMX_HOST_EVENTS_COUNT 1u
#define OMX_HOST_EVENTS_INIT { "remote_pages_changed" }

#endif /* OMX_CLAP_CORE_VOCABULARY_H */
