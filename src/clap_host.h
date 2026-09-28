/*
 * This file is part of omx-clap-host.
 *
 * omx-clap-host is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * omx-clap-host is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with omx-clap-host.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 */

/*
************************************************************************************************************************
*
************************************************************************************************************************
*/

#ifndef CLAP_HOST_H
#define CLAP_HOST_H


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <clap/clap.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>


/*
************************************************************************************************************************
*           CONFIGURATION DEFINES
************************************************************************************************************************
*/

#define CLAP_HOST_PARAM_QUEUE_DEPTH     256     // power of two
#define CLAP_HOST_EVENTS_PER_CYCLE      64
#define CLAP_HOST_MAX_CHANNELS          8
#define CLAP_HOST_STATE_MAX             (1024 * 1024)
#define CLAP_HOST_LOG_SIZE              256


/*
************************************************************************************************************************
*           DATA TYPES
************************************************************************************************************************
*/

/* Processing state, driven by the control thread and answered by the audio thread */
enum {
    CLAP_HOST_IDLE = 0,
    CLAP_HOST_ARMED,
    CLAP_HOST_PROCESSING,
    CLAP_HOST_STOPPING,
    CLAP_HOST_STOPPED
};

typedef struct CLAP_PARAM_RECORD_T {
    clap_id id;
    double value;
    void *cookie;
} clap_param_record_t;

/* Single producer (control thread), single consumer (audio thread) */
typedef struct CLAP_PARAM_QUEUE_T {
    clap_param_record_t records[CLAP_HOST_PARAM_QUEUE_DEPTH];
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
} clap_param_queue_t;

typedef struct CLAP_BINARY_T clap_binary_t;

typedef struct CLAP_INSTANCE_T {
    clap_binary_t *binary;
    const clap_plugin_descriptor_t *desc;
    const clap_plugin_t *plugin;
    const clap_plugin_params_t *params;
    const clap_plugin_latency_t *latency;
    const clap_plugin_audio_ports_t *audio_ports;
    const clap_plugin_note_ports_t *note_ports;
    const clap_plugin_state_t *state;
    const clap_plugin_preset_load_t *preset_load;
    clap_host_t host;

    uint32_t input_channels;
    uint32_t output_channels;

    // the plugin's own bypass parameter, CLAP_INVALID_ID when it has none
    clap_id bypass_param;
    void *bypass_cookie;
    double bypass_off;
    double bypass_on;

    double sample_rate;
    uint32_t max_frames;
    int active;

    float *input_buffers[CLAP_HOST_MAX_CHANNELS];
    float *output_buffers[CLAP_HOST_MAX_CHANNELS];
    clap_audio_buffer_t audio_in;
    clap_audio_buffer_t audio_out;
    clap_event_param_value_t events[CLAP_HOST_EVENTS_PER_CYCLE];
    uint32_t events_count;
    clap_input_events_t in_events;
    clap_output_events_t out_events;
    clap_process_t process;
    clap_param_queue_t queue;
    int64_t steady_time;

    pthread_t main_thread;
    pthread_t audio_thread;
    int audio_role_held;

    _Atomic uint32_t run_state;
    _Atomic uint32_t bypass;
    int rendered_wet;
    uint32_t latency_frames;

    _Atomic uint32_t restart_requested;
    _Atomic uint32_t callback_requested;
    _Atomic uint32_t flush_requested;
    _Atomic uint32_t latency_changed;
    _Atomic uint32_t state_dirty;
    _Atomic uint32_t thread_violations;
    _Atomic uint32_t log_pending;
    char log_slot[CLAP_HOST_LOG_SIZE];

    _Atomic uint32_t runs;
    _Atomic uint32_t process_errors;
    _Atomic uint32_t oversize_cycles;
    _Atomic uint32_t events_delivered;
    _Atomic uint32_t constant_channels;
} clap_instance_t;


/*
************************************************************************************************************************
*           FUNCTION PROTOTYPES
************************************************************************************************************************
*/

/* control thread */
int clap_host_open(const char *path, const char *id, clap_instance_t **instance);
int clap_host_activate(clap_instance_t *instance, double sample_rate, uint32_t max_frames);
void clap_host_set_audio_thread(clap_instance_t *instance, pthread_t thread);
void clap_host_arm(clap_instance_t *instance);
void clap_host_stop(clap_instance_t *instance);
int clap_host_restart(clap_instance_t *instance, uint32_t max_frames);
void clap_host_deactivate(clap_instance_t *instance);
void clap_host_close(clap_instance_t *instance);
uint32_t clap_host_binaries_open(void);

int clap_host_param_set(clap_instance_t *instance, clap_id id, double value);
int clap_host_param_get(clap_instance_t *instance, clap_id id, double *value);
int clap_host_bypass(clap_instance_t *instance, int value);
int clap_host_bypassed(clap_instance_t *instance);
int clap_host_state_save(clap_instance_t *instance, const char *filename);
int clap_host_state_load(clap_instance_t *instance, const char *filename);
int clap_host_preset_load(clap_instance_t *instance, const char *location);
void clap_host_idle(clap_instance_t *instance);

/* audio thread */
void clap_host_run(clap_instance_t *instance, const float *const *inputs, float *const *outputs, uint32_t nframes);
void clap_host_denormals_off(void);


/*
************************************************************************************************************************
*           END HEADER
************************************************************************************************************************
*/

#endif
