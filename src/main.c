/*
 * wooting-aim v0.7: Adaptive keyboard tuner for CS2
 *
 * New in v0.7:
 *   - Velocity-aware dynamic AP/RT (scales with proximity to accuracy threshold)
 *   - Jiggle peek detection (rapid A-D-A pre-arms next direction)
 *   - Binary velocity estimation (CS2 treats input as ON/OFF, not analog)
 *   - Counter-strafe phase decay (ultra-aggro first 80ms, then relaxes)
 *   - CPU yield via SwitchToThread() with configurable poll rate
 *
 * v0.6 features:
 *   - CS2 Game State Integration (weapon detection, round phase)
 *   - Weapon-specific AP/RT profiles (rifle/awp/pistol/smg/knife)
 *   - Windows timer resolution optimization (0.5ms)
 *
 * Core features:
 *   - Dual-axis counter-strafe detection (A/D + W/S)
 *   - Crouch-peek optimization (L-Ctrl detection)
 *   - Predictive pre-arming (detects finger lift before counter-press)
 *   - Per-key AP/RT tuning via config file
 *   - Counter-strafe statistics logging
 *   - Auto-start with CS2 (--watch mode)
 */

/* winsock2 must come before windows.h */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <tlhelp32.h>
#include "../include/wooting-analog-sdk.h"
#include "hid_writer.h"

#pragma comment(lib, "ws2_32.lib")

/* HID Usage IDs */
#define HID_W     0x1A
#define HID_A     0x04
#define HID_S     0x16
#define HID_D     0x07
#define HID_LCTRL 0xE0

/* Key indices for per-key arrays */
#define K_W 0
#define K_A 1
#define K_S 2
#define K_D 3

#define DEAD_ZONE   0.01f
#define PROFILE_IDX 0

#define GSI_PORT    58732
#define GSI_BUF_SIZE 8192

/* Jiggle peek detection */
#define JIGGLE_WINDOW_MS   300.0   /* max time between counter-strafes to count as jiggle */
#define JIGGLE_MIN_COUNT   2       /* min counter-strafes in window to trigger jiggle mode */
#define JIGGLE_PREARM_MS   300.0   /* how long jiggle mode persists after last counter-strafe */

/* Counter-strafe phase decay (based on CS2 mechanics research) */
#define PHASE_ULTRA_MS     80.0    /* ultra-aggressive phase - matches AK counter-strafe to 34% */
#define PHASE_DECAY_MS     200.0   /* total decay window (after ultra, linearly relax) */

/* Velocity-aware scaling */
#define VEL_AGGRO_ZONE     0.50f   /* above 50% of threshold: scale toward more aggressive */
#define VEL_MIN_AP_FACTOR  0.5f    /* at peak velocity, AP = weapon_ap * this factor */

/* ================================================================
 * WEAPON CATEGORIES
 * ================================================================ */
typedef enum {
    WCAT_RIFLE,
    WCAT_AWP,
    WCAT_PISTOL,
    WCAT_SMG,
    WCAT_KNIFE,
    WCAT_OTHER,
    WCAT_COUNT
} WeaponCategory;

static const char *wcat_names[] = {
    "RIFLE", "AWP", "PISTOL", "SMG", "KNIFE", "OTHER"
};

static WeaponCategory categorize_weapon_type(const char *type) {
    if (!type[0]) return WCAT_OTHER;
    if (strcmp(type, "Rifle") == 0 || strcmp(type, "Machine Gun") == 0)
        return WCAT_RIFLE;
    if (strcmp(type, "SniperRifle") == 0) return WCAT_AWP;
    if (strcmp(type, "Pistol") == 0) return WCAT_PISTOL;
    if (strcmp(type, "Submachine Gun") == 0 || strcmp(type, "Shotgun") == 0)
        return WCAT_SMG;
    if (strcmp(type, "Knife") == 0) return WCAT_KNIFE;
    return WCAT_OTHER;
}

/* Weapon max speed lookup for velocity estimation (units/second) */
static float weapon_max_speed(const char *name) {
    if (!name[0]) return 225.0f;
    if (strstr(name, "knife") || strstr(name, "bayonet")) return 250.0f;
    if (strstr(name, "awp")) return 200.0f;
    if (strstr(name, "ak47")) return 215.0f;
    if (strstr(name, "m4a1")) return 225.0f;
    if (strstr(name, "deagle") || strstr(name, "revolver")) return 230.0f;
    if (strstr(name, "ssg08")) return 230.0f;
    if (strstr(name, "g3sg1") || strstr(name, "scar20")) return 215.0f;
    if (strstr(name, "galil")) return 215.0f;
    if (strstr(name, "famas")) return 220.0f;
    if (strstr(name, "aug")) return 220.0f;
    if (strstr(name, "sg556")) return 210.0f;
    if (strstr(name, "glock") || strstr(name, "hkp2000") || strstr(name, "usp") ||
        strstr(name, "p250") || strstr(name, "fiveseven") || strstr(name, "tec9") ||
        strstr(name, "cz75") || strstr(name, "elite")) return 240.0f;
    if (strstr(name, "mp9") || strstr(name, "mac10") || strstr(name, "bizon"))
        return 240.0f;
    if (strstr(name, "ump45") || strstr(name, "p90")) return 230.0f;
    if (strstr(name, "mp7") || strstr(name, "mp5")) return 220.0f;
    if (strstr(name, "negev")) return 150.0f;
    if (strstr(name, "m249")) return 195.0f;
    if (strstr(name, "nova") || strstr(name, "mag7") || strstr(name, "sawedoff"))
        return 220.0f;
    if (strstr(name, "xm1014")) return 215.0f;
    if (strstr(name, "c4") || strstr(name, "flashbang") || strstr(name, "hegrenade") ||
        strstr(name, "smokegrenade") || strstr(name, "molotov") || strstr(name, "incgrenade") ||
        strstr(name, "decoy")) return 245.0f;
    return 225.0f;
}

/* ================================================================
 * CONFIG
 * ================================================================ */
typedef struct {
    float ap;
    float rt;
} WeaponProfile;

typedef struct {
    /* Base settings (used when GSI not connected) */
    float ap_normal;
    float ap_aggro;
    float rt_normal;
    float rt_aggro;
    float write_interval_ms;
    float predict_threshold;
    float predict_min_peak;
    float crouch_rt_factor;
    int   ws_adaptive;
    int   stats_enabled;

    /* Weapon profiles (override ap_aggro/rt_aggro when GSI active) */
    WeaponProfile weapon[WCAT_COUNT];

    /* GSI */
    int gsi_enabled;
    int gsi_port;

    /* Velocity estimation */
    int vel_enabled;

    /* v0.7 features */
    int   jiggle_enabled;    /* jiggle peek detection */
    int   vel_scale_enabled; /* velocity-aware AP scaling */
    int   phase_decay;       /* counter-strafe phase decay */
    float poll_rate_hz;      /* target poll rate (0=unlimited) */
} Config;

static Config g_cfg = {
    .ap_normal         = 1.2f,
    .ap_aggro          = 0.4f,   /* Changed from 0.1 based on research */
    .rt_normal         = 1.0f,
    .rt_aggro          = 0.1f,
    .write_interval_ms = 50.0f,
    .predict_threshold = 0.70f,
    .predict_min_peak  = 0.30f,
    .crouch_rt_factor  = 0.5f,
    .ws_adaptive       = 0,
    .stats_enabled     = 1,

    .weapon = {
        [WCAT_RIFLE]  = { 0.4f, 0.1f },
        [WCAT_AWP]    = { 0.8f, 0.4f },
        [WCAT_PISTOL] = { 0.3f, 0.1f },
        [WCAT_SMG]    = { 0.5f, 0.2f },
        [WCAT_KNIFE]  = { 1.5f, 1.0f },
        [WCAT_OTHER]  = { 1.0f, 0.5f },
    },

    .gsi_enabled = 1,
    .gsi_port    = GSI_PORT,
    .vel_enabled = 1,

    .jiggle_enabled    = 1,
    .vel_scale_enabled = 1,
    .phase_decay       = 1,
    .poll_rate_hz      = 8000.0f,  /* 8kHz matches keyboard polling rate */
};

static void config_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "# wooting-aim v0.7 configuration\n\n");
            fprintf(f, "# Base settings (used when GSI not connected)\n");
            fprintf(f, "ap_normal=%.1f\n", g_cfg.ap_normal);
            fprintf(f, "ap_aggro=%.1f\n", g_cfg.ap_aggro);
            fprintf(f, "rt_normal=%.1f\n", g_cfg.rt_normal);
            fprintf(f, "rt_aggro=%.1f\n", g_cfg.rt_aggro);
            fprintf(f, "write_interval_ms=%.0f\n", g_cfg.write_interval_ms);
            fprintf(f, "predict_threshold=%.2f\n", g_cfg.predict_threshold);
            fprintf(f, "predict_min_peak=%.2f\n", g_cfg.predict_min_peak);
            fprintf(f, "crouch_rt_factor=%.2f\n", g_cfg.crouch_rt_factor);
            fprintf(f, "ws_adaptive=%d\n", g_cfg.ws_adaptive);
            fprintf(f, "stats_enabled=%d\n\n", g_cfg.stats_enabled);
            fprintf(f, "# Weapon profiles (AP/RT when counter-strafing, GSI active)\n");
            fprintf(f, "rifle_ap=%.1f\nrifle_rt=%.1f\n", g_cfg.weapon[WCAT_RIFLE].ap, g_cfg.weapon[WCAT_RIFLE].rt);
            fprintf(f, "awp_ap=%.1f\nawp_rt=%.1f\n", g_cfg.weapon[WCAT_AWP].ap, g_cfg.weapon[WCAT_AWP].rt);
            fprintf(f, "pistol_ap=%.1f\npistol_rt=%.1f\n", g_cfg.weapon[WCAT_PISTOL].ap, g_cfg.weapon[WCAT_PISTOL].rt);
            fprintf(f, "smg_ap=%.1f\nsmg_rt=%.1f\n", g_cfg.weapon[WCAT_SMG].ap, g_cfg.weapon[WCAT_SMG].rt);
            fprintf(f, "knife_ap=%.1f\nknife_rt=%.1f\n\n", g_cfg.weapon[WCAT_KNIFE].ap, g_cfg.weapon[WCAT_KNIFE].rt);
            fprintf(f, "# GSI settings\n");
            fprintf(f, "gsi_enabled=%d\n", g_cfg.gsi_enabled);
            fprintf(f, "gsi_port=%d\n\n", g_cfg.gsi_port);
            fprintf(f, "# Velocity estimation\n");
            fprintf(f, "vel_enabled=%d\n\n", g_cfg.vel_enabled);
            fprintf(f, "# v0.7 features\n");
            fprintf(f, "jiggle_enabled=%d\n", g_cfg.jiggle_enabled);
            fprintf(f, "vel_scale_enabled=%d\n", g_cfg.vel_scale_enabled);
            fprintf(f, "phase_decay=%d\n", g_cfg.phase_decay);
            fprintf(f, "poll_rate_hz=%.0f\n", g_cfg.poll_rate_hz);
            fclose(f);
            printf("[CFG] Default config created: %s\n", path);
        }
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char key[64];
        float val;
        if (sscanf(line, "%63[^=]=%f", key, &val) == 2) {
            if      (strcmp(key, "ap_normal") == 0)         g_cfg.ap_normal = val;
            else if (strcmp(key, "ap_aggro") == 0)          g_cfg.ap_aggro = val;
            else if (strcmp(key, "rt_normal") == 0)         g_cfg.rt_normal = val;
            else if (strcmp(key, "rt_aggro") == 0)          g_cfg.rt_aggro = val;
            else if (strcmp(key, "write_interval_ms") == 0) g_cfg.write_interval_ms = val;
            else if (strcmp(key, "predict_threshold") == 0) g_cfg.predict_threshold = val;
            else if (strcmp(key, "predict_min_peak") == 0)  g_cfg.predict_min_peak = val;
            else if (strcmp(key, "crouch_rt_factor") == 0)  g_cfg.crouch_rt_factor = val;
            else if (strcmp(key, "ws_adaptive") == 0)       g_cfg.ws_adaptive = (int)val;
            else if (strcmp(key, "stats_enabled") == 0)     g_cfg.stats_enabled = (int)val;
            else if (strcmp(key, "rifle_ap") == 0)          g_cfg.weapon[WCAT_RIFLE].ap = val;
            else if (strcmp(key, "rifle_rt") == 0)          g_cfg.weapon[WCAT_RIFLE].rt = val;
            else if (strcmp(key, "awp_ap") == 0)            g_cfg.weapon[WCAT_AWP].ap = val;
            else if (strcmp(key, "awp_rt") == 0)            g_cfg.weapon[WCAT_AWP].rt = val;
            else if (strcmp(key, "pistol_ap") == 0)         g_cfg.weapon[WCAT_PISTOL].ap = val;
            else if (strcmp(key, "pistol_rt") == 0)         g_cfg.weapon[WCAT_PISTOL].rt = val;
            else if (strcmp(key, "smg_ap") == 0)            g_cfg.weapon[WCAT_SMG].ap = val;
            else if (strcmp(key, "smg_rt") == 0)            g_cfg.weapon[WCAT_SMG].rt = val;
            else if (strcmp(key, "knife_ap") == 0)          g_cfg.weapon[WCAT_KNIFE].ap = val;
            else if (strcmp(key, "knife_rt") == 0)          g_cfg.weapon[WCAT_KNIFE].rt = val;
            else if (strcmp(key, "gsi_enabled") == 0)       g_cfg.gsi_enabled = (int)val;
            else if (strcmp(key, "gsi_port") == 0)          g_cfg.gsi_port = (int)val;
            else if (strcmp(key, "vel_enabled") == 0)       g_cfg.vel_enabled = (int)val;
            else if (strcmp(key, "jiggle_enabled") == 0)    g_cfg.jiggle_enabled = (int)val;
            else if (strcmp(key, "vel_scale_enabled") == 0) g_cfg.vel_scale_enabled = (int)val;
            else if (strcmp(key, "phase_decay") == 0)       g_cfg.phase_decay = (int)val;
            else if (strcmp(key, "poll_rate_hz") == 0)      g_cfg.poll_rate_hz = val;
        }
    }
    fclose(f);
    printf("[CFG] Loaded: %s\n", path);
}

/* ================================================================
 * GSI - GAME STATE INTEGRATION
 * ================================================================ */
typedef struct {
    char weapon_name[64];
    char weapon_type[32];
    WeaponCategory weapon_cat;
    float weapon_speed;
    char round_phase[16];  /* "live", "freezetime", "over" */
    int health;
    bool connected;
    LARGE_INTEGER last_update;
    CRITICAL_SECTION lock;
} GSIState;

static GSIState g_gsi = {0};

/* Extract a JSON string value near a specific position.
 * Searches for "key" and extracts the quoted value after it. */
static bool json_extract_str(const char *json, const char *start, const char *end,
                              const char *key, char *buf, int buf_size) {
    buf[0] = '\0';
    const char *k = start;
    while ((k = strstr(k, key)) != NULL && k < end) {
        k += strlen(key);
        /* skip whitespace, colon, whitespace */
        while (k < end && (*k == ' ' || *k == '\t' || *k == ':')) k++;
        if (k < end && *k == '"') {
            k++;
            int i = 0;
            while (k < end && *k != '"' && i < buf_size - 1) {
                buf[i++] = *k++;
            }
            buf[i] = '\0';
            return true;
        }
        break;
    }
    return false;
}

static int json_extract_int(const char *json, const char *start, const char *end,
                             const char *key) {
    const char *k = strstr(start, key);
    if (!k || k >= end) return -1;
    k += strlen(key);
    while (k < end && (*k == ' ' || *k == '\t' || *k == ':')) k++;
    return atoi(k);
}

static void parse_gsi_json(const char *json, int len) {
    char weapon_name[64] = {0};
    char weapon_type[32] = {0};
    char round_phase[16] = {0};
    int health = -1;

    /* Find round phase */
    const char *round_section = strstr(json, "\"round\"");
    if (round_section) {
        const char *phase_end = round_section + 200;
        if (phase_end > json + len) phase_end = json + len;
        json_extract_str(json, round_section, phase_end, "\"phase\"", round_phase, sizeof(round_phase));
    }

    /* Find player health */
    const char *state_section = strstr(json, "\"state\"");
    if (state_section) {
        /* Only look at player.state, not weapon state */
        const char *health_area = state_section + 200;
        if (health_area > json + len) health_area = json + len;
        health = json_extract_int(json, state_section, health_area, "\"health\"");
    }

    /* Find active weapon: search for "state": "active" in weapons section */
    const char *weapons = strstr(json, "\"weapons\"");
    if (weapons) {
        const char *active = weapons;
        while ((active = strstr(active, "\"active\"")) != NULL) {
            /* Check that this is preceded by "state" */
            const char *check = (active > weapons + 30) ? active - 30 : weapons;
            if (!strstr(check, "\"state\"")) {
                active++;
                continue;
            }

            /* Find the enclosing {} block */
            const char *block_start = active;
            int brace = 0;
            while (block_start > weapons) {
                block_start--;
                if (*block_start == '}') brace++;
                if (*block_start == '{') {
                    if (brace == 0) break;
                    brace--;
                }
            }

            /* Find block end */
            const char *block_end = active + 200;
            if (block_end > json + len) block_end = json + len;

            json_extract_str(json, block_start, block_end, "\"name\"", weapon_name, sizeof(weapon_name));
            json_extract_str(json, block_start, block_end, "\"type\"", weapon_type, sizeof(weapon_type));
            break;
        }
    }

    /* Update shared state */
    EnterCriticalSection(&g_gsi.lock);
    if (weapon_name[0]) {
        strncpy(g_gsi.weapon_name, weapon_name, sizeof(g_gsi.weapon_name) - 1);
        strncpy(g_gsi.weapon_type, weapon_type, sizeof(g_gsi.weapon_type) - 1);
        g_gsi.weapon_cat = categorize_weapon_type(weapon_type);
        g_gsi.weapon_speed = weapon_max_speed(weapon_name);
    }
    if (round_phase[0])
        strncpy(g_gsi.round_phase, round_phase, sizeof(g_gsi.round_phase) - 1);
    if (health >= 0) g_gsi.health = health;
    g_gsi.connected = true;
    QueryPerformanceCounter(&g_gsi.last_update);
    LeaveCriticalSection(&g_gsi.lock);
}

/* GSI HTTP server thread */
static volatile bool g_gsi_running = true;

static DWORD WINAPI gsi_thread(LPVOID param) {
    (void)param;

    SOCKET server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock == INVALID_SOCKET) {
        printf("[GSI] Socket creation failed: %d\n", WSAGetLastError());
        return 1;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons((u_short)g_cfg.gsi_port);

    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("[GSI] Bind failed on port %d: %d\n", g_cfg.gsi_port, WSAGetLastError());
        closesocket(server_sock);
        return 1;
    }

    if (listen(server_sock, 5) == SOCKET_ERROR) {
        printf("[GSI] Listen failed: %d\n", WSAGetLastError());
        closesocket(server_sock);
        return 1;
    }

    printf("[GSI] Server listening on 127.0.0.1:%d\n", g_cfg.gsi_port);

    /* Set non-blocking for graceful shutdown */
    u_long nonblock = 1;
    ioctlsocket(server_sock, FIONBIO, &nonblock);

    while (g_gsi_running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_sock, &readfds);
        struct timeval tv = {0, 500000}; /* 500ms timeout */

        int sel = select(0, &readfds, NULL, NULL, &tv);
        if (sel <= 0) continue;

        SOCKET client = accept(server_sock, NULL, NULL);
        if (client == INVALID_SOCKET) continue;

        /* Read HTTP request */
        char buf[GSI_BUF_SIZE];
        int total = 0;
        int content_length = 0;
        char *body = NULL;

        /* Set client to blocking with timeout */
        u_long blocking = 0;
        ioctlsocket(client, FIONBIO, &blocking);
        int timeout_ms = 2000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));

        /* Read headers + body */
        while (total < GSI_BUF_SIZE - 1) {
            int n = recv(client, buf + total, GSI_BUF_SIZE - 1 - total, 0);
            if (n <= 0) break;
            total += n;
            buf[total] = '\0';

            /* Check if we have full headers */
            if (!body) {
                body = strstr(buf, "\r\n\r\n");
                if (body) {
                    body += 4;
                    /* Extract Content-Length */
                    const char *cl = strstr(buf, "Content-Length:");
                    if (!cl) cl = strstr(buf, "content-length:");
                    if (cl) {
                        cl += 15;
                        while (*cl == ' ') cl++;
                        content_length = atoi(cl);
                    }
                }
            }

            /* Check if we have the full body */
            if (body && content_length > 0) {
                int body_received = total - (int)(body - buf);
                if (body_received >= content_length) break;
            }
        }

        /* Send 200 OK */
        const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
        send(client, resp, (int)strlen(resp), 0);
        closesocket(client);

        /* Parse the body */
        if (body && content_length > 0) {
            parse_gsi_json(body, content_length);
        }
    }

    closesocket(server_sock);
    return 0;
}

/* Create GSI config file in CS2's cfg directory */
static void create_gsi_config(void) {
    /* Try to find Steam path from registry */
    char steam_path[MAX_PATH] = {0};
    HKEY key;
    bool found_steam = false;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0,
                      KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD size = sizeof(steam_path);
        DWORD type;
        if (RegQueryValueExA(key, "SteamPath", NULL, &type,
                             (LPBYTE)steam_path, &size) == ERROR_SUCCESS) {
            found_steam = true;
            for (int i = 0; steam_path[i]; i++)
                if (steam_path[i] == '/') steam_path[i] = '\\';
        }
        RegCloseKey(key);
    }

    /* Build cfg path */
    const char *suffix = "\\steamapps\\common\\Counter-Strike Global Offensive\\game\\csgo\\cfg";
    char cfg_dir[MAX_PATH];

    /* Try Steam path first, then common locations */
    const char *try_bases[4];
    int try_count = 0;
    if (found_steam) try_bases[try_count++] = steam_path;
    try_bases[try_count++] = "C:\\Program Files (x86)\\Steam";
    try_bases[try_count++] = "D:\\Steam";
    try_bases[try_count++] = "D:\\SteamLibrary";

    for (int i = 0; i < try_count; i++) {
        snprintf(cfg_dir, sizeof(cfg_dir), "%s%s", try_bases[i], suffix);
        if (GetFileAttributesA(cfg_dir) != INVALID_FILE_ATTRIBUTES) {
            char filepath[MAX_PATH];
            snprintf(filepath, sizeof(filepath), "%s\\gamestate_integration_wooting_aim.cfg", cfg_dir);

            if (GetFileAttributesA(filepath) != INVALID_FILE_ATTRIBUTES) {
                printf("[GSI] Config exists: %s\n", filepath);
                return;
            }

            FILE *f = fopen(filepath, "w");
            if (f) {
                fprintf(f, "\"wooting-aim\"\n{\n");
                fprintf(f, "    \"uri\" \"http://127.0.0.1:%d\"\n", g_cfg.gsi_port);
                fprintf(f, "    \"timeout\" \"2.0\"\n");
                fprintf(f, "    \"buffer\" \"0.0\"\n");
                fprintf(f, "    \"throttle\" \"0.0\"\n");
                fprintf(f, "    \"heartbeat\" \"10.0\"\n");
                fprintf(f, "    \"data\"\n    {\n");
                fprintf(f, "        \"provider\" \"1\"\n");
                fprintf(f, "        \"player_id\" \"1\"\n");
                fprintf(f, "        \"player_state\" \"1\"\n");
                fprintf(f, "        \"player_weapons\" \"1\"\n");
                fprintf(f, "        \"round\" \"1\"\n");
                fprintf(f, "    }\n}\n");
                fclose(f);
                printf("[GSI] Config created: %s\n", filepath);
                return;
            }
        }
    }

    printf("[GSI] CS2 cfg directory not found.\n");
    printf("[GSI] Create gamestate_integration_wooting_aim.cfg manually in:\n");
    printf("[GSI]   <Steam>/steamapps/common/Counter-Strike Global Offensive/game/csgo/cfg/\n");
    printf("[GSI] Content:\n");
    printf("[GSI]   \"wooting-aim\" { \"uri\" \"http://127.0.0.1:%d\" ... }\n", g_cfg.gsi_port);
}

/* ================================================================
 * TIMER RESOLUTION (Windows NT)
 * ================================================================ */
typedef LONG (NTAPI *NtSetTimerResolution_t)(ULONG, BOOLEAN, PULONG);
static NtSetTimerResolution_t g_NtSetTimerResolution = NULL;

static void set_timer_resolution(void) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return;
    g_NtSetTimerResolution = (NtSetTimerResolution_t)
        GetProcAddress(ntdll, "NtSetTimerResolution");
    if (!g_NtSetTimerResolution) return;

    ULONG current = 0;
    g_NtSetTimerResolution(5000, TRUE, &current); /* 5000 * 100ns = 0.5ms */
    printf("[SYS] Timer resolution: 0.5ms (was %.1fms)\n", current / 10000.0);
}

static void restore_timer_resolution(void) {
    if (g_NtSetTimerResolution) {
        ULONG current;
        g_NtSetTimerResolution(5000, FALSE, &current);
    }
}

/* ================================================================
 * VELOCITY ESTIMATION (CS2 friction model)
 * ================================================================ */
#define SV_FRICTION    5.2f
#define SV_ACCELERATE  5.5f
#define SV_STOPSPEED   80.0f

typedef struct {
    float vel;       /* estimated velocity (units/s) */
    float max_speed; /* current weapon max speed */
    LARGE_INTEGER last_update;
} VelEstimator;

/*
 * Binary velocity update - CS2 treats keyboard input as ON/OFF.
 * Analog depth does NOT affect movement speed in CS2.
 * Uses exact Source 2 friction model: geometric decay above sv_stopspeed,
 * linear decay below. Counter-strafe adds ~18.48 u/s/tick toward opposite.
 *
 * Research source: Quake III bg_pmove.c lineage, confirmed for CS2.
 * Per-tick decay factor (64 tick): 1 - 5.2 * 0.015625 = 0.91875
 * Per-tick fixed decel (below stopspeed): 80 * 5.2 * 0.015625 = 6.5 u/s
 */
static void vel_update(VelEstimator *ve, float pos_analog, float neg_analog,
                        float max_speed, LARGE_INTEGER now, double freq) {
    ve->max_speed = max_speed;

    double elapsed = (double)(now.QuadPart - ve->last_update.QuadPart) / freq;
    if (elapsed <= 0 || elapsed > 0.1) {
        ve->last_update = now;
        return;
    }
    ve->last_update = now;
    float dt = (float)elapsed;

    /* CS2 input is binary: key actuated = full speed command */
    bool pos_key = pos_analog > DEAD_ZONE;
    bool neg_key = neg_analog > DEAD_ZONE;

    /* Apply friction (Source 2 model) */
    float speed = fabsf(ve->vel);
    if (speed > 0.001f) {
        float control = (speed < SV_STOPSPEED) ? SV_STOPSPEED : speed;
        float drop = control * SV_FRICTION * dt;
        float new_speed = speed - drop;
        if (new_speed < 0.0f) new_speed = 0.0f;
        ve->vel *= (new_speed / speed);
    }

    /* Apply acceleration - binary (full speed or nothing) */
    float wish = 0.0f;
    if (pos_key && !neg_key) wish = 1.0f;
    else if (neg_key && !pos_key) wish = -1.0f;

    if (wish != 0.0f) {
        float current_in_wish = ve->vel * wish;
        float add_speed = max_speed - current_in_wish;
        if (add_speed > 0.0f) {
            float accel_speed = SV_ACCELERATE * dt * max_speed;
            if (accel_speed > add_speed) accel_speed = add_speed;
            ve->vel += accel_speed * wish;
        }
    }

    /* Clamp */
    if (fabsf(ve->vel) > max_speed)
        ve->vel = (ve->vel > 0) ? max_speed : -max_speed;
    if (fabsf(ve->vel) < 0.5f)
        ve->vel = 0.0f;
}

/* ================================================================
 * GLOBAL CLEANUP
 * ================================================================ */
/* Forward declarations for cleanup */
typedef struct { FILE *file; } Stats;
static void stats_close(Stats *st);

static volatile bool g_running = true;
static WootingHID *g_hid = NULL;
static bool g_adaptive = false;
static HANDLE g_gsi_thread = NULL;
static Stats *g_stats = NULL;  /* for cleanup on Ctrl+C */

static void restore_and_cleanup(void) {
    if (g_hid && g_adaptive) {
        printf("\n\nRestoring keyboard to normal settings...\n");
        KeySetting ap[] = {
            { KEY_W_ROW, KEY_W_COL, g_cfg.ap_normal },
            { KEY_A_ROW, KEY_A_COL, g_cfg.ap_normal },
            { KEY_S_ROW, KEY_S_COL, g_cfg.ap_normal },
            { KEY_D_ROW, KEY_D_COL, g_cfg.ap_normal },
        };
        KeySetting rt[] = {
            { KEY_W_ROW, KEY_W_COL, g_cfg.rt_normal },
            { KEY_A_ROW, KEY_A_COL, g_cfg.rt_normal },
            { KEY_S_ROW, KEY_S_COL, g_cfg.rt_normal },
            { KEY_D_ROW, KEY_D_COL, g_cfg.rt_normal },
        };
        wooting_hid_write_actuation(g_hid, PROFILE_IDX, ap, 4, false);
        wooting_hid_write_rt(g_hid, PROFILE_IDX, rt, 4, false);
        printf("Settings restored.\n");
    }

    /* Stop GSI server */
    g_gsi_running = false;
    if (g_gsi_thread) {
        WaitForSingleObject(g_gsi_thread, 3000);
        CloseHandle(g_gsi_thread);
    }

    /* Cleanup winsock */
    WSACleanup();

    /* Flush and close stats file */
    if (g_stats) stats_close(g_stats);

    /* Restore timer */
    restore_timer_resolution();

    if (g_hid) wooting_hid_close(g_hid);
    wooting_analog_uninitialise();
}

static BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_CLOSE_EVENT || event == CTRL_C_EVENT ||
        event == CTRL_BREAK_EVENT || event == CTRL_LOGOFF_EVENT ||
        event == CTRL_SHUTDOWN_EVENT) {
        g_running = false;
        restore_and_cleanup();
        return TRUE;
    }
    return FALSE;
}

/* ================================================================
 * AXIS STATE MACHINE (used for both H and V axes)
 * ================================================================ */
typedef enum {
    S_IDLE,
    S_STRAFE_POS,
    S_STRAFE_NEG,
    S_COUNTER_POS,
    S_COUNTER_NEG,
} AxisState;

static const char *axis_names[] = { "I", "S+", "S-", "C+", "C-" };

typedef struct {
    AxisState state, prev;
    float pos_peak, neg_peak;
    bool predictive;
    LARGE_INTEGER counter_start;
    double counter_ms;
    unsigned long long counter_count;
    double counter_total_ms;

    /* Jiggle peek detection */
    LARGE_INTEGER jiggle_times[4]; /* timestamps of recent counter-strafes */
    int jiggle_idx;
    bool is_jiggle;                /* true when jiggle pattern detected */
    LARGE_INTEGER jiggle_last;     /* timestamp of last jiggle detection */
} Axis;

static void axis_update(Axis *ax, float pos, float neg,
                         float prev_pos, float prev_neg, double freq) {
    ax->prev = ax->state;
    ax->predictive = false;

    bool pp = pos > DEAD_ZONE, np = neg > DEAD_ZONE;
    bool pr = pos > DEAD_ZONE && prev_pos <= DEAD_ZONE;
    bool nr = neg > DEAD_ZONE && prev_neg <= DEAD_ZONE;

    switch (ax->state) {
    case S_IDLE:
        if (pp && !np) { ax->state = S_STRAFE_POS; ax->pos_peak = pos; ax->neg_peak = 0; }
        if (np && !pp) { ax->state = S_STRAFE_NEG; ax->neg_peak = neg; ax->pos_peak = 0; }
        break;

    case S_STRAFE_POS:
        if (!pp && !np) { ax->state = S_IDLE; break; }
        if (pos > ax->pos_peak) ax->pos_peak = pos;
        if (ax->pos_peak > g_cfg.predict_min_peak &&
            pos < ax->pos_peak * g_cfg.predict_threshold)
            ax->predictive = true;
        if (nr) { ax->state = S_COUNTER_NEG; QueryPerformanceCounter(&ax->counter_start); }
        break;

    case S_STRAFE_NEG:
        if (!pp && !np) { ax->state = S_IDLE; break; }
        if (neg > ax->neg_peak) ax->neg_peak = neg;
        if (ax->neg_peak > g_cfg.predict_min_peak &&
            neg < ax->neg_peak * g_cfg.predict_threshold)
            ax->predictive = true;
        if (pr) { ax->state = S_COUNTER_POS; QueryPerformanceCounter(&ax->counter_start); }
        break;

    case S_COUNTER_POS:
    case S_COUNTER_NEG: {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        ax->counter_ms = (double)(now.QuadPart - ax->counter_start.QuadPart) * 1000.0 / freq;
        if (!pp && !np) ax->state = S_IDLE;
        else if (pp && !np) { ax->state = S_STRAFE_POS; ax->pos_peak = pos; }
        else if (np && !pp) { ax->state = S_STRAFE_NEG; ax->neg_peak = neg; }
        break;
    }
    }

    if (ax->state != ax->prev &&
        (ax->prev == S_COUNTER_POS || ax->prev == S_COUNTER_NEG)) {
        ax->counter_count++;
        ax->counter_total_ms += ax->counter_ms;
    }

    /* Jiggle peek: record counter-strafe entry timestamps */
    if (ax->state != ax->prev &&
        (ax->state == S_COUNTER_POS || ax->state == S_COUNTER_NEG)) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        ax->jiggle_times[ax->jiggle_idx & 3] = now;
        ax->jiggle_idx = (ax->jiggle_idx + 1) & 0x7FFFFFFF;

        /* Check if enough recent counter-strafes within the window */
        int recent = 0;
        for (int i = 0; i < 4; i++) {
            if (ax->jiggle_times[i].QuadPart == 0) continue;
            double age = (double)(now.QuadPart - ax->jiggle_times[i].QuadPart) * 1000.0 / freq;
            if (age < JIGGLE_WINDOW_MS) recent++;
        }
        if (recent >= JIGGLE_MIN_COUNT) {
            ax->is_jiggle = true;
            ax->jiggle_last = now;
        }
    }

    /* Expire jiggle mode */
    if (ax->is_jiggle) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double since_last = (double)(now.QuadPart - ax->jiggle_last.QuadPart) * 1000.0 / freq;
        if (since_last > JIGGLE_PREARM_MS) ax->is_jiggle = false;
    }
}

/* ================================================================
 * STATISTICS
 * ================================================================ */
static void stats_init(Stats *st, const char *path) {
    st->file = fopen(path, "a");
    if (st->file) {
        fseek(st->file, 0, SEEK_END);
        if (ftell(st->file) == 0)
            fprintf(st->file, "timestamp,axis,direction,counter_strafe_ms,weapon\n");
        printf("[STATS] Logging to: %s\n", path);
    }
}

static void stats_log(Stats *st, const char *axis, const char *dir, double ms, const char *weapon) {
    if (!st->file) return;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(st->file, "%04d-%02d-%02d %02d:%02d:%02d,%s,%s,%.2f,%s\n",
            t->tm_year+1900, t->tm_mon+1, t->tm_mday,
            t->tm_hour, t->tm_min, t->tm_sec, axis, dir, ms, weapon);
    fflush(st->file);
}

static void stats_close(Stats *st) {
    if (st->file) fclose(st->file);
    st->file = NULL;
}

/* ================================================================
 * PROCESS DETECTION (for --watch mode)
 * ================================================================ */
static bool is_process_running(const char *name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    bool found = false;

    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) { found = true; break; }
        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);
    return found;
}

/* ================================================================
 * MAIN CONTEXT + ADAPTIVE LOGIC
 * ================================================================ */
typedef struct {
    float w, a, s, d, ctrl;
    float prev_w, prev_a, prev_s, prev_d;

    Axis h;   /* horizontal: A(neg) / D(pos) */
    Axis v;   /* vertical:   S(neg) / W(pos) */
    bool crouching;

    float target_ap[4];
    float target_rt[4];
    float current_ap[4];
    float current_rt[4];

    bool needs_write;
    LARGE_INTEGER last_write_time;
    unsigned long long write_count;
    unsigned long long frame;

    /* GSI state snapshot (local copy) */
    WeaponCategory weapon_cat;
    char weapon_name[64];
    char round_phase[16];
    float weapon_speed;
    bool gsi_active;

    /* Velocity estimation */
    VelEstimator vel_h;
    VelEstimator vel_v;

    Stats stats;
} AimContext;

/*
 * Get the base AP/RT for aggressive mode, considering GSI weapon.
 */
static void get_base_aggro(AimContext *ctx, float *ap, float *rt) {
    if (ctx->gsi_active && ctx->weapon_cat < WCAT_COUNT) {
        *ap = g_cfg.weapon[ctx->weapon_cat].ap;
        *rt = g_cfg.weapon[ctx->weapon_cat].rt;
    } else {
        *ap = g_cfg.ap_aggro;
        *rt = g_cfg.rt_aggro;
    }
}

/*
 * Velocity-aware AP scaling.
 * When moving fast (above 50% of accuracy threshold), lower AP further
 * for faster counter-strafe response.
 */
static float vel_scale_ap(float base_ap, float vel_ratio) {
    /* vel_ratio = |velocity| / (max_speed * 0.34) clamped to 0-1 */
    if (vel_ratio < VEL_AGGRO_ZONE) return base_ap;
    /* Linear scale: at vel_ratio=1.0, AP = base_ap * VEL_MIN_AP_FACTOR */
    float t = (vel_ratio - VEL_AGGRO_ZONE) / (1.0f - VEL_AGGRO_ZONE);
    float factor = 1.0f - t * (1.0f - VEL_MIN_AP_FACTOR);
    float result = base_ap * factor;
    if (result < 0.15f) result = 0.15f; /* prevent ghost inputs from stem wobble */
    return result;
}

/*
 * Counter-strafe phase decay.
 * In the first PHASE_ULTRA_MS: use minimum AP (0.1mm)
 * Then linearly relax back to base_ap over PHASE_DECAY_MS.
 */
static float phase_decay_ap(float base_ap, double counter_ms) {
    /* Min AP = 0.15mm to prevent ghost inputs from lateral stem wobble.
     * Research: sub-0.15mm AP causes phantom triggers from 0.5mm wobble. */
    const float min_ap = 0.15f;
    if (counter_ms < PHASE_ULTRA_MS) return min_ap;
    if (counter_ms > PHASE_DECAY_MS) return base_ap;
    float t = (float)(counter_ms - PHASE_ULTRA_MS) / (float)(PHASE_DECAY_MS - PHASE_ULTRA_MS);
    return min_ap + t * (base_ap - min_ap);
}

/*
 * Combine both axes + crouch + weapon into per-key targets.
 */
static void update_targets(AimContext *ctx) {
    /* Read GSI state (thread-safe) */
    EnterCriticalSection(&g_gsi.lock);
    ctx->weapon_cat   = g_gsi.weapon_cat;
    strncpy(ctx->weapon_name, g_gsi.weapon_name, sizeof(ctx->weapon_name) - 1);
    strncpy(ctx->round_phase, g_gsi.round_phase, sizeof(ctx->round_phase) - 1);
    ctx->weapon_speed = g_gsi.weapon_speed;
    ctx->gsi_active   = g_gsi.connected;
    LeaveCriticalSection(&g_gsi.lock);

    /* During freezetime or when dead: relax to normal */
    bool freezetime = ctx->gsi_active &&
        (strcmp(ctx->round_phase, "freezetime") == 0 ||
         strcmp(ctx->round_phase, "over") == 0);

    /* If weapon is grenade/C4/other and GSI active, relax */
    bool non_combat = ctx->gsi_active && ctx->weapon_cat == WCAT_OTHER;

    float ap[4], rt[4];
    for (int i = 0; i < 4; i++) {
        ap[i] = g_cfg.ap_normal;
        rt[i] = g_cfg.rt_normal;
    }

    if (freezetime || non_combat) {
        /* Keep normal settings */
        goto check_changed;
    }

    float base_ap, base_rt;
    get_base_aggro(ctx, &base_ap, &base_rt);

    /* Velocity-aware AP scaling */
    float vel_ap = base_ap;
    if (g_cfg.vel_scale_enabled && g_cfg.vel_enabled) {
        float total_vel = sqrtf(ctx->vel_h.vel * ctx->vel_h.vel +
                                ctx->vel_v.vel * ctx->vel_v.vel);
        float max_spd = ctx->weapon_speed > 0 ? ctx->weapon_speed : 225.0f;
        float threshold = max_spd * 0.34f;
        float vel_ratio = (threshold > 0) ? total_vel / threshold : 0.0f;
        if (vel_ratio > 1.0f) vel_ratio = 1.0f;
        vel_ap = vel_scale_ap(base_ap, vel_ratio);
    }

    /* Horizontal: A=neg(K_A), D=pos(K_D) */
    switch (ctx->h.state) {
    case S_IDLE:
        /* Jiggle mode: pre-arm both directions */
        if (g_cfg.jiggle_enabled && ctx->h.is_jiggle) {
            ap[K_A] = vel_ap; rt[K_A] = base_rt;
            ap[K_D] = vel_ap; rt[K_D] = base_rt;
        }
        break;
    case S_STRAFE_POS: /* D held */
        rt[K_D] = base_rt;
        ap[K_A] = vel_ap;
        if (ctx->h.predictive || (g_cfg.jiggle_enabled && ctx->h.is_jiggle))
            rt[K_A] = base_rt;
        break;
    case S_STRAFE_NEG: /* A held */
        rt[K_A] = base_rt;
        ap[K_D] = vel_ap;
        if (ctx->h.predictive || (g_cfg.jiggle_enabled && ctx->h.is_jiggle))
            rt[K_D] = base_rt;
        break;
    case S_COUNTER_POS: { /* pressing D to counter */
        float c_ap = vel_ap;
        if (g_cfg.phase_decay) c_ap = phase_decay_ap(vel_ap, ctx->h.counter_ms);
        ap[K_D] = c_ap; rt[K_D] = base_rt;
        rt[K_A] = base_rt;
        break;
    }
    case S_COUNTER_NEG: { /* pressing A to counter */
        float c_ap = vel_ap;
        if (g_cfg.phase_decay) c_ap = phase_decay_ap(vel_ap, ctx->h.counter_ms);
        ap[K_A] = c_ap; rt[K_A] = base_rt;
        rt[K_D] = base_rt;
        break;
    }
    }

    /* Vertical: S=neg(K_S), W=pos(K_W) - only if ws_adaptive enabled */
    if (g_cfg.ws_adaptive) {
        switch (ctx->v.state) {
        case S_IDLE:
            if (g_cfg.jiggle_enabled && ctx->v.is_jiggle) {
                ap[K_W] = vel_ap; rt[K_W] = base_rt;
                ap[K_S] = vel_ap; rt[K_S] = base_rt;
            }
            break;
        case S_STRAFE_POS:
            rt[K_W] = base_rt;
            ap[K_S] = vel_ap;
            if (ctx->v.predictive || (g_cfg.jiggle_enabled && ctx->v.is_jiggle))
                rt[K_S] = base_rt;
            break;
        case S_STRAFE_NEG:
            rt[K_S] = base_rt;
            ap[K_W] = vel_ap;
            if (ctx->v.predictive || (g_cfg.jiggle_enabled && ctx->v.is_jiggle))
                rt[K_W] = base_rt;
            break;
        case S_COUNTER_POS: {
            float c_ap = vel_ap;
            if (g_cfg.phase_decay) c_ap = phase_decay_ap(vel_ap, ctx->v.counter_ms);
            ap[K_W] = c_ap; rt[K_W] = base_rt;
            rt[K_S] = base_rt;
            break;
        }
        case S_COUNTER_NEG: {
            float c_ap = vel_ap;
            if (g_cfg.phase_decay) c_ap = phase_decay_ap(vel_ap, ctx->v.counter_ms);
            ap[K_S] = c_ap; rt[K_S] = base_rt;
            rt[K_W] = base_rt;
            break;
        }
        }
    }

    /* Crouch optimization:
     * Crouching speed = ~34% of running speed (already at accuracy threshold).
     * Tighten RT for snappy response but relax AP since less deceleration needed.
     * Research: crouching = 34% of MaxPlayerSpeed, so you're shootable while moving. */
    if (ctx->crouching) {
        for (int i = 0; i < 4; i++) {
            float crt = rt[i] * g_cfg.crouch_rt_factor;
            if (crt < base_rt) crt = base_rt;
            rt[i] = crt;
            /* Relax AP slightly when crouching - already near accuracy zone */
            if (ap[i] < g_cfg.ap_normal) {
                ap[i] = ap[i] + (g_cfg.ap_normal - ap[i]) * 0.3f;
            }
        }
    }

check_changed:;
    bool changed = false;
    for (int i = 0; i < 4; i++) {
        if (ap[i] != ctx->target_ap[i] || rt[i] != ctx->target_rt[i]) {
            changed = true; break;
        }
    }

    if (changed) {
        memcpy(ctx->target_ap, ap, sizeof(ap));
        memcpy(ctx->target_rt, rt, sizeof(rt));
        ctx->needs_write = true;
    }
}

static void do_write(AimContext *ctx, WootingHID *hid, double freq) {
    if (!ctx->needs_write || !hid) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = (double)(now.QuadPart - ctx->last_write_time.QuadPart) * 1000.0 / freq;
    if (elapsed < g_cfg.write_interval_ms) return;

    KeySetting ap[] = {
        { KEY_W_ROW, KEY_W_COL, ctx->target_ap[K_W] },
        { KEY_A_ROW, KEY_A_COL, ctx->target_ap[K_A] },
        { KEY_S_ROW, KEY_S_COL, ctx->target_ap[K_S] },
        { KEY_D_ROW, KEY_D_COL, ctx->target_ap[K_D] },
    };
    KeySetting rt[] = {
        { KEY_W_ROW, KEY_W_COL, ctx->target_rt[K_W] },
        { KEY_A_ROW, KEY_A_COL, ctx->target_rt[K_A] },
        { KEY_S_ROW, KEY_S_COL, ctx->target_rt[K_S] },
        { KEY_D_ROW, KEY_D_COL, ctx->target_rt[K_D] },
    };

    wooting_hid_write_actuation(hid, PROFILE_IDX, ap, 4, false);
    wooting_hid_write_rt(hid, PROFILE_IDX, rt, 4, false);

    memcpy(ctx->current_ap, ctx->target_ap, sizeof(ctx->target_ap));
    memcpy(ctx->current_rt, ctx->target_rt, sizeof(ctx->target_rt));
    ctx->needs_write = false;
    ctx->last_write_time = now;
    ctx->write_count++;
}

/* ================================================================
 * DISPLAY
 * ================================================================ */
static void print_bar(const char *label, float val) {
    int bars = (int)(val * 20.0f);
    printf(" %s:", label);
    for (int i = 0; i < 20; i++) putchar(i < bars ? '#' : '.');
}

/* ================================================================
 * MAIN
 * ================================================================ */
int main(int argc, char *argv[]) {
    bool adaptive_mode = false;
    bool watch_mode    = false;
    bool demo_mode     = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--adaptive") == 0) adaptive_mode = true;
        else if (strcmp(argv[i], "--watch") == 0) watch_mode = true;
        else if (strcmp(argv[i], "--demo") == 0) demo_mode = true;
    }

    SetConsoleCtrlHandler(console_handler, TRUE);

    printf("=== wooting-aim v0.7 ===\n\n");

    /* Launch options reminder */
    printf("[TIP] CS2 launch options recomandate: -noreflex -high\n");
    printf("[TIP] NVIDIA Control Panel: Low Latency Mode = Ultra, V-Sync = On\n\n");

    /* Timer resolution */
    set_timer_resolution();

    /* Init winsock */
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    /* Load config */
    config_load("wooting-aim.cfg");
    printf("[CFG] AP:%.1f->%.1f  RT:%.1f->%.1f  Predict:%.0f%%  Crouch:x%.1f\n",
           g_cfg.ap_normal, g_cfg.ap_aggro,
           g_cfg.rt_normal, g_cfg.rt_aggro,
           (1.0f - g_cfg.predict_threshold) * 100.0f,
           g_cfg.crouch_rt_factor);
    printf("[CFG] Weapon profiles: RIFLE(%.1f/%.1f) AWP(%.1f/%.1f) PISTOL(%.1f/%.1f) SMG(%.1f/%.1f) KNIFE(%.1f/%.1f)\n",
           g_cfg.weapon[WCAT_RIFLE].ap, g_cfg.weapon[WCAT_RIFLE].rt,
           g_cfg.weapon[WCAT_AWP].ap, g_cfg.weapon[WCAT_AWP].rt,
           g_cfg.weapon[WCAT_PISTOL].ap, g_cfg.weapon[WCAT_PISTOL].rt,
           g_cfg.weapon[WCAT_SMG].ap, g_cfg.weapon[WCAT_SMG].rt,
           g_cfg.weapon[WCAT_KNIFE].ap, g_cfg.weapon[WCAT_KNIFE].rt);

    /* GSI setup */
    InitializeCriticalSection(&g_gsi.lock);
    if (g_cfg.gsi_enabled) {
        create_gsi_config();
        g_gsi_thread = CreateThread(NULL, 0, gsi_thread, NULL, 0, NULL);
        if (!g_gsi_thread)
            printf("[GSI] Failed to start server thread.\n");
    }

    /* --- Watch mode: wait for CS2 --- */
    if (watch_mode) {
        printf("\nWaiting for CS2 to start...\n");
        while (g_running && !is_process_running("cs2.exe")) {
            Sleep(2000);
        }
        if (!g_running) { restore_and_cleanup(); return 0; }
        printf("CS2 detected! Starting adaptive mode.\n");
        Sleep(3000);
        adaptive_mode = true;
    }

    /* --- SDK init --- */
    if (!demo_mode) {
        printf("\nInitializing Wooting Analog SDK...\n");
        int ret = wooting_analog_initialise();
        if (ret < 0) {
            printf("ERROR: SDK init failed (code %d)\n", ret);
            printf("Press Enter to exit...\n");
            getchar();
            restore_and_cleanup();
            return 1;
        }
        printf("SDK initialized. Devices found: %d\n", ret);

        WootingAnalog_DeviceInfo_FFI *devices[4];
        int dev_count = wooting_analog_get_connected_devices_info(devices, 4);
        for (int i = 0; i < dev_count; i++) {
            printf("  Device %d: %s (%s) VID:%04X PID:%04X\n",
                   i, devices[i]->device_name, devices[i]->manufacturer_name,
                   devices[i]->vendor_id, devices[i]->product_id);
        }
        wooting_analog_set_keycode_mode(WootingAnalog_KeycodeType_HID);
    }

    /* --- HID writer init --- */
    WootingHID *hid = NULL;
    g_adaptive = adaptive_mode;
    if (adaptive_mode || demo_mode) {
        printf("\nInitializing HID writer...\n");
        hid = wooting_hid_open();
        g_hid = hid;
        if (!hid) {
            printf("WARNING: HID writer failed to open.\n");
        } else {
            if (!wooting_hid_handshake(hid))
                printf("WARNING: Handshake failed.\n");
            if (!wooting_hid_activate_profile(hid, PROFILE_IDX))
                printf("WARNING: Profile activation failed.\n");
        }
    }

    /* --- Demo mode --- */
    if (demo_mode && hid) {
        printf("\n=== DEMO MODE ===\n");
        printf("D key alternates: AP 0.1mm <-> 3.8mm every 3s.\n");
        printf("Hold D lightly to feel the difference.\n\n");

        bool aggro = false;
        while (g_running) {
            aggro = !aggro;
            float ap_val = aggro ? 0.1f : 3.8f;
            float rt_val = aggro ? 0.1f : 1.0f;
            KeySetting a[] = {{ KEY_D_ROW, KEY_D_COL, ap_val }};
            KeySetting r[] = {{ KEY_D_ROW, KEY_D_COL, rt_val }};
            wooting_hid_write_actuation(hid, PROFILE_IDX, a, 1, false);
            wooting_hid_write_rt(hid, PROFILE_IDX, r, 1, false);
            printf("\r  D -> AP:%.1fmm RT:%.1fmm [%s]   ",
                   ap_val, rt_val, aggro ? "AGGRO" : "NORMAL");
            fflush(stdout);
            Sleep(3000);
        }
        restore_and_cleanup();
        return 0;
    }

    /* --- Main loop setup --- */
    LARGE_INTEGER perf_freq;
    QueryPerformanceFrequency(&perf_freq);
    double freq = (double)perf_freq.QuadPart;

    AimContext ctx = {0};
    for (int i = 0; i < 4; i++) {
        ctx.current_ap[i] = g_cfg.ap_normal;
        ctx.current_rt[i] = g_cfg.rt_normal;
        ctx.target_ap[i]  = g_cfg.ap_normal;
        ctx.target_rt[i]  = g_cfg.rt_normal;
    }
    QueryPerformanceCounter(&ctx.last_write_time);
    ctx.vel_h.max_speed = 225.0f;
    ctx.vel_v.max_speed = 225.0f;
    QueryPerformanceCounter(&ctx.vel_h.last_update);
    ctx.vel_v.last_update = ctx.vel_h.last_update;

    /* Stats */
    if (g_cfg.stats_enabled && adaptive_mode) {
        stats_init(&ctx.stats, "wooting-aim-stats.csv");
        g_stats = &ctx.stats;
    }

    if (adaptive_mode && hid) {
        printf("\n*** ADAPTIVE MODE v4 ***\n");
        printf("Dual-axis | Crouch-peek | Predictive | GSI | VelScale | Jiggle | PhaseDecay\n");
        printf("Close this window to stop.\n\n");
    } else if (!adaptive_mode) {
        printf("\nRead-only mode. Use --adaptive or --watch for tuning.\n");
        printf("Close this window to stop.\n\n");
    }

    LARGE_INTEGER fps_timer, loop_start, loop_end;
    QueryPerformanceCounter(&fps_timer);
    unsigned long long fps_reads = 0;
    double actual_hz = 0;

    /* Velocity update rate limiter (~1000 Hz) */
    LARGE_INTEGER vel_timer;
    QueryPerformanceCounter(&vel_timer);
    float time_to_accurate_ms = 0.0f;  /* predicted ms until shootable */

    while (g_running) {
        QueryPerformanceCounter(&loop_start);

        /* Save previous values */
        ctx.prev_w = ctx.w; ctx.prev_a = ctx.a;
        ctx.prev_s = ctx.s; ctx.prev_d = ctx.d;

        /* Read analog values */
        ctx.w = wooting_analog_read_analog(HID_W);
        ctx.a = wooting_analog_read_analog(HID_A);
        ctx.s = wooting_analog_read_analog(HID_S);
        ctx.d = wooting_analog_read_analog(HID_D);
        ctx.ctrl = wooting_analog_read_analog(HID_LCTRL);

        if (ctx.w < 0) ctx.w = 0;
        if (ctx.a < 0) ctx.a = 0;
        if (ctx.s < 0) ctx.s = 0;
        if (ctx.d < 0) ctx.d = 0;
        if (ctx.ctrl < 0) ctx.ctrl = 0;

        ctx.crouching = ctx.ctrl > DEAD_ZONE;

        /* Update both axes */
        axis_update(&ctx.h, ctx.d, ctx.a, ctx.prev_d, ctx.prev_a, freq);
        axis_update(&ctx.v, ctx.w, ctx.s, ctx.prev_w, ctx.prev_s, freq);

        /* Velocity estimation (~1000 Hz update rate) */
        if (g_cfg.vel_enabled) {
            double vel_elapsed = (double)(loop_start.QuadPart - vel_timer.QuadPart) * 1000.0 / freq;
            if (vel_elapsed >= 1.0) {
                float max_spd = ctx.weapon_speed > 0 ? ctx.weapon_speed : 225.0f;
                vel_update(&ctx.vel_h, ctx.d, ctx.a, max_spd, loop_start, freq);
                vel_update(&ctx.vel_v, ctx.w, ctx.s, max_spd, loop_start, freq);
                vel_timer = loop_start;

                /* Predict time to accuracy threshold (Source 2 discrete model) */
                float total_v = sqrtf(ctx.vel_h.vel * ctx.vel_h.vel +
                                      ctx.vel_v.vel * ctx.vel_v.vel);
                float threshold = max_spd * 0.34f;
                bool is_counter = (ctx.h.state == S_COUNTER_POS || ctx.h.state == S_COUNTER_NEG ||
                                   ctx.v.state == S_COUNTER_POS || ctx.v.state == S_COUNTER_NEG);
                if (total_v <= threshold) {
                    time_to_accurate_ms = 0.0f;
                } else {
                    /* Iterate discrete model: k=0.91875, accel=~18.48/tick */
                    float v = total_v;
                    float accel_per_tick = SV_ACCELERATE * (1.0f/64.0f) * max_spd;
                    int ticks = 0;
                    while (v > threshold && ticks < 100) {
                        if (v >= SV_STOPSPEED) v *= 0.91875f;
                        else v -= 6.5f;
                        if (is_counter) v -= accel_per_tick;
                        if (v < 0) v = 0;
                        ticks++;
                    }
                    time_to_accurate_ms = ticks * 15.625f;
                }
            }
        }

        /* Print state transitions */
        /* Counter-strafe quality classification (CS2ST research):
         * Perfect: 65-95ms (80ms +/-15ms)
         * Good: 60-120ms
         * Late: >120ms, Early: <60ms */
        if (ctx.h.state != ctx.h.prev) {
            const char *wname = ctx.gsi_active ? ctx.weapon_name : "";
            if (ctx.h.prev == S_COUNTER_POS || ctx.h.prev == S_COUNTER_NEG) {
                const char *q = (ctx.h.counter_ms >= 65 && ctx.h.counter_ms <= 95) ? "PERF" :
                                (ctx.h.counter_ms >= 60 && ctx.h.counter_ms <= 120) ? "GOOD" :
                                (ctx.h.counter_ms < 60) ? "FAST" : "LATE";
                printf("\n[H] %s->%s (%.1fms %s)", axis_names[ctx.h.prev],
                       axis_names[ctx.h.state], ctx.h.counter_ms, q);
                if (g_cfg.stats_enabled)
                    stats_log(&ctx.stats, "H",
                              ctx.h.prev == S_COUNTER_POS ? "D" : "A",
                              ctx.h.counter_ms, wname);
            } else {
                printf("\n[H] %s->%s", axis_names[ctx.h.prev], axis_names[ctx.h.state]);
            }
        }
        if (ctx.v.state != ctx.v.prev) {
            const char *wname = ctx.gsi_active ? ctx.weapon_name : "";
            if (ctx.v.prev == S_COUNTER_POS || ctx.v.prev == S_COUNTER_NEG) {
                const char *q = (ctx.v.counter_ms >= 65 && ctx.v.counter_ms <= 95) ? "PERF" :
                                (ctx.v.counter_ms >= 60 && ctx.v.counter_ms <= 120) ? "GOOD" :
                                (ctx.v.counter_ms < 60) ? "FAST" : "LATE";
                printf("\n[V] %s->%s (%.1fms %s)", axis_names[ctx.v.prev],
                       axis_names[ctx.v.state], ctx.v.counter_ms, q);
                if (g_cfg.stats_enabled)
                    stats_log(&ctx.stats, "V",
                              ctx.v.prev == S_COUNTER_POS ? "W" : "S",
                              ctx.v.counter_ms, wname);
            } else {
                printf("\n[V] %s->%s", axis_names[ctx.v.prev], axis_names[ctx.v.state]);
            }
        }

        /* Adaptive tuning */
        if (adaptive_mode && hid) {
            update_targets(&ctx);
            do_write(&ctx, hid, freq);
        }

        QueryPerformanceCounter(&loop_end);
        fps_reads++;
        ctx.frame++;

        /* Watch mode: check if CS2 is still running every ~5s */
        if (watch_mode && (ctx.frame % 25000000) == 0) {
            if (!is_process_running("cs2.exe")) {
                printf("\nCS2 closed. Shutting down.\n");
                g_running = false;
            }
        }

        /* Display update every 500ms */
        double fps_elapsed = (double)(loop_end.QuadPart - fps_timer.QuadPart) * 1000.0 / freq;
        if (fps_elapsed >= 500.0) {
            actual_hz = (double)fps_reads / (fps_elapsed / 1000.0);
            fps_reads = 0;
            fps_timer = loop_end;

            printf("\r[%.1fM]", actual_hz / 1000000.0);
            print_bar("A", ctx.a);
            print_bar("D", ctx.d);
            printf(" [H:%s%s%s V:%s%s%s%s]",
                   axis_names[ctx.h.state],
                   ctx.h.predictive ? "*" : "",
                   ctx.h.is_jiggle ? "J" : "",
                   axis_names[ctx.v.state],
                   ctx.v.predictive ? "*" : "",
                   ctx.v.is_jiggle ? "J" : "",
                   ctx.crouching ? " C" : "");

            /* GSI info */
            if (ctx.gsi_active) {
                printf(" %s/%s", wcat_names[ctx.weapon_cat],
                       ctx.round_phase[0] ? ctx.round_phase : "?");
            } else {
                printf(" noGSI");
            }

            if (adaptive_mode) {
                printf(" A:%.1f/%.1f D:%.1f/%.1f",
                       ctx.current_ap[K_A], ctx.current_rt[K_A],
                       ctx.current_ap[K_D], ctx.current_rt[K_D]);
            }

            /* Velocity estimation + time-to-accurate */
            if (g_cfg.vel_enabled) {
                float total_vel = sqrtf(ctx.vel_h.vel * ctx.vel_h.vel +
                                        ctx.vel_v.vel * ctx.vel_v.vel);
                float max_spd = ctx.weapon_speed > 0 ? ctx.weapon_speed : 225.0f;
                float threshold = max_spd * 0.34f;
                if (total_vel < threshold)
                    printf(" v:%.0fOK", total_vel);
                else
                    printf(" v:%.0f>%.0fms", total_vel, time_to_accurate_ms);
            }

            printf(" #%llu", ctx.write_count);

            /* Stats summary */
            if (ctx.h.counter_count > 0) {
                printf(" avg:%.0fms", ctx.h.counter_total_ms / ctx.h.counter_count);
            }

            printf("   ");
            fflush(stdout);
        }

        /* Poll rate limiter: yield CPU when running faster than target */
        if (g_cfg.poll_rate_hz > 0) {
            double target_us = 1000000.0 / g_cfg.poll_rate_hz;
            QueryPerformanceCounter(&loop_end);
            double loop_us = (double)(loop_end.QuadPart - loop_start.QuadPart) * 1000000.0 / freq;
            if (loop_us < target_us) {
                /* Yield to reduce CPU from 100% to ~5-15% */
                SwitchToThread();
            }
        }
    }

    /* Print session summary */
    printf("\n\n=== SESSION SUMMARY ===\n");
    if (ctx.h.counter_count > 0)
        printf("H counter-strafes: %llu  avg: %.1f ms\n",
               ctx.h.counter_count, ctx.h.counter_total_ms / ctx.h.counter_count);
    if (ctx.v.counter_count > 0)
        printf("V counter-strafes: %llu  avg: %.1f ms\n",
               ctx.v.counter_count, ctx.v.counter_total_ms / ctx.v.counter_count);
    printf("HID writes: %llu\n", ctx.write_count);

    stats_close(&ctx.stats);
    restore_and_cleanup();
    DeleteCriticalSection(&g_gsi.lock);
    return 0;
}
