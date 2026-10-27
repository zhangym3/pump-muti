// Actual pinned SDK allocation/binding functions with allocator, crypto, NVS
// and model lookup stubs. This is NOT a device, RF, or durable-storage test.
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BT_ERR(...) ((void)0)
#define BT_WARN(...) ((void)0)
#define BT_INFO(...) ((void)0)
#define BLE_MESH_KEY_UNUSED 0xffff
#define CONFIG_BLE_MESH_SETTINGS 1
#define IS_ENABLED(v) (v)
#include "sdk_appkey_types.inc"
struct bt_mesh_subnet { uint16_t net_idx; };
struct bt_mesh_model { uint16_t keys[TEST_MODEL_CAPACITY]; };
struct bt_mesh_elem { uint16_t addr; };
static struct {
    uint16_t p_app_idx_next;
    struct bt_mesh_app_key *p_app_keys[TEST_APPKEY_CAPACITY];
    struct bt_mesh_subnet *p_sub[2];
} bt_mesh;
static struct bt_mesh_model model;
static struct bt_mesh_elem element = {1};
static struct bt_mesh_subnet subnet[2] = {{0}, {1}};
static bool fail_alloc, fail_aid, fail_random;
static unsigned allocations, stores, model_stores;
static void *bt_mesh_calloc(size_t n) {
    if (fail_alloc) return NULL;
    void *p = calloc(1, n); if (p) ++allocations; return p;
}
static void bt_mesh_free(void *p) { if (p) { --allocations; free(p); } }
static int bt_mesh_rand(uint8_t *p, size_t n) { memset(p, 0x69, n); return fail_random ? -EIO : 0; }
static int bt_mesh_app_id(const uint8_t *p, uint8_t *id) { *id = p[0] & 0x3f; return fail_aid ? -EIO : 0; }
static void bt_mesh_store_p_app_idx(void) { ++stores; }
static void bt_mesh_store_p_app_key(struct bt_mesh_app_key *p) { assert(p); ++stores; }
static void bt_mesh_store_mod_bind(struct bt_mesh_model *p) { assert(p == &model); ++model_stores; }
static struct bt_mesh_elem *bt_mesh_elem_find(uint16_t a) { return a == 1 ? &element : NULL; }
static struct bt_mesh_model *bt_mesh_model_find(struct bt_mesh_elem *e, uint16_t id) {
    return e == &element && id == 1 ? &model : NULL;
}
static struct bt_mesh_model *bt_mesh_model_find_vnd(struct bt_mesh_elem *e, uint16_t cid, uint16_t id) {
    return cid == 0x02e5 ? bt_mesh_model_find(e, id) : NULL;
}
#include "sdk_appkey_extracted.inc"
static void reset(void) {
    for (unsigned i = 0; i < ARRAY_SIZE(bt_mesh.p_app_keys); ++i) bt_mesh_free(bt_mesh.p_app_keys[i]);
    memset(&bt_mesh, 0, sizeof(bt_mesh)); memset(&model, 0xff, sizeof(model));
    bt_mesh.p_sub[0] = &subnet[0]; bt_mesh.p_sub[1] = &subnet[1];
    stores = model_stores = 0; fail_alloc = fail_aid = fail_random = false;
    assert(allocations == 0);
}
static void key_bytes(unsigned n, uint8_t key[16]) {
    memset(key, 0x53, 16); key[0] = (uint8_t)(n + 1); key[1] = (uint8_t)((n + 1) >> 8);
}
int main(void) {
    _Static_assert(TEST_APPKEY_CAPACITY > 0 && TEST_APPKEY_CAPACITY < 255, "test bounds");
    reset(); uint8_t key[16]; key_bytes(0, key); uint16_t index = 0xffff;
    assert(bt_mesh_provisioner_local_app_key_add(key, 0, NULL) == -EINVAL);
    index = 0x1000; assert(bt_mesh_provisioner_local_app_key_add(key, 0, &index) == -EINVAL);
    index = 0xffff; assert(bt_mesh_provisioner_local_app_key_add(key, 2, &index) == -ENODEV);
    fail_alloc = true; assert(bt_mesh_provisioner_local_app_key_add(key, 0, &index) == -ENOMEM);
    fail_alloc = false; fail_aid = true;
    assert(bt_mesh_provisioner_local_app_key_add(key, 0, &index) == -EIO);
    fail_aid = false; fail_random = true;
    assert(bt_mesh_provisioner_local_app_key_add(NULL, 0, &index) == -EIO);
    assert(!allocations && !stores && bt_mesh.p_app_idx_next == 0);
    fail_random = false;
    for (unsigned i = 0; i < TEST_APPKEY_CAPACITY; ++i) {
        key_bytes(i, key); index = 0xffff;
        assert(bt_mesh_provisioner_local_app_key_add(key, 0, &index) == 0 && index == i);
        assert(bt_mesh.p_app_keys[i]->net_idx == 0 && bt_mesh.p_app_keys[i]->app_idx == i);
        assert(!memcmp(bt_mesh.p_app_keys[i]->keys[0].val, key, 16));
        int result = bt_mesh_provisioner_bind_local_model_app_idx(1, 1, 0x02e5, index);
        assert(result == (i < TEST_MODEL_CAPACITY ? 0 : -ENOMEM));
    }
    struct bt_mesh_app_key snapshot[TEST_APPKEY_CAPACITY];
    struct bt_mesh_model model_snapshot = model;
    for (unsigned i = 0; i < TEST_APPKEY_CAPACITY; ++i) snapshot[i] = *bt_mesh.p_app_keys[i];
    unsigned writes = stores, binds = model_stores;
    key_bytes(TEST_APPKEY_CAPACITY, key); index = 0xffff;
    assert(bt_mesh_provisioner_local_app_key_add(key, 0, &index) == -ENOMEM);
    assert(index == 0xffff && stores == writes && allocations == TEST_APPKEY_CAPACITY);
    index = 0; assert(bt_mesh_provisioner_local_app_key_add(key, 0, &index) == -EEXIST);
    key_bytes(0, key); index = 0xffff;
    assert(bt_mesh_provisioner_local_app_key_add(key, 0, &index) == 0 && index == 0);
    assert(bt_mesh_provisioner_bind_local_model_app_idx(1, 1, 0x02e5, index) == 0);
    assert(stores == writes && model_stores == binds);
    assert(bt_mesh_provisioner_bind_local_model_app_idx(2, 1, 0x02e5, index) == -ENODEV);
    assert(bt_mesh_provisioner_bind_local_model_app_idx(1, 2, 0x02e5, index) == -ENODEV);
    assert(bt_mesh_provisioner_bind_local_model_app_idx(1, 1, 0x02e5, 4095) == -ENODEV);
    // SDK deduplication is by key bytes, not by net_idx or the requested index.
    // A future coordinator MUST reconcile the returned index and actual subnet.
    index = 1234; assert(bt_mesh_provisioner_local_app_key_add(key, 1, &index) == 0 && index == 0);
    assert(bt_mesh.p_app_keys[0]->net_idx == 0);
    for (unsigned i = 0; i < TEST_APPKEY_CAPACITY; ++i)
        assert(!memcmp(&snapshot[i], bt_mesh.p_app_keys[i], sizeof(snapshot[i])));
    assert(!memcmp(&model_snapshot, &model, sizeof(model)) && stores == writes && model_stores == binds);
    reset();
    bt_mesh.p_app_idx_next = 0x1000; index = 0xffff;
    assert(bt_mesh_provisioner_local_app_key_add(key, 0, &index) == -EIO && !allocations && !stores);
    printf("PASS actual SDK AppKey=%d model-bindings=%d: capacity, exhaustion, no eviction, idempotence, errors; cross-subnet alias requires coordinator validation\n",
           TEST_APPKEY_CAPACITY, TEST_MODEL_CAPACITY);
    reset(); return 0;
}
