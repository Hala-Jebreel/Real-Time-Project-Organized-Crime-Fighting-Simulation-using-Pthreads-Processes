#ifndef IPC_UTILS_H
#define IPC_UTILS_H

#include <mqueue.h>
#include <pthread.h>     // ✅ Required for pthread_rwlock_t
#include <semaphore.h>
#include <stdint.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include "config.h"      // ✅ Brings in Config definition
#include <stdbool.h>   // for bool

#define SHM_NAME   "/ocf_sim_shm"
#define MAX_GANGS  100
#define MAX_MEMBERS_PER_GANG 256
#define MAX_INTELS_PER_THREAD  50

#ifndef NUM_MISSIONS
#define NUM_MISSIONS 7
#endif

typedef struct transmission {
    int        from;       // sender thread id
    int        to;         // receiver thread id
    time_t     ts;         // timestamp
    struct transmission *next;
} transmission_t;

typedef struct intel {
    char            *text;     // pointer to the intel string
    transmission_t *history;   // head of singly‐linked list of events
} intel_t;
// ───────────── REGION-0 : global scoreboard ─────────────
typedef struct {
    uint32_t plans_thwarted;
    uint32_t plans_success;
    uint32_t agents_executed;
} scoreboard_t;

// ───────────── REGION-1 : gang state (one per gang) ─────
typedef struct {
    uint32_t members_alive;
    uint32_t next_mission_id;
    uint8_t  jailed;
    uint8_t  prison_sentence_duration;    // seconds to hold them;
} gang_state_t;

// ───────────── REGION-2 : police findings ───────────────
typedef struct {
    uint32_t tips_waiting;
    uint32_t arrests_made;
} police_state_t;


// ───────────── Message Queue Structure ─────────────
typedef struct {
    mqd_t   mq;              // POSIX message queue descriptor
    size_t  msg_size;        // Max message size
    char    name[100];        // Queue name for logs
} police_queue_t;// hala***************************************************************************************************************
////////////////////////    MAYS ADDED   S   //////////////////
typedef enum {
    INFO,        // existing
    THWART,      // existing
    ARREST_ALL   // ← new full-gang arrest
} police_action_t;

typedef struct {
    police_action_t action;     // INFO, THWART, or ARREST_ALL
    int            gang_id;
    int            member_id;
    char           mission[100]; // <<— add this
    bool           is_correct;
    bool           is_crime;
    double         confidence;
    int            num_to_arrest;
} police_report_t;

////////////////////////    MAYS ADDED   E   //////////////////

typedef struct {
    int id;
    int gang_id;
    int rank;
    int manager_id;//_Talin added SAT
    int leader_id;  // ✅ FIXED
    int leader_rank;
    const char* mission_name;//-Talin SUN
    const char *intel_list[MAX_INTELS_PER_THREAD];//-Talin SUN
    int intel_count;//-Talin SUN
    int has_new_intel;// if a member thrad has gotten any new intel
    int leader_intel_used[MAX_INTEL_ENTRIES];// keep track of intel that leader sent
    int is_agent;
    pthread_barrier_t *barrier;
    int prep_ticks;
    int prep_interval_us;
    int mission_duration_s;
    float info_accuracy;
    float send_prob;//_Taleen added SAT
    double credibility;  // 0.0 - 1.0 range // added MAYS
    police_queue_t* pq;  // ✅ Add this line
    //ADDED HALA
    float crime_knowledge[NUM_MISSIONS]; // نسبة معرفة العميل بكل جريمة
    int is_dead;  //  added halaaaaaaaaaaaaaaaa
    int *peers;       // array of peer IDs
    int peer_count;   // number of peers at this rank
    double peer_prob; // probability of sending to a peer
    int prep_level;      // progress from 0 to 4 //_Mayar added 
    int is_ready;        // becomes 1 when prep_level reaches 4
} thread_args_t;
//───────────────────────────── inner mesg queues structure────────────────Talin SAT
typedef struct {
  int   from_id;
  time_t timestamp;
  char  text[128];
} message_t;

typedef struct {
  pthread_mutex_t  mtx;
  pthread_cond_t   cond;
  message_t       *fifo;
  int              head, tail, capacity;
} msg_queue_t;

// global, sized to NUM_MEMBERS after you know it
//static msg_queue_t *queues;   ///EDITED MAYS

// MAYS FRI S
typedef struct {
    int gang_id;
    int crime_id;
    char event[32]; // "UPDATE_SUSPICION", "ARREST_ALL", etc.
} gui_msg_t;

// ───────────────── overall shared block ─────────────────
typedef struct {
    pthread_rwlock_t  rwlock;

    sem_t sem_score;
    sem_t sem_gang[MAX_GANGS];
    sem_t sem_police;
    sem_t sem_cfg;

    scoreboard_t score;                  // REGION-0
    gang_state_t gang[MAX_GANGS];        // REGION-1
    police_state_t police;               // REGION-2
    double    suspicion[MAX_GANGS]; // ← new: cumulative suspicion per gang ///ADDED
    Config cfg;                          // REGION-3: full config struct
    // ✅ Add these:
    int gang_ranks[MAX_GANGS][MAX_MEMBERS_PER_GANG];
    int gang_prep_levels[MAX_GANGS][MAX_MEMBERS_PER_GANG];
    pid_t gang_pids[MAX_GANGS];
   // int prison_time_remaining[MAX_GANGS];
    int gang_member_dead[MAX_GANGS][MAX_MEMBERS_PER_GANG]; // 0 = alive, 1 = dead

} shm_layout_t;


#define SHM_SIZE   ((off_t)sizeof(shm_layout_t))

// ───────────── Shared memory helpers ─────────────
static inline shm_layout_t* shm_parent_create(void) {
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd == -1) return NULL;
    if (ftruncate(fd, SHM_SIZE) == -1) { close(fd); return NULL; }

    shm_layout_t *p = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;

    memset(p, 0, sizeof *p);

    pthread_rwlockattr_t rwattr;
    pthread_rwlockattr_init(&rwattr);
    pthread_rwlockattr_setpshared(&rwattr, PTHREAD_PROCESS_SHARED);
    pthread_rwlock_init(&p->rwlock, &rwattr);

    sem_init(&p->sem_score, 1, 1);
    for (int i = 0; i < MAX_GANGS; ++i)
        sem_init(&p->sem_gang[i], 1, 1);
    sem_init(&p->sem_police, 1, 1);
    sem_init(&p->sem_cfg, 1, 1);

    return p;
}

static inline shm_layout_t* shm_child_attach(void) {
    int fd = shm_open(SHM_NAME, O_RDWR, 0600);
    if (fd == -1) return NULL;

    shm_layout_t *p = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return (p == MAP_FAILED) ? NULL : p;
}

// ───────────── Convenience wrappers ─────────────
static inline void score_inc_plans_thwarted(shm_layout_t *shm) {
    sem_wait(&shm->sem_score);
    shm->score.plans_thwarted++;
    sem_post(&shm->sem_score);
}

static inline void gang_set_jailed(shm_layout_t *shm, int g, int jailed) {
    sem_wait(&shm->sem_gang[g]);
    shm->gang[g].jailed = (uint8_t)jailed;
    sem_post(&shm->sem_gang[g]);
}

static inline uint32_t police_get_tips(shm_layout_t *shm) {
    uint32_t v;
    sem_wait(&shm->sem_police);
    v = shm->police.tips_waiting;
    sem_post(&shm->sem_police);
    return v;
}
// 🟩 Add this declaration if not already declared above
int pq_open(police_queue_t* pq, const char* name);
int pq_close(police_queue_t* pq);
int pq_send(police_queue_t* pq, const police_report_t* msg);
int pq_recv(police_queue_t *pq, police_report_t *report_out);
//__Talin
int pq_open_read(police_queue_t *pq, const char *name);
//-end Talin

#endif // IPC_UTILS_H