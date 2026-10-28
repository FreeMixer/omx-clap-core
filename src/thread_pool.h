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

/* The thread pool, the library's and internal (#395, docs/thread-pool.md): one pool of helper threads for the whole
 * process, made by the first activate of an instance granted it and ended by the last deactivate, the instances'
 * clap.thread-pool requests handed to it. */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <stddef.h>
#include <stdint.h>

struct omx_clap_instance;
struct omx_clap_thread_pool_info;

/* control thread: one more instance holding the pool, made when it is the first. 1 held; 0 serial, with `line` the log
 * line that says why ("thread pool: serial: ...") */
int thread_pool_acquire(int32_t priority, char *line, size_t size);

/* control thread: one instance fewer; the last ends the pool, its helpers joined */
void thread_pool_release(void);

/* the audio thread of an instance holding the pool, inside its request_exec: tasks 0 to num - 1 of `in`, run by the
 * helpers and the calling thread, all ended when it returns 0; *late is 1 when the wait for the helpers ran past
 * `budget_ns`. -1: not run (another request holds the pool, or num is 1 or above the hand-out word's 65535), the caller
 * runs them serial */
int thread_pool_run(struct omx_clap_instance *in, uint32_t num, uint64_t budget_ns, int *late);

/* whether the calling thread is a helper running a task of `in` (the audio role of the instance, for that task) */
int thread_pool_on_helper(const struct omx_clap_instance *in);

/* whether the calling thread is a helper of the pool */
int thread_pool_is_helper(void);

/* the pool as it stands */
void thread_pool_info(struct omx_clap_thread_pool_info *out);

#endif /* THREAD_POOL_H */
