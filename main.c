#include "raylib.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ─── Compatibility ───────────────────────────────────────────────────────────
#ifndef Clamp
#define Clamp(value, min, max) ((value) < (min) ? (min) : ((value) > (max) ? (max) : (value)))
#endif

// ─── Constants ───────────────────────────────────────────────────────────────
#define SCREEN_W        1280
#define SCREEN_H        720
#define PLAYER_SIZE     28
#define ENEMY_SIZE      28
#define BULLET_SIZE     18
#define BULLET_SPEED    600.0f
#define PLAYER_SPEED    220.0f
#define RAT_SPEED       90.0f
#define DASH_SPEED      900.0f
#define DASH_DURATION   0.18f
#define DASH_COOLDOWN   1.2f
#define CROSS_COOLDOWN  0.0f
#define CROSS_BULLETS   8
#define MAX_BULLETS     128
#define MAX_ENEMIES     16
#define MAX_HITS_BULLET 16
#define MAX_PARTICLES   256
#define PERCENT_WINDUP 0.10f
#define PERCENT_LOCK 0.90f
#define SQUARE_SIZE 48
#define SQUARE_PAD 4
#define SQUARE_SIZEf 48.0f

// ─── Types ────────────────────────────────────────────────────────────────────
typedef enum {
    SHAPE_NONE,
    SHAPE_RECT,
    SHAPE_CIRCLE,
    SHAPE_CROSS,
} ShapeKind;

typedef struct {
    ShapeKind kind;
    int width;
    int height;
    int size;
} Shape2D;

typedef enum {
    BULLET_NORMAL,
    BULLET_PIERCING,
} BulletKind;

typedef struct {
    Vector2 pos;
    Vector2 vel;
    float dist_travelled;
    bool    active;
    float   lifetime;
    Color   color;
    float   size;
    int enemies_hit_idx[MAX_ENEMIES];
    int enemies_hit_count;
    BulletKind kind;
} Bullet;

typedef enum { ENEMY_DUMMY, ENEMY_RAT } EnemyType;

typedef struct {
    Vector2    pos;
    float      hp;
    float      maxHp;
    bool       active;
    EnemyType  type;
    float      flashTimer;
} Enemy;

typedef struct {
    Vector2 pos;
    Vector2 vel;
    float   life;
    Color   color;
    float   size;
} Particle;

// ── Auto-attack phase ────────────────────────────────────────────────────────
//  ATK_IDLE    : no target selected or player moved and reset the windup
//  ATK_WINDUP  : player is standing still; timer counts UP toward the 10% threshold
//  ATK_LOCKED  : player is locked in place for the remaining 90% of the swing;
//                any movement input is stored in moveBuffer and applied on exit
typedef enum { ATK_IDLE, ATK_WINDUP, ATK_LOCKED } AtkState;

typedef enum {
    SPELL_NONE,
    SPELL_LIGHTNING_BOLT,
    SPELL_CONJURE_FIRE,
    SPELL_BERSERK,
    SPELL_WATER_PILLAR,
    SPELL_WATER_WAVE,
    SPELL_ENERGY_HULL,
    SPELL_PRECISE_SHOT,
} SpellKind;

typedef struct {
    Vector2  pos;
    float    hp;
    float    maxHp;
    // dash
    bool     dashing;
    float    dashTimer;
    float    dashCooldown;
    Vector2  dashDir;
    // cross ability
    float    crossCooldown;
    // invincibility frames
    float    iFrames;
    // auto-attack
    float    atkSpeed;       // attacks per second; fullAtkDuration = 2 / atkSpeed
    AtkState atkState;
    float    atkWindupTimer; // counts UP during windup phase
    float    atkLockTimer;   // counts DOWN during locked phase
    int      atkTargetIdx;   // index into enemies[], -1 = no target
    Vector2  moveBuffer;     // last held direction, flushed when lock ends
    SpellKind spellSelected;
} Player;

// ─── Globals ──────────────────────────────────────────────────────────────────
static Player   player;
static Bullet   bullets[MAX_BULLETS];
static Enemy    enemies[MAX_ENEMIES];
static Particle particles[MAX_PARTICLES];
static int      score      = 0;
static float    screenShake  = 0.0f;
// Auto-attack flash line (drawn for a short duration after each hit)
static Vector2  atkLineFrom  = {0,0};
static Vector2  atkLineTo    = {0,0};
static float    atkLineTimer = 0.0f;
#define ATK_LINE_DURATION 0.1f

// ─── Helpers ──────────────────────────────────────────────────────────────────
static float V2Len(Vector2 v) { 
    return sqrtf(v.x*v.x + v.y*v.y); 
}

static Vector2 V2Norm(Vector2 v) {
    float l = V2Len(v);
    if (l < 0.0001f) return (Vector2){0,0};
    return (Vector2){v.x/l, v.y/l};
}

static Vector2 V2Scale(Vector2 v, float scale) {
    return (Vector2){v.x*scale, v.y*scale};
}

static Vector2 V2Sum(Vector2 v1, Vector2 v2) {
    return (Vector2){v1.x + v2.x, v1.y + v2.y};
}

static Rectangle EntityRect(Vector2 pos, float size) {
    return (Rectangle){ pos.x - size/2, pos.y - size/2, size, size };
}

static void SpawnParticle(Vector2 pos, Color col, float speed, float life, float size) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &particles[i];
        if (p->life <= 0) {
            float angle = (float)(rand() % 360) * DEG2RAD;
            float spd   = speed * (0.4f + (rand()%100)/100.0f * 0.6f);
            p->pos   = pos;
            p->vel   = (Vector2){ cosf(angle)*spd, sinf(angle)*spd };
            p->life  = life;
            p->color = col;
            p->size  = size;
            return;
        }
    }
}

static void SpawnBurst(Vector2 pos, Color col, int count) {
    for (int i = 0; i < count; i++)
        SpawnParticle(pos, col, 180.0f + rand()%120, 0.35f + (rand()%30)/100.0f, 4.0f);
}

static void FireBullet(Vector2 from, Vector2 dir, Color col, int size, BulletKind kind, float lifetime) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        Bullet *b = &bullets[i];
        if (!b->active) {
            b->pos      = from;
            b->vel      = (Vector2){ dir.x*BULLET_SPEED, dir.y*BULLET_SPEED };
            b->active   = true;
            b->lifetime = lifetime == 0 ? 4.5f : lifetime;
            b->color    = col;
            b->size     = size;
            b->kind     = kind;
            b->enemies_hit_count = 0;
            return;
        }
    }
}

void SpawnAtkArea(Shape2D shape, Vector2 pos){
    
}

// ─── Init ─────────────────────────────────────────────────────────────────────
static void Init(void) {
    player.pos            = (Vector2){SCREEN_W/2.0f, SCREEN_H/2.0f};
    player.hp             = 100;
    player.maxHp          = 100;
    player.dashing        = false;
    player.dashTimer      = 0;
    player.dashCooldown   = 0;
    player.crossCooldown  = 0;
    player.iFrames        = 0;
    player.atkSpeed       = 1.5f;   // fullAtkDuration = 2 / 1.5 ≈ 1.33s
    player.atkState       = ATK_IDLE;
    player.atkWindupTimer = 0;
    player.atkLockTimer   = 0;
    player.atkTargetIdx   = -1;
    player.moveBuffer     = (Vector2){0,0};
    player.spellSelected  = SPELL_ENERGY_HULL;

    // Spawn dummy enemies in a rough ring
    int dummies_count = 6;
    float angles[] = {30,90,150,210,270,330};
    for (int i = 0; i < dummies_count; i++) {
        float a = angles[i] * DEG2RAD;
        enemies[i].pos      = (Vector2){ SCREEN_W/2 + cosf(a)*320, SCREEN_H/2 + sinf(a)*260 };
        enemies[i].hp       = 50;
        enemies[i].maxHp    = 50;
        enemies[i].active   = true;
        enemies[i].type     = ENEMY_DUMMY;
        enemies[i].flashTimer = 0;
    }

    // Spawn one rat
    enemies[dummies_count].pos       = (Vector2){ 100, 100 };
    enemies[dummies_count].hp        = 30;
    enemies[dummies_count].maxHp     = 30;
    enemies[dummies_count].active    = true;
    enemies[dummies_count].type      = ENEMY_RAT;
    enemies[dummies_count].flashTimer= 0;
    // Clear the rest
    for (int i = dummies_count+1; i < MAX_ENEMIES; i++) enemies[i].active = false;

    memset(bullets,   0, sizeof bullets);
    memset(particles, 0, sizeof particles);
    score       = 0;
    screenShake = 0;
}
// old burst spell (cool idea, make more of these)
// for (int i = 0; i < CROSS_BULLETS; i++) {
//     float angle = (360.0f / CROSS_BULLETS) * i * DEG2RAD;
//     Vector2 dir = { cosf(angle), sinf(angle) };
//     if(i<=2) {
//         printf("cross = dir.x = %f, dir.y = %f\n", dir.x, dir.y);
//     }
//     for (int j = 0; j < MAX_BULLETS; j++) {
//         if (!bullets[j].active) {
//             bullets[j].pos      = mp;
//             bullets[j].vel      = (Vector2){ dir.x * BULLET_SPEED * 0.85f,
//                                              dir.y * BULLET_SPEED * 0.85f };
//             bullets[j].active   = true;
//             bullets[j].lifetime = 1.8f;
//             bullets[j].color    = (Color){255, 80, 200, 255};
//             bullets[j].size     = 10;
//             break;
//         }
//     }
// }
// SpawnBurst(mp, (Color){255,80,200,255}, 20);

void handle_spells(Player* p, float dt) {
    if (p->crossCooldown > 0) p->crossCooldown -= dt;
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && p->crossCooldown <= 0) {
        switch(p->spellSelected) {
            case SPELL_LIGHTNING_BOLT:
                {
                Vector2 mp = GetMousePosition();
                Vector2 p_to_mp = {mp.x - p->pos.x , mp.y - p->pos.y};
                Vector2 p_to_mp_norm = V2Norm(p_to_mp);
                FireBullet(p->pos, p_to_mp_norm,(Color){255,80,200,255}, BULLET_NORMAL, BULLET_NORMAL, 0.f);
                p->crossCooldown = CROSS_COOLDOWN;
                screenShake = 0.3f;
                }
                break;
            
            case SPELL_CONJURE_FIRE:
                {
                Vector2 mp = GetMousePosition();
                float fmod_x = fmod(mp.x, SQUARE_SIZEf);
                float fmod_y = fmod(mp.y, SQUARE_SIZEf);
                Vector2 point = { mp.x - fmod_x, mp.y - fmod_y};
                Rectangle r = {point.x, point.y, SQUARE_SIZE, SQUARE_SIZE};
                for (int i = 0; i < MAX_ENEMIES; i++) {
                    Enemy* e = &enemies[i]; 
                    if(!e->active){
                        continue;
                    } 
                    if(CheckCollisionRecs(r, EntityRect(e->pos, ENEMY_SIZE + 6))){
                        printf("e collided! type = %d", e->type );
                        e->hp -= 10; 
                    }
                }
                p->crossCooldown = CROSS_COOLDOWN;
                screenShake = 0.3f;
                }
                break;

            case SPELL_BERSERK:
                {
                Vector2 pos = p->pos;
                float fmod_x = fmod(pos.x, SQUARE_SIZEf);
                float fmod_y = fmod(pos.y, SQUARE_SIZEf);
                Vector2 point = { pos.x - fmod_x, pos.y - fmod_y};
                Color c = (Color){ 230, 41, 55, 150 };
                Rectangle r = {point.x - SQUARE_SIZE, point.y - SQUARE_SIZE, 3*SQUARE_SIZE, 3*SQUARE_SIZE};
                for (int i = 0; i < MAX_ENEMIES; i++) {
                    Enemy* e = &enemies[i]; 
                    if(!e->active){
                        continue;
                    } 
                    if(CheckCollisionRecs(r, EntityRect(e->pos, ENEMY_SIZE))){
                        printf("e collided! type = %d", e->type );
                        e->hp -= 10; 
                    }
                }
                p->crossCooldown = CROSS_COOLDOWN;
                screenShake = 0.3f;
                }
                break;

            case SPELL_ENERGY_HULL:
                {
                Vector2 mp = GetMousePosition();
                Vector2 p_to_mp = {mp.x - p->pos.x , mp.y - p->pos.y};
                Vector2 p_to_mp_norm = V2Norm(p_to_mp);
                FireBullet(p->pos, p_to_mp_norm,(Color){255,80,200,255}, 100, BULLET_PIERCING, 0.4f);
                p->crossCooldown = CROSS_COOLDOWN;
                screenShake = 0.3f;
                }
                break;
            case SPELL_WATER_PILLAR:
                {
                Vector2 pos = GetMousePosition();
                float fmod_x = fmod(pos.x, SQUARE_SIZEf);
                float fmod_y = fmod(pos.y, SQUARE_SIZEf);
                Vector2 point = { pos.x - fmod_x, pos.y - fmod_y};
                Color c = (Color){ 230, 41, 55, 150 };
                Rectangle r = {point.x - SQUARE_SIZE, point.y - SQUARE_SIZE, 3*SQUARE_SIZE, 3*SQUARE_SIZE};
                for (int i = 0; i < MAX_ENEMIES; i++) {
                    Enemy* e = &enemies[i]; 
                    if(!e->active){
                        continue;
                    } 
                    if(CheckCollisionRecs(r, EntityRect(e->pos, ENEMY_SIZE))){
                        printf("e collided! type = %d", e->type );
                        e->hp -= 10; 
                    }
                }
                p->crossCooldown = CROSS_COOLDOWN;
                screenShake = 0.3f;
                }
                break;

            default:
                break;
        }
    }
}

void handle_player_stuff(float dt){
    // Screen shake decay
    if (screenShake > 0) screenShake -= dt * 8.0f;
    if (screenShake < 0) screenShake = 0;
    if (atkLineTimer > 0) atkLineTimer -= dt;
    // ── Derived attack timing ─────────────────────────────────────────────────
    // fullAtkDuration = 2 / atkSpeed
    float fullAtkDuration  = 2.0f / player.atkSpeed;
    float windupDuration   = fullAtkDuration * PERCENT_WINDUP;
    float lockDuration     = fullAtkDuration * PERCENT_LOCK;

    // ── Read movement input (always, used for buffer too) ─────────────────────
    Vector2 move = {0,0};
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP))    move.y -= 1;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN))   move.y += 1;
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))   move.x -= 1;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT))  move.x += 1;
    move = V2Norm(move);

    bool playerMoving = V2Len(move) > 0.1f;

    // ── ESC: untarget ────────────────────────────────────────────────────────
    if (IsKeyPressed(KEY_ESCAPE)) {
        player.atkTargetIdx   = -1;
        player.atkState       = ATK_IDLE;
        player.atkWindupTimer = 0;
        // Does NOT clear atkLockTimer — if already locked, finish the animation
    }

    // ── Validate current target (may have died) ───────────────────────────────
    // Target dies in WINDUP  -> attack never committed, reset fully.
    // Target dies in LOCKED  -> animation still plays out; clear index only.
    if (player.atkTargetIdx >= 0 && !enemies[player.atkTargetIdx].active) {
        player.atkTargetIdx = -1;
        if (player.atkState == ATK_WINDUP) {
            player.atkState       = ATK_IDLE;
            player.atkWindupTimer = 0;
        }
        // ATK_LOCKED: leave state and timer alone — lock runs to completion
    }

    // ── LMB: select/deselect target ──────────────────────────────────────────
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        Vector2 mp = GetMousePosition();
        int hit = -1;
        for (int i = 0; i < MAX_ENEMIES; i++) {
            if (!enemies[i].active) continue;
            if (CheckCollisionPointRec(mp, EntityRect(enemies[i].pos, ENEMY_SIZE + 6))) {
                hit = i;
                break;
            }
        }
        if (hit >= 0) {
            // Clicking a new target resets the windup
            if (player.atkTargetIdx != hit) {
                player.atkWindupTimer = 0;
                player.atkState       = ATK_IDLE;
            }
            player.atkTargetIdx = hit;
        } else {
            // Clicked empty space — deselect
            player.atkTargetIdx = -1;
            player.atkState     = ATK_IDLE;
            player.atkWindupTimer = 0;
        }
    }

    // ── Auto-attack state machine ─────────────────────────────────────────────
    switch (player.atkState) {

        case ATK_IDLE:
            // Begin windup only if we have a target and the player is not moving
            if (player.atkTargetIdx >= 0 && !playerMoving && !player.dashing) {
                player.atkState       = ATK_WINDUP;
                player.atkWindupTimer = 0;
            }
            break;

        case ATK_WINDUP:
            if (playerMoving || player.dashing) {
                // Player moved — cancel windup, go back to idle
                player.atkState       = ATK_IDLE;
                player.atkWindupTimer = 0;
            } else if (player.atkTargetIdx < 0) {
                player.atkState = ATK_IDLE;
            } else {
                player.atkWindupTimer += dt;
                if (player.atkWindupTimer >= windupDuration) {
                    // Commit to the attack — enter locked phase
                    player.atkState     = ATK_LOCKED;
                    player.atkLockTimer = lockDuration;
                    player.moveBuffer   = (Vector2){0,0};

                    // Instant-hit auto attack: deal damage, then show a flash line
                    Enemy *t = &enemies[player.atkTargetIdx];
                    t->hp -= 10;
                    t->flashTimer = 0.12f;
                    SpawnBurst(t->pos, WHITE, 6);
                    atkLineFrom  = player.pos;
                    atkLineTo    = t->pos;
                    atkLineTimer = ATK_LINE_DURATION;
                    screenShake = 0.12f;
                    // Muzzle particles
                    for (int i = 0; i < 5; i++)
                        SpawnParticle(player.pos, (Color){255,220,60,200}, 120, 0.18f, 4.0f);
                }
            }
            break;

        case ATK_LOCKED:
            // Buffer the latest movement input so the player can queue a kite
            if (playerMoving) player.moveBuffer = move;

            player.atkLockTimer -= dt;
            if (player.atkLockTimer <= 0) {
                // Attack animation done — unlock
                player.atkState       = ATK_IDLE;
                player.atkWindupTimer = 0;
                // Immediately apply buffered movement so kiting feels responsive
                // (actual position update happens below in the movement block)
            }
            break;
    }

    // ── Dash ──────────────────────────────────────────────────────────────────
    if (player.dashCooldown > 0) player.dashCooldown -= dt;
    if (player.dashing) {
        player.dashTimer -= dt;
        if (player.dashTimer <= 0) player.dashing = false;
    }

    if (IsKeyPressed(KEY_SPACE) && player.dashCooldown <= 0) {
        // Dash cancels any attack state
        player.atkState       = ATK_IDLE;
        player.atkWindupTimer = 0;
        player.atkLockTimer   = 0;

        Vector2 dir = playerMoving ? move : (Vector2){1,0};
        if (!playerMoving) {
            Vector2 mp = GetMousePosition();
            dir = V2Norm((Vector2){mp.x - player.pos.x, mp.y - player.pos.y});
        }
        player.dashing     = true;
        player.dashTimer   = DASH_DURATION;
        player.dashCooldown= DASH_COOLDOWN;
        player.dashDir     = dir;
        player.iFrames     = DASH_DURATION + 0.05f;
        for (int i = 0; i < 12; i++)
            SpawnParticle(player.pos, (Color){255,100,60,200}, 80, 0.3f, 5.0f);
    }

    // ── Apply movement ────────────────────────────────────────────────────────
    if (player.dashing) {
        player.pos.x += player.dashDir.x * DASH_SPEED * dt;
        player.pos.y += player.dashDir.y * DASH_SPEED * dt;
    } else if (player.atkState == ATK_LOCKED) {
        // Locked — no movement, but buffer is being stored above
    } else {
        // Normal movement; after a lock ends, moveBuffer holds the queued direction
        Vector2 effectiveMove = playerMoving ? move : player.moveBuffer;
        // Clear the buffer once we've consumed it for one frame after lock ends
        if (!playerMoving && player.atkState != ATK_LOCKED) player.moveBuffer = (Vector2){0,0};
        player.pos.x += effectiveMove.x * PLAYER_SPEED * dt;
        player.pos.y += effectiveMove.y * PLAYER_SPEED * dt;
        // If the player is actually moving now, reset idle→windup next frame
    }

    // Clamp to screen
    player.pos.x = Clamp(player.pos.x, PLAYER_SIZE/2, SCREEN_W - PLAYER_SIZE/2);
    player.pos.y = Clamp(player.pos.y, PLAYER_SIZE/2, SCREEN_H - PLAYER_SIZE/2);

    if (player.iFrames > 0) player.iFrames -= dt;

    // ── Cross ability (RMB) ──────────────────────────────────────────────────
    handle_spells(&player, dt);

}

void handle_particles(float dt) {
    // ── Update particles ──────────────────────────────────────────────────────
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &particles[i];
        if (p->life <= 0) continue;
        p->pos.x += p->vel.x * dt;
        p->pos.y += p->vel.y * dt;
        p->vel.x *= 0.92f;
        p->vel.y *= 0.92f;
        p->life  -= dt;
        p->size  *= 0.97f;
    }
}


void handle_enemies(float dt) {
    // ── Update enemies ────────────────────────────────────────────────────────
    Vector2 mp = GetMousePosition();
    float fmod_x = fmod(mp.x, SQUARE_SIZEf);
    float fmod_y = fmod(mp.y, SQUARE_SIZEf);
    Vector2 point = { mp.x - fmod_x, mp.y - fmod_y};
    Rectangle r = {point.x, point.y, SQUARE_SIZE, SQUARE_SIZE};
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &enemies[i];

        // if(CheckCollisionRecs(r, EntityRect(e->pos, ENEMY_SIZE + 6))){
        //     printf("COLIDING!!");
        // }
        if (!e->active) continue;
        if(e->hp <= 0){
            e->active = false;
            SpawnBurst(e->pos, e->type == ENEMY_RAT ?
                       (Color){80,120,255,255} : (Color){200,200,200,255}, 20);
        } 
        if (e->flashTimer > 0) e->flashTimer -= dt;

        if (e->type == ENEMY_RAT) {
            Vector2 dir = V2Norm((Vector2){player.pos.x - e->pos.x,
                                           player.pos.y - e->pos.y});
            e->pos.x += dir.x * RAT_SPEED * dt;
            e->pos.y += dir.y * RAT_SPEED * dt;

            if (player.iFrames <= 0 &&
                CheckCollisionRecs(EntityRect(player.pos, PLAYER_SIZE),
                                   EntityRect(e->pos, ENEMY_SIZE))) {
                player.hp      -= 15;
                player.iFrames  = 0.5f;
                screenShake     = 0.5f;
                SpawnBurst(player.pos, RED, 12);
            }
        }
    }

}

void handle_bullets(float dt){
    // ── Update bullets ────────────────────────────────────────────────────────
    for (int i = 0; i < MAX_BULLETS; i++) {
        Bullet *b = &bullets[i];
        if (!b->active) continue;
        b->pos.x    += b->vel.x * dt;
        b->pos.y    += b->vel.y * dt;
        b->lifetime -= dt;
        if (rand()%3 == 0)
            SpawnParticle(b->pos, Fade(b->color, 0.5f), 20, 0.12f, b->size * 0.5f);

        if (b->lifetime <= 0 || b->pos.x < 0 || b->pos.x > SCREEN_W ||
            b->pos.y < 0 || b->pos.y > SCREEN_H) {
            b->active = false;
            b->enemies_hit_count = 0;
            continue;
        }
        Rectangle br = EntityRect(b->pos, b->size);
        for (int j = 0; j < MAX_ENEMIES; j++) {
            Enemy *e = &enemies[j];
            if (!e->active) continue;
            bool was_hit_already = false;
            if (CheckCollisionRecs(br, EntityRect(e->pos, ENEMY_SIZE))) {
                // TODO: cleanup after enemies are hit
                for(int x = 0; x < b->enemies_hit_count; x++){
                    int eIdx = b->enemies_hit_idx[x];
                    if(eIdx == j){
                        was_hit_already = true;
                        break;
                    }
                } 
                if(was_hit_already){
                    continue;
                } else {
                    b->enemies_hit_idx[b->enemies_hit_count] = j;
                    b->enemies_hit_count += 1;
                }
                e->hp -= 10;
                e->flashTimer = 0.1f;
                if(b->kind == BULLET_NORMAL){
                    b->active = false;
                    b->enemies_hit_count = 0;
                }
                SpawnBurst(e->pos, WHITE, 6);
                screenShake = 0.15f;
                if (e->hp <= 0) {
                    e->active = false;
                    SpawnBurst(e->pos, e->type == ENEMY_RAT ?
                               (Color){80,120,255,255} : (Color){200,200,200,255}, 20);
                    score += (e->type == ENEMY_RAT) ? 50 : 20;
                    screenShake = 0.4f;
                }
                break;
            }
        }
    }
}

// ─── Update ───────────────────────────────────────────────────────────────────
static void Update(float dt) {
    handle_player_stuff(dt);
    handle_enemies(dt);
    handle_bullets(dt);
    handle_particles(dt);
}

void handle_draw_spell_preview(){

    Vector2 mp = GetMousePosition();
    switch(player.spellSelected){
        case SPELL_LIGHTNING_BOLT:
            {
            // float alpha = atkLineTimer / ATK_LINE_DURATION;
            Color lc = Fade(WHITE, 0.85f);
            Vector2 from = player.pos;
            Vector2 to = mp;
            Vector2 full_dir = (Vector2){from.x - to.x, from.y - to.y};
            float length = V2Len(full_dir);
            if(abs(length) > 300.f) {
                Vector2 toN = V2Norm(to);
                length = V2Len(full_dir) - 300.f;
                to = V2Sum(to, V2Scale(V2Norm(full_dir), length));
            }
            Vector2 dir = V2Scale(V2Norm(full_dir), 5.f);
            Vector2 r1 = {dir.y, -dir.x};
            Vector2 r2 = {-dir.y, dir.x};

            // 2 main parallel lines
            DrawLineEx(V2Sum(from, r1), V2Sum(to, r1), 1.f, lc);
            DrawLineEx(V2Sum(from, r2), V2Sum(to, r2), 1.f, lc);

            float spacing = 12.f;
            Vector2 along = V2Norm(full_dir);
            int steps     = (int)(length / spacing);

            for (int i = 1; i < steps; i++) {
                float t = i * spacing;
                Vector2 center = V2Sum(from, V2Scale(along, -t));

                // Always cross from r1 side to r2 side at the NEXT step along
                Vector2 a = V2Sum(center, r1);
                Vector2 b = V2Sum(V2Sum(center, V2Scale(along, -spacing)), r2);

                DrawLineEx(a, b, 1.f, lc);
            }

            }

            break;
        case SPELL_CONJURE_FIRE:
            {
            float fmod_x = fmod(mp.x, SQUARE_SIZEf);
            float fmod_y = fmod(mp.y, SQUARE_SIZEf);
            Vector2 p = { mp.x - fmod_x, mp.y - fmod_y};
            Rectangle r = {p.x + SQUARE_PAD, p.y + SQUARE_PAD, SQUARE_SIZE - SQUARE_PAD, SQUARE_SIZE - SQUARE_PAD};
            Color c = (Color){ 230, 41, 55, 150 };
            DrawRectangleRounded(r, .3f, 0, c);
            }
            break;

        case SPELL_BERSERK:
            {
            Vector2 pos = player.pos;
            float fmod_x = fmod(pos.x, SQUARE_SIZEf);
            float fmod_y = fmod(pos.y, SQUARE_SIZEf);
            Vector2 p = { pos.x - fmod_x, pos.y - fmod_y};
            Color c = (Color){ 230, 41, 55, 150 };
            Rectangle r = {p.x, p.y, SQUARE_SIZE, SQUARE_SIZE};
            for (int i = -1; i < 2; i++){
                for (int j = -1; j < 2; j++){
                    Rectangle nr = {
                            p.x + i*SQUARE_SIZE + SQUARE_PAD, 
                            p.y + j*SQUARE_SIZE + SQUARE_PAD, 
                            SQUARE_SIZE - SQUARE_PAD, 
                            SQUARE_SIZE - SQUARE_PAD
                        };
                    DrawRectangleRounded(nr, .3f, 0, c);
                }
            }
            }
            break;

        case SPELL_ENERGY_HULL:
            {
            Color lc = Fade(WHITE, 0.85f);
            Vector2 from = player.pos;
            Vector2 to = mp;
            Vector2 full_dir = (Vector2){from.x - to.x, from.y - to.y};
            if(abs(V2Len(full_dir)) > 200.f) {
                Vector2 toN = V2Norm(to);
                to = V2Sum(to, V2Scale(V2Norm(full_dir), V2Len(full_dir) - 200.f));
            }

            Vector2 dir = V2Scale(V2Norm(full_dir), 30.f);
            Vector2 r1 = {dir.y, -dir.x};
            Vector2 r2 = {-dir.y, dir.x};

            DrawLineEx(V2Sum(from, r1), V2Sum(to, r1), 1.f, lc);
            DrawLineEx(V2Sum(from, r2), V2Sum(to, r2), 1.f, lc);
            }
            break;

        case SPELL_WATER_PILLAR:
            {
            Vector2 pos = GetMousePosition();
            float fmod_x = fmod(pos.x, SQUARE_SIZEf);
            float fmod_y = fmod(pos.y, SQUARE_SIZEf);
            Vector2 p = { pos.x - fmod_x, pos.y - fmod_y};
            Color c = (Color){ 230, 41, 55, 150 };
            Rectangle r = {p.x, p.y, SQUARE_SIZE, SQUARE_SIZE};
            for (int i = -1; i < 2; i++){
                for (int j = -1; j < 2; j++){
                    Rectangle nr = {
                            p.x + i*SQUARE_SIZE + SQUARE_PAD, 
                            p.y + j*SQUARE_SIZE + SQUARE_PAD, 
                            SQUARE_SIZE - SQUARE_PAD, 
                            SQUARE_SIZE - SQUARE_PAD
                        };
                    DrawRectangleRounded(nr, .3f, 0, c);
                }
            }
            }
            break;

        default:
            break;
    }
}

// ─── Draw ─────────────────────────────────────────────────────────────────────
static void draw_bullets(Vector2 shake) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        Bullet *b = &bullets[i];
        if (!b->active) continue;
        DrawRectangle((int)(b->pos.x - b->size/2 + shake.x),
                      (int)(b->pos.y - b->size/2 + shake.y),
                      (int)b->size, (int)b->size, b->color);
        DrawRectangle((int)(b->pos.x - b->size/2 - 2 + shake.x),
                      (int)(b->pos.y - b->size/2 - 2 + shake.y),
                      (int)b->size+4, (int)b->size+4, Fade(b->color, 0.2f));
    }
}
static void Draw(void) {
    float fullAtkDuration = 2.0f / player.atkSpeed;
    float windupDuration  = fullAtkDuration * 0.10f;
    float lockDuration    = fullAtkDuration * 0.90f;

    Vector2 shake = {0,0};
    if (screenShake > 0) {
        shake.x = (float)(rand()%9 - 4) * screenShake;
        shake.y = (float)(rand()%9 - 4) * screenShake;
    }

    BeginDrawing();
    ClearBackground((Color){18, 18, 28, 255});

    // Subtle grid
    for (int x = 0; x < SCREEN_W; x += SQUARE_SIZE)
        DrawLine(x + (int)shake.x, 0, x + (int)shake.x, SCREEN_H, (Color){30,30,50,255});
    for (int y = 0; y < SCREEN_H; y += SQUARE_SIZE)
        DrawLine(0, y + (int)shake.y, SCREEN_W, y + (int)shake.y, (Color){30,30,50,255});

    // ── Draw line to selected target ─────────────────────────────────────────
    if (player.atkTargetIdx >= 0 && enemies[player.atkTargetIdx].active) {
        Enemy *t = &enemies[player.atkTargetIdx];
        // Dashed line style via segments
        Vector2 from = player.pos;
        Vector2 to   = t->pos;
        float len    = V2Len((Vector2){to.x-from.x, to.y-from.y});
        Vector2 dir  = V2Norm((Vector2){to.x-from.x, to.y-from.y});
        float segLen = 10.0f, gapLen = 6.0f, travelled = 0;
        while (travelled < len) {
            float start = travelled;
            float end   = Clamp(travelled + segLen, 0, len);
            DrawLineEx(
                (Vector2){from.x + dir.x*start + shake.x, from.y + dir.y*start + shake.y},
                (Vector2){from.x + dir.x*end   + shake.x, from.y + dir.y*end   + shake.y},
                1.5f, (Color){255,220,60,120}
            );
            travelled += segLen + gapLen;
        }
        // Target highlight ring
        DrawCircleLines((int)(t->pos.x + shake.x), (int)(t->pos.y + shake.y),
                        ENEMY_SIZE/2 + 6, (Color){255,220,60,180});
    }

    // ── Auto-attack flash line ─────────────────────────────────────────────────
    if (atkLineTimer > 0) {
        float alpha = atkLineTimer / ATK_LINE_DURATION;
        Color lc = Fade(WHITE, alpha * 0.9f);
        DrawLineEx(
            (Vector2){atkLineFrom.x + shake.x, atkLineFrom.y + shake.y},
            (Vector2){atkLineTo.x   + shake.x, atkLineTo.y   + shake.y},
            1.5f, lc
        );
    }

    // ── Particles ─────────────────────────────────────────────────────────────
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &particles[i];
        if (p->life <= 0) continue;
        float alpha = Clamp(p->life / 0.4f, 0, 1);
        Color c = Fade(p->color, alpha * 0.9f);
        DrawRectangle((int)(p->pos.x - p->size/2 + shake.x),
                      (int)(p->pos.y - p->size/2 + shake.y),
                      (int)p->size, (int)p->size, c);
    }

    // ── Spell Shape Show ──────────────────────────────────────────────────────
    handle_draw_spell_preview();

    // ── Enemies ───────────────────────────────────────────────────────────────
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &enemies[i];
        if (!e->active) continue;
        Color col;
        if (e->type == ENEMY_DUMMY) col = (Color){160, 160, 160, 255};
        else                        col = (Color){80,  120, 255, 255};
        if (e->flashTimer > 0)      col = WHITE;

        DrawRectangle((int)(e->pos.x - ENEMY_SIZE/2 + shake.x),
                      (int)(e->pos.y - ENEMY_SIZE/2 + shake.y),
                      ENEMY_SIZE, ENEMY_SIZE, col);

        float barW  = ENEMY_SIZE + 4;
        float hpPct = e->hp / e->maxHp;
        DrawRectangle((int)(e->pos.x - barW/2 + shake.x),
                      (int)(e->pos.y - ENEMY_SIZE/2 - 8 + shake.y),
                      (int)barW, 4, (Color){60,0,0,200});
        DrawRectangle((int)(e->pos.x - barW/2 + shake.x),
                      (int)(e->pos.y - ENEMY_SIZE/2 - 8 + shake.y),
                      (int)(barW * hpPct), 4,
                      e->type == ENEMY_RAT ? (Color){80,120,255,255} : (Color){200,80,80,255});

        if (e->type == ENEMY_RAT)
            DrawText("RAT", (int)(e->pos.x - 12 + shake.x),
                     (int)(e->pos.y + ENEMY_SIZE/2 + 4 + shake.y), 10,
                     (Color){80,120,255,180});
    }

    // ── Bullets ───────────────────────────────────────────────────────────────
    draw_bullets(shake);

    // ── Player ────────────────────────────────────────────────────────────────
    Color playerCol = RED;
    if (player.iFrames > 0 && (int)(player.iFrames * 20) % 2 == 0)
        playerCol = Fade(RED, 0.3f);
    if (player.dashing)               playerCol = ORANGE;
    if (player.atkState == ATK_LOCKED) playerCol = (Color){255, 180, 60, 255}; // gold = locked

    DrawRectangle((int)(player.pos.x - PLAYER_SIZE/2 + shake.x),
                  (int)(player.pos.y - PLAYER_SIZE/2 + shake.y),
                  PLAYER_SIZE, PLAYER_SIZE, playerCol);
    DrawRectangleLines((int)(player.pos.x - PLAYER_SIZE/2 + shake.x),
                       (int)(player.pos.y - PLAYER_SIZE/2 + shake.y),
                       PLAYER_SIZE, PLAYER_SIZE, WHITE);

    // Windup charge arc around player
    if (player.atkState == ATK_WINDUP && windupDuration > 0) {
        float pct = player.atkWindupTimer / windupDuration;
        // Draw a growing arc to show the charge
        int   cx  = (int)(player.pos.x + shake.x);
        int   cy  = (int)(player.pos.y + shake.y);
        int   r   = PLAYER_SIZE/2 + 5;
        float endAngle = -90.0f + 360.0f * pct;
        DrawCircleSector((Vector2){(float)cx,(float)cy}, (float)r,
                         -90.0f, endAngle, 20, Fade((Color){255,220,60,255}, 0.35f));
        DrawCircleSectorLines((Vector2){(float)cx,(float)cy}, (float)r,
                              -90.0f, endAngle, 20, (Color){255,220,60,200});
    }

    // Lock timer bar below player
    if (player.atkState == ATK_LOCKED && lockDuration > 0) {
        float pct  = player.atkLockTimer / lockDuration;
        int   barX = (int)(player.pos.x - 20 + shake.x);
        int   barY = (int)(player.pos.y + PLAYER_SIZE/2 + 4 + shake.y);
        DrawRectangle(barX, barY, 40, 4, (Color){40,40,40,220});
        DrawRectangle(barX, barY, (int)(40 * pct), 4, (Color){255,180,60,255});
        DrawRectangleLines(barX, barY, 40, 4, WHITE);
    }

    // ── HUD ───────────────────────────────────────────────────────────────────
    DrawText("HP", 14, 14, 16, WHITE);
    DrawRectangle(44, 14, 160, 16, (Color){60,0,0,220});
    DrawRectangle(44, 14, (int)(160.0f * (player.hp / player.maxHp)), 16,
                  (Color){220, 50, 50, 255});
    DrawRectangleLines(44, 14, 160, 16, WHITE);

    float dashPct = 1.0f - Clamp(player.dashCooldown / DASH_COOLDOWN, 0, 1);
    DrawText("DASH [SPACE]", 14, 40, 13, (Color){200,200,200,200});
    DrawRectangle(14, 56, 100, 8, (Color){40,40,40,220});
    DrawRectangle(14, 56, (int)(100 * dashPct), 8,
                  player.dashing ? ORANGE : (Color){255,160,60,255});
    DrawRectangleLines(14, 56, 100, 8, WHITE);

    float crossPct = 1.0f - Clamp(player.crossCooldown / CROSS_COOLDOWN, 0, 1);
    DrawText("Lightning Bolt [RMB]", 14, 70, 13, (Color){200,200,200,200});
    DrawRectangle(14, 86, 100, 8, (Color){40,40,40,220});
    DrawRectangle(14, 86, (int)(100 * crossPct), 8, (Color){255,80,200,255});
    DrawRectangleLines(14, 86, 100, 8, WHITE);

    // Attack state label
    const char *atkLabel =
        player.atkState == ATK_WINDUP ? "WINDING UP..." :
        player.atkState == ATK_LOCKED ? "ATTACKING!" : "";
    if (atkLabel[0])
        DrawText(atkLabel, 14, 100, 13, (Color){255,220,60,220});

    char scoreBuf[32];
    snprintf(scoreBuf, sizeof scoreBuf, "SCORE: %d", score);
    DrawText(scoreBuf, SCREEN_W - MeasureText(scoreBuf, 20) - 14, 14, 20, YELLOW);

    DrawText("LMB: Target enemy | SPACE: Dash | RMB: Cross Blast | WASD: Move/Kite",
             SCREEN_W/2 - 300, SCREEN_H - 26, 14, (Color){180,180,180,160});

    // Game over / win
    int alive = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) if (enemies[i].active) alive++;
    if (player.hp <= 0) {
        DrawRectangle(0,0,SCREEN_W,SCREEN_H,(Color){0,0,0,140});
        const char *msg = "YOU DIED";
        DrawText(msg, SCREEN_W/2 - MeasureText(msg,60)/2, SCREEN_H/2 - 40, 60, RED);
        DrawText("Press R to restart", SCREEN_W/2 - 100, SCREEN_H/2 + 30, 22, WHITE);
    } else if (alive == 0) {
        DrawRectangle(0,0,SCREEN_W,SCREEN_H,(Color){0,0,0,140});
        const char *msg = "CLEARED!";
        DrawText(msg, SCREEN_W/2 - MeasureText(msg,60)/2, SCREEN_H/2 - 40, 60, GREEN);
        char sc[32]; snprintf(sc,sizeof sc,"Final Score: %d", score);
        DrawText(sc, SCREEN_W/2 - MeasureText(sc,26)/2, SCREEN_H/2 + 30, 26, YELLOW);
        DrawText("Press R to restart", SCREEN_W/2 - 100, SCREEN_H/2 + 70, 22, WHITE);
    }

    EndDrawing();
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main(void) {
    InitWindow(SCREEN_W, SCREEN_H, "Action Game");
    SetTargetFPS(60);
    HideCursor();
    Init();

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        if (IsKeyPressed(KEY_R)) Init();

        bool gameOver = (player.hp <= 0);
        int  alive    = 0;
        for (int i = 0; i < MAX_ENEMIES; i++) if (enemies[i].active) alive++;
        bool gameWon = (alive == 0);

        if (!gameOver && !gameWon) Update(dt);

        Draw();

        // Custom crosshair cursor on top
        Vector2 mp = GetMousePosition();
        DrawLine((int)mp.x - 10, (int)mp.y, (int)mp.x + 10, (int)mp.y, WHITE);
        DrawLine((int)mp.x, (int)mp.y - 10, (int)mp.x, (int)mp.y + 10, WHITE);
        DrawCircleLines((int)mp.x, (int)mp.y, 6, WHITE);
    }

    CloseWindow();
    return 0;
}
