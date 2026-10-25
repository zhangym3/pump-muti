/* Execute the real SDK sequence allocation/storage/restore functions.
 * NVS and the pending work queue are stubs; no device or RF access. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); exit(1); \
} } while (0)
#define CONFIG_BLE_MESH_SETTINGS 1
#define CONFIG_BLE_MESH_SEQ_STORE_RATE 128
#define IS_ENABLED(value) (value)
#define BT_ERR(...) ((void)0)
#define BT_WARN(...) ((void)0)
#define BT_INFO(...) ((void)0)
#define IV_UPDATE_SEQ_LIMIT 0xc00000
enum { BLE_MESH_SEQ_PENDING, BLE_MESH_IVU_IN_PROGRESS };
static struct { uint32_t seq, iv_index; unsigned flags; } bt_mesh;
struct seq_val { uint8_t val[3]; };
static uint8_t saved[3];
static bool exists, pending;
static unsigned writes;
static uint32_t sys_get_le24(const uint8_t *data) {
    return data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16);
}
static void sys_put_le24(uint32_t value, uint8_t *data) {
    for (unsigned i = 0; i < 3; ++i) data[i] = (uint8_t)(value >> (8 * i));
}
static int bt_mesh_load_core_settings(const char *name, uint8_t *value,
                                      size_t size, bool *found) {
    CHECK(strcmp(name, "mesh/seq") == 0 && size == sizeof(saved));
    *found = exists;
    if (exists) memcpy(value, saved, size);
    return 0;
}
static int bt_mesh_save_core_settings(const char *name, const uint8_t *value, size_t size) {
    CHECK(strcmp(name, "mesh/seq") == 0 && size == sizeof(saved));
    memcpy(saved, value, size);
    exists = true;
    ++writes;
    return 0;
}
static void schedule_store(int flag) { CHECK(flag == BLE_MESH_SEQ_PENDING); pending = true; }
static bool bt_mesh_atomic_test_bit(unsigned flags, int bit) { return (flags & (1U << bit)) != 0; }
static bool bt_mesh_primary_subnet_exist(void) { return true; }
static void bt_mesh_beacon_ivu_initiator(bool value) { (void)value; CHECK(false); }
static bool bt_mesh_net_iv_update(uint32_t index, bool update) {
    (void)index; (void)update; CHECK(false); return false;
}
static void bt_mesh_net_sec_update(void *value) { (void)value; CHECK(false); }

#include "sdk_sequence_extracted.inc"

static void flush(void) {
    if (pending) { pending = false; store_pending_seq(); }
}
static void reboot(void) {
    memset(&bt_mesh, 0, sizeof(bt_mesh));
    pending = false;
    CHECK(seq_set("mesh/seq") == 0);
}
static void fresh_network(void) {
    memset(&bt_mesh, 0, sizeof(bt_mesh));
    memset(saved, 0, sizeof(saved));
    exists = pending = false;
    writes = 0;
    /* Fresh Provisioner has no mesh/seq: net_create stores IV only. */
}
static uint32_t send(void) { uint32_t seq = bt_mesh_next_seq(); flush(); return seq; }

int main(void) {
    /* Reboot after short or multi-block sessions, with pending writes flushed. */
    for (unsigned count = 1; count <= 257; ++count) {
        fresh_network();
        uint32_t previous = 0;
        for (unsigned i = 0; i < count; ++i) previous = send();
        reboot();
        uint32_t next = send();
        CHECK(next > previous);
        CHECK(exists);
    }
    /* Legacy affected networks can have sent 0..126 without a checkpoint. */
    for (uint32_t peer = 0; peer < 127; ++peer) {
        fresh_network();
        reboot();
        uint32_t next = send();
        CHECK(next > peer);
        reboot();
        CHECK(send() > next);
    }
    /* Repeated reboots with both short and multi-block sessions. */
    fresh_network();
    uint32_t peer = send();
    for (unsigned boot = 0; boot < 300; ++boot) {
        reboot();
        for (unsigned i = 0; i < 1 + (boot * 43) % 300; ++i) {
            uint32_t next = send();
            CHECK(next > peer);
            peer = next;
        }
    }
    /* Preserve the existing skip rule for an already saved checkpoint. */
    const uint32_t checkpoints[] = {0, 1, 127, 128, 129, 255, 256, 4096};
    for (unsigned i = 0; i < sizeof(checkpoints)/sizeof(checkpoints[0]); ++i) {
        fresh_network();
        exists = true;
        sys_put_le24(checkpoints[i], saved);
        reboot();
        CHECK(bt_mesh.seq == (checkpoints[i] / 128 + 1) * 128 - 1);
    }
    /* One extra write in the initial block, rather than one per packet. */
    fresh_network();
    for (unsigned i = 0; i < 127; ++i) send();
    CHECK(writes == 1);
    send();
    CHECK(writes == 2);
    puts("PASS SDK sequence: initial block, missing checkpoint, repeated reboot, write rate");
    return 0;
}
