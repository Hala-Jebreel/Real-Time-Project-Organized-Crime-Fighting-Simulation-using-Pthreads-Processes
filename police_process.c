#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include "ipc_utils.h" // pq_open, pq_recv, pq_close, shm_child_attach
#include "config.h"    // extern Config cfg
#include <signal.h>

#define POLICE_QUEUE_NAME "/ocf_sim_police"
///////////////////////     MAYS ADDED  S      //////////////////////////////
#define POLICE_BIN "./police_process"
#define GANG_BIN "./gang_process"
#define GUI_BIN "./gui"
#define GUI_QUEUE_NAME "/ocf_sim_gui"
// Track how many hints we’ve seen per gang per crime
static int hint_count[MAX_GANGS][MAX_CRIMES] = {{0}};
// How strongly misleading intel penalizes other missions
#define MISINFO_PENALTY 0.5

// Per-gang, per-mission cumulative “scores”
static double mission_score[MAX_GANGS][MAX_CRIMES];
typedef struct
{
    police_queue_t *pq;
    int gang_id;
    shm_layout_t *shm;
} listen_args_t;
// Return the index [0..num_crimes) matching this mission name
// If you always report crime‐names (per above), you can keep the simple map:
// new mission_index: find which crime owns this snippet
static int mission_index(const char *snippet, shm_layout_t *shm)
{
    for (int i = 0; i < shm->cfg.num_crimes; ++i)
    {
        Crime *c = &shm->cfg.crimes[i];
        for (int j = 0; j < c->legit_prep_intel_count; ++j)
        {
            if (strcmp(c->legit_prep_intel[j], snippet) == 0)
                return i;
        }
    }
    return -1; // truly unknown
}

///////////////////////     MAYS ADDED  E      //////////////////////////////

static void *listener_thread(void *vp)
{
    listen_args_t *a = vp;
    police_report_t report;
    shm_layout_t *shm = a->shm;
    Config cfg = shm->cfg;

    a->pq->msg_size = sizeof(report);
    strncpy(a->pq->name, POLICE_QUEUE_NAME, sizeof(a->pq->name) - 1);
    a->pq->name[sizeof(a->pq->name) - 1] = '\0'; // Ensure null-termination
    a->pq->mq = 3; // Use the same mq as in gang_process

    printf("[Listener %d] Thread started, queue=\"%s\"\n",
           a->gang_id, a->pq->name);
    fflush(stdout);

    while (1)
    {
        ssize_t n = pq_recv(a->pq, &report);
        if (n == -1)
        {
            if (errno == EINTR)
                continue;
            perror("[Police] pq_recv");
            break;
        }
        if (n != sizeof(report))
        {
            fprintf(stderr, "[Police] Bad report size %zd (expected %zu)\n",
                    n, sizeof(report));
            continue;
        }

        // 1) Find the crime index
        int g = report.gang_id;
        int m = mission_index(report.mission, shm);
        if (m < 0)
        {
            printf("[Listener %d] UNKNOWN snippet: “%s”\n", g, report.mission);
            continue;
        }
            // ── bump this crime’s suspicion score by confidence × weight
    double w = cfg.hint_suspicion_weight[m];
    if (w <= 0.0) w = 1.0;
    mission_score[g][m] += report.confidence * w;

        ////
        // —— count this hint ——
        hint_count[g][m] += 1;
        // threshold = half the number of legit intel entries (rounded up)
        int needed = (shm->cfg.crimes[m].legit_prep_intel_count + 1) / 2;
        if (hint_count[g][m] >= needed)
        {
            // send immediate full arrest
            police_report_t arrest = {
                .action = ARREST_ALL,
                .gang_id = g,
                .confidence = 1.0,
                .num_to_arrest = shm->cfg.gang_members_max};
            strncpy(arrest.mission,
                    shm->cfg.crimes[m].name,
                    sizeof(arrest.mission) - 1);
            pq_send(a->pq, &arrest);
            // reset counters so we don’t re-arrest on future repeats
            sem_wait(&shm->sem_police);
            shm->suspicion[g] = 0.0;
            sem_post(&shm->sem_police);
            for (int k = 0; k < cfg.num_crimes; ++k)
                mission_score[g][k] = 0.0;
            hint_count[g][m] = 0;
            // skip normal scoring for this report
            continue;
        }
        printf("\n[Listener %d] snippet \"%s\" → crime[%d]=\"%s\"\n",
               g, report.mission, m, shm->cfg.crimes[m].name);

        // 2) Update the per-crime score using configured weights
        //double w = cfg.hint_suspicion_weight[m];
        if (w <= 0.0)
            w = 1.0;
        mission_score[g][m] += report.confidence * w; // penalize other missions for potential misinformation
        for (int k = 0; k < cfg.num_crimes; ++k)
        {
            if (k == m)
                continue;
            mission_score[g][k] *=
                (1.0 - report.confidence * cfg.misinfo_penalty);
        }
        // 3) Recompute total suspicion
        double total = 0.0;
        for (int k = 0; k < cfg.num_crimes; ++k)
            total += mission_score[g][k];
        // if nobody’s reported yet, use a tiny epsilon to avoid NaN
        if (total < 1e-6)
            total = 1e-6;

        // 4) Write back into shared memory
        sem_wait(&shm->sem_police);
        shm->suspicion[g] = total;
        sem_post(&shm->sem_police);

        // gui_notify(g, m, "UPDATE_SUSPICION");
        //  5) Print raw & percentage breakdown
        printf("[Police][Gang %d] tip → \"%s\" (conf=%.2f)\n",
               g, report.mission, report.confidence);

        printf(" \n\n raw scores:");
        for (int k = 0; k < cfg.num_crimes; ++k)
        {
            printf("\n \"%s\"=%.2f",
                   shm->cfg.crimes[k].name,
                   mission_score[g][k]);
        }
        printf("\n");

        printf("  percentages:");
        for (int k = 0; k < cfg.num_crimes; ++k)
        {
            double pct = mission_score[g][k] / total * 100.0;
            printf(" \n \"%s\"=%.1f%%",
                   shm->cfg.crimes[k].name,
                   pct);
        }
        printf("\n");

        fflush(stdout);
    }

    pq_close(a->pq);
    return NULL;
}
///////////////////////     MAYS ADDED S     //////////////////////////////

static void *brain_thread(void *vp)
{
    shm_layout_t *shm = vp;
    Config        cfg = shm->cfg;

    // open the same queue you use for THWART
    police_queue_t pq;
    if (pq_open(&pq, POLICE_QUEUE_NAME) < 0) {
        perror("brain: pq_open (write)");
        return NULL;
    }

    while (1) {
        // optional shutdown
        sem_wait(&shm->sem_score);
        if (shm->score.plans_thwarted >= cfg.max_thwarted_plans) {
            sem_post(&shm->sem_score);
            printf("[Brain] reached max_thwarted_plans=%d, exiting\n",
                   cfg.max_thwarted_plans);
            break;
        }
        sem_post(&shm->sem_score);

        usleep(shm->cfg.status_update_interval_s * 1000000);
        printf("[Brain] evaluating gangs…\n");

        // just logging suspicion
        for (int g = 0; g < cfg.num_gangs; ++g) {
            sem_wait(&shm->sem_police);
            double s = shm->suspicion[g];
            sem_post(&shm->sem_police);

            // find top mission index
            int best = 0;
            double best_score = mission_score[g][0];
            for (int k = 1; k < cfg.num_crimes; ++k) {
                if (mission_score[g][k] > best_score) {
                    best_score = mission_score[g][k];
                    best = k;
                }
            }
            printf("[Brain] Gang %d: suspicion=%.2f → \"%s\" (%.2f)\n",
                   g, s, shm->cfg.crimes[best].name, best_score);
        }

        // decide THWART vs ARREST
        for (int g = 0; g < cfg.num_gangs; ++g) {
            sem_wait(&shm->sem_police);
            double s = shm->suspicion[g];
            sem_post(&shm->sem_police);
          int sentence = shm->gang[g].prison_sentence_duration;

            if (s >= 0.2) {
                // — ARREST via SIGUSR1 —
                pid_t pid = shm->gang_pids[g];
                if (pid > 0) {
                    printf("🚨 Gang[%d] has been arrested! Holding for few seconds\n ,SIGUSR1 to Gang[%d] (pid=%d)\n", g, pid);
                    kill(pid, SIGUSR1);
                    sem_wait(&shm->sem_police);
                    shm->gang[g].jailed = 1;
                    sem_post(&shm->sem_police);
                    fflush(stdout);

                }
                
            fflush(stdout);

            // simulate jail time
            sleep(sentence);

            printf("🔓 Gang[%d] released from jail, resuming operations.\n",
                   g);
                // reset local and shared suspicion
                sem_wait(&shm->sem_police);
                  shm->gang[g].jailed = 0;
                  shm->suspicion[g] = 0.0;
                sem_post(&shm->sem_police);
                for (int k = 0; k < cfg.num_crimes; ++k)
                    mission_score[g][k] = 0.0;

                // bump thwarted count
                sem_wait(&shm->sem_score);
                  shm->score.plans_thwarted++;
                sem_post(&shm->sem_score);
                            fflush(stdout);

            }
            else if (s >= cfg.police_confirmation_threshold) {
                // — THWART via queue —
                police_report_t rpt = {
                  .action        = THWART,
                  .gang_id       = g,
                  .confidence    = 0.0,
                  .num_to_arrest = 1
                };
                pq_send(&pq, &rpt);

                sem_wait(&shm->sem_police);
                  shm->suspicion[g] *= cfg.agent_knowledge_decay_rate;
                sem_post(&shm->sem_police);
            }
        }
    }

    pq_close(&pq);
    return NULL;
}

///////////////////////     MAYS ADDED E     //////////////////////////////

int main(int argc, char *argv[])
{
    ///////////////////////     MAYS ADDED fri s     //////////////////////////////

    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <gang_id> [<gang_id> ...]\n", argv[0]);
        return EXIT_FAILURE;
    }
    int num_listened_gangs = argc - 1;
    int listened_gangs[num_listened_gangs];
    for (int i = 0; i < num_listened_gangs; i++)
        listened_gangs[i] = atoi(argv[i + 1]);
    ///////////////////////     MAYS ADDED fri e     //////////////////////////////

    shm_layout_t *shm = shm_child_attach();
    if (!shm)
    {
        perror("[Police] shm_child_attach");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "[Police] attached shared memory at %p\n", (void *)shm);

    // Read config from shared memory
    Config cfg = shm->cfg;
    // ensure every crime contributes by default
    for (int i = 0; i < cfg.num_crimes; ++i)
        if (cfg.hint_suspicion_weight[i] == 0.0)
            cfg.hint_suspicion_weight[i] = 1.0;

    fprintf(stderr, "[Police] cfg.num_gangs = %d\n", cfg.num_gangs);

    // Open the shared queue for receiving
    police_queue_t shared_pq;
    if (pq_open_read(&shared_pq, POLICE_QUEUE_NAME) < 0)
    {
        fprintf(stderr, "[Police] pq_open_read failed: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }
    fprintf(stderr, "[Police] pq_open_read succeeded: name=\"%s\" mq=%d buf=%zu\n",
            shared_pq.name,
            (int)shared_pq.mq,
            shared_pq.msg_size);

    // Spawn listener threads
    pthread_t thr[cfg.num_gangs];
    listen_args_t args[cfg.num_gangs];

    for (int i = 0; i < cfg.num_gangs; ++i)
    {
        args[i].pq = &shared_pq;
        args[i].gang_id = i;
        args[i].shm = shm;

        if (pthread_create(&thr[i], NULL, listener_thread, &args[i]) != 0)
        {
            perror("[Police] pthread_create");
            pq_close(&shared_pq);
            return EXIT_FAILURE;
        }
    }
    ////////////////////////    ADDED MAYS S       ////////////////////
    // after you've opened shared_pq and spawned listeners:
    pthread_t brain_thr;
    if (pthread_create(&brain_thr, NULL, brain_thread, shm) != 0)
    {
        perror("main: pthread_create brain");
        exit(EXIT_FAILURE);
    }

    ////////////////////////    ADDED MAYS E       ////////////////////

    // Join threads (blocks indefinitely)
    for (int i = 0; i < cfg.num_gangs; ++i)
    {
        pthread_join(thr[i], NULL);
    }
    pthread_cancel(brain_thr);
    pthread_join(brain_thr, NULL);
    // Cleanup
    pq_close(&shared_pq);

    return 0;
}
