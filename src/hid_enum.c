/* Enumerate all Wooting HID interfaces and test various report sizes */
#include <stdio.h>
#include <string.h>
#include <hidapi/hidapi.h>

static void test_interface(const char *path, unsigned short usage_page) {
    hid_device *h = hid_open_path(path);
    if (!h) {
        printf("  OPEN FAILED: %ls\n", hid_error(NULL));
        return;
    }
    printf("  Opened OK\n");

    /* Test feature report with report ID 0 and 1, various sizes */
    int report_ids[] = {0, 1};
    int sizes[] = {8, 9, 33, 65};

    for (int r = 0; r < 2; r++) {
        for (int s = 0; s < 4; s++) {
            unsigned char buf[256] = {0};
            buf[0] = (unsigned char)report_ids[r];
            int ret = hid_get_feature_report(h, buf, sizes[s]);
            if (ret >= 0) {
                printf("  GET_FEATURE(rid=%d, size=%d): OK (%d bytes) [%02X %02X %02X %02X %02X %02X %02X %02X]\n",
                       report_ids[r], sizes[s], ret,
                       buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
            }
        }
    }

    /* Test send feature report with report ID 1, magic bytes */
    unsigned char feat[9] = {0x01, 0xD1, 0xDA, 0x27, 0x00, 0x00, 0x00, 0x00, 0x00}; /* cmd 39 = handshake */
    int ret = hid_send_feature_report(h, feat, 9);
    printf("  SEND_FEATURE(rid=1, cmd=39, 9 bytes): %d %s\n", ret, ret < 0 ? "FAIL" : "OK");
    if (ret < 0) printf("    Error: %ls\n", hid_error(h));

    /* Try with 8 bytes */
    ret = hid_send_feature_report(h, feat, 8);
    printf("  SEND_FEATURE(rid=1, cmd=39, 8 bytes): %d %s\n", ret, ret < 0 ? "FAIL" : "OK");
    if (ret < 0) printf("    Error: %ls\n", hid_error(h));

    /* Test hid_write with report ID 1, 33 bytes (pad to 32+1) */
    unsigned char wbuf[33];
    memset(wbuf, 0, sizeof(wbuf));
    wbuf[0] = 0x01; /* report ID 1 */
    wbuf[1] = 0xD1;
    wbuf[2] = 0xDA;
    wbuf[3] = 0x27; /* cmd 39 = handshake */
    wbuf[4] = 0x05; /* body len low */
    wbuf[5] = 0x00; /* body len high */
    wbuf[6] = 0x01; /* handshake byte */
    wbuf[7] = 0x5E; /* magic LE */
    wbuf[8] = 0x46;
    wbuf[9] = 0x45;
    wbuf[10] = 0x7A;
    ret = hid_write(h, wbuf, 33);
    printf("  WRITE(rid=1, 33 bytes, handshake): %d %s\n", ret, ret < 0 ? "FAIL" : "OK");
    if (ret < 0) printf("    Error: %ls\n", hid_error(h));

    /* Try reading response after any successful write */
    if (ret >= 0) {
        unsigned char rbuf[256] = {0};
        ret = hid_read_timeout(h, rbuf, sizeof(rbuf), 500);
        printf("  READ response: %d bytes\n", ret);
        if (ret > 0) {
            printf("    Data: ");
            for (int i = 0; i < ret && i < 32; i++) printf("%02X ", rbuf[i]);
            printf("\n");
        }
    }

    hid_close(h);
}

int main(void) {
    hid_init();

    struct hid_device_info *devs = hid_enumerate(0x31E3, 0);
    struct hid_device_info *cur = devs;

    printf("=== Wooting HID Interface Test ===\n\n");

    /* Only test vendor-specific interfaces */
    while (cur) {
        if (cur->usage_page == 0xFF54 || cur->usage_page == 0xFF55 || cur->usage_page == 0xFF00) {
            printf("Interface MI_%d (usage_page=0x%04X, usage=0x%04X):\n",
                   cur->interface_number, cur->usage_page, cur->usage);
            printf("  Path: %s\n", cur->path);
            test_interface(cur->path, cur->usage_page);
            printf("\n");
        }
        cur = cur->next;
    }

    hid_free_enumeration(devs);
    hid_exit();
    return 0;
}
