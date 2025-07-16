/* file: config.h */
#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#define MAX_CRIMES           10
#define MAX_CRIME_NAME_LEN  128
#define MAX_INTEL_ENTRIES    10
#define MAX_INTEL_LEN       256

/* Simulation parameters */
//_____________________________________Talin added SAT
typedef struct {
    char   name[MAX_CRIME_NAME_LEN];
    char   legit_prep_intel[MAX_INTEL_ENTRIES][MAX_INTEL_LEN];
    int    legit_prep_intel_count;
} Crime;

//* Main simulation configuration */
typedef struct {
    /* Gangs and membership */
    int   num_gangs;                  // actual number of gangs in simulation
    int   gang_members_min;
    int   gang_members_max;
    int   ranking_levels;             // hierarchy depth within gang

    /* Crime definitions */
    Crime crimes[MAX_CRIMES];         // list of all crime types
    int   num_crimes;                 // actual number of crimes loaded

    /* Timing and preparation */
    int   preparation_time;           // seconds for mission preparation
    double required_prep_level;       // normalized [0..1] intel pieces required
    int   prison_sentence_duration;   // seconds to hold gang after arrest

    /* Communication probabilities */
    double info_spread_factor;        // multiplier for truth spread among members
    double false_information_probability; // base probability of sending misinformation
    double peer_prob;                 // per-tick probability of peer-to-peer sharing
    double send_prob;                 // base chance of sending intel per tick

    /* Agent infiltration & knowledge */
    double agent_infiltration_rate;   // fraction of members who are agents
    double agent_knowledge_gain_rate; // knowledge increment per correct intel
    double agent_knowledge_decay_rate;// decay factor after confirmation thresholds

    /* Suspicion thresholds */
    double agent_suspicion_threshold; // threshold at which police take full arrest action
    int    police_confirmation_threshold; // threshold for partial thwart action
    double hint_suspicion_weight[MAX_CRIMES]; // per-crime multiplier for suspicion gain
    double misinfo_penalty;           // multiplier to penalize other crime scores

    /* Success/failure limits */
    int    max_thwarted_plans;
    int    max_successful_plans;
    int    max_executed_agents;
    double plan_success_rate;         // base probability of a plan succeeding

    /* Kill simulation */
    double kill_rate;                 // per-second chance a member dies during mission

    /* Simulation control */
    int   graphics_refresh_ms;
    int   logging_verbosity;
    int   random_seed;
    int   ipc_timeout_ms;
    int   status_update_interval_s;
    int   max_simulation_runtime_s;
    int   report_batch_size;
    
    int num_missions;  //new new new HALA: new field for number of missions*****************

} Config;

/* Global instance (defined in config.c) */
extern Config cfg;
void print_config();
/* Load the JSON file at `path` into `cfg`. Returns 0 on success, -1 on error. */
int load_config_json(const char *path);
//_____________________________________________________________________-Talin SUN
int load_crimes_json(const char *path);



// Crime data structures
typedef struct {
    char   *name;
    char  **legit_prep_intel;
    size_t  legit_count;
    char  **misinformation;
    size_t  misinfo_count;
} crime_t;

typedef struct {
    crime_t *crimes;
    size_t   crime_count;
} crime_data_t;

extern Config cfg;
extern crime_data_t crime_data;


/**
 * Free memory allocated for crime_data.
 */
void free_crime_data(void);

#endif // CONFIG_H