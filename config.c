/* file: config.c */
#include "config.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOKENS 512 /* enough to cover large JSON */

/* Read entire file into a malloc’d, NUL-terminated buffer */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = malloc(len + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, len, f) != (size_t)len)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* Define the global storage */
Config cfg;
//_--Added crime parsing talin SAT

/* Compare a token to a literal string */
static int tok_eq(const char *json, jsontok_t *t, const char *s)
{
    return (t->end - t->start == (int)strlen(s)) && strncmp(json + t->start, s, t->end - t->start) == 0;
}

int load_config_json(const char *path)
{
    char *json = read_file(path);
    if (!json)
    {
        perror("read_file");
        return -1;
    }

    json_parser parser;
    json_init(&parser);

    jsontok_t tokens[MAX_TOKENS];
    int ntok = json_parse(&parser, json, strlen(json), tokens, MAX_TOKENS);
    if (ntok < 0)
    {
        fprintf(stderr, "JSON parse error %d\n", ntok);
        free(json);
        return -1;
    }

    /* Iterate all string tokens and assign config values */
    for (int i = 1; i < ntok; i++)
    {
        if (tokens[i].type != JSON_STRING)
            continue;
        char *val = json + tokens[i + 1].start;

        if (tok_eq(json, &tokens[i], "num_gangs"))
            cfg.num_gangs = atoi(val);
        else if (tok_eq(json, &tokens[i], "gang_members_min"))
            cfg.gang_members_min = atoi(val);
        else if (tok_eq(json, &tokens[i], "gang_members_max"))
            cfg.gang_members_max = atoi(val);
        else if (tok_eq(json, &tokens[i], "ranking_levels"))
            cfg.ranking_levels = atoi(val);
        else if (tok_eq(json, &tokens[i], "preparation_time"))
            cfg.preparation_time = atoi(val);
        else if (tok_eq(json, &tokens[i], "required_prep_level"))
            cfg.required_prep_level = atof(val);
        else if (tok_eq(json, &tokens[i], "info_spread_factor"))
            cfg.info_spread_factor = atof(val);
        else if (tok_eq(json, &tokens[i], "false_information_probability"))
            cfg.false_information_probability = atof(val);
        else if (tok_eq(json, &tokens[i], "agent_infiltration_rate"))
            cfg.agent_infiltration_rate = atof(val);
        else if (tok_eq(json, &tokens[i], "agent_knowledge_gain_rate"))
            cfg.agent_knowledge_gain_rate = atof(val);
        else if (tok_eq(json, &tokens[i], "agent_knowledge_decay_rate"))
            cfg.agent_knowledge_decay_rate = atof(val);
        else if (tok_eq(json, &tokens[i], "agent_suspicion_threshold"))
            cfg.agent_suspicion_threshold = atof(val);
        else if (tok_eq(json, &tokens[i], "plan_success_rate"))
            cfg.plan_success_rate = atof(val);
        else if (tok_eq(json, &tokens[i], "police_confirmation_threshold"))
            cfg.police_confirmation_threshold = atoi(val);
        else if (tok_eq(json, &tokens[i], "prison_sentence_duration"))
            cfg.prison_sentence_duration = atoi(val);
        else if (tok_eq(json, &tokens[i], "kill_rate"))
            cfg.kill_rate = atof(val);
        else if (tok_eq(json, &tokens[i], "max_thwarted_plans"))
            cfg.max_thwarted_plans = atoi(val);
        else if (tok_eq(json, &tokens[i], "max_successful_plans"))
            cfg.max_successful_plans = atoi(val);
        else if (tok_eq(json, &tokens[i], "max_executed_agents"))
            cfg.max_executed_agents = atoi(val);
        else if (tok_eq(json, &tokens[i], "graphics_refresh_ms"))
            cfg.graphics_refresh_ms = atoi(val);
        else if (tok_eq(json, &tokens[i], "logging_verbosity"))
            cfg.logging_verbosity = atoi(val);
        else if (tok_eq(json, &tokens[i], "random_seed"))
            cfg.random_seed = atoi(val);
        else if (tok_eq(json, &tokens[i], "ipc_timeout_ms"))
            cfg.ipc_timeout_ms = atoi(val);
        else if (tok_eq(json, &tokens[i], "status_update_interval_s"))
            cfg.status_update_interval_s = atoi(val);
        else if (tok_eq(json, &tokens[i], "max_simulation_runtime_s"))
            cfg.max_simulation_runtime_s = atoi(val);
        else if (tok_eq(json, &tokens[i], "report_batch_size"))
            cfg.report_batch_size = atoi(val);
        else if (tok_eq(json, &tokens[i], "send_prob"))
            cfg.send_prob = atof(val); // Added by Talin SAT
        else if (tok_eq(json, &tokens[i], "peer_prob"))
            cfg.peer_prob = atof(val); // Added by Talin SAT
        else if (tok_eq(json, &tokens[i], "num_missions"))
              cfg.num_missions = atoi(val);  // HALA: parse number of missions

    }

    free(json);
    return 0;
}
int load_crimes_json(const char *path)
{
    char *js;
    jsontok_t tokens[512];
    int ntok;

    if (json_load(path, &js, tokens, &ntok) < 0)
        return -1;

    // 1) find the "crimes" object
    for (int i = 1; i < ntok; i++)
    {
        if (tokens[i].type == JSON_STRING && tok_eq(js, &tokens[i], "crimes"))
        {
            int obj_i = i + 1; // should be JSON_OBJECT
            int n_crimes = tokens[obj_i].size;
            cfg.num_crimes = n_crimes;

            // 2) allocate your array of Crime
            // cfg.crimes = calloc(n_crimes, sizeof *cfg.crimes);
            if (!cfg.crimes)
            {
                perror("calloc");
                free(js);
                return -1;
            }

            // 3) iterate each key→array pair
            int idx = obj_i + 1; // first key token
            for (int c = 0; c < n_crimes; c++)
            {
                jsontok_t *name_t = &tokens[idx++];
                int namelen = name_t->end - name_t->start;
                strncpy(cfg.crimes[c].name,
                        js + name_t->start, namelen);
                cfg.crimes[c].name[namelen] = '\0';

                jsontok_t *arr_t = &tokens[idx++];
                int n_items = arr_t->size;
                cfg.crimes[c].legit_prep_intel_count = n_items;

                for (int a = 0; a < n_items; a++)
                {
                    jsontok_t *s = &tokens[idx++];
                    int sl = s->end - s->start;
                    // assume MAX_INTEL_LEN is large enough
                    strncpy(cfg.crimes[c].legit_prep_intel[a],
                            js + s->start, sl);
                    cfg.crimes[c].legit_prep_intel[a][sl] = '\0';
                }
            }

            free(js);
            return 0;
        }
    }

    fprintf(stderr, "ERROR: no \"crimes\" key found\n");
    free(js);
    return -1;
}
void print_config()
{
    printf("=== Simulation Configuration ===\n");
    printf("num_gangs: %d\n", cfg.num_gangs);
    printf("gang_members_min: %d\n", cfg.gang_members_min);
    printf("gang_members_max: %d\n", cfg.gang_members_max);
    printf("ranking_levels: %d\n", cfg.ranking_levels);
    printf("preparation_time: %d\n", cfg.preparation_time);
    printf("required_prep_level: %f\n", cfg.required_prep_level);
    printf("info_spread_factor: %f\n", cfg.info_spread_factor);
    printf("false_information_probability: %f\n", cfg.false_information_probability);
    printf("agent_infiltration_rate: %f\n", cfg.agent_infiltration_rate);
    printf("agent_knowledge_gain_rate: %f\n", cfg.agent_knowledge_gain_rate);
    printf("agent_knowledge_decay_rate: %f\n", cfg.agent_knowledge_decay_rate);
    printf("agent_suspicion_threshold: %f\n", cfg.agent_suspicion_threshold);
    printf("plan_success_rate: %f\n", cfg.plan_success_rate);
    printf("police_confirmation_threshold: %d\n", cfg.police_confirmation_threshold);
    printf("prison_sentence_duration: %d\n", cfg.prison_sentence_duration);
    printf("kill_rate: %f\n", cfg.kill_rate);
    printf("max_thwarted_plans: %d\n", cfg.max_thwarted_plans);
    printf("max_successful_plans: %d\n", cfg.max_successful_plans);
    printf("max_executed_agents: %d\n", cfg.max_executed_agents);
    printf("graphics_refresh_ms: %d\n", cfg.graphics_refresh_ms);
    printf("logging_verbosity: %d\n", cfg.logging_verbosity);
    printf("random_seed: %d\n", cfg.random_seed);
    printf("ipc_timeout_ms: %d\n", cfg.ipc_timeout_ms);
    printf("status_update_interval_s: %d\n", cfg.status_update_interval_s);
    printf("max_simulation_runtime_s: %d\n", cfg.max_simulation_runtime_s);
    printf("report_batch_size: %d\n", cfg.report_batch_size);

    printf("num_crimes: %d\n", cfg.num_crimes);
    printf("num_missions: %d\n", cfg.num_missions);

    for (int i = 0; i < cfg.num_crimes; i++)
    {
        const Crime *cr = &cfg.crimes[i];
        printf(" Crime[%d].name: %s\n", i, cr->name);
        printf("  legit_prep_intel_count: %d\n", cr->legit_prep_intel_count);
        for (int j = 0; j < cr->legit_prep_intel_count; j++)
        {
            printf("    - %s\n", cr->legit_prep_intel[j]);
        }
    }
    printf("================================\n");
}