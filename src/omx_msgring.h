// SPDX-License-Identifier: GPL-3.0-or-later
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

/** @file
 * @brief A single-producer single-consumer ring of length-prefixed records. */

/*
 * omx_msgring.h: the variable-length SPSC message ring between an in-process plugin's RT `run()`
 * and its own non-RT worker thread. Part of the shared host-services layer: used by the CLAP
 * core's hosts and by an LV2 adapter's worker, with nothing of any one console in it.
 *
 * LV2's worker extension moves OPAQUE, VARIABLE-SIZE messages both ways (a `schedule_work`
 * request from `run()` to the worker, a `respond` from `work()` back to the next `run()`), so a
 * fixed-width sample ring is the wrong primitive. Each instance owns TWO of these, each with
 * exactly one producer and one consumer.
 *
 * RECORD. A 4-byte size header, then the payload, padded to a 4-byte multiple, CONTIGUOUS in
 * the buffer — the consumer hands the plugin a pointer into the ring, never a copy. A record
 * that does not fit before the end of the buffer leaves a WRAP marker where it would have
 * started and is written at offset 0; the skipped tail counts as used until the consumer
 * passes it, so the free-space arithmetic stays one subtraction.
 *
 * FULL IS AN ANSWER, NEVER A WAIT. `omx_msgring_push` returns -1 when the record does not fit
 * (the host maps it to `LV2_WORKER_ERR_NO_SPACE`, which the extension declares legal) and
 * writes nothing: the RT never spins on a hung worker.
 *
 * WHERE THE MEMORY IS. The caller's: the control thread allocates `cap` bytes before the stage
 * is published and frees them after it is unpublished. `cap` is a power of two, at least 16.
 * This header allocates nothing, locks nothing, makes no syscall — both ends are RT-safe.
 *
 * ORDERING. `head` is written only by the producer, `tail` only by the consumer. The producer
 * publishes a record with a RELEASE store of `head` after its bytes are written; the consumer
 * ACQUIRE-loads `head` before reading them, and RELEASE-stores `tail` only after it is done with
 * the payload, so the producer never overwrites bytes the consumer is still reading.
 */
#ifndef OMX_MSGRING_H
#define OMX_MSGRING_H

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define OMX_MSGRING_WRAP 0xFFFFFFFFu  ///< the length word that marks a wrap to the start of the ring
#define OMX_MSGRING_HDR ((uint32_t)sizeof(uint32_t))  ///< the size of a record header, in bytes

/** @brief A single-producer single-consumer ring of length-prefixed records over caller-owned storage. */
struct omx_msgring {
  uint8_t *buf;          /**< cap bytes, caller-owned */
  uint32_t cap;          /**< power of two, >= 16 */
  _Atomic uint32_t head; /**< producer's position, in bytes, free-running (wraps at 2^32) */
  _Atomic uint32_t tail; /**< consumer's position, same arithmetic */
};

/** The bytes a record of `size` payload occupies: header + payload rounded up to 4. */
static inline uint32_t omx_msgring_record_bytes(uint32_t size) {
  return OMX_MSGRING_HDR + ((size + 3u) & ~3u);
}

/** Bind `buf` (cap bytes) as an empty ring. Returns -1 (and binds nothing) for a cap that is not
 * a power of two >= 16 or a NULL buffer. Control thread, before either end runs. */
static inline int omx_msgring_init(struct omx_msgring *r, void *buf, uint32_t cap) {
  if (r == NULL || buf == NULL || cap < 16u || (cap & (cap - 1u)) != 0u) return -1;
  r->buf = (uint8_t *)buf;
  r->cap = cap;
  atomic_store_explicit(&r->head, 0u, memory_order_relaxed);
  atomic_store_explicit(&r->tail, 0u, memory_order_relaxed);
  return 0;
}

/** Bytes currently held (records, their padding and any wrap skip not yet passed). */
static inline uint32_t omx_msgring_used(const struct omx_msgring *r) {
  return atomic_load_explicit(&r->head, memory_order_acquire) -
         atomic_load_explicit(&r->tail, memory_order_acquire);
}

/**
 * PRODUCER. Append one record, or refuse it whole. Returns 0 on success, -1 when it does not fit
 * (including a record larger than the ring could ever hold). Never waits.
 */
static inline int omx_msgring_push(struct omx_msgring *r, const void *data, uint32_t size) {
  if (size > r->cap) return -1; /* also keeps record_bytes from overflowing */
  const uint32_t need = omx_msgring_record_bytes(size);
  const uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
  const uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
  uint32_t off = h & (r->cap - 1u);
  const uint32_t contig = r->cap - off;
  const uint32_t total = need > contig ? contig + need : need;
  if (total > r->cap - (h - t)) return -1;
  uint32_t at = h;
  if (need > contig) {
    const uint32_t wrap = OMX_MSGRING_WRAP;
    memcpy(r->buf + off, &wrap, OMX_MSGRING_HDR); /* contig >= 4: every offset is 4-aligned */
    at += contig;
    off = 0;
  }
  memcpy(r->buf + off, &size, OMX_MSGRING_HDR);
  if (size > 0) memcpy(r->buf + off + OMX_MSGRING_HDR, data, size);
  atomic_store_explicit(&r->head, at + need, memory_order_release);
  return 0;
}

/**
 * CONSUMER. The oldest record's payload (and its size), or NULL when the ring is empty. The
 * pointer stays valid until {@link omx_msgring_pop}. Passing a wrap marker frees the skipped
 * tail at once.
 */
static inline const void *omx_msgring_peek(struct omx_msgring *r, uint32_t *size) {
  uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
  const uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
  if (t == h) return NULL;
  uint32_t off = t & (r->cap - 1u);
  uint32_t hdr;
  memcpy(&hdr, r->buf + off, OMX_MSGRING_HDR);
  if (hdr == OMX_MSGRING_WRAP) {
    t += r->cap - off;
    atomic_store_explicit(&r->tail, t, memory_order_release);
    if (t == h) return NULL; /* unreachable: a wrap marker is only ever written before a record */
    off = 0;
    memcpy(&hdr, r->buf, OMX_MSGRING_HDR);
  }
  *size = hdr;
  return r->buf + off + OMX_MSGRING_HDR;
}

/** CONSUMER. Release the record {@link omx_msgring_peek} returned. No-op on an empty ring. */
static inline void omx_msgring_pop(struct omx_msgring *r) {
  uint32_t size;
  if (omx_msgring_peek(r, &size) == NULL) return;
  const uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
  atomic_store_explicit(&r->tail, t + omx_msgring_record_bytes(size), memory_order_release);
}

#endif /* OMX_MSGRING_H */
