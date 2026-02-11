/*
 * hid_writer.h - Wooting HID protocol for writing AP/RT per-key
 *
 * Protocol details reverse-engineered from Wootility WebHID traffic.
 * Uses hidapi to communicate with the keyboard's vendor interface.
 */

#ifndef HID_WRITER_H
#define HID_WRITER_H

#include <stdbool.h>
#include <stdint.h>

/* Wooting 60HE matrix positions for WASD */
#define KEY_W_ROW 2
#define KEY_W_COL 2
#define KEY_A_ROW 3
#define KEY_A_COL 1
#define KEY_S_ROW 3
#define KEY_S_COL 2
#define KEY_D_ROW 3
#define KEY_D_COL 3

/* Report commands */
#define CMD_ACTUATION       21
#define CMD_RAPID_TRIGGER   25
#define CMD_ACTIVATE_PROFILE 23
#define CMD_RELOAD_PROFILE  38
#define CMD_HANDSHAKE       39
#define CMD_SAVE_PROFILE    42
#define CMD_GET_ACTUATION   49
#define CMD_GET_RT          54

/* Response status codes */
#define STATUS_SUCCESS      0x88
#define STATUS_BUSY         0x77
#define STATUS_UNSUPPORTED  0xAA

/* Opaque handle */
typedef struct WootingHID WootingHID;

/* Key-value pair for per-key configuration */
typedef struct {
    uint8_t row;
    uint8_t col;
    float   mm;     /* 0.0 - 4.0 mm */
} KeySetting;

/*
 * Open connection to Wooting keyboard via vendor HID interface.
 * Returns NULL on failure.
 */
WootingHID *wooting_hid_open(void);

/*
 * Close connection and free resources.
 */
void wooting_hid_close(WootingHID *dev);

/*
 * Perform handshake (required before any write).
 * Returns true on success.
 */
bool wooting_hid_handshake(WootingHID *dev);

/*
 * Activate a profile (0-3) on the keyboard.
 * Returns true on success.
 */
bool wooting_hid_activate_profile(WootingHID *dev, int profile_idx);

/*
 * Write actuation points for specific keys (RAM only, no flash save).
 * keys: array of KeySetting, count: number of entries.
 * profile_idx: 0-3.
 * Returns true on success.
 */
bool wooting_hid_write_actuation(WootingHID *dev, int profile_idx,
                                  const KeySetting *keys, int count, bool save);

/*
 * Write rapid trigger sensitivity for specific keys.
 * save=true: persist to flash. save=false: RAM only (for real-time tuning).
 */
bool wooting_hid_write_rt(WootingHID *dev, int profile_idx,
                           const KeySetting *keys, int count, bool save);

/*
 * Save current profile to flash. Use sparingly (flash wear).
 * Returns true on success.
 */
bool wooting_hid_save_to_flash(WootingHID *dev);

/*
 * Read current actuation profile from keyboard.
 * Fills response buffer, returns number of bytes read or -1 on error.
 */
int wooting_hid_read_actuation(WootingHID *dev, int profile_idx,
                                uint8_t *buf, int buf_size);

/*
 * Read current RT profile from keyboard.
 */
int wooting_hid_read_rt(WootingHID *dev, int profile_idx,
                         uint8_t *buf, int buf_size);

/*
 * Convert mm (0.0-4.0) to firmware value (7-255).
 */
uint8_t mm_to_firmware(float mm);

/*
 * Convert firmware value (0-255) to mm (0.0-4.0).
 */
float firmware_to_mm(uint8_t val);

#endif /* HID_WRITER_H */
