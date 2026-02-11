/*
 * test_math.c - Unit tests for wooting-aim pure functions
 *
 * Tests velocity model, phase decay, vel scaling, mm conversion,
 * config parsing, weapon categorization, protobuf encoding.
 *
 * Build: gcc -O0 -g -Wall -fsanitize=address,undefined -I./include -o test_math.exe src/test_math.c
 * (no SDK/HID dependencies)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <stdint.h>

/* ── test framework ── */
static int g_pass = 0, g_fail = 0;

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        printf("  FAIL: %s (line %d)\n", #expr, __LINE__); g_fail++; \
    } else { g_pass++; } \
} while(0)

#define ASSERT_FLOAT_EQ(a, b, eps) do { \
    float _a = (a), _b = (b); \
    if (fabsf(_a - _b) > (eps)) { \
        printf("  FAIL: %s == %.6f, expected %.6f +/-%.4f (line %d)\n", \
               #a, _a, _b, (eps), __LINE__); g_fail++; \
    } else { g_pass++; } \
} while(0)

#define ASSERT_INT_EQ(a, b) do { \
    int _a = (a), _b = (b); \
    if (_a != _b) { \
        printf("  FAIL: %s == %d, expected %d (line %d)\n", \
               #a, _a, _b, __LINE__); g_fail++; \
    } else { g_pass++; } \
} while(0)

#define TEST(name) static void test_##name(void)
#define RUN(name) do { printf("[TEST] " #name "\n"); test_##name(); } while(0)

/* ── copied from hid_writer.c (to test without linking) ── */

uint8_t mm_to_firmware(float mm) {
    int val = (int)(mm / 4.0f * 255.0f + 0.5f);
    if (val < 7)   val = 7;
    if (val > 255)  val = 255;
    return (uint8_t)val;
}

float firmware_to_mm(uint8_t val) {
    return (float)val / 255.0f * 4.0f;
}

static uint8_t linear_key_index(uint8_t row, uint8_t col) {
    return (uint8_t)(((row & 7) << 5) | (col & 31));
}

static uint16_t encode_key_entry(uint8_t firmware_val, uint8_t row, uint8_t col) {
    uint8_t idx = linear_key_index(row, col);
    return (uint16_t)((firmware_val << 8) | idx);
}

static int encode_varint(uint8_t *buf, uint32_t value) {
    int i = 0;
    while (value > 0x7F) {
        buf[i++] = (uint8_t)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    buf[i++] = (uint8_t)(value & 0x7F);
    return i;
}

/* ── copied from main.c ── */

#define DEAD_ZONE   0.01f
#define SV_FRICTION    5.2f
#define SV_ACCELERATE  5.5f
#define SV_STOPSPEED   80.0f

#define PHASE_ULTRA_MS     80.0
#define PHASE_DECAY_MS     200.0
#define VEL_AGGRO_ZONE     0.50f
#define VEL_MIN_AP_FACTOR  0.5f

typedef enum {
    WCAT_RIFLE, WCAT_AWP, WCAT_PISTOL, WCAT_SMG, WCAT_KNIFE, WCAT_OTHER, WCAT_COUNT
} WeaponCategory;

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

static float weapon_max_speed(const char *name) {
    if (!name[0]) return 225.0f;
    if (strstr(name, "knife") || strstr(name, "bayonet")) return 250.0f;
    if (strstr(name, "awp")) return 200.0f;
    if (strstr(name, "ak47")) return 215.0f;
    if (strstr(name, "m4a1")) return 225.0f;
    if (strstr(name, "deagle") || strstr(name, "revolver")) return 230.0f;
    if (strstr(name, "ssg08")) return 230.0f;
    if (strstr(name, "negev")) return 150.0f;
    if (strstr(name, "m249")) return 195.0f;
    return 225.0f;
}

static float vel_scale_ap(float base_ap, float vel_ratio) {
    if (vel_ratio < VEL_AGGRO_ZONE) return base_ap;
    float t = (vel_ratio - VEL_AGGRO_ZONE) / (1.0f - VEL_AGGRO_ZONE);
    float factor = 1.0f - t * (1.0f - VEL_MIN_AP_FACTOR);
    float result = base_ap * factor;
    if (result < 0.15f) result = 0.15f;
    return result;
}

static float phase_decay_ap(float base_ap, double counter_ms) {
    const float min_ap = 0.15f;
    if (counter_ms < PHASE_ULTRA_MS) return min_ap;
    if (counter_ms > PHASE_DECAY_MS) return base_ap;
    float t = (float)(counter_ms - PHASE_ULTRA_MS) / (float)(PHASE_DECAY_MS - PHASE_ULTRA_MS);
    return min_ap + t * (base_ap - min_ap);
}

/* Simplified vel_update for testing (no LARGE_INTEGER) */
static float vel_step(float vel, bool pos_key, bool neg_key, float max_speed, float dt) {
    float speed = fabsf(vel);
    if (speed > 0.001f) {
        float control = (speed < SV_STOPSPEED) ? SV_STOPSPEED : speed;
        float drop = control * SV_FRICTION * dt;
        float new_speed = speed - drop;
        if (new_speed < 0.0f) new_speed = 0.0f;
        vel *= (new_speed / speed);
    }

    float wish = 0.0f;
    if (pos_key && !neg_key) wish = 1.0f;
    else if (neg_key && !pos_key) wish = -1.0f;

    if (wish != 0.0f) {
        float current_in_wish = vel * wish;
        float add_speed = max_speed - current_in_wish;
        if (add_speed > 0.0f) {
            float accel_speed = SV_ACCELERATE * dt * max_speed;
            if (accel_speed > add_speed) accel_speed = add_speed;
            vel += accel_speed * wish;
        }
    }

    if (fabsf(vel) > max_speed)
        vel = (vel > 0) ? max_speed : -max_speed;
    if (fabsf(vel) < 0.5f)
        vel = 0.0f;

    return vel;
}

/* ════════════════════════ TESTS ════════════════════════ */

TEST(mm_to_firmware_boundaries) {
    /* 0mm should clamp to minimum (7) */
    ASSERT_INT_EQ(mm_to_firmware(0.0f), 7);
    /* Negative should clamp to 7 */
    ASSERT_INT_EQ(mm_to_firmware(-1.0f), 7);
    /* 4.0mm = full travel = 255 */
    ASSERT_INT_EQ(mm_to_firmware(4.0f), 255);
    /* > 4.0mm should clamp to 255 */
    ASSERT_INT_EQ(mm_to_firmware(5.0f), 255);
    /* 2.0mm = midpoint = ~128 */
    int mid = mm_to_firmware(2.0f);
    ASSERT_TRUE(mid >= 127 && mid <= 128);
    /* 0.1mm typical aggressive AP */
    int aggro = mm_to_firmware(0.1f);
    ASSERT_TRUE(aggro >= 6 && aggro <= 8);
}

TEST(firmware_to_mm_roundtrip) {
    /* Check roundtrip for common values */
    for (float mm = 0.2f; mm <= 3.8f; mm += 0.2f) {
        uint8_t fw = mm_to_firmware(mm);
        float back = firmware_to_mm(fw);
        ASSERT_FLOAT_EQ(back, mm, 0.02f);
    }
}

TEST(firmware_to_mm_boundaries) {
    ASSERT_FLOAT_EQ(firmware_to_mm(0), 0.0f, 0.001f);
    ASSERT_FLOAT_EQ(firmware_to_mm(255), 4.0f, 0.001f);
}

TEST(key_encoding) {
    /* W key: row=2, col=2 -> index = (2<<5)|2 = 66 */
    ASSERT_INT_EQ(linear_key_index(2, 2), 66);
    /* A key: row=3, col=1 -> index = (3<<5)|1 = 97 */
    ASSERT_INT_EQ(linear_key_index(3, 1), 97);
    /* S key: row=3, col=2 -> index = (3<<5)|2 = 98 */
    ASSERT_INT_EQ(linear_key_index(3, 2), 98);
    /* D key: row=3, col=3 -> index = (3<<5)|3 = 99 */
    ASSERT_INT_EQ(linear_key_index(3, 3), 99);

    /* Row/col masking: row & 7, col & 31 */
    ASSERT_INT_EQ(linear_key_index(8, 0), linear_key_index(0, 0)); /* row overflow wraps */
    ASSERT_INT_EQ(linear_key_index(0, 32), linear_key_index(0, 0)); /* col overflow wraps */
}

TEST(encode_key_entry_format) {
    /* fw_val=64 (1.0mm), row=3, col=3 (D key) -> (64<<8) | 99 = 16483 */
    uint16_t entry = encode_key_entry(64, 3, 3);
    ASSERT_INT_EQ(entry, (64 << 8) | 99);

    /* High value: fw_val=255, row=2, col=2 (W key) -> (255<<8)|66 = 65346 */
    entry = encode_key_entry(255, 2, 2);
    ASSERT_INT_EQ(entry, (255 << 8) | 66);
}

TEST(varint_encoding) {
    uint8_t buf[8];
    int len;

    /* Single byte: values 0-127 */
    len = encode_varint(buf, 0);
    ASSERT_INT_EQ(len, 1);
    ASSERT_INT_EQ(buf[0], 0);

    len = encode_varint(buf, 127);
    ASSERT_INT_EQ(len, 1);
    ASSERT_INT_EQ(buf[0], 127);

    /* Two bytes: values 128-16383 */
    len = encode_varint(buf, 128);
    ASSERT_INT_EQ(len, 2);
    ASSERT_INT_EQ(buf[0], 0x80);
    ASSERT_INT_EQ(buf[1], 0x01);

    len = encode_varint(buf, 300);
    ASSERT_INT_EQ(len, 2);
    ASSERT_INT_EQ(buf[0], 0xAC);
    ASSERT_INT_EQ(buf[1], 0x02);

    /* Three bytes: typical key entry value ~16000-65535 */
    len = encode_varint(buf, 16483);
    ASSERT_INT_EQ(len, 3);
    /* Verify decode: 0x83 0x80 0x01 = (0x03) | (0x00 << 7) | (0x01 << 14) */
    uint32_t decoded = (buf[0] & 0x7F) | ((buf[1] & 0x7F) << 7) | ((buf[2] & 0x7F) << 14);
    ASSERT_INT_EQ((int)decoded, 16483);
}

TEST(weapon_categorization) {
    ASSERT_INT_EQ(categorize_weapon_type("Rifle"), WCAT_RIFLE);
    ASSERT_INT_EQ(categorize_weapon_type("Machine Gun"), WCAT_RIFLE);
    ASSERT_INT_EQ(categorize_weapon_type("SniperRifle"), WCAT_AWP);
    ASSERT_INT_EQ(categorize_weapon_type("Pistol"), WCAT_PISTOL);
    ASSERT_INT_EQ(categorize_weapon_type("Submachine Gun"), WCAT_SMG);
    ASSERT_INT_EQ(categorize_weapon_type("Shotgun"), WCAT_SMG);
    ASSERT_INT_EQ(categorize_weapon_type("Knife"), WCAT_KNIFE);
    ASSERT_INT_EQ(categorize_weapon_type(""), WCAT_OTHER);
    ASSERT_INT_EQ(categorize_weapon_type("C4"), WCAT_OTHER);
    ASSERT_INT_EQ(categorize_weapon_type("Grenade"), WCAT_OTHER);
}

TEST(weapon_max_speed_values) {
    ASSERT_FLOAT_EQ(weapon_max_speed("weapon_ak47"), 215.0f, 0.1f);
    ASSERT_FLOAT_EQ(weapon_max_speed("weapon_awp"), 200.0f, 0.1f);
    ASSERT_FLOAT_EQ(weapon_max_speed("weapon_knife"), 250.0f, 0.1f);
    ASSERT_FLOAT_EQ(weapon_max_speed("weapon_m4a1_silencer"), 225.0f, 0.1f);
    ASSERT_FLOAT_EQ(weapon_max_speed("weapon_deagle"), 230.0f, 0.1f);
    ASSERT_FLOAT_EQ(weapon_max_speed("weapon_negev"), 150.0f, 0.1f);
    ASSERT_FLOAT_EQ(weapon_max_speed("weapon_m249"), 195.0f, 0.1f);
    ASSERT_FLOAT_EQ(weapon_max_speed(""), 225.0f, 0.1f);  /* default */
    ASSERT_FLOAT_EQ(weapon_max_speed("weapon_unknown"), 225.0f, 0.1f);  /* unknown */
}

TEST(vel_scale_ap_behavior) {
    /* Below aggro zone: no scaling */
    ASSERT_FLOAT_EQ(vel_scale_ap(0.4f, 0.0f), 0.4f, 0.001f);
    ASSERT_FLOAT_EQ(vel_scale_ap(0.4f, 0.3f), 0.4f, 0.001f);
    ASSERT_FLOAT_EQ(vel_scale_ap(0.4f, 0.49f), 0.4f, 0.001f);

    /* At aggro zone boundary */
    ASSERT_FLOAT_EQ(vel_scale_ap(0.4f, 0.50f), 0.4f, 0.001f);

    /* At max velocity: AP = base * 0.5 */
    ASSERT_FLOAT_EQ(vel_scale_ap(0.4f, 1.0f), 0.2f, 0.001f);

    /* Midpoint of scaling zone (0.75) */
    float mid = vel_scale_ap(0.4f, 0.75f);
    ASSERT_TRUE(mid > 0.2f && mid < 0.4f);

    /* Very low base AP: should clamp to 0.15 */
    ASSERT_FLOAT_EQ(vel_scale_ap(0.15f, 1.0f), 0.15f, 0.001f);
}

TEST(phase_decay_ap_timing) {
    float base = 0.4f;

    /* Ultra phase (0-80ms): minimum AP */
    ASSERT_FLOAT_EQ(phase_decay_ap(base, 0.0), 0.15f, 0.001f);
    ASSERT_FLOAT_EQ(phase_decay_ap(base, 40.0), 0.15f, 0.001f);
    ASSERT_FLOAT_EQ(phase_decay_ap(base, 79.0), 0.15f, 0.001f);

    /* After decay window: full base AP */
    ASSERT_FLOAT_EQ(phase_decay_ap(base, 200.0), base, 0.001f);
    ASSERT_FLOAT_EQ(phase_decay_ap(base, 300.0), base, 0.001f);

    /* Mid-decay (140ms = halfway between 80 and 200) */
    float mid = phase_decay_ap(base, 140.0);
    float expected = 0.15f + 0.5f * (base - 0.15f);
    ASSERT_FLOAT_EQ(mid, expected, 0.01f);

    /* Monotonically increasing */
    float prev = phase_decay_ap(base, 0.0);
    for (double ms = 10.0; ms <= 250.0; ms += 10.0) {
        float cur = phase_decay_ap(base, ms);
        ASSERT_TRUE(cur >= prev);
        prev = cur;
    }
}

TEST(phase_decay_negative_time) {
    /* Negative counter_ms should return min_ap (ultra phase) */
    ASSERT_FLOAT_EQ(phase_decay_ap(0.4f, -10.0), 0.15f, 0.001f);
}

TEST(velocity_friction_model) {
    float dt = 1.0f / 64.0f;  /* 64 tick */
    float max_speed = 215.0f;  /* AK-47 */

    /* From max speed, no keys: friction only decay */
    float vel = max_speed;
    vel = vel_step(vel, false, false, max_speed, dt);
    /* Expected: 215 * (1 - 5.2 * 0.015625) = 215 * 0.91875 = 197.53 */
    ASSERT_FLOAT_EQ(vel, 215.0f * 0.91875f, 1.0f);

    /* Continue friction for many ticks - should approach zero */
    for (int i = 0; i < 200; i++)
        vel = vel_step(vel, false, false, max_speed, dt);
    ASSERT_FLOAT_EQ(vel, 0.0f, 0.5f);
}

TEST(velocity_counter_strafe) {
    float dt = 1.0f / 64.0f;
    float max_speed = 215.0f;

    /* Start at max speed in positive direction */
    float vel = max_speed;

    /* Counter-strafe: press negative key */
    int ticks = 0;
    float threshold = max_speed * 0.34f;  /* ~73.1 u/s */
    while (fabsf(vel) > threshold && ticks < 100) {
        vel = vel_step(vel, false, true, max_speed, dt);
        ticks++;
    }

    /* Should reach threshold in ~5-7 ticks (78-109ms) based on research */
    printf("    Counter-strafe to 34%%: %d ticks (%.1f ms)\n", ticks, ticks * 15.625f);
    ASSERT_TRUE(ticks >= 3 && ticks <= 12);
}

TEST(velocity_counter_strafe_to_zero) {
    float dt = 1.0f / 64.0f;
    float max_speed = 215.0f;

    float vel = max_speed;
    int ticks = 0;
    while (vel > 0.5f && ticks < 100) {
        vel = vel_step(vel, false, true, max_speed, dt);
        ticks++;
    }

    /* Should reach zero in ~7-10 ticks (~110-156ms) */
    printf("    Counter-strafe to zero: %d ticks (%.1f ms)\n", ticks, ticks * 15.625f);
    ASSERT_TRUE(ticks >= 5 && ticks <= 15);
}

TEST(velocity_friction_only_to_zero) {
    float dt = 1.0f / 64.0f;
    float max_speed = 215.0f;

    float vel = max_speed;
    int ticks = 0;
    while (vel > 0.5f && ticks < 200) {
        vel = vel_step(vel, false, false, max_speed, dt);
        ticks++;
    }

    /* Friction only: ~25-30 ticks (~390-470ms) based on research */
    printf("    Friction-only to zero: %d ticks (%.1f ms)\n", ticks, ticks * 15.625f);
    ASSERT_TRUE(ticks >= 20 && ticks <= 40);
}

TEST(velocity_both_keys_no_movement) {
    float dt = 1.0f / 64.0f;
    float max_speed = 215.0f;

    /* Both keys pressed: no movement (wish=0) */
    float vel = 0.0f;
    vel = vel_step(vel, true, true, max_speed, dt);
    ASSERT_FLOAT_EQ(vel, 0.0f, 0.001f);

    /* From moving, both keys: only friction, no accel */
    vel = 100.0f;
    float prev = vel;
    vel = vel_step(vel, true, true, max_speed, dt);
    ASSERT_TRUE(vel < prev);  /* friction reduces */
    ASSERT_TRUE(vel > 0.0f);  /* but doesn't add accel */
}

TEST(velocity_acceleration_from_zero) {
    float dt = 1.0f / 64.0f;
    float max_speed = 215.0f;

    /* From zero, press positive */
    float vel = 0.0f;
    vel = vel_step(vel, true, false, max_speed, dt);
    /* Expected: SV_ACCELERATE * dt * max_speed = 5.5 * 0.015625 * 215 = 18.48 */
    ASSERT_FLOAT_EQ(vel, 5.5f * dt * max_speed, 0.1f);
}

TEST(velocity_clamp_max_speed) {
    float dt = 1.0f / 64.0f;
    float max_speed = 215.0f;

    /* Accelerate for many ticks - should not exceed max_speed */
    float vel = 0.0f;
    for (int i = 0; i < 200; i++)
        vel = vel_step(vel, true, false, max_speed, dt);
    ASSERT_TRUE(vel <= max_speed + 0.01f);
    ASSERT_TRUE(vel >= max_speed - 1.0f);
}

TEST(velocity_stopspeed_behavior) {
    float dt = 1.0f / 64.0f;
    float max_speed = 215.0f;

    /* At exactly stopspeed, friction should use stopspeed as control */
    float vel = SV_STOPSPEED;
    vel = vel_step(vel, false, false, max_speed, dt);
    /* drop = 80 * 5.2 * 0.015625 = 6.5 */
    ASSERT_FLOAT_EQ(vel, 80.0f - 6.5f, 0.1f);

    /* Below stopspeed: control = stopspeed, so drop = 6.5 every tick */
    vel = 50.0f;
    vel = vel_step(vel, false, false, max_speed, dt);
    ASSERT_FLOAT_EQ(vel, 50.0f - 6.5f, 0.1f);
}

TEST(vel_scale_minimum_clamp) {
    /* Even with very small base_ap, result should be >= 0.15 */
    ASSERT_TRUE(vel_scale_ap(0.1f, 1.0f) >= 0.15f);
    ASSERT_TRUE(vel_scale_ap(0.05f, 1.0f) >= 0.15f);
}

TEST(phase_decay_with_low_base) {
    /* If base_ap < min_ap (0.15), phase_decay should still return min_ap */
    float result = phase_decay_ap(0.1f, 0.0);
    ASSERT_FLOAT_EQ(result, 0.15f, 0.001f);

    /* After ultra phase with low base, decay should go from 0.15 to 0.1 (base) */
    result = phase_decay_ap(0.1f, 200.0);
    ASSERT_FLOAT_EQ(result, 0.1f, 0.001f);

    /* Mid-decay: interpolation between 0.15 and 0.1 */
    result = phase_decay_ap(0.1f, 140.0);
    float expected = 0.15f + 0.5f * (0.1f - 0.15f);
    ASSERT_FLOAT_EQ(result, expected, 0.01f);  /* 0.125 */
}

/* ═══════════════════════ MAIN ═══════════════════════ */

int main(void) {
    printf("=== wooting-aim unit tests ===\n\n");

    printf("--- mm/firmware conversion ---\n");
    RUN(mm_to_firmware_boundaries);
    RUN(firmware_to_mm_roundtrip);
    RUN(firmware_to_mm_boundaries);

    printf("\n--- key encoding ---\n");
    RUN(key_encoding);
    RUN(encode_key_entry_format);
    RUN(varint_encoding);

    printf("\n--- weapon system ---\n");
    RUN(weapon_categorization);
    RUN(weapon_max_speed_values);

    printf("\n--- velocity-aware AP ---\n");
    RUN(vel_scale_ap_behavior);
    RUN(vel_scale_minimum_clamp);

    printf("\n--- phase decay ---\n");
    RUN(phase_decay_ap_timing);
    RUN(phase_decay_negative_time);
    RUN(phase_decay_with_low_base);

    printf("\n--- velocity model (Source 2) ---\n");
    RUN(velocity_friction_model);
    RUN(velocity_counter_strafe);
    RUN(velocity_counter_strafe_to_zero);
    RUN(velocity_friction_only_to_zero);
    RUN(velocity_both_keys_no_movement);
    RUN(velocity_acceleration_from_zero);
    RUN(velocity_clamp_max_speed);
    RUN(velocity_stopspeed_behavior);

    printf("\n=== RESULTS: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
