/*
 * %CopyrightBegin%
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright Ericsson AB 2026. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * %CopyrightEnd%
 */

/*
 * Prototype io_uring backend for prim_file write/pwrite.
 *
 * Design:
 *   - One shared io_uring instance, initialised at NIF load time.
 *   - A single harvester thread calls io_uring_wait_cqe in a loop and sends
 *     {file_completion, Ref, ok | {error, Reason}} to the waiting process.
 *   - write_nif / pwrite_nif submit IORING_OP_WRITEV, store a pending_op as
 *     user_data, and return {completion, Ref} immediately.
 *   - The iov array is heap-allocated here and freed by the harvester.
 */

#include "erl_nif.h"
#include "config.h"
#include "sys.h"
#include "erl_driver.h"
#include "prim_file_nif.h"

#ifdef HAVE_IO_URING

#include <liburing.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#define URING_QUEUE_DEPTH 256

extern ERL_NIF_TERM am_ok;
extern ERL_NIF_TERM am_error;
extern ERL_NIF_TERM am_file_completion;
extern ERL_NIF_TERM am_completion;

static struct io_uring uring;
static ErlNifTid harvester_tid;
static volatile int harvester_stop = 0;

typedef struct {
    ErlNifPid    caller;
    ERL_NIF_TERM ref;
    ErlNifEnv   *ref_env;   /* owns ref and pins the write data binaries */
    ErlNifIOVec *iovec;     /* points into binaries owned by ref_env */
    struct iovec *iov;      /* alias into iovec->iov */
    int           iovlen;
} pending_op_t;

static void *harvester_thread(void *arg)
{
    ErlNifEnv *env = enif_alloc_env();

    while (!harvester_stop) {
        struct io_uring_cqe *cqe;
        int ret;
        pending_op_t *op;
        int res;
        ERL_NIF_TERM result, msg;

        ret = io_uring_wait_cqe(&uring, &cqe);
        if (ret < 0) {
            if (ret == -EINTR) continue;
            break;
        }

        op  = (pending_op_t *)io_uring_cqe_get_data(cqe);
        res = cqe->res;
        io_uring_cqe_seen(&uring, cqe);

        if (op == NULL) break; /* wakeup sentinel */

        if (res >= 0) {
            result = am_ok;
        } else {
            result = enif_make_tuple2(env, am_error,
                enif_make_atom(env, erl_errno_id(-res)));
        }

        msg = enif_make_tuple3(env,
            am_file_completion,
            enif_make_copy(env, op->ref),
            result);

        enif_send(NULL, &op->caller, env, msg);
        enif_clear_env(env);

        enif_free_env(op->ref_env);  /* releases ref and unpins write data */
        enif_free(op);
    }

    enif_free_env(env);
    return NULL;
}

int efile_uring_init(void)
{
    int ret = io_uring_queue_init(URING_QUEUE_DEPTH, &uring, 0);
    if (ret < 0) return ret;

    harvester_stop = 0;
    if (enif_thread_create("efile_uring_harvester", &harvester_tid,
                           harvester_thread, NULL, NULL) != 0) {
        io_uring_queue_exit(&uring);
        return -1;
    }
    return 0;
}

void efile_uring_destroy(void)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&uring);
    if (sqe) {
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data(sqe, NULL);
        io_uring_submit(&uring);
    }
    harvester_stop = 1;
    enif_thread_join(harvester_tid, NULL);
    io_uring_queue_exit(&uring);
}

static pending_op_t *make_op(ErlNifEnv *call_env, ERL_NIF_TERM iovec_term,
                              ERL_NIF_TERM ref, ErlNifPid *caller)
{
    pending_op_t *op;
    ErlNifIOVec vec, *iovec = &vec;
    ERL_NIF_TERM tail;

    op = enif_alloc(sizeof(pending_op_t));
    if (!op) return NULL;

    /* Allocate a persistent env. enif_inspect_iovec on this env pins the
     * underlying binaries so the GC won't move or collect them until we
     * call enif_free_env in the harvester. No data copy needed. */
    op->ref_env = enif_alloc_env();
    if (!op->ref_env) { enif_free(op); return NULL; }

    if (!enif_inspect_iovec(op->ref_env, 1024,
                            enif_make_copy(op->ref_env, iovec_term),
                            &tail, &iovec)) {
        enif_free_env(op->ref_env);
        enif_free(op);
        return NULL;
    }

    op->iovec  = iovec;
    op->iov    = iovec->iov;
    op->iovlen = iovec->iovcnt;
    op->caller = *caller;
    op->ref    = enif_make_copy(op->ref_env, ref);
    return op;
}

int efile_writev_async(efile_unix_t *u, ErlNifEnv *env,
                       ERL_NIF_TERM iovec_term,
                       ERL_NIF_TERM ref, ErlNifPid *caller)
{
    struct io_uring_sqe *sqe;
    pending_op_t *op;

    sqe = io_uring_get_sqe(&uring);
    if (!sqe) return -1;

    op = make_op(env, iovec_term, ref, caller);
    if (!op) return -1;

    io_uring_prep_writev(sqe, u->fd, op->iov, op->iovlen, -1);
    io_uring_sqe_set_data(sqe, op);
    io_uring_submit(&uring);
    return 0;
}

int efile_pwritev_async(efile_unix_t *u, ErlNifEnv *env,
                        Sint64 offset, ERL_NIF_TERM iovec_term,
                        ERL_NIF_TERM ref, ErlNifPid *caller)
{
    struct io_uring_sqe *sqe;
    pending_op_t *op;

    sqe = io_uring_get_sqe(&uring);
    if (!sqe) return -1;

    op = make_op(env, iovec_term, ref, caller);
    if (!op) return -1;

    io_uring_prep_writev(sqe, u->fd, op->iov, op->iovlen, (off_t)offset);
    io_uring_sqe_set_data(sqe, op);
    io_uring_submit(&uring);
    return 0;
}

#endif /* HAVE_IO_URING */
