// file: main.c
#define _GNU_SOURCE // for asprintf
#define _XOPEN_SOURCE 600
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <mqueue.h>
#include <pthread.h>

#include "config.h"    // load_config_json(), extern Config cfg
#include "ipc_utils.h" // shm_parent_create(), shm_unlink(), SHM_NAME
#include "ipc_utils.h" // police_report_t for mq attributes

#define POLICE_BIN "./police_process"
#define GANG_BIN "./gang_process"
#define GUI_BIN "./gui"
#define MQ_NAME "/ocf_sim_police"

#define GUI_QUEUE_NAME  "/ocf_sim_gui"   //////MAYS ADDED FRI
static police_queue_t gui_queue;   // global handle for incoming GUI notifications

// ─── Referee listener ───
static void* referee_thread(void* arg);

// fork & exec a child, returning its pid
static pid_t spawn_child(const char *path, char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    if (pid == 0)
    {
        execvp(path, argv);
        perror("execvp");
        exit(EXIT_FAILURE);
    }
    return pid;
}


int main(int argc, char **argv)
{
    const char *cfg_path = (argc > 1 ? argv[1] : "config.json");

    // 1) Load JSON config
    if (load_config_json(cfg_path) != 0)
    {
        fprintf(stderr, "ERROR: could not load config '%s'\n", cfg_path);
        return EXIT_FAILURE;
    }
    if (load_crimes_json("crimes.json") < 0)
    {
        fprintf(stderr, "Failed to load crimes data\n");
        exit(1);
    }

    print_config();

    // 2) Create & initialize shared memory + semaphores + rwlock
    shm_layout_t *shm = shm_parent_create();
    if (!shm)
    {
        perror("shm_parent_create");
        return EXIT_FAILURE;
    }

    // 3) Snapshot the config into shared memory
    pthread_rwlock_wrlock(&shm->rwlock);
    shm->cfg = cfg;
    pthread_rwlock_unlock(&shm->rwlock);

    // 4) Create the POSIX message queue for agent→police reports

    struct mq_attr attr = {
        .mq_flags = 0,
        .mq_maxmsg = cfg.report_batch_size,
        .mq_msgsize = sizeof(police_report_t),
        .mq_curmsgs = 0};
    printf(
        "DEBUG: mq_attr: flags=%ld, maxmsg=%ld, msgsize=%ld, curmsgs=%ld\n",
        (long)attr.mq_flags,
        (long)attr.mq_maxmsg,
        (long)attr.mq_msgsize,
        (long)attr.mq_curmsgs);

// Force removal of any stale queue
mq_unlink(MQ_NAME);

// Now create it fresh with the right msgsize
    mqd_t mq = mq_open(MQ_NAME,
                       O_CREAT | O_RDWR,
                       0600,
                       &attr);

    if (mq == (mqd_t)-1)
    {
        perror("mq_open here in main line 96");
        shm_unlink(SHM_NAME);
        return EXIT_FAILURE;
    }
    mq_close(mq); // children will reopen in send or recv mode

/////////////////////MAYS FRI 
    // ─── Create GUI notification queue ───
    mq_unlink(GUI_QUEUE_NAME);
    struct mq_attr gui_attr = {
        .mq_flags   = 0,
        .mq_maxmsg  = cfg.report_batch_size,
        .mq_msgsize = sizeof(gui_msg_t),
        .mq_curmsgs = 0
    };
    mqd_t gui_mq = mq_open(GUI_QUEUE_NAME,
                           O_CREAT | O_RDWR,
                           0600,
                           &gui_attr);
    if (gui_mq == (mqd_t)-1) {
        perror("mq_open GUI_QUEUE_NAME");
        shm_unlink(SHM_NAME);
        return EXIT_FAILURE;
    }
    mq_close(gui_mq);
   
////////////////////////////////////    ADDED MAYS S       /////////////////////////////

    // --- spawn referee ---
    pthread_t ref_thr;
    if (pthread_create(&ref_thr, NULL, referee_thread, shm) != 0) {
        perror("main: pthread_create referee");
        exit(EXIT_FAILURE);
    }

////////////////////////////////////    ADDED MAYS E      /////////////////////////////

     // ___________________________________________________________________________Talin
    // 5a) Spawn all gang processes first, passing each gang’s numeric ID
    for (int g = 0; g < cfg.num_gangs; g++) {
        // 1. Build the string argument for this gang’s ID
        char *gid_str;
        if (asprintf(&gid_str, "%d", g) < 0) {
            perror("asprintf");
            exit(EXIT_FAILURE);
        }

        // 2. Create a local argv array (in scope!)
        char *const gang_argv[] = {
            GANG_BIN,   // "./gang_process"
            gid_str,    // "<g>"
            NULL
        };

        // 3. Fork+exec the gang process
        pid_t pid = spawn_child(GANG_BIN, gang_argv);

        // 4. Record its PID into shared memory so Brain can signal it later
        shm->gang_pids[g] = pid;

        // 5. Clean up our temporary string
        free(gid_str);
    }


    // 5b) Spawn the police process, passing it *all* gang-ID strings
    int police_argc = 1 + cfg.num_gangs;
    char **police_argv = calloc(police_argc + 1, sizeof(char *));
    if (!police_argv)
    {
        perror("calloc");
        exit(EXIT_FAILURE);
    }

    int idx = 0;
    police_argv[idx++] = POLICE_BIN; // "./police_process"
    for (int g = 0; g < cfg.num_gangs; g++)
    {
        char *pstr;
        if (asprintf(&pstr, "%d", g) < 0)
        {
            perror("asprintf");
            exit(EXIT_FAILURE);
        }
        police_argv[idx++] = pstr; // "<g>"
    }
    police_argv[idx] = NULL; // argv must be NULL-terminated

    spawn_child(POLICE_BIN, police_argv);

////////////////////////////////////    ADDED MAYS S       /////////////////////////////
    // 5c) Referee: open the police queue for listening to arrest orders
    police_queue_t referee_pq;
    if (pq_open_read(&referee_pq, MQ_NAME) < 0) {
        perror("[Referee] pq_open_read");
        exit(EXIT_FAILURE);
    }

///////////////////////////////////    ADDED MAYS E      /////////////////////////////
char *const gui_argv[] = { GUI_BIN, NULL }; /// added by mayar spawn gui 
    spawn_child(GUI_BIN, gui_argv);
//______________________________________________________________________end Talin
    // 6) Wait for all child processes
    int status;
    while (wait(&status) > 0)
    {
        // optionally log child exit statuses
    }
////////////////////////////////////    ADDED MAYS S      /////////////////////////////
   // All children have exited → stop the referee thread
   pthread_cancel(ref_thr);
   pthread_join(ref_thr, NULL);

////////////////////////////////////    ADDED MAYS E      /////////////////////////////

    // 7) Cleanup IPC
    mq_unlink(MQ_NAME);
    if (shm_unlink(SHM_NAME) == -1)
    {
        perror("shm_unlink");
    }

    printf("🏁 All child processes have exited; HQ shutting down.\n");
    return 0;
}
////////////////////////////////////    ADDED MAYS  S      /////////////////////////////

// Referee thread: receive police_report_t and act on ARREST_ALL / THWART
static void *referee_thread(void *arg) {
    shm_layout_t    *shm = (shm_layout_t*)arg;
    police_queue_t   pq;

    if (pq_open_read(&pq, MQ_NAME) < 0) {
        perror("referee: pq_open_read");
        return NULL;
    }

    while (1) {
        police_report_t rpt;
        ssize_t n = pq_recv(&pq, &rpt);
        if (n == -1 && errno == EINTR) continue;
        if (n != sizeof(rpt)) {
            if (n == -1) perror("referee: pq_recv");
            break;
        }

        switch (rpt.action) {
          case THWART:
            // partial: jail top leader only
            sem_wait(&shm->sem_gang[rpt.gang_id]);
              shm->gang[rpt.gang_id].jailed = 1;
              shm->score.plans_thwarted++;
            sem_post(&shm->sem_gang[rpt.gang_id]);
            break;

          case ARREST_ALL:
            // full gang arrest
            sem_wait(&shm->sem_gang[rpt.gang_id]);
              shm->gang[rpt.gang_id].members_alive = 0;
              shm->gang[rpt.gang_id].jailed       = 1;
              shm->score.plans_thwarted++;
                     // if we’ve thwarted enough plans, shut down the HQ
         if (shm->score.plans_thwarted >= shm->cfg.max_thwarted_plans) {
             printf("🚨 Reached max_thwarted_plans=%d → shutting down simulation\n",
                    shm->cfg.max_thwarted_plans);
             exit(0);
         }
            sem_post(&shm->sem_gang[rpt.gang_id]);
            break;

          default:
            // INFO or others—no action
            break;
        }
    }

    pq_close(&pq);
    return NULL;
}
////////////////////////////////////    ADDED MAYS E      /////////////////////////////
