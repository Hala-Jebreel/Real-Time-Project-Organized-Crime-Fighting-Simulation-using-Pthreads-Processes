#define _GNU_SOURCE  // for clock_gettime
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <mqueue.h>
#include "ipc_utils.h"  // police_queue_t, police_report_t

// Helper: make absolute timeout (unused here but kept for completeness)
static void make_abs_timeout(struct timespec *ts, int ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += (ms % 1000) * 1000000L;
    ts->tv_sec  += ms / 1000;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

// ─── Open queue for *sending* (gang) ───────────────────────
int pq_open(police_queue_t *pq, const char *name) {
    struct mq_attr attr;
    // (Re)create queue if not already; you may call mq_unlink() in HQ before creating.
    mqd_t m = mq_open(name, O_CREAT | O_WRONLY | O_NONBLOCK, 0600, NULL);
    printf("[DEBUG pq_open] name=%s → mqd=%d\n", name, (int)m);

    if (m == (mqd_t)-1) return -1;
    // fetch its attributes
    if (mq_getattr(m, &attr) == -1) {
        mq_close(m);
        return -1;
    }
    pq->mq       = m;
    pq->msg_size = attr.mq_msgsize;
    strncpy(pq->name, name, sizeof(pq->name)-1);
    pq->name[sizeof(pq->name)-1] = '\0';
    return 0;
}

// ─── Open queue for *receiving* (police) ──────────────────
int pq_open_read(police_queue_t *pq, const char *name) {
    struct mq_attr attr;
    mqd_t m = mq_open(name, O_RDONLY);
    if (m == (mqd_t)-1) return -1;
    if (mq_getattr(m, &attr) == -1) {
        mq_close(m);
        return -1;
    }
    pq->mq       = m;
    pq->msg_size = attr.mq_msgsize;
    strncpy(pq->name, name, sizeof(pq->name)-1);
    pq->name[sizeof(pq->name)-1] = '\0';
    return 0;
}

// ─── Send a report ────────────────────────────────────────
int pq_send(police_queue_t *pq, const police_report_t *r) {
    int ret = mq_send(pq->mq, (const char*)r, sizeof(*r), 0);
    if (ret == -1) {
        int e = errno;
        printf("[DEBUG pq_send] '%s' failed: errno=%d (%s)\n",
               pq->name, e, strerror(e));
    }
    return ret;
}

// ─── Receive a report ─────────────────────────────────────
int pq_recv(police_queue_t *pq, police_report_t *out) {
    if (!pq || pq->mq == (mqd_t)-1) {
        printf("[DEBUG pq_recv] invalid mq descriptor for '%s'\n", pq?pq->name:"(null)");
        return -1;
    }
    unsigned int prio;
    printf("[DEBUG pq_recv] calling mq_receive on '%s' (mq=%d, buf=%zu)\n",
           pq->name, (int)pq->mq, pq->msg_size);
    ssize_t bytes = mq_receive(pq->mq, (char*)out, pq->msg_size, &prio);
    if (bytes < 0) {
        int e = errno;
        printf("[DEBUG pq_recv] mq_receive failed on '%s': errno=%d (%s)\n",
               pq->name, e, strerror(e));
        return -1;
    }
    printf("[DEBUG pq_recv] received %zd bytes prio=%u\n", bytes, prio);
    return (int)bytes;
}

// ─── Close the queue ───────────────────────────────────────
int pq_close(police_queue_t *pq) {
    return mq_close(pq->mq);
}
// MAYS FRI S


int gui_pq_open(police_queue_t *pq, const char *name) {
    return pq_open(pq, name);
}
int gui_pq_send(police_queue_t *pq, const gui_msg_t *msg) {
    return mq_send(pq->mq, (const char*)msg, sizeof(*msg), 0);
}
