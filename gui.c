// gui_dynamic_layout.c
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <GL/glut.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipc_utils.h"
#include <stdbool.h> // for bool, true, false


#define WIN_W 1024
#define WIN_H 700
#define POLICE_CHARS 1
#define CHAR_PER_G 3

enum { STATE_IDLE = 0, STATE_WALK = 1, STATE_DEAD = 2 };

static const char *policeFiles[POLICE_CHARS][3] = {
    {"images/police_idle.png", "images/police_run.png", "images/police_shot.png"},
    {"images/police2_idle.png", "images/police2_run.png", "images/police2_shot.png"},
    {"images/police3_idle.png", "images/police3_run.png", "images/police3_shot.png"}
};
static const char *idleFiles[CHAR_PER_G] = {"images/idle_gang.png", "images/idle_gang2.png", "images/idle_gang3.png"};
static const char *runFiles[CHAR_PER_G]  = {"images/run_gang.png",  "images/run_gang2.png",  "images/run_gang3.png"};
static const char *deadFiles[CHAR_PER_G] = {"images/dead_gang.png", "images/dead_gang2.png", "images/dead_gang3.png"};
static const char *prisonFile = "images/prison.png";

typedef struct {
    GLuint tex;
    int w, h, cols, rows, fw, fh;
} Sheet;

static Sheet policeSheets[POLICE_CHARS][3];
static Sheet gangSheets[3][CHAR_PER_G];
static Sheet prisonSheet;

static int gangFrame = 0;
static int animDelay = 250;
static shm_layout_t *shm = NULL;

static void loadSheet(const char *path, Sheet *s, int cols, int rows) {
    stbi_set_flip_vertically_on_load(1);
    int comps;
    unsigned char *data = stbi_load(path, &s->w, &s->h, &comps, 4);
    if (!data) { fprintf(stderr, "ERROR loading %s\n", path); exit(1); }
    s->cols = cols; s->rows = rows;
    s->fw = s->w / cols; s->fh = s->h / rows;
    glGenTextures(1, &s->tex);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s->w, s->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data);
}

static void drawFrame(const Sheet *s, int frame, int x, int y) {
    int c = frame % s->cols, r = frame / s->cols;
    float u0 = c * (float)s->fw / s->w, v0 = r * (float)s->fh / s->h;
    float u1 = (c+1) * (float)s->fw / s->w, v1 = (r+1) * (float)s->fh / s->h;
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, s->tex);
    glColor3f(1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(u0,v0); glVertex2f(x, y);
    glTexCoord2f(u1,v0); glVertex2f(x+s->fw, y);
    glTexCoord2f(u1,v1); glVertex2f(x+s->fw, y+s->fh);
    glTexCoord2f(u0,v1); glVertex2f(x, y+s->fh);
    glEnd();
    glDisable(GL_TEXTURE_2D); glDisable(GL_BLEND);
}


static void drawFrameScaled(const Sheet *s, int frame, int x, int y, float scale) {
    int c = frame % s->cols, r = frame / s->cols;
    float u0 = c * (float)s->fw / s->w, v0 = r * (float)s->fh / s->h;
    float u1 = (c+1) * (float)s->fw / s->w, v1 = (r+1) * (float)s->fh / s->h;

    int sw = s->fw * scale;
    int sh = s->fh * scale;

    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, s->tex);
    glColor3f(1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(u0,v0); glVertex2f(x, y);
    glTexCoord2f(u1,v0); glVertex2f(x+sw, y);
    glTexCoord2f(u1,v1); glVertex2f(x+sw, y+sh);
    glTexCoord2f(u0,v1); glVertex2f(x, y+sh);
    glEnd();
    glDisable(GL_TEXTURE_2D); glDisable(GL_BLEND);
}


static void drawText(int x, int y, const char *s) {
    glColor3f(1, 1, 1); glRasterPos2i(x, y);
    while (*s) {
        if (*s == '\n') { y -= 18; glRasterPos2i(x, y); }
        else glutBitmapCharacter(GLUT_BITMAP_HELVETICA_18, *s);
        s++;
    }
}

static void display() {
    glClearColor(0.3f, 0.3f, 0.35f, 1); glClear(GL_COLOR_BUFFER_BIT);
    int W = glutGet(GLUT_WINDOW_WIDTH), H = glutGet(GLUT_WINDOW_HEIGHT);
    int pad = 20;

    int policePanel = W * 0.25;
    int gangPanelW = W - policePanel;
    int startX = policePanel + pad;

    // Draw police background
    glColor3f(0.2f, 0.2f, 0.25f);
    glBegin(GL_QUADS);
    glVertex2f(0,H); glVertex2f(policePanel,H);
    glVertex2f(policePanel,0); glVertex2f(0,0);
    glEnd();

    drawText(pad, H - pad - 10, "POLICE");
    Sheet *ps0 = &policeSheets[0][STATE_IDLE];
    drawFrame(&policeSheets[0][STATE_IDLE], 0, pad + 10, H - pad - ps0->fh - 40);

//drawText(pad + 5, H - pad - ps0->fh - 100, "📢 Received info from agent!");  // Always show for test

    drawFrame(&prisonSheet, 0, pad, pad + 20);

    pthread_rwlock_rdlock(&shm->rwlock);
    int thwarted = shm->score.plans_thwarted;
    int successful = shm->score.plans_success;
    int arrests = shm->police.arrests_made;
    int num_gangs = shm->cfg.num_gangs;
    

    // 👇 Capture suspicion per gang before unlocking
    double suspicion_vals[MAX_GANGS];
    for (int g = 0; g < num_gangs; g++) {
        suspicion_vals[g] = shm->suspicion[g];
    }
    pthread_rwlock_unlock(&shm->rwlock);

    pthread_rwlock_rdlock(&shm->rwlock);
int tips = shm->police.tips_waiting;
pthread_rwlock_unlock(&shm->rwlock);

// reserve position for police info display
int y = H - pad - ps0->fh - 60;
// Background box for police state text
// glEnable(GL_BLEND);
// glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
// glColor4f(0.1f, 0.1f, 0.1f, 0.6f);  // dark gray with transparency

// glBegin(GL_QUADS);
// glVertex2f(pad - 5, y + 110);  // top-left
// glVertex2f(pad + 240, y + 110);  // top-right
// glVertex2f(pad + 240, y - 10);  // bottom-right
// glVertex2f(pad - 5, y - 10);  // bottom-left
// glEnd();

// glDisable(GL_BLEND);
// glColor3f(1, 1, 1); // reset to white text

if (tips > 0) {
    drawText(pad + 5, y, "📢 Received info from agent!");
} else {
    drawText(pad + 5, y, "🕵 Info: waiting tip...");
}
y -= 25;  // adjust for next line


char line[128];
drawText(pad + 5, y, "📢 Police State:");
y -= 22;
snprintf(line, sizeof(line), "  - Thwarted: %d", thwarted);
drawText(pad + 5, y, line); y -= 20;

snprintf(line, sizeof(line), "  - Success: %d", successful);
drawText(pad + 5, y, line); y -= 20;

snprintf(line, sizeof(line), "  - Arrests: %d", arrests);
drawText(pad + 5, y, line); y -= 20;


for (int g = 0; g < num_gangs; g++) {
    char suspicionLine[64];
    snprintf(suspicionLine, sizeof(suspicionLine), "Suspicion[Gang %d]: %.2f", g + 1, suspicion_vals[g]);
    drawText(pad + 5, y, suspicionLine);
    y -= 20;
}


    glColor3f(1,1,1); glBegin(GL_LINES); glVertex2f(policePanel,0); glVertex2f(policePanel,H); glEnd();

    pthread_rwlock_rdlock(&shm->rwlock);


    // Auto layout decision
    bool useGridLayout = false;
    for (int g = 0; g < num_gangs; g++) {
        if (shm->gang[g].members_alive > 9) {
            useGridLayout = true;
            break;
        }
    }

    if (num_gangs > 3) useGridLayout = true;

    if (!useGridLayout) {
        // VERTICAL STACK LAYOUT
        int y_cursor = H - 40;
        
    for (int g = 0; g < num_gangs; g++) {

    if (shm->gang[g].jailed) {
    char label[64];
    snprintf(label, sizeof(label), "Gang %d in prison", g + 1);
    drawText(startX + 10, y_cursor - 30, label);

    char timer[64];
    snprintf(timer, sizeof(timer), "⏳ Time left: %d sec", shm->cfg.prison_sentence_duration);
    drawText(startX + 10, y_cursor - 60, timer);

    drawFrameScaled(&prisonSheet, 0, startX + 10, y_cursor - 150, 0.8f);

    int count = 0;
    int cols = 5;
    int spacingX = 50;
    int spacingY = 60;
    int px = startX + 40;
    int py = y_cursor - 180;

    for (int i = 0; i < shm->cfg.gang_members_max; i++) {
        // Skip dead members
        if (shm->gang_member_dead[g][i]) continue;

        int col = count % cols;
        int row = count / cols;
        int x = px + col * spacingX;
        int y = py - row * spacingY;

        Sheet *sh = &gangSheets[STATE_IDLE][i % CHAR_PER_G];
        drawFrameScaled(sh, 0, x, y, 0.45f);

        char rankBuf[32];
        snprintf(rankBuf, sizeof(rankBuf), "Rank: %d", shm->gang_ranks[g][i]);
        drawText(x, y - 20, rankBuf);

        count++;
    }

    y_cursor -= 220;
    continue;  // Skip gang section
}




            int alive = shm->gang[g].members_alive;
            int cols = (alive < 5) ? alive : 5;
            if (cols <= 0) cols = 1;

            int rows = (alive + cols - 1) / cols;
            if (rows <= 0) rows = 1;


            float scale;
            if (num_gangs == 1)
                    scale = 0.9f;   // Very large for 1 gang
            else if (num_gangs == 2)
                    scale = 0.75f;  // Medium large
            else
                scale = 0.55f;  // Default for 3 gangs

            int spacingX = gangPanelW / (cols + 1);
            int spacingY = 160;

            int blockHeight = rows * spacingY + 80;
            int titleY = y_cursor - 25;
            // char title[128];
            // snprintf(title, sizeof(title), "Gang %d  %s", g + 1, shm->cfg.crimes[g % shm->cfg.num_crimes].name);
            char title[128];
snprintf(title, sizeof(title), "Gang %d  %s", g + 1, shm->cfg.crimes[g % shm->cfg.num_crimes].name);
drawText(startX + 10, titleY, title);

// If jailed, draw ARRESTED in red next to it
if (shm->gang[g].jailed) {
    glColor3f(1.0f, 0.0f, 0.0f);  // red
    drawText(startX + 300, titleY, "ARRESTED");
    glColor3f(1, 1, 1);  // reset to white
}




            drawText(startX + 10, titleY, title);

            int memberStartY = y_cursor - 100 - (blockHeight - (rows * spacingY)) / 2;
            for (int i = 0; i < alive; i++) {
                int row = i / cols, col = i % cols;
                int x = startX + (col + 1) * spacingX;
                int y = memberStartY - row * spacingY;
                
                /////  Change gang member to “run” during prep and back to idle or “READY” //////// to run while preparing ///
                int prep = shm->gang_prep_levels[g][i];
    int is_ready = (prep >= shm->cfg.required_prep_level);
    int state = STATE_IDLE;

    // Draw labels above character
// if (shm->gang_leaders[g] == i) {
//     drawText(x, y + 60, "👑 Leader");
// }
// if (shm->gang_secret_agents[g][i]) {
//     drawText(x, y + 40, "🕵 Agent");
// }

    /// trying animation //////
    // // If the member is not ready and has started prepping
    // if (prep > 0 && !is_ready) {
    //     state = STATE_WALK; // Show run
    // }
    // If the member is killed
    // else if (shm->gang[g].killed[i]) {  // <- Add this to shm_layout_t
    //     state = STATE_DEAD;
    // }

    Sheet *sh = &gangSheets[state][i % CHAR_PER_G];

    drawFrameScaled(sh, state == STATE_WALK ? gangFrame : 0, x, y, scale);

                int rank = shm->gang_ranks[g][i];
                

                char rankBuf[32], prepBuf[32];
                snprintf(rankBuf, sizeof(rankBuf), "Rank: %d", rank);
                
              
                if (is_ready)
                    snprintf(prepBuf, sizeof(prepBuf), "Prep: READY");
                else
                    snprintf(prepBuf, sizeof(prepBuf), "Prep: %d", prep);


                int frame = (state == STATE_WALK) ? gangFrame : 0;
                drawFrameScaled(sh, frame, x, y, scale);

                drawText(x, y - 20, prepBuf);
                drawText(x, y - 40, rankBuf);
            }

            int afterY = y_cursor - blockHeight + 10;
            glColor3f(1, 1, 1); glBegin(GL_LINES);
            glVertex2f(startX, afterY); glVertex2f(W - pad, afterY);
            glEnd();
            y_cursor = afterY - 15;
        }
    } else {
        // GRID LAYOUT (2x2, 3x2...)
        int cols = (num_gangs <= 2) ? num_gangs : 2;
        int rows = (num_gangs + cols - 1) / cols;
        int cellW = gangPanelW / cols;
        int cellH = H / rows;

        for (int g = 0; g < num_gangs; g++) {
            int gx = g % cols, gy = g / cols;
            int ox = startX + gx * cellW;
            int oy = H - gy * cellH - pad;

            int alive = shm->gang[g].members_alive;
            int mcols = (alive <= 5) ? alive : 5;
            if (mcols <= 0) mcols = 1;

            int mrows = (alive + mcols - 1) / mcols;
            if (mrows <= 0) mrows = 1;


            int spacingX = cellW / (mcols + 1);
            int spacingY = cellH / (mrows + 1);
            float scale = 0.5f;

            char title[128];
            snprintf(title, sizeof(title), "Gang %d  %s", g + 1, shm->cfg.crimes[g % shm->cfg.num_crimes].name);
            drawText(ox + 10, oy - 20, title);

            for (int i = 0; i < alive; i++) {
                int row = i / mcols, col = i % mcols;
                int x = ox + (col + 1) * spacingX;
                int y = oy - 60 - row * spacingY;

                //Sheet *sh = &gangSheets[STATE_IDLE][i % CHAR_PER_G];
                int rank = shm->gang_ranks[g][i];
                //int prep = shm->gang_prep_levels[g][i];

                int prep = shm->gang_prep_levels[g][i];
                int is_ready = (prep >= shm->cfg.required_prep_level);
                int state = STATE_IDLE;
                if (shm->gang[g].jailed == 0) {
                    if (shm->gang_member_dead[g][i]) {
                        state = STATE_DEAD;
                    } else if (!is_ready && prep > 0) {
                        state = STATE_WALK;
                    }
                }
                Sheet *sh = &gangSheets[state][i % CHAR_PER_G];


                char rankBuf[32], prepBuf[32];
                snprintf(rankBuf, sizeof(rankBuf), "Rank: %d", rank);
                snprintf(prepBuf, sizeof(prepBuf), "Prep: %d", prep);

                int frame = (state == STATE_WALK) ? gangFrame : 0;
                drawFrameScaled(sh, frame, x, y, scale);
                drawText(x, y - 20, prepBuf);
                drawText(x, y - 40, rankBuf);
            }
        }
    }

    pthread_rwlock_unlock(&shm->rwlock);
    glutSwapBuffers();
}

static void reshape(int w, int h) {
    glViewport(0,0,w,h);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); gluOrtho2D(0,w,0,h);
    glMatrixMode(GL_MODELVIEW);
}

static void timerfunc(int) {
    gangFrame = (gangFrame + 1) % gangSheets[STATE_WALK][0].cols;
    glutPostRedisplay();
    glutTimerFunc(animDelay, timerfunc, 0);
}

int main(int argc, char **argv) {
    shm = shm_child_attach();
    if (!shm) { fprintf(stderr, "❌ Cannot attach to shared memory\n"); return EXIT_FAILURE; }
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB);
    glutInitWindowSize(WIN_W, WIN_H);
    glutCreateWindow("La Casa de Papel Simulation");

    for (int p = 0; p < POLICE_CHARS; p++) {
        loadSheet(policeFiles[p][0], &policeSheets[p][STATE_IDLE], 6, 1);
        loadSheet(policeFiles[p][1], &policeSheets[p][STATE_WALK], 7, 1);
        loadSheet(policeFiles[p][2], &policeSheets[p][STATE_DEAD], 5, 1);
    }
    for (int i = 0; i < CHAR_PER_G; i++) {
        loadSheet(idleFiles[i], &gangSheets[STATE_IDLE][i], 6, 1);
        loadSheet(runFiles[i],  &gangSheets[STATE_WALK][i], 7, 1);
        loadSheet(deadFiles[i], &gangSheets[STATE_DEAD][i], 5, 1);
    }
    loadSheet(prisonFile, &prisonSheet, 1, 1);

    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutTimerFunc(animDelay, timerfunc, 0);
    glutMainLoop();
    return 0;
}