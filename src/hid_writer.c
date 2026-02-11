/*
 * hid_writer.c - Wooting HID protocol implementation
 *
 * Handles low-level communication with Wooting keyboards via hidapi.
 * Implements the vendor-specific protocol for writing actuation points
 * and rapid trigger settings per-key.
 */

#include "hid_writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <hidapi/hidapi.h>

/* Wooting vendor ID */
#define WOOTING_VID     0x31E3

/* Usage page for V3 protocol (60HE, 80HE, UWU, etc.)
 * MUST be 0xFF55 (MI_02). 0xFF54 (MI_04) does NOT support writes. */
#define V3_USAGE_PAGE  0xFF55

/* Magic bytes */
#define MAGIC_0  0xD1
#define MAGIC_1  0xDA

/* Handshake secret */
#define HANDSHAKE_BYTE  0x01
#define HANDSHAKE_MAGIC 0x7A45465E

/* Max report sizes per report ID */
static const int REPORT_SIZES[] = {
    0,     /* unused */
    32,    /* report 1 */
    62,    /* report 2 */
    254,   /* report 3 */
    510,   /* report 4 */
    1022,  /* report 5 */
    2046,  /* report 6 */
};
#define NUM_REPORT_SIZES 7

struct WootingHID {
    hid_device *handle;
    int active_profile;
};

/* ---------- helpers ---------- */

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

/* Encode a uint16 as protobuf varint, return bytes written */
static int encode_varint(uint8_t *buf, uint32_t value) {
    int i = 0;
    while (value > 0x7F) {
        buf[i++] = (uint8_t)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    buf[i++] = (uint8_t)(value & 0x7F);
    return i;
}

/* Pick report ID based on data size (excluding report ID byte) */
static int pick_report_id(int data_size) {
    for (int i = 1; i < NUM_REPORT_SIZES; i++) {
        if (data_size <= REPORT_SIZES[i])
            return i;
    }
    return NUM_REPORT_SIZES - 1;
}

/* ---------- low-level HID ---------- */

/* Send a feature report (command). Total 8 bytes: [rid=1, magic, cmd, param_le_4] */
static bool send_command(WootingHID *dev, uint8_t cmd, uint32_t param) {
    uint8_t buf[9];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x01;  /* report ID 1 */
    buf[1] = MAGIC_0;
    buf[2] = MAGIC_1;
    buf[3] = cmd;
    buf[4] = (uint8_t)(param & 0xFF);
    buf[5] = (uint8_t)((param >> 8) & 0xFF);
    buf[6] = (uint8_t)((param >> 16) & 0xFF);
    buf[7] = (uint8_t)((param >> 24) & 0xFF);

    int ret = hid_send_feature_report(dev->handle, buf, 9);
    if (ret < 0) {
        fprintf(stderr, "[HID] send_command(%d) failed: %ls\n", cmd, hid_error(dev->handle));
        return false;
    }
    return true;
}

/* Read feature report response. Returns status byte or -1 on error. */
/*
 * Parse a response buffer (common to both feature and input reports).
 * Format: [rid, D1, DA, cmd_echo, status, bodylen_lo, bodylen_hi, body...]
 * For input reports, rid is stripped by hid_read, so offset=0.
 * For feature reports, buf[0]=rid, so offset=1.
 */
static int parse_response(const uint8_t *buf, int len, int offset,
                          uint8_t *body, int body_size, int *body_len) {
    if (len < offset + 5) return -1;

    if (buf[offset] != MAGIC_0 || buf[offset + 1] != MAGIC_1) return -1;

    uint8_t status = buf[offset + 3];
    uint16_t blen = buf[offset + 4] | (buf[offset + 5] << 8);

    if (body && body_size > 0 && blen > 0) {
        int copy = ((int)blen < body_size) ? (int)blen : body_size;
        int avail = len - (offset + 6);
        if (copy > avail) copy = avail;
        memcpy(body, buf + offset + 6, copy);
        if (body_len) *body_len = copy;
    }

    return status;
}

/* Read response via feature report (after send_command) */
static int read_feature_response(WootingHID *dev, uint8_t *body, int body_size, int *body_len) {
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x01;

    int ret = hid_get_feature_report(dev->handle, buf, sizeof(buf));
    if (ret < 1) return -1;

    /* buf[0]=rid(1), buf[1]=D1, buf[2]=DA, buf[3]=cmd, buf[4]=status, ... */
    return parse_response(buf, ret, 1, body, body_size, body_len);
}

/* Read response via input report (after hid_write / send_data) */
static int read_input_response(WootingHID *dev, uint8_t *body, int body_size, int *body_len) {
    uint8_t buf[2048];
    memset(buf, 0, sizeof(buf));

    int ret = hid_read_timeout(dev->handle, buf, sizeof(buf), 1000);
    if (ret < 6) {
        fprintf(stderr, "[HID] read_input: got %d bytes (need >=6)\n", ret);
        if (ret > 0) {
            fprintf(stderr, "[HID] data: ");
            for (int i = 0; i < ret && i < 16; i++) fprintf(stderr, "%02X ", buf[i]);
            fprintf(stderr, "\n");
        }
        return -1;
    }

    fprintf(stderr, "[HID] read_input: %d bytes [%02X %02X %02X %02X %02X %02X %02X]\n",
            ret, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);

    /* On Windows, hid_read includes report ID: [rid, D1, DA, cmd, status, bodylen...] */
    return parse_response(buf, ret, 1, body, body_size, body_len);
}

/*
 * Send a data report (protoWithOptions format).
 * Format: [report_id, magic(2), cmd, options, bodylen_le(2), protobuf..., padding]
 */
static bool send_data(WootingHID *dev, uint8_t cmd, uint8_t options,
                      const uint8_t *proto, int proto_len) {
    /* Header: magic(2) + cmd(1) + options(1) + bodylen(2) = 6 bytes */
    int data_size = 6 + proto_len;
    int rid = pick_report_id(data_size);
    int pad_size = REPORT_SIZES[rid];

    /* Buffer: report_id(1) + padded data */
    int buf_size = 1 + pad_size;
    uint8_t *buf = calloc(buf_size, 1);
    if (!buf) return false;

    buf[0] = (uint8_t)rid;
    buf[1] = MAGIC_0;
    buf[2] = MAGIC_1;
    buf[3] = cmd;
    buf[4] = options;
    buf[5] = (uint8_t)(proto_len & 0xFF);
    buf[6] = (uint8_t)((proto_len >> 8) & 0xFF);
    memcpy(buf + 7, proto, proto_len);

    int ret = hid_write(dev->handle, buf, buf_size);
    free(buf);

    if (ret < 0) {
        fprintf(stderr, "[HID] send_data failed: %ls\n", hid_error(dev->handle));
        return false;
    }

    /* Delay after write - shorter for RAM-only writes */
    bool is_save = (options & 1);
    Sleep(is_save ? 50 : 5);

    /* Flush any response */
    { uint8_t tmp[2048]; hid_read_timeout(dev->handle, tmp, sizeof(tmp), is_save ? 50 : 5); }

    return true;
}

/* ---------- protobuf builder (partial profile) ---------- */

/*
 * Build a partial key profile protobuf (field 2, tag 0x12).
 * Each entry is: tag 0x08 + varint(encoded_key_entry).
 * Returns bytes written to buf, or -1 on overflow.
 */
static int build_partial_proto(uint8_t *buf, int buf_size,
                                const KeySetting *keys, int count) {
    /* First, build inner data (all entries with tag 0x08) */
    uint8_t inner[512];
    int inner_len = 0;

    for (int i = 0; i < count; i++) {
        uint8_t fw_val = mm_to_firmware(keys[i].mm);
        uint16_t entry = encode_key_entry(fw_val, keys[i].row, keys[i].col);

        if (inner_len + 4 > (int)sizeof(inner)) return -1;
        inner[inner_len++] = 0x08; /* tag: field 1, varint */
        inner_len += encode_varint(inner + inner_len, entry);
    }

    /* Wrap in field 2 (tag 0x12 = field 2, length-delimited) */
    int pos = 0;
    if (pos + 1 + 5 + inner_len > buf_size) return -1;

    buf[pos++] = 0x12; /* field 2, wire type 2 (length-delimited) */
    pos += encode_varint(buf + pos, inner_len);
    memcpy(buf + pos, inner, inner_len);
    pos += inner_len;

    return pos;
}

/* ---------- public API ---------- */

WootingHID *wooting_hid_open(void) {
    if (hid_init() != 0) {
        fprintf(stderr, "[HID] hid_init() failed\n");
        return NULL;
    }

    /* Enumerate Wooting devices, find the one with vendor usage page */
    struct hid_device_info *devs = hid_enumerate(WOOTING_VID, 0);
    struct hid_device_info *cur = devs;
    char *path = NULL;

    while (cur) {
        if (cur->usage_page == V3_USAGE_PAGE) {
            path = _strdup(cur->path);
            printf("[HID] Found: %ls (VID:%04X PID:%04X) usage_page:0x%04X iface:%d\n",
                   cur->product_string, cur->vendor_id, cur->product_id,
                   cur->usage_page, cur->interface_number);
            break;
        }
        cur = cur->next;
    }
    hid_free_enumeration(devs);

    if (!path) {
        fprintf(stderr, "[HID] No Wooting device found with usage page 0x%04X\n",
                V3_USAGE_PAGE);
        return NULL;
    }

    hid_device *handle = hid_open_path(path);
    free(path);

    if (!handle) {
        fprintf(stderr, "[HID] hid_open_path() failed: %ls\n", hid_error(NULL));
        return NULL;
    }

    /* Set non-blocking mode (matches Python implementation) */
    hid_set_nonblocking(handle, 1);

    WootingHID *dev = calloc(1, sizeof(WootingHID));
    dev->handle = handle;
    dev->active_profile = -1;

    printf("[HID] Device opened (non-blocking)\n");
    return dev;
}

void wooting_hid_close(WootingHID *dev) {
    if (!dev) return;
    if (dev->handle) hid_close(dev->handle);
    free(dev);
    hid_exit();
}

bool wooting_hid_handshake(WootingHID *dev) {
    if (!dev) return false;

    /*
     * Send handshake via feature report (simpler, more reliable).
     * Then also send via data report as some firmware versions need it.
     */

    /* Method 1: Feature report handshake */
    bool ok = send_command(dev, CMD_HANDSHAKE, HANDSHAKE_MAGIC);
    if (ok) {
        int status = read_feature_response(dev, NULL, 0, NULL);
        if (status == STATUS_SUCCESS) {
            printf("[HID] Handshake OK (feature report)\n");
            return true;
        }
    }

    /* Method 2: Data report handshake */
    int data_size = 2 + 1 + 2 + 5;
    int rid = pick_report_id(data_size);
    int pad_size = REPORT_SIZES[rid];
    int buf_size = 1 + pad_size;

    uint8_t *buf = calloc(buf_size, 1);
    buf[0] = (uint8_t)rid;
    buf[1] = MAGIC_0;
    buf[2] = MAGIC_1;
    buf[3] = CMD_HANDSHAKE;
    buf[4] = 5;
    buf[5] = 0;
    buf[6] = HANDSHAKE_BYTE;
    buf[7] = (uint8_t)(HANDSHAKE_MAGIC & 0xFF);
    buf[8] = (uint8_t)((HANDSHAKE_MAGIC >> 8) & 0xFF);
    buf[9] = (uint8_t)((HANDSHAKE_MAGIC >> 16) & 0xFF);
    buf[10] = (uint8_t)((HANDSHAKE_MAGIC >> 24) & 0xFF);

    int ret = hid_write(dev->handle, buf, buf_size);
    free(buf);

    if (ret < 0) {
        fprintf(stderr, "[HID] Handshake data write failed: %ls\n", hid_error(dev->handle));
        return false;
    }

    /* Wait and flush reads (matches Python: sleep(0.05) + _flush_read()) */
    Sleep(50);
    {
        uint8_t tmp[2048];
        while (hid_read_timeout(dev->handle, tmp, sizeof(tmp), 50) > 0) {}
    }

    printf("[HID] Handshake OK\n");
    return true;
}

bool wooting_hid_activate_profile(WootingHID *dev, int profile_idx) {
    if (!dev || profile_idx < 0 || profile_idx > 3) return false;
    if (dev->active_profile == profile_idx) return true;

    if (!send_command(dev, CMD_ACTIVATE_PROFILE, (uint32_t)profile_idx)) {
        fprintf(stderr, "[HID] Activate profile %d send failed\n", profile_idx);
        return false;
    }
    Sleep(50);
    { uint8_t tmp[2048]; while (hid_read_timeout(dev->handle, tmp, sizeof(tmp), 50) > 0) {} }

    /* NOTE: Skip RELOAD for RAM writes - reload resets RAM back to flash defaults.
     * Python write_keys uses activate_profile(reload=False). */

    dev->active_profile = profile_idx;
    printf("[HID] Profile %d activated (no reload)\n", profile_idx);
    return true;
}

bool wooting_hid_write_actuation(WootingHID *dev, int profile_idx,
                                  const KeySetting *keys, int count, bool save) {
    if (!dev || !keys || count <= 0) return false;

    uint8_t proto[512];
    int proto_len = build_partial_proto(proto, sizeof(proto), keys, count);
    if (proto_len < 0) {
        fprintf(stderr, "[HID] Protobuf build failed\n");
        return false;
    }

    uint8_t options = (uint8_t)((save ? 1 : 0) | (profile_idx << 1));

    return send_data(dev, CMD_ACTUATION, options, proto, proto_len);
}

bool wooting_hid_write_rt(WootingHID *dev, int profile_idx,
                           const KeySetting *keys, int count, bool save) {
    if (!dev || !keys || count <= 0) return false;

    uint8_t proto[512];
    int proto_len = build_partial_proto(proto, sizeof(proto), keys, count);
    if (proto_len < 0) {
        fprintf(stderr, "[HID] Protobuf build failed\n");
        return false;
    }

    uint8_t options = (uint8_t)((save ? 1 : 0) | (profile_idx << 1));

    return send_data(dev, CMD_RAPID_TRIGGER, options, proto, proto_len);
}

bool wooting_hid_save_to_flash(WootingHID *dev) {
    if (!dev) return false;
    if (!send_command(dev, CMD_SAVE_PROFILE, 0))
        return false;

    Sleep(200);
    { uint8_t tmp[2048]; while (hid_read_timeout(dev->handle, tmp, sizeof(tmp), 50) > 0) {} }

    printf("[HID] Save to flash sent\n");
    return true;
}

/*
 * Send GET command and read profile data.
 * The response comes as input report(s): first an ack, then the data.
 */
static int read_profile(WootingHID *dev, uint8_t cmd, int profile_idx,
                         uint8_t *buf, int buf_size) {
    /* Send GET command via feature report */
    if (!send_command(dev, cmd, (uint32_t)profile_idx))
        return -1;

    /* Read ack (status only, body_len may be 0) */
    uint8_t resp[2048];
    int ret = hid_read_timeout(dev->handle, resp, sizeof(resp), 1000);
    if (ret < 7) {
        fprintf(stderr, "[HID] read_profile: ack too short (%d bytes)\n", ret);
        return -1;
    }

    /* Parse: [rid, D1, DA, cmd, status, bodylen_lo, bodylen_hi, body...] */
    if (resp[1] != MAGIC_0 || resp[2] != MAGIC_1) return -1;
    uint8_t status = resp[4];
    uint16_t blen = resp[5] | (resp[6] << 8);

    if (status != STATUS_SUCCESS) {
        fprintf(stderr, "[HID] read_profile: status=0x%02X\n", status);
        return -1;
    }

    /* If body is in this response, copy it */
    if (blen > 0 && ret > 7) {
        int copy = ((int)blen < buf_size) ? (int)blen : buf_size;
        int avail = ret - 7;
        if (copy > avail) copy = avail;
        memcpy(buf, resp + 7, copy);
        return copy;
    }

    /* Body might come in a separate input report */
    ret = hid_read_timeout(dev->handle, resp, sizeof(resp), 1000);
    if (ret > 0) {
        int copy = (ret < buf_size) ? ret : buf_size;
        memcpy(buf, resp, copy);
        return copy;
    }

    return 0; /* command succeeded but no body data */
}

int wooting_hid_read_actuation(WootingHID *dev, int profile_idx,
                                uint8_t *buf, int buf_size) {
    return read_profile(dev, CMD_GET_ACTUATION, profile_idx, buf, buf_size);
}

int wooting_hid_read_rt(WootingHID *dev, int profile_idx,
                         uint8_t *buf, int buf_size) {
    return read_profile(dev, CMD_GET_RT, profile_idx, buf, buf_size);
}
