/* SPDX-License-Identifier: GPL-3.0-or-later */
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


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <ctype.h>
#include <getopt.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod-host.h"
#include "host-dispatch.h"
#include "socket.h"
#include "protocol.h"
#include "effects.h"
#include "host_verbs.h"
#include "clap_chain.h"
#include "quiet_death.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

#define PID_FILE        "/tmp/omx-clap-host.pid"
/* the one version of the tree, from the file VERSION through the Makefile */
#ifndef OMX_CLAP_VERSION
#error "build with make: the version is passed in OMX_CLAP_VERSION from the file VERSION"
#endif
#define VERSION         OMX_CLAP_VERSION
#define IDLE_INTERVAL_MS 20
/* plugin-hostd's chain line, which its package declares (FreeMixer/plugin-hostd#131) */
#define VERB_CHAIN      "chain"


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

/* SIGTERM and SIGINT run term_signal on whichever thread the kernel picks, PipeWire's loop threads among them, and two
 * signals can run it on two threads at once: running is atomic, and the socket is finished once, by the first of the
 * signal and quit to get there (finish) */
static atomic_int running;
static atomic_flag finished = ATOMIC_FLAG_INIT;
static volatile int quitting;

static const host_backend_t g_clap_backend = {
    effects_add,
    effects_remove,
    effects_bypass,
    effects_set_parameter,
    effects_get_parameter,
    effects_preset_load,
    effects_state_save,
    effects_state_load,
    effects_connect,
    effects_disconnect,
};

static const char g_help_msg[] =
    "add clap:<path>#<plugin_id> <instance_number> [client_name]\n"
    "add lv2:<uri> <instance_number> [client_name]\n"
    "remove <instance_number>\n"
    "bypass <instance_number> <bypass_value>\n"
    "param_set <instance_number> <param_id> <param_value>\n"
    "param_get <instance_number> <param_id>\n"
    "monitor_output <instance_number> <output_symbol>\n"
    OMX_VERB_PIN_EXPECT " <instance_number> <scheme>:<sha256>\n"
    OMX_VERB_TRACK_INFO " <instance_number> <name> <#RRGGBB|" HOST_WORD_NONE "> [bus|return|master]\n"
    OMX_VERB_REMOTE_PAGES " <instance_number>\n"
    OMX_VERB_REMOTE_PAGE_GET " <instance_number> <page>\n"
    OMX_VERB_PARAM_INFO " <instance_number> <param_id>\n"
    VERB_CHAIN " <chain> <instance_number> [<instance_number> ...]\n"
    OMX_VERB_AUDIO_PORTS " <instance_number>\n"
    "show_external_ui <instance_number>\n"
    OMX_VERB_HIDE_EXTERNAL_UI " <instance_number>\n"
    "preset_load <instance_number> <preset_file>\n"
    "state_save <dir>\n"
    "state_load <dir>\n"
    "connect <origin_port> <destination_port>\n"
    "disconnect <origin_port> <destination_port>\n"
    "cpu_load\n"
    OMX_VERB_IDLE_HOLD " [off|<microseconds>]\n"
    OMX_VERB_NOTE_DROPS " <instance_number>\n"
    OMX_VERB_NOTE_OUT_DROPS " <instance_number>\n"
    "set_bpm <beats_per_minute>\n"
    "set_bpb <beats_per_bar>\n"
    "transport <rolling> <beats_per_bar> <beats_per_minute>\n"
    "transport_sync <mode>\n"
    "help\n"
    "quit\n";


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS
************************************************************************************************************************
*/

/* add, as mod-host answers it, with the hosting.* code of a refusal after the error code: "resp -102 <code>" */
static void add_cb(proto_t *proto)
{
    char reply[256];
    const char *client_name = proto->list_count > 3 ? proto->list[3] : NULL;
    int resp = effects_add(proto->list[1], atoi(proto->list[2]), client_name);

    if (resp < 0 && effects_add_reason()[0])
    {
        snprintf(reply, sizeof(reply), "resp %i %s", resp, effects_add_reason());
        protocol_response(reply, proto);
    }
    else
        protocol_response_int(resp, proto);
}

static void cpu_load_cb(proto_t *proto)
{
    char buffer[128];
    sprintf(buffer, "resp 0 %.04f", effects_jack_cpu_load());
    protocol_response(buffer, proto);
}

/* idle_hold: the CPU idle hold as it stands (#296); idle_hold <off|microseconds>: the target changed at once */
static void idle_hold_cb(proto_t *proto)
{
    char buffer[128];

    if (proto->list_count == 2)
    {
        protocol_response_int(effects_idle_hold_set(proto->list[1]), proto);
        return;
    }
    if (proto->list_count > 2)
    {
        protocol_response_int(ERR_INVALID_OPERATION, proto);
        return;
    }
    effects_idle_hold(buffer, sizeof(buffer));
    protocol_response(buffer, proto);
}

static void set_bpm_cb(proto_t *proto)
{
    protocol_response_int(effects_set_bpm(atof(proto->list[1])), proto);
}

static void set_bpb_cb(proto_t *proto)
{
    protocol_response_int(effects_set_bpb(atof(proto->list[1])), proto);
}

static void transport_cb(proto_t *proto)
{
    protocol_response_int(effects_transport(atoi(proto->list[1]), atof(proto->list[2]), atof(proto->list[3])), proto);
}

static void transport_sync_cb(proto_t *proto)
{
    protocol_response_int(effects_transport_sync(proto->list[1]), proto);
}

static void note_out_drops_cb(proto_t *proto)
{
    char reply[128];
    int resp = effects_note_out_drops(atoi(proto->list[1]), reply, sizeof(reply));

    if (resp == SUCCESS)
        protocol_response(reply, proto);
    else
        protocol_response_int(resp, proto);
}

static void note_drops_cb(proto_t *proto)
{
    char reply[128];
    int resp = effects_note_drops(atoi(proto->list[1]), reply, sizeof(reply));

    if (resp == SUCCESS)
        protocol_response(reply, proto);
    else
        protocol_response_int(resp, proto);
}

static void note_input_cb(proto_t *proto)
{
    char reply[64];
    int resp = host_note_valid(proto->list, (int)proto->list_count) ? effects_note_input(atoi(proto->list[1]), reply, sizeof(reply))
                                                                  : ERR_INVALID_OPERATION;

    if (resp == SUCCESS)
        protocol_response(reply, proto);
    else
        protocol_response_int(resp, proto);
}

/* note_on, note_off and all_notes_off as data/verbs.json declares them: each word in its range, else -902 */
static void note_cb(proto_t *proto)
{
    uint8_t messages[16][3];
    uint32_t count = 0, c;

    if (!host_note_valid(proto->list, (int)proto->list_count))
    {
        protocol_response_int(ERR_INVALID_OPERATION, proto);
        return;
    }
    if (!strcmp(proto->list[0], OMX_VERB_ALL_NOTES_OFF))
    {
        for (c = 0; c < 16; c++)
        {
            messages[count][0] = (uint8_t)(0xb0 | c);
            messages[count][1] = 123;
            messages[count++][2] = 0;
        }
    }
    else
    {
        messages[0][0] = (uint8_t)((strcmp(proto->list[0], OMX_VERB_NOTE_ON) ? 0x80 : 0x90) | atoi(proto->list[2]));
        messages[0][1] = (uint8_t)atoi(proto->list[3]);
        messages[0][2] = (uint8_t)atoi(proto->list[4]);
        count = 1;
    }
    protocol_response_int(effects_note(atoi(proto->list[1]), (const uint8_t (*)[3])messages, count), proto);
}

static void pin_expect_cb(proto_t *proto)
{
    protocol_response_int(effects_pin_expect(atoi(proto->list[1]), proto->list[2]), proto);
}

static void track_info_cb(proto_t *proto)
{
    int resp = ERR_INVALID_OPERATION;

    if (host_track_info_valid(proto->list, (int)proto->list_count))
        resp = effects_track_info(atoi(proto->list[1]), proto->list[2], proto->list[3],
                                  proto->list_count > 4 ? proto->list[4] : NULL);
    protocol_response_int(resp, proto);
}

/* a word of the chain line: a non-negative decimal number below `limit` */
static int chain_word(const char *word, long limit, int *out)
{
    char *end;
    long value;

    if (!isdigit((unsigned char)word[0]))
        return 0;
    value = strtol(word, &end, 10);
    if (*end != '\0' || value >= limit)
        return 0;
    *out = (int)value;
    return 1;
}

/* chain <chain> <instance> [<instance> ...], as plugin-hostd sends it to a worker that runs chains in one client: each
 * word a number, else -902 */
static void chain_cb(proto_t *proto)
{
    int instances[OMX_CLAP_CHAIN_MEMBERS_MAX], id, n = 0, resp = ERR_INVALID_OPERATION;
    uint32_t i;

    if (proto->list_count >= 3 && proto->list_count - 2 <= OMX_CLAP_CHAIN_MEMBERS_MAX
        && chain_word(proto->list[1], INT32_MAX, &id))
    {
        for (i = 2; i < proto->list_count && chain_word(proto->list[i], MAX_INSTANCES, &instances[n]); i++)
            n++;
        if (i == proto->list_count)
            resp = effects_chain(id, instances, n);
    }
    protocol_response_int(resp, proto);
}

static void remote_pages_cb(proto_t *proto)
{
    protocol_response_int(effects_remote_pages(atoi(proto->list[1])), proto);
}

static void remote_page_get_cb(proto_t *proto)
{
    char reply[SOCKET_MSG_BUFFER_SIZE];
    char *end;
    long page = strtol(proto->list[2], &end, 10);
    int resp = ERR_INVALID_OPERATION;

    if (*end == '\0' && end != proto->list[2] && page >= 0 && page <= INT32_MAX)
        resp = effects_remote_page_get(atoi(proto->list[1]), (int)page, reply, sizeof(reply));
    if (resp == SUCCESS)
        protocol_response(reply, proto);
    else
        protocol_response_int(resp, proto);
}

static void param_info_cb(proto_t *proto)
{
    char reply[SOCKET_MSG_BUFFER_SIZE];
    const int resp = effects_param_info(atoi(proto->list[1]), proto->list[2], reply, sizeof(reply));

    if (resp == SUCCESS)
        protocol_response(reply, proto);
    else
        protocol_response_int(resp, proto);
}

/* the description of an instance's audio ports, a line per port with the JACK ports that carry it */
static void audio_ports_cb(proto_t *proto)
{
    char *reply = NULL;
    const int resp = effects_audio_ports(atoi(proto->list[1]), &reply);

    if (resp == SUCCESS)
        protocol_response(reply, proto);
    else
        protocol_response_int(resp, proto);
    free(reply);
}

/* show_external_ui, as mod-host answers it, with the hosting.window.* code after -404 when no window is shown:
 * "resp -404 <code>" */
static void show_external_ui_cb(proto_t *proto)
{
    char reply[128];
    const char *code;
    const int resp = effects_show_ui(atoi(proto->list[1]), &code);

    if (resp == ERR_EXTERNAL_UI_UNAVAILABLE && code)
    {
        snprintf(reply, sizeof(reply), "resp %i %s", resp, code);
        protocol_response(reply, proto);
    }
    else
        protocol_response_int(resp, proto);
}

static void hide_external_ui_cb(proto_t *proto)
{
    protocol_response_int(effects_hide_ui(atoi(proto->list[1])), proto);
}

static void help_cb(proto_t *proto)
{
    proto->response = 0;
    printf("%s", g_help_msg);
    fflush(stdout);
}

/* the reply goes out after this returns; the loop ends on the idle call that follows it */
static void quit_cb(proto_t *proto)
{
    protocol_response("resp 0", proto);
    quitting = 1;
}

// the loop ends and the socket is finished, once
static void finish(void)
{
    atomic_store(&running, 0);
    if (!atomic_flag_test_and_set(&finished))
        socket_finish();
}

static void idle_cb(void)
{
    effects_idle();
    if (quitting)
        finish();
}

static void term_signal(int sig)
{
    finish();

    return; (void)sig;
}

/* mod-host's verbs this host does not answer, -902 each. host_dispatch_register_unsupported registers these and also the
 * nine this host answers itself (set_bpm, set_bpb, transport, transport_sync, monitor_output, track_info, remote_pages,
 * remote_page_get, param_info): a second entry under a verb is dead, and the protocol library's table holds
 * PROTOCOL_MAX_COMMANDS entries and drops any past it in silence, which with the note verbs was quit itself. */
static void register_unsupported(void)
{
    static const char *const commands[] = {
        EFFECT_PRESET_SAVE, EFFECT_PRESET_SHOW, EFFECT_PARAM_MON, EFFECT_PATCH_GET, EFFECT_PATCH_SET, EFFECT_LICENSEE,
        MONITOR_ADDR_SET, MONITOR_MIDI_PROGRAM, MIDI_LEARN, MIDI_MAP, MIDI_UNMAP, CC_MAP, CC_VALUE_SET, CC_UNMAP, CV_MAP,
        CV_UNMAP, HMI_MAP, HMI_UNMAP, LOAD_COMMANDS, SAVE_COMMANDS, BUNDLE_ADD, BUNDLE_REMOVE, STATE_TMPDIR,
        FEATURE_ENABLE, OUTPUT_DATA_READY,
        NULL
    };
    const char *const *command;

    for (command = commands; *command; command++)
        protocol_add_command(*command, host_dispatch_unsupported_cb);
}

static int host_init(int socket_port, int feedback_port)
{
    // the first command registered under a verb is the one that answers it: this add, not the dispatch's
    protocol_add_command(EFFECT_ADD, add_cb);
    host_dispatch_register(&g_clap_backend);
    host_dispatch_register_monitor_output(effects_monitor_output);
    protocol_add_command(OMX_VERB_PIN_EXPECT_FMT, pin_expect_cb);
    protocol_add_command(OMX_VERB_TRACK_INFO_FMT, track_info_cb);
    protocol_add_command(OMX_VERB_REMOTE_PAGES_FMT, remote_pages_cb);
    protocol_add_command(OMX_VERB_REMOTE_PAGE_GET_FMT, remote_page_get_cb);
    protocol_add_command(OMX_VERB_PARAM_INFO_FMT, param_info_cb);
    protocol_add_command(VERB_CHAIN " ...", chain_cb);
    protocol_add_command(OMX_VERB_AUDIO_PORTS_FMT, audio_ports_cb);
    protocol_add_command(SHOW_EXTERNAL_UI, show_external_ui_cb);
    protocol_add_command(OMX_VERB_HIDE_EXTERNAL_UI_FMT, hide_external_ui_cb);
    protocol_add_command(EFFECT_SET_BPM, set_bpm_cb);
    protocol_add_command(EFFECT_SET_BPB, set_bpb_cb);
    protocol_add_command(TRANSPORT, transport_cb);
    protocol_add_command(TRANSPORT_SYNC, transport_sync_cb);
    register_unsupported();
    protocol_add_command(CPU_LOAD, cpu_load_cb);
    protocol_add_command(OMX_VERB_IDLE_HOLD_FMT, idle_hold_cb);
    protocol_add_command(OMX_VERB_NOTE_DROPS_FMT, note_drops_cb);
    protocol_add_command(OMX_VERB_NOTE_INPUT_FMT, note_input_cb);
    protocol_add_command(OMX_VERB_NOTE_ON_FMT, note_cb);
    protocol_add_command(OMX_VERB_NOTE_OFF_FMT, note_cb);
    protocol_add_command(OMX_VERB_ALL_NOTES_OFF_FMT, note_cb);
    protocol_add_command(OMX_VERB_NOTE_OUT_DROPS_FMT, note_out_drops_cb);
    protocol_add_command(HELP, help_cb);
    protocol_add_command(QUIT, quit_cb);

    if (effects_init() != SUCCESS)
        return -1;

    if (socket_start(socket_port, feedback_port, SOCKET_MSG_BUFFER_SIZE) < 0)
        return -1;

    socket_set_receive_cb(protocol_parse);
    socket_set_idle_cb(idle_cb);
    socket_set_idle_interval(IDLE_INTERVAL_MS);
    return 0;
}


/*
************************************************************************************************************************
*           MAIN FUNCTION
************************************************************************************************************************
*/

int main(int argc, char **argv)
{
    static struct option long_options[] = {
        {"nofork", no_argument, 0, 'n'},
        {"verbose", no_argument, 0, 'v'},
        {"socket-port", required_argument, 0, 'p'},
        {"feedback-port", required_argument, 0, 'f'},
        {"state-root", required_argument, 0, 's'},
        {"follow-jack-transport", no_argument, 0, 't'},
        {"channels", required_argument, 0, 'c'},
        {"idle-hold", required_argument, 0, 'i'},
        {"show-ui", no_argument, 0, 'S'},
        {"thread-pool", required_argument, 0, 'T'},
        {"version", no_argument, 0, 'V'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt, opt_index = 0;
    int nofork = 0, verbose = 0;
    int socket_port = SOCKET_DEFAULT_PORT, feedback_port = 0;
    struct sigaction sig;

    while ((opt = getopt_long(argc, argv, "nvp:f:s:tc:i:SVh", long_options, &opt_index)) != -1)
    {
        switch (opt)
        {
            case 'n':
                nofork = 1;
                break;

            case 'v':
                verbose = 1;
                nofork = 1;
                break;

            case 'p':
                socket_port = atoi(optarg);
                break;

            case 'f':
                feedback_port = atoi(optarg);
                break;

            case 's':
                effects_set_state_root(optarg);
                break;

            case 't':
                effects_follow_jack_transport(1);
                break;

            case 'c':
                if (strcmp(optarg, "1") != 0 && strcmp(optarg, "2") != 0)
                {
                    fprintf(stderr, "-c %s: the lane is 1 or 2 channels\n", optarg);
                    exit(EXIT_FAILURE);
                }
                effects_set_lane_channels((uint32_t)atoi(optarg));
                break;

            case 'i':
            {
                int32_t target;

                if (!effects_idle_hold_word(optarg, &target))
                {
                    fprintf(stderr, "-i %s: the idle hold is off, or a latency in microseconds (0 for no idle state)\n", optarg);
                    exit(EXIT_FAILURE);
                }
                effects_set_idle_hold(target);
                break;
            }

            case 'S':
                effects_show_ui_at_add(1);
                break;

            case 'T':
            {
                int mode;

                if (!effects_thread_pool_word(optarg, &mode))
                {
                    fprintf(stderr, "--thread-pool=%s: the thread pool is qualified, off or on\n", optarg);
                    exit(EXIT_FAILURE);
                }
                effects_set_thread_pool(mode);
                break;
            }

            case 'V':
                printf("%s version: %s\n", argv[0], VERSION);
                exit(EXIT_SUCCESS);
                break;

            case 'h':
                printf(
                    "Usage: %s [-vhtS] [-p <port>] [-f <port>] [-s <dir>] [-c <channels>] [-i <off|us>]\n"
                    "       [--thread-pool=<qualified|off|on>]\n"
                    "  -v, --verbose                  verbose messages\n"
                    "  -p, --socket-port=<port>       socket port definition\n"
                    "  -f, --feedback-port=<port>     feedback port definition\n"
                    "  -s, --state-root=<dir>         where LV2 plugins make their state files\n"
                    "                                 (default $XDG_STATE_HOME/omx-clap-host)\n"
                    "  -t, --follow-jack-transport    hand every plugin the JACK transport\n"
                    "  -c, --channels=<channels>      the lane's width, 1 or 2 (default 2): asked of a plugin\n"
                    "                                 that offers layouts\n"
                    "  -i, --idle-hold=<off|us>       what /dev/cpu_dma_latency is held at while a plugin is\n"
                    "                                 active: microseconds (default 10, C1 only; 0 no idle\n"
                    "                                 state) or off; the idle_hold command changes it later\n"
                    "  -S, --show-ui                  show each plugin's own window at its add,\n"
                    "                                 over Wayland first, then X11; show_external_ui and\n"
                    "                                 hide_external_ui do it later\n"
                    "      --thread-pool=<mode>       which plugins run their clap.thread-pool tasks on helper\n"
                    "                                 threads, one per core of the affinity mask but one:\n"
                    "                                 qualified (default: those whose qualification grants\n"
                    "                                 it; none is granted on this command line yet, so all\n"
                    "                                 run serial), on (every plugin that asks) or off\n"
                    "  -n, --nofork                   run in nonforking mode\n"
                    "  -V, --version                  print program version and exit\n"
                    "  -h, --help                     print this help and exit\n"
                    "A crash ends the host without a core dump; " QUIET_DEATH_KEEP_CORES_ENV "=1 keeps it.\n",
                argv[0]);
                exit(EXIT_SUCCESS);
        }
    }

    if (! nofork)
    {
        int pid = fork();
        if (pid != 0)
        {
            FILE *fd;

            printf("Forking... child PID: %d\n", pid);

            fd = fopen(PID_FILE, "w");
            if (fd == NULL)
            {
                fprintf(stderr, "can't open PID File\n");
            }
            else
            {
                fprintf(fd, "%d\n", pid);
                fclose(fd);
            }
            exit(EXIT_SUCCESS);
        }
    }

    // a plugin that crashes takes the host down at once, not after its core dump (quiet_death.h)
    quiet_death_install();

    if (host_init(socket_port, feedback_port) != 0)
        exit(EXIT_FAILURE);

    // before the handlers, so a signal that comes first ends the loop before it starts
    atomic_store(&running, 1);
    memset(&sig, 0, sizeof(sig));
    sig.sa_handler = term_signal;
    sig.sa_flags   = SA_RESTART;
    sigemptyset(&sig.sa_mask);
    sigaction(SIGTERM, &sig, NULL);
    sigaction(SIGINT, &sig, NULL);

    protocol_verbose(verbose);

    printf("omx-clap-host ready!\n");
    fflush(stdout);

    while (atomic_load(&running)) socket_run(0);

    finish();
    effects_finish();
    protocol_remove_commands();

    return 0;
}
