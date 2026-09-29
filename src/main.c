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

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod-host.h"
#include "host-dispatch.h"
#include "socket.h"
#include "protocol.h"
#include "effects.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

#define PID_FILE        "/tmp/omx-clap-host.pid"
#define VERSION         "0.1.0"


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

static volatile int running;
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
    "remove <instance_number>\n"
    "bypass <instance_number> <bypass_value>\n"
    "param_set <instance_number> <param_id> <param_value>\n"
    "param_get <instance_number> <param_id>\n"
    "preset_load <instance_number> <preset_file>\n"
    "state_save <dir>\n"
    "state_load <dir>\n"
    "connect <origin_port> <destination_port>\n"
    "disconnect <origin_port> <destination_port>\n"
    "cpu_load\n"
    "help\n"
    "quit\n";


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS
************************************************************************************************************************
*/

static void cpu_load_cb(proto_t *proto)
{
    char buffer[128];
    sprintf(buffer, "resp 0 %.04f", effects_jack_cpu_load());
    protocol_response(buffer, proto);
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

static void idle_cb(void)
{
    effects_idle();
    if (quitting)
    {
        running = 0;
        socket_finish();
    }
}

static void term_signal(int sig)
{
    running = 0;
    socket_finish();

    return; (void)sig;
}

static int host_init(int socket_port, int feedback_port)
{
    host_dispatch_register(&g_clap_backend);
    host_dispatch_register_unsupported();
    protocol_add_command(CPU_LOAD, cpu_load_cb);
    protocol_add_command(HELP, help_cb);
    protocol_add_command(QUIT, quit_cb);

    if (effects_init() != SUCCESS)
        return -1;

    if (socket_start(socket_port, feedback_port, SOCKET_MSG_BUFFER_SIZE) < 0)
        return -1;

    socket_set_receive_cb(protocol_parse);
    socket_set_idle_cb(idle_cb);
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
        {"version", no_argument, 0, 'V'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt, opt_index = 0;
    int nofork = 0, verbose = 0;
    int socket_port = SOCKET_DEFAULT_PORT, feedback_port = 0;
    struct sigaction sig;

    while ((opt = getopt_long(argc, argv, "nvp:f:Vh", long_options, &opt_index)) != -1)
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

            case 'V':
                printf("%s version: %s\n", argv[0], VERSION);
                exit(EXIT_SUCCESS);
                break;

            case 'h':
                printf(
                    "Usage: %s [-vh] [-p <port>]\n"
                    "  -v, --verbose                  verbose messages\n"
                    "  -p, --socket-port=<port>       socket port definition\n"
                    "  -f, --feedback-port=<port>     feedback port definition\n"
                    "  -n, --nofork                   run in nonforking mode\n"
                    "  -V, --version                  print program version and exit\n"
                    "  -h, --help                     print this help and exit\n",
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

    if (host_init(socket_port, feedback_port) != 0)
        exit(EXIT_FAILURE);

    memset(&sig, 0, sizeof(sig));
    sig.sa_handler = term_signal;
    sig.sa_flags   = SA_RESTART;
    sigemptyset(&sig.sa_mask);
    sigaction(SIGTERM, &sig, NULL);
    sigaction(SIGINT, &sig, NULL);

    protocol_verbose(verbose);

    printf("omx-clap-host ready!\n");
    fflush(stdout);

    running = 1;
    while (running) socket_run(0);

    socket_finish();
    effects_finish();
    protocol_remove_commands();

    return 0;
}
