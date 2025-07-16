#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include "ipc_utils.h"
#include <signal.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

#define POLICE_QUEUE_NAME "/ocf_sim_police"
// #define NUM_MISSIONS 7
#define zeta 0.1
#define alpha 0.8
#define CLAMP(x, lo, hi) (((x) < (lo)) ? (lo) : ((x) > (hi)) ? (hi) \
                                                             : (x))
// ######################################################################Talin Graph
#define MAX_INTEL 1024
#define CRED_MIN 0.05
#define CRED_MAX 0.95

// How much weight each factor carries — tune these so they sum <= 1.0
#define RANK_COEFF 0.3 // up to +0.3 from rank
#define PERF_COEFF 0.5 // up to +0.5 from performance
#define LUCK_COEFF 0.2 // up to ±0.2 from luck
static intel_t *intel_db[MAX_INTEL];
static int intel_count = 0;
// ######################################################################3

// ───── In-process FIFO queues (for leader→member messages) ─────
static msg_queue_t *queues;
// ───── Prototypes for our in-process FIFO helpers ─────
void send_message(int from_id, int to_id, const char *text);
message_t receive_message(int my_id);
int try_receive_message(int my_id, message_t *out);

int **subordinates;
int *sub_count;
int NUM_MEMBERS;
pthread_t *members;
thread_args_t *member_args;
int mission_intel_count;
shm_layout_t *shm;
volatile int arrested = 0;

static void handle_sigusr1(int signo)
{
    arrested = 1;
    for (int i = 0; i < NUM_MEMBERS; ++i)
    {
        pthread_kill(members[i], SIGUSR2);
    }
}
static void handle_sigusr2(int signo)
{
    // NOTE: sleep() is async-signal-safe on POSIX, so this is okay.
    printf("🐌 Thread %lu got SIGUSR1 — sleeping 15s…\n",
           (unsigned long)pthread_self());
    sleep(15);
    printf("🏃 Thread %lu resuming\n",
           (unsigned long)pthread_self());
}

float get_base_prob(int rank)
{
    int max_rank = shm->cfg.ranking_levels;
    if (max_rank <= 0)
        return 1; // no valid ranks configured
    // clamp rank to [0, max_rank]
    if (rank < 0)
        rank = 1; // minimum rank is 1
    if (rank > max_rank)
        rank = max_rank;

    return (float)rank / (float)max_rank;
}
// ################################################################Talin graph
//  1) create or find an intel object
static intel_t *find_or_create_intel(const char *text)
{
    for (int i = 0; i < intel_count; i++)
        if (strcmp(intel_db[i]->text, text) == 0)
            return intel_db[i];
    if (intel_count >= MAX_INTEL)
        return NULL; // out of space

    intel_t *in = malloc(sizeof *in);
    in->text = strdup(text);
    in->history = NULL;
    intel_db[intel_count++] = in;
    return in;
}

// 2) record every send
void record_transmission(const char *text, int from, int to)
{
    intel_t *in = find_or_create_intel(text);
    if (!in)
        return; // fail silently if too many
    transmission_t *t = malloc(sizeof *t);
    t->from = from;
    t->to = to;
    t->ts = time(NULL);
    t->next = in->history;
    in->history = t;
}

// 3) a simple trace for a particular intel
void dump_intel_history(const char *text)
{
    intel_t *in = find_or_create_intel(text);
    if (!in)
    {
        printf("No intel found for “%s”\n", text);
        return;
    }
    printf("History for “%s”:\n", in->text);
    for (transmission_t *t = in->history; t; t = t->next)
    {
        printf("  %ld: %d → %d\n", t->ts, t->from, t->to);
    }
}
// ------------------------------------------------------------------
// Helper: Find one path from start to target in a directed graph.
//   adj      : adjacency lists (size num_members, each is an array of ints)
//   adj_count: number of neighbors in each adj[u]
//   visited  : temp array, initialized to false
//   stack    : temp array to hold current path
//   depth    : current depth in stack
//   out_path : output array (size num_members) to receive the found path
//   out_len  : output length
// Returns true if a path is found.
static bool dfs_find_path(int start, int target,
                          int num_members,
                          int **adj, int *adj_count,
                          bool *visited,
                          int *stack, int depth,
                          int *out_path, int *out_len)
{
    if (start == target)
    {
        // record final node
        stack[depth] = start;
        // copy stack[0..depth] into out_path
        for (int i = 0; i <= depth; i++)
            out_path[i] = stack[i];
        *out_len = depth + 1;
        return true;
    }
    visited[start] = true;
    stack[depth] = start;

    for (int i = 0; i < adj_count[start]; i++)
    {
        int nbr = adj[start][i];
        if (!visited[nbr])
        {
            if (dfs_find_path(nbr, target,
                              num_members,
                              adj, adj_count,
                              visited,
                              stack, depth + 1,
                              out_path, out_len))
                return true;
        }
    }
    return false;
}

// ###########################################################

int rank_occurrence[11] = {0};
// ##############################################33 ranking levels
float assign_info_accuracy(int rank)
{
    float base = get_base_prob(rank);
    int index = rank_occurrence[rank]++;
    return base + ((index % shm->cfg.ranking_levels) / 100.0);
}
// ------------------------------------------------------------------
// Replacement analyze function — no file I/O, uses intel_db[] graph.

int analyze_distribution_log(thread_args_t *members,
                             int num_members,
                             int leader_id)
{
    printf("\n\U0001F50E [INVESTIGATION] Tracing all leader-intel paths…\n");

    // --- allocate working arrays ---
    int **adj = malloc(num_members * sizeof(int *));
    int *adj_count = calloc(num_members, sizeof(int));
    bool *visited = calloc(num_members, sizeof(bool));
    int *stack = malloc(num_members * sizeof(int));
    int *out_path = malloc(num_members * sizeof(int));

    // --- new: tally of how many times each member was “suspected” ---
    int *suspicion_count = calloc(num_members, sizeof(int));

    for (int ii = 0; ii < intel_count; ii++)
    {
        intel_t *in = intel_db[ii];

        // 1) build adjacency for this intel’s transmissions
        for (transmission_t *t = in->history; t; t = t->next)
            if (t->from >= 0 && t->from < num_members)
                adj_count[t->from]++;

        for (int u = 0; u < num_members; u++)
        {
            if (adj_count[u] > 0)
                adj[u] = malloc(adj_count[u] * sizeof(int));
            adj_count[u] = 0; // reset as insertion index
        }
        for (transmission_t *t = in->history; t; t = t->next)
        {
            int u = t->from;
            adj[u][adj_count[u]++] = t->to;
        }

        printf("\n📜 Intel: “%s”\n", in->text);

        // 2) for each member, see if there’s a path from leader → m
        for (int m = 0; m < num_members; m++)
        {
            memset(visited, 0, num_members * sizeof(bool));
            int path_len = 0;
            bool found = dfs_find_path(leader_id, m,
                                       num_members,
                                       adj, adj_count,
                                       visited,
                                       stack, 0,
                                       out_path, &path_len);
            if (found)
            {
                // count one accusation for m
                suspicion_count[m]++;

                // (optional) still print the path details if you like:
                printf("🔍 Accusation[%d] for Member[%d]:\n    ",
                       suspicion_count[m], m);
                for (int k = 0; k < path_len; k++)
                {
                    int node = out_path[k];
                    printf("%d(%.2f)%s",
                           node, members[node].credibility,
                           (k + 1 < path_len) ? " → " : "\n");
                }
            }
        }

        // free this intel’s adj lists
        for (int u = 0; u < num_members; u++)
            if (adj[u])
                free(adj[u]);
    }

    // --- find who got suspected the most ---
    int most_susp_id = 0;
    int max_accusations = suspicion_count[0];
    for (int m = 1; m < num_members; m++)
    {
        if (suspicion_count[m] > max_accusations)
        {
            max_accusations = suspicion_count[m];
            most_susp_id = m;
        }
    }

    printf("\n🔔 Most suspicious member: [%d] with %d accusations\n\n",
           most_susp_id, max_accusations);

    // clean up
    // free(suspicion_count);
    free(adj_count);
    free(visited);
    free(stack);
    free(out_path);

    return most_susp_id;
}

void *leader_thread(void *arg)
{

    thread_args_t *ta = arg;
    //// added new mayar
    // ##############################################################################
    sem_wait(&shm->sem_gang[ta->gang_id]);
    shm->gang_ranks[ta->gang_id][ta->id] = ta->rank;
    shm->gang_prep_levels[ta->gang_id][ta->id] = ta->prep_level; // likely 0 at the start
    sem_post(&shm->sem_gang[ta->gang_id]);
    ////

    printf("\U0001F451 Leader[%d] from Gang[%d] waiting for members… (TID=%lu, \U0001F451Rank=%d)\n",
           ta->id, ta->gang_id, (unsigned long)pthread_self(), ta->rank);
    fflush(stdout);
    for (int mission_num = 1; mission_num <= shm->cfg.num_missions; mission_num++)
    {
        printf("🚀 leader gang[] Starting Mission #%d\n", ta->gang_id, mission_num);

        // define mission info at leader's side
        srand(time(NULL) ^ ta->gang_id);
        int mission_index = rand() % shm->cfg.num_crimes;
        ta->mission_name = shm->cfg.crimes[mission_index].name;
        Crime *c = &shm->cfg.crimes[mission_index];
        for (int i = 0; i < c->legit_prep_intel_count; i++)
            ta->leader_intel_used[i] = 0;
        int remaining = c->legit_prep_intel_count;

        printf("\U0001F4E2 Leader[%d] selected mission: %s\n", ta->id, ta->mission_name);
        fflush(stdout);
        mission_intel_count = c->legit_prep_intel_count; // Talin FRI: store mission intel count

        pthread_barrier_wait(ta->barrier);
        // ——— send info(intel) to subordinates while preparing  ———
        for (int tick = 1; tick <= ta->prep_ticks; tick++)
        {
            usleep(ta->prep_interval_us);

            // Talin FRI: compute how far along we are [0.0 .. 1.0]
            double progress = tick / (double)ta->prep_ticks; // Talin FRI

            // Talin FRI: ramp base_send up over time to avoid long end silence
            double base_send = CLAMP(progress, 0.0, 1.0); // Talin FRI

            // Talin FRI: small random variation
            double luck = ((double)rand() / RAND_MAX - 0.5) * 0.05; // ±2.5% Talin FRI

            // Talin FRI: final send probability combining ramp and luck
            double send_prob = CLAMP(base_send + luck,
                                     0.0, 1.0); // Talin FRI

            int me = ta->id;
            int nsub = sub_count[me];
            for (int si = 0; si < nsub; si++)
            {
                int sub_id = subordinates[me][si];
                double r = rand() / (double)RAND_MAX;
                if (r < send_prob && remaining > 0)
                {
                    // pick a random unused intel_idx
                    int intel_idx;
                    do
                    {
                        intel_idx = rand() % c->legit_prep_intel_count;
                    } while (ta->leader_intel_used[intel_idx]);

                    // mark it used
                    ta->leader_intel_used[intel_idx] = 1;
                    remaining--;

                    const char *intel = c->legit_prep_intel[intel_idx];
                    // broadcast exactly this snippet
                    send_message(me, sub_id, intel);
                    printf("✉  Leader[%d] → Member[%d]: “%s”\n",
                           me, sub_id, intel);
                }
            }
        }

        pthread_barrier_wait(ta->barrier);

        printf("\U0001F680 Leader[%d] starting mission (duration=%ds)…\n", ta->id, ta->mission_duration_s);
        fflush(stdout);
        // simulate death during mission
        for (int sec = 0; sec < ta->mission_duration_s; sec++)
        {
            sleep(1);
            double r = rand() / (double)RAND_MAX;
            if (r < shm->cfg.kill_rate)
            {
                printf("☠ Leader[%d] was killed during mission!\n", ta->id);
                ta->is_dead = 1;
                pthread_exit(NULL);
            }
        }
        pthread_barrier_wait(ta->barrier);
        printf("\U0001F3C1 Leader[%d] mission complete, waiting at barrier…\n", ta->id);
        fflush(stdout);
        if (arrested)
        {
            int suspected_agent_thread_id = analyze_distribution_log(member_args, NUM_MEMBERS, ta->id);
            printf("\U0001F6A8 Leader[%d] arrested suspected agent thread %d\n", ta->id, suspected_agent_thread_id);
            pthread_cancel(members[suspected_agent_thread_id]);
            // and (optionally) reap it right away
            pthread_join(members[suspected_agent_thread_id], NULL);
            printf("✅ Killed thread %d\n", suspected_agent_thread_id);
            arrested = 0; // reset for next mission
        }

    }
    return NULL;
}

void *member_thread(void *arg)
{
    thread_args_t *ta = arg;
    strncpy(ta->pq->name, POLICE_QUEUE_NAME, sizeof(ta->pq->name) - 1);
    ta->pq->name[sizeof(ta->pq->name) - 1] = '\0'; // Ensure null-termination
    ta->pq->msg_size = sizeof(police_report_t);
    ta->pq->mq = 3;
    const char *emoji = ta->is_agent ? "\U0001F575" : "\U0001F91D";
    // const char *trust_info = ta->trusted ? "\U0001F9E0 Trusted Info" : "\U0001F925 Misled";
    const char *crown = (ta->id == ta->leader_id) ? " \U0001F451 Leader" : "";
    // e.g. their existing “send_prob”

    //// added new mayar
    sem_wait(&shm->sem_gang[ta->gang_id]);
    shm->gang_ranks[ta->gang_id][ta->id] = ta->rank;
    shm->gang_prep_levels[ta->gang_id][ta->id] = ta->prep_level;
    sem_post(&shm->sem_gang[ta->gang_id]);
    /////

    printf("%s Member[%d] from Gang[%d] ready and waiting… (TID=%lu, Rank=%d)has started with credibility %f %s\n",
           emoji, ta->id, ta->gang_id, (unsigned long)pthread_self(), ta->rank, ta->credibility, crown);
    fflush(stdout);
    for (int mission_num = 1; mission_num <= shm->cfg.num_missions; mission_num++)
    {
        printf("🚀member %d gang [%d] Starting Mission #%d\n", ta->id, ta->gang_id, mission_num);

        pthread_barrier_wait(ta->barrier);

        for (int tick = 1; tick <= ta->prep_ticks; tick++)
        {
            if (arrested){
                continue;
            }
            ////////////////////////////////////////// double base_true = ta->send_prob; //__Talin THU
            usleep(ta->prep_interval_us);
            // hala start add***************************************************************************************
            // double r = rand() / (double)RAND_MAX;
            // if (r < shm->cfg.kill_rate)
            // {
            //     printf("☠ Member[%d] was killed during mission!\n", ta->id);
            //     ta->is_dead = 1;
            //     pthread_exit(NULL);
            // }
            printf("\u2699 Member[%d] from Gang[%d] (Rank=%d) prep tick %d/%d\n",
                   ta->id, ta->gang_id, ta->rank, tick, ta->prep_ticks);
            fflush(stdout);

            int me = ta->id;
            int nsub = sub_count[me];
            for (int si = 0; si < nsub; si++)
            {
                int sub_id = subordinates[me][si];
                //////////////////////////////////////////  double base_true = ta->send_prob; //__Talin THU
                // scale so that high-credibility folks almost never lie,
                // low-cred folks lie a lot more(must be tied to each mission)
                thread_args_t *target = &member_args[sub_id]; // however you reference it
                double cred_t = target->credibility;
                double max_false = shm->cfg.false_information_probability; // in config.h config
                // probability of lying to low-cred folks is higher:
                double p_false = max_false * (1.0 - cred_t);
                // probability of truth to trusted folks:
                double p_true = CLAMP(shm->cfg.info_spread_factor * cred_t,
                                      0.0, 1.0);
                // ensure they don’t exceed 100%:
                if (p_true + p_false > 1.0)
                    p_false = 1.0 - p_true;
                double r = rand() / (double)RAND_MAX;
                printf("Member[%d]: r=%.2f, p_true=%.2f, p_false=%.2f\n",
                       me, r, p_true, p_false);
                if (r < p_true && ta->has_new_intel)
                {
                    send_message(me, sub_id, ta->intel_list[ta->intel_count - 1]);
                    printf("✉  Manager[%d] → Member[%d]: \"%s\"\n",
                           me, sub_id, ta->intel_list[ta->intel_count - 1]);
                    fflush(stdout);
                    ta->has_new_intel = 0;
                }
                //--Talin THU
                else if (r < p_true + p_false && ta->has_new_intel)
                {
                    // pick a random crime
                    int crime_i = rand() % shm->cfg.num_crimes;
                    Crime *crime = &shm->cfg.crimes[crime_i];

                    // sanity: skip if that crime has no intel entries
                    if (crime->legit_prep_intel_count <= 0)
                        return NULL; ///////////////////////////////////////////////////////ADDEDDDD 2:28PM fRI

                    // pick a random intel entry from that crime
                    int intel_i = rand() % crime->legit_prep_intel_count;
                    const char *misinfo = crime->legit_prep_intel[intel_i];

                    // send it
                    send_message(me, sub_id, misinfo);
                    // pick a false string here
                    printf("⚠ Manager[%d] → Member[%d] (MISINFO)%s\n", me, sub_id, misinfo);
                    ta->has_new_intel = 0;
                }
            }
            //__Talin FRI peer communication
            // ——— peer communication ———
            for (int pi = 0; pi < ta->peer_count; pi++)
            {
                int peer_id = ta->peers[pi];
                double r = rand() / (double)RAND_MAX;
                if (r < ta->peer_prob && ta->has_new_intel)
                {
                    // share your newest intel
                    const char *intel = ta->intel_list[ta->intel_count - 1];
                    send_message(ta->id, peer_id, intel);
                    printf("🔄 Member[%d] ↔ Member[%d]: “%s”\n", ta->id, peer_id, intel);
                    fflush(stdout);
                    ta->has_new_intel = 0;
                }
            }
            // ta->has_new_intel = 0; ############################################################################كارثة

            message_t incoming;
            while (try_receive_message(ta->id, &incoming))
            {
                int seen = 0;
                for (int i = 0; i < ta->intel_count; i++)
                    if (strcmp(ta->intel_list[i], incoming.text) == 0)
                    {
                        seen = 1;
                        break;
                    }
                if (!seen && ta->intel_count < MAX_INTELS_PER_THREAD)
                { // ----- edited by mayar
                    ta->intel_list[ta->intel_count++] = strdup(incoming.text);
                    printf("📬 Member[%d] received intel: “%s”\n", ta->id, incoming.text);
                    ta->has_new_intel = 1;
                    ta->prep_level++;
                    sem_wait(&shm->sem_gang[ta->gang_id]); //// added new mayar
                    shm->gang_prep_levels[ta->gang_id][ta->id] = ta->prep_level;
                    sem_post(&shm->sem_gang[ta->gang_id]); ////

                    if (ta->prep_level >= 4 && !ta->is_ready)
                    {
                        ta->is_ready = 1;
                        printf("🎯 Member[%d] is now READY with 4 pieces of intel!\n", ta->id);
                        fflush(stdout);
                        break;
                    }
                    else
                    {
                        printf("📈 Member[%d] prep level increased to %d\n", ta->id, ta->prep_level);
                    }
                }
            }

            // ADDED HALA: agent handles new intel + update crime knowledge
            if (ta->is_agent && ta->has_new_intel)
            {
                const char *reported_intel = ta->intel_list[ta->intel_count - 1];

                // ADDED HALA: agent updates crime-specific knowledge

                for (int ci = 0; ci < shm->cfg.num_crimes; ci++)
                {
                    // check if the intel matches any legit intel of this crime
                    Crime *crime = &shm->cfg.crimes[ci];
                    for (int j = 0; j < crime->legit_prep_intel_count; j++)
                    {
                        if (strcmp(reported_intel, crime->legit_prep_intel[j]) == 0)
                        {
                            ta->crime_knowledge[ci] += 0.1f;
                            if (ta->crime_knowledge[ci] > 1.0f)
                                ta->crime_knowledge[ci] = 1.0f;

                            // ✅ This is the print statement you want:
                            printf("✅ Agent[%d] received correct intel: \"%s\" → Matched crime: \"%s\" → Knowledge now = %.2f\n",
                                   ta->id, reported_intel, crime->name, ta->crime_knowledge[ci]);
                            fflush(stdout);
                            break;
                        }
                    }
                }

                // const char *correctness = ta->trusted ? "\u2705 (Correct)" : "\u274C (Wrong)";
                printf("\U0001F575‍♂ Agent Member[%d] from Gang[%d] reported intel: \"%s\" \n",
                       ta->id, ta->gang_id, reported_intel);
                fflush(stdout);

                police_report_t report = {
                    .gang_id = ta->gang_id,
                    .member_id = ta->id,
                    .confidence = ta->credibility,
                };
                // copy the mission text
                strncpy(report.mission, reported_intel, sizeof(report.mission) - 1);
                report.mission[sizeof(report.mission) - 1] = '\0';

                if (pq_send(ta->pq, &report) == -1)
                {
                    perror("❌ Failed to send report to police queue");
                }
                else
                {
                    printf("📨 Agent Member[%d] sent report to police queue (conf=%.2f).\n",
                           ta->id);
                }
            }
            if (tick == ta->prep_ticks)
            {
                break;
            }
        }

        // if (ta->intel_count > 0)
        //     ta->credibility += 0.05;
        // else
        //     ta->credibility -= 0.03;

        double rank_norm = (double)ta->rank / (double)shm->cfg.ranking_levels;
        double perf_norm = (mission_intel_count > 0)
                               ? (double)ta->intel_count / (double)mission_intel_count
                               : 0.0;
        double luck = ((double)rand() / RAND_MAX - 0.5) * LUCK_COEFF;
        double delta = RANK_COEFF * rank_norm + PERF_COEFF * perf_norm + luck;
        ta->credibility = CLAMP(ta->credibility + delta, CRED_MIN, CRED_MAX);

        printf("📈 Member[%d] credibility after mission: %.2f\n", ta->id, ta->credibility);

        // ADDED HALA: print full crime knowledge for agents
        if (ta->is_agent)
        {
            printf("📊 Agent[%d] knowledge summary:\n", ta->id);
            for (int ci = 0; ci < shm->cfg.num_crimes; ci++)
            {
                printf("   🔍 %s → %.2f\n", shm->cfg.crimes[ci].name, ta->crime_knowledge[ci]);
            }
        }

        printf("\u2705 Member[%d] prep done, waiting at barrier…\n", ta->id);
        fflush(stdout);
        // befor mission wait ____________________________________________-Talin FRI
        pthread_barrier_wait(ta->barrier);
        // starting mission ____________________________________________-Talin FRI
        printf("\U0001F3C1 Member[%d] starting mission (duration=%ds)…\n", ta->id, ta->mission_duration_s);
        fflush(stdout);
        pthread_barrier_wait(ta->barrier);
        printf("\U0001F3C6 Member[%d] mission complete…\n", ta->id);
    }
    return NULL;
}

void print_rank_histogram(int *ranks, int count)
{
    int histogram[11] = {0};
    for (int i = 0; i < count; i++)
        histogram[ranks[i]]++;

    printf("\U0001F4CA Rank Distribution:\n");
    for (int r = 0; r <= 10; r++)
    {
        if (histogram[r] > 0)
            printf("\U0001F538 Rank %d: %d member(s)\n", r, histogram[r]);
    }
    printf("\u2014\u2014\u2014\u2014\u2014\u2014\u2014\u2014\u2014\u2014\u2014\u2014\u2014\u2014\u2014\u2014\u2014\u2014\u2014\u2014\u2014\u2014\u2014\u2014\n");
}

void assign_rank_distribution(int *ranks, int num_members, int *leader_rank_out, int *leader_id_out)
{
    int ranking_levels = shm->cfg.ranking_levels;
    int leader_index = rand() % num_members;
    *leader_rank_out = ranking_levels;
    *leader_id_out = leader_index;
    ranks[leader_index] = ranking_levels;

    int filled = 0;
    for (int i = 0; i < num_members; i++)
    {
        if (i == leader_index)
            continue;
        int rank = rand() % ranking_levels;
        ranks[i] = rank;
        filled++;
    }

    print_rank_histogram(ranks, num_members);
}

void send_message(int from_id, int to_id, const char *text)
{
    msg_queue_t *q = &queues[to_id];
    pthread_mutex_lock(&q->mtx);

    // maybe resize if full…
    int idx = q->tail % q->capacity;
    q->fifo[idx].from_id = from_id;
    q->fifo[idx].timestamp = time(NULL);
    strncpy(q->fifo[idx].text, text, sizeof(q->fifo[idx].text) - 1);
    q->tail++;
    record_transmission(text, from_id, to_id); // record the transmission
    // log it
    // FILE *logf = fopen("distribution.log", "a");
    // fprintf(logf, "%ld Thread[%d] -> Thread[%d]: \"%s\"\n",
    //         q->fifo[idx].timestamp, from_id, to_id, text);
    // fclose(logf);

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mtx);
}

message_t receive_message(int my_id)
{
    msg_queue_t *q = &queues[my_id];
    pthread_mutex_lock(&q->mtx);
    while (q->head == q->tail)
        pthread_cond_wait(&q->cond, &q->mtx);

    message_t msg = q->fifo[q->head % q->capacity];
    q->head++;
    pthread_mutex_unlock(&q->mtx);
    return msg;
}

// returns true if a message was dequeued into *out, false if queue was empty
int try_receive_message(int my_id, message_t *out)
{
    msg_queue_t *q = &queues[my_id];
    pthread_mutex_lock(&q->mtx);
    if (q->head == q->tail)
    {
        // nothing waiting
        pthread_mutex_unlock(&q->mtx);
        return 0;
    }
    *out = q->fifo[q->head % q->capacity];
    q->head++;
    pthread_mutex_unlock(&q->mtx);
    return 1;
}

int main(int argc, char *argv[])
{
    printf("\U0001F3AC [GangProcess] Starting up...\n");

    if (argc < 2)
    {
        fprintf(stderr, "\u274C Usage: %s <gang_id>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int gang_id = atoi(argv[1]);

    // 🔄 Place srand BEFORE any call to rand()
    srand((unsigned int)(time(NULL) ^ (uintptr_t)pthread_self()));

    printf("\U0001F522 Parsed gang_id = %d\n", gang_id);
    fflush(stdout);

    shm = shm_child_attach();
    if (!shm)
    {
        perror("\u274C shm_child_attach");
        exit(EXIT_FAILURE);
    }

    // register signal handler for SIGUSR1
    struct sigaction sa;
    sa.sa_handler = handle_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // so interrupted syscalls auto-restart
    if (sigaction(SIGUSR1, &sa, NULL) < 0)
    {
        perror("sigaction(SIGUSR1)");
        exit(EXIT_FAILURE);
    }
    struct sigaction sa2;
    sa2.sa_handler = handle_sigusr2;
    sigemptyset(&sa2.sa_mask);
    sa2.sa_flags = SA_RESTART; // so interrupted syscalls auto-restart
    if (sigaction(SIGUSR1, &sa2, NULL) < 0)
    {
        perror("sigaction(SIGUSR2)");
        exit(EXIT_FAILURE);
    }

    police_queue_t pq;
    pq.mq = 3;
    if (pq_open(&pq, POLICE_QUEUE_NAME) == -1)
    {
        perror("\u274C pq_open");
        exit(EXIT_FAILURE);
    }
    printf("\U0001F4E2 Opened police queue:%s (mq=%d, msg_size=%zu)\n",
           pq.name, (int)pq.mq, pq.msg_size);
    fflush(stdout);
    // __________________________________________________________________________________--talin FRI moved to global scope
    int min = shm->cfg.gang_members_min;
    int max = shm->cfg.gang_members_max;
    NUM_MEMBERS = min + rand() % (max - min + 1);
    sem_wait(&shm->sem_gang[gang_id]); /// added
    shm->gang[gang_id].members_alive = NUM_MEMBERS;
    sem_post(&shm->sem_gang[gang_id]);
    // initialize inner queues_Talin SAT
    queues = calloc(NUM_MEMBERS, sizeof(*queues));
    for (int i = 0; i < NUM_MEMBERS; i++)
    {
        pthread_mutex_init(&queues[i].mtx, NULL);
        pthread_cond_init(&queues[i].cond, NULL);
        queues[i].capacity = 16; // or whatever
        queues[i].fifo = malloc(sizeof(message_t) * 16);
        queues[i].head = queues[i].tail = 0;
    }

    printf("\U0001F465 Gang[%d] has %d members this round.\n", gang_id, NUM_MEMBERS);
    printf("🎲 Randomly chosen number of members for Gang[%d]: %d (min=%d, max=%d)\n", gang_id, NUM_MEMBERS, min, max);

    int PREP_TICKS = (int)(shm->cfg.required_prep_level * 10);
    int PREP_INTERVAL_US = (shm->cfg.preparation_time * 1000000) / PREP_TICKS;
    int MISSION_DURATION_S = shm->cfg.preparation_time;

    int *ranks = malloc(NUM_MEMBERS * sizeof(int));
    int leader_rank, leader_id;
    assign_rank_distribution(ranks, NUM_MEMBERS, &leader_rank, &leader_id);

    //_Assign Managers_Talin SAT
    int *manager = malloc(sizeof(int) * NUM_MEMBERS);
    for (int i = 0; i < NUM_MEMBERS; i++)
    {
        if (i == leader_id)
        {
            manager[i] = -1;
        }
        else
        {
            // find the “closest” higher-ranked thread
            int best = leader_id, best_rank = ranks[leader_id];
            for (int j = 0; j < NUM_MEMBERS; j++)
            {
                if (ranks[j] < ranks[i] && ranks[j] < best_rank)
                {
                    best = j;
                    best_rank = ranks[j];
                }
            }
            manager[i] = best;
        }
        printf("Member[%d] (Rank=%d) → Manager = %s[%d]\n",
               i, ranks[i],
               (manager[i] >= 0 ? "Member" : "None"),
               manager[i]);
    }
    //-Assignb Subordinates__Talin SAT
    // right after you fill manager[i] …
    subordinates = calloc(NUM_MEMBERS, sizeof(int *));
    sub_count = calloc(NUM_MEMBERS, sizeof(int));

    // first pass: count how many under each
    for (int i = 0; i < NUM_MEMBERS; i++)
    {
        int m = manager[i];
        if (m >= 0)
            sub_count[m]++;
    }

    // allocate each list
    for (int m = 0; m < NUM_MEMBERS; m++)
    {
        if (sub_count[m] > 0)
            subordinates[m] = malloc(sub_count[m] * sizeof(int));
        sub_count[m] = 0; // reuse as “insertion index”
    }

    // second pass: fill them
    for (int i = 0; i < NUM_MEMBERS; i++)
    {
        int m = manager[i];
        if (m >= 0)
        {
            subordinates[m][sub_count[m]++] = i;
        }
    }
    for (int m = 0; m < NUM_MEMBERS; m++)
    {
        if (sub_count[m] > 0)
        {
            printf("Manager Member[%d] has %d subordinate(s):", m, sub_count[m]);
            for (int si = 0; si < sub_count[m]; si++)
                printf(" %d", subordinates[m][si]);
            printf("\n");
        }
    }
    int max_rank = shm->cfg.ranking_levels;
    int *rank_count = calloc(max_rank + 1, sizeof(int));
    for (int i = 0; i < NUM_MEMBERS; i++)
        rank_count[ranks[i]]++;

    // --- allocate arrays
    int **rank_members = calloc(max_rank + 1, sizeof(int *));
    for (int r = 0; r <= max_rank; r++)
    {
        if (rank_count[r] > 0)
            rank_members[r] = malloc(rank_count[r] * sizeof(int));
        rank_count[r] = 0; // reuse as insertion index
    }

    // --- fill them
    for (int i = 0; i < NUM_MEMBERS; i++)
    {
        int r = ranks[i];
        rank_members[r][rank_count[r]++] = i;
    }
    // (optionally) print for debugging:
    for (int r = 0; r <= max_rank; r++)
    {
        if (rank_count[r] > 1)
        {
            printf("Rank %d peers:", r);
            for (int j = 0; j < rank_count[r]; j++)
                printf(" %d", rank_members[r][j]);
            printf("\n");
        }
    }
    pthread_barrier_t mission_barrier;
    pthread_barrier_init(&mission_barrier, NULL, NUM_MEMBERS); // +1 was changed to +2 !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

    pthread_t leader;
    thread_args_t leader_args = {
        .id = leader_id,
        .gang_id = gang_id,
        .rank = leader_rank,
        .manager_id = -1,
        .leader_rank = leader_rank,
        .leader_id = leader_id,
        .barrier = &mission_barrier,
        .prep_ticks = PREP_TICKS,
        .prep_interval_us = PREP_INTERVAL_US,
        .mission_duration_s = MISSION_DURATION_S,
        .is_dead = 0,
        .is_agent = 0,
        .send_prob = 0.6,
        .credibility = 1,
        .pq = &pq};

    pthread_create(&leader, NULL, leader_thread, &leader_args);
    //__Talin FRI moved to global scope
    members = calloc(NUM_MEMBERS, sizeof(pthread_t));
    member_args = calloc(NUM_MEMBERS, sizeof(thread_args_t));
    int *agent_flags = calloc(NUM_MEMBERS, sizeof(int));
    if (!members || !member_args)
    {
        perror("calloc");
        exit(EXIT_FAILURE);
    }

    int agent_count = 1 + rand() % 2;
    for (int i = 0; i < agent_count;)
    {
        //__Talin SUN prevent leaders from becoming agents
        int idx = rand() % NUM_MEMBERS;
        if (!agent_flags[idx] && idx != leader_id)
        {
            agent_flags[idx] = 1;
            i++;
        }
    }

    for (int i = 0; i < NUM_MEMBERS; i++)
    {
        //__Talin FRI
        if (i == leader_id)
        {
            member_args[i] = leader_args; // copy leader args
            continue;                     // skip leader
        }
        float info_accuracy = assign_info_accuracy(ranks[i]);
        printf("\U0001F522 Member[%d] from Gang[%d] | Rank=%d → Probability of true info = %.2f%%\n",
               i, gang_id, ranks[i], info_accuracy * 100);

        //__Talin tue
        // note this code must be added before each mission to ensure that sen probabilities changes based on credibility
        double luck = (((double)rand() / RAND_MAX) - 0.5) * zeta;
        double blended = alpha * (shm->cfg.send_prob) + (1 - alpha) * 0.5;
        // ← drop fmin/fmax and use CLAMP
        double send_prob = CLAMP(blended + luck, 0.0, 1.0);
        member_args[i] = (thread_args_t){
            .id = i,
            .gang_id = gang_id,
            .rank = ranks[i],
            .manager_id = manager[i],
            .leader_rank = leader_rank,
            .prep_ticks = PREP_TICKS,
            .has_new_intel = 0,
            .is_dead = 0,
            .prep_interval_us = PREP_INTERVAL_US,
            .mission_duration_s = MISSION_DURATION_S,
            .barrier = &mission_barrier,
            .is_agent = agent_flags[i],
            .pq = &pq,
            .info_accuracy = info_accuracy,
            .credibility = info_accuracy, // ADDED MAYS everyone starts equal — credibility earned later
            .send_prob = send_prob,
            .prep_level = 0, // added mayar
            .is_ready = 0    // added mayar
        };

        member_args[i].peer_count = rank_count[ranks[i]] - 1;
        member_args[i].peers = malloc(sizeof(int) * member_args[i].peer_count);

        int idx = 0;
        for (int j = 0; j < rank_count[ranks[i]]; j++)
        {
            int peer = rank_members[ranks[i]][j];
            if (peer != i)
                member_args[i].peers[idx++] = peer;
        }
        member_args[i].peer_prob = shm->cfg.peer_prob; // e.g. 5% chance per tick

        // added by hala :
        // ADDED HALA: initialize crime knowledge to 0
        for (int c = 0; c < NUM_MISSIONS; c++)
        {
            member_args[i].crime_knowledge[c] = 0.0f;
        }
        pthread_create(&members[i], NULL, member_thread, &member_args[i]);
    }
    pthread_barrier_wait(&mission_barrier + 1);
    // analyze_distribution_log(member_args, NUM_MEMBERS); /// added by mayar

    //     if (gang_was_caught) {  /// make it after arresting ///
    //     analyze_distribution_log(member_args, NUM_MEMBERS);
    // }
    /////////////////////////  --- ADDED MAYS STARTS ----- ///////////////////////

    for (int i = 0; i < NUM_MEMBERS; i++)
    {

        double base_cred = member_args[i].credibility;

        // 1) random_pct ∈ [0, 0.025]
        double random_pct = ((double)rand() / (double)RAND_MAX) * 0.025;

        // 2) luck = base_cred × random_pct
        double luck = base_cred * random_pct;

        // 3) effective credibility = base + luck
        double effective_cred = base_cred + luck;

        // promotion/demotion on effective_cred
        if (effective_cred > 0.75 && member_args[i].rank < leader_rank)
        {
            member_args[i].rank++;
            printf("🔼 Member[%d] promoted to Rank %d (Cred: %.2f + Luck: %.3f → Eff: %.2f)\n",
                   i,
                   member_args[i].rank,
                   base_cred,
                   luck,
                   effective_cred);
        }
        else if (effective_cred < 0.20 && member_args[i].rank > 0)
        {
            member_args[i].rank--;
            printf("🔽 Member[%d] demoted to Rank %d (Cred: %.2f + Luck: %.3f → Eff: %.2f)\n",
                   i,
                   member_args[i].rank,
                   base_cred,
                   luck,
                   effective_cred);
        }
    }
    for (int i = 0; i < NUM_MEMBERS; i++)
    {
        printf("📊 Member[%d] Final Rank: %d | Base Cred: %.2f\n",
               member_args[i].id,
               member_args[i].rank,
               member_args[i].credibility);
    }
    /////////////////////////  --- ADDED MAYS ENDS ----- ///////////////////////
    pthread_join(leader, NULL);
    for (int i = 0; i < NUM_MEMBERS; i++)
        pthread_join(members[i], NULL);
    // hala start add *********************************************************************************************************************
    // HIRE NEW MEMBERS LOGIC STARTS HERE (STEP 5)

    int dead_members = 0;
    for (int i = 0; i < NUM_MEMBERS; i++)
    {
        if (member_args[i].is_dead)
        {
            dead_members++;
        }
    }
    if (leader_args.is_dead)
        dead_members++;

    printf("💀 Total dead members this mission: %d\n", dead_members);

    int current_alive = NUM_MEMBERS - dead_members;
    int hire_needed = shm->cfg.gang_members_min - current_alive;

    if (hire_needed <= 0)
    {
        printf("No new members needed.\n");
    }
    else
    {
        printf("🧑‍💼 Need to hire %d new member(s) to meet minimum threshold.\n", hire_needed);
        if (hire_needed > 0)
        {
            for (int h = 0; h < hire_needed; h++)
            {
                int new_id = NUM_MEMBERS + h;
                int new_rank = rand() % shm->cfg.ranking_levels;
                printf("🆕 Hiring new member [%d] with Rank %d\n", new_id, new_rank);
            }
        }
    }

    // === STEP 3: REBUILD MANAGERS ===
    int new_total_members = NUM_MEMBERS + hire_needed;
    int *new_ranks = malloc(new_total_members * sizeof(int));
    int *new_manager = malloc(new_total_members * sizeof(int));

    // copy alive ranks
    int alive_index = 0;
    for (int i = 0; i < NUM_MEMBERS; i++)
    {
        if (!member_args[i].is_dead)
        {
            new_ranks[alive_index] = member_args[i].rank;
            alive_index++;
        }
    }

    // add new hires
    for (int h = 0; h < hire_needed; h++)
    {
        int new_rank = rand() % shm->cfg.ranking_levels;
        new_ranks[alive_index] = new_rank;
        alive_index++;
    }

    // assign managers
    for (int i = 0; i < new_total_members; i++)
    {
        if (i == 0)
        {
            new_manager[i] = -1;
        }
        else
        {
            int best = 0;
            int best_rank = new_ranks[0];
            for (int j = 0; j < i; j++)
            {
                if (new_ranks[j] < new_ranks[i] && new_ranks[j] < best_rank)
                {
                    best = j;
                    best_rank = new_ranks[j];
                }
            }
            new_manager[i] = best;
        }
    }

    // print managers
    for (int i = 0; i < new_total_members; i++)
    {
        if (new_manager[i] == -1)
            printf("👑 Member[%d] (Rank=%d) is Leader\n", i, new_ranks[i]);
        else
            printf("🧑‍🤝‍🧑 Member[%d] (Rank=%d) → Manager = Member[%d] (Rank=%d)\n",
                   i, new_ranks[i], new_manager[i], new_ranks[new_manager[i]]);
    }

    // === STEP 4: APPLY PROMOTION/DEMOTION ===

    for (int i = 0; i < new_total_members; i++)
    {
        double base_cred = 0.5 + ((rand() % 50) / 100.0);
        double random_pct = ((double)rand() / (double)RAND_MAX) * 0.025;
        double luck = base_cred * random_pct;
        double effective_cred = base_cred + luck;

        if (effective_cred > 0.75 && new_ranks[i] < shm->cfg.ranking_levels)
        {
            new_ranks[i]++;
            printf("🔼 Member[%d] promoted to Rank %d (Cred: %.2f + Luck: %.3f → Eff: %.2f)\n",
                   i, new_ranks[i], base_cred, luck, effective_cred);
        }
        else if (effective_cred < 0.20 && new_ranks[i] > 0)
        {
            new_ranks[i]--;
            printf("🔽 Member[%d] demoted to Rank %d (Cred: %.2f + Luck: %.3f → Eff: %.2f)\n",
                   i, new_ranks[i], base_cred, luck, effective_cred);
        }
    }

    // hala end add********************************************************************************************************
    //__Talin fri
    free(members);
    free(member_args);

    pthread_barrier_destroy(&mission_barrier);
    pq_close(&pq);
    free(agent_flags);
    free(ranks);
    free(new_ranks);   //***************************************************************************************
    free(new_manager); //*******************************************************************************

    printf("\u2705 [GangProcess] Exiting cleanly.\n");
    return EXIT_SUCCESS;
}