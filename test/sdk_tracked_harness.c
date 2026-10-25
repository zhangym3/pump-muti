/* Real patched SDK functions are extracted by run_sdk_tests.py.
 * Stubs model allocation, BTC delivery and core completion deterministically.
 * These are host tests, not a substitute for device/RTOS/radio fault injection. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
typedef int btc_ble_mesh_model_act_t;
typedef int esp_ble_mesh_dev_role_t;
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM, ESP_FAIL };
enum { BTC_BLE_MESH_ACT_MODEL_PUBLISH, BTC_BLE_MESH_ACT_SERVER_MODEL_SEND,
       BTC_BLE_MESH_ACT_CLIENT_MODEL_SEND, BTC_BLE_MESH_ACT_SERVER_MODEL_UPDATE_STATE };
enum { ESP_BLE_MESH_TRACKED_ACCEPTED = 1, ESP_BLE_MESH_TRACKED_REJECTED,
       ESP_BLE_MESH_TRACKED_SAR_ENDED, ESP_BLE_MESH_TRACKED_SAR_TIMEOUT };
#define BTC_SIG_API_CALL 1
#define BTC_PID_MODEL 2
#define ROLE_NODE 0
#define BT_STATUS_SUCCESS 0
#define ESP_BLE_MESH_KEY_UNUSED 0xffff
#define ESP_BLE_MESH_ADDR_UNASSIGNED 0
#define ESP_BLE_MESH_TAG_SEND_SEGMENTED 1
#define ESP_BLE_MESH_MIC_LONG 8
#define ESP_BLE_MESH_MIC_SHORT 4
#define BLE_MESH_MIC_SHORT 4
#define ESP_BLE_MESH_SDU_MAX_LEN 384
#define ESP_BLE_MESH_TX_SDU_MAX 444
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define BIT(n) (1U << (n))
#define TRANS_SEQ_ZERO_MASK 0x1fffU
#define SAR_DIAG_LOG(...) ((void)0)
#define BLE_MESH_ADDR_IS_UNICAST(a) ((a) > 0 && (a) < 0x8000)
#define BT_ERR(...) ((void)0)
#define BT_DBG(...) ((void)0)
static bool host_enabled = true;
#define ESP_BLE_HOST_STATUS_CHECK(s) do { if (!host_enabled) return ESP_FAIL; } while (0)
#define ESP_BLE_HOST_STATUS_ENABLED 1

struct net_buf_simple { uint8_t data[512]; size_t len, size; };
typedef struct { struct net_buf_simple *msg; bool send_rel, send_szmic; } model_pub;
typedef struct bt_mesh_model { model_pub *pub; } esp_ble_mesh_model_t;
typedef struct bt_mesh_msg_ctx { uint16_t addr, net_idx, app_idx; uint8_t send_tag, send_szmic; } esp_ble_mesh_msg_ctx_t;
typedef struct { uint8_t value; } esp_ble_mesh_server_state_value_t;
typedef struct {
    struct { esp_ble_mesh_model_t *model; } model_publish;
    struct { esp_ble_mesh_model_t *model; esp_ble_mesh_msg_ctx_t *ctx; bool need_rsp;
             uint32_t opcode; uint16_t length; uint8_t *data; int32_t msg_timeout; uint32_t ticket; } model_send;
    struct { esp_ble_mesh_server_state_value_t *value; } model_update_state;
} btc_ble_mesh_model_args_t;
typedef struct { int sig, pid, act; void *arg; } btc_msg_t;
struct bt_mesh_send_cb { void (*start)(uint16_t,int,void *); void (*end)(int,void *); };
struct seg_tx {
    const struct bt_mesh_send_cb *cb;
    void *cb_data;
    uint64_t seq_auth;
    uint16_t src, dst;
    uint8_t seg_n;
    void *seg[32];
};
static struct seg_tx seg_tx[8];
static int allocations, fail_allocation, live_allocations, core_error, core_calls, legacy_calls;
static int buffer_allocations, transfer_error;
static bool buffer_fail, early_end;
static btc_msg_t pending_msg;
static btc_ble_mesh_model_args_t pending_arg;
static bool pending;
static struct { uint32_t ticket; uint8_t type; int error; } events[16];
static unsigned event_count;

static void *bt_mesh_malloc(size_t size) {
    if (++allocations == fail_allocation) return NULL;
    void *p = malloc(size); assert(p); ++live_allocations; return p;
}
static void bt_mesh_free(void *p) { if (p) { --live_allocations; free(p); } }
static struct net_buf_simple *bt_mesh_alloc_buf(size_t size) {
    static struct net_buf_simple buffer;
    if (buffer_fail) return NULL;
    assert(size <= sizeof(buffer.data)); buffer.len=0; buffer.size=size;
    ++buffer_allocations; return &buffer;
}
static void bt_mesh_free_buf(struct net_buf_simple *buf) { assert(buf); --buffer_allocations; }
static void net_buf_simple_add_mem(struct net_buf_simple *buf, const void *data, size_t len) {
    assert(buf->len+len <= buf->size); if (len) memcpy(buf->data+buf->len,data,len); buf->len+=len;
}
static void bt_mesh_model_msg_init(struct net_buf_simple *buf, uint32_t opcode) { buf->len=0; (void)opcode; }
static esp_err_t esp_ble_mesh_model_msg_opcode_init(uint8_t *data, uint32_t opcode) {
    unsigned len=opcode<0x100?1:opcode<0x10000?2:3;
    for (unsigned i=0;i<len;++i) data[i]=(uint8_t)(opcode>>(8*i));
    return ESP_OK;
}
static void btc_ble_mesh_model_send_comp_cb(esp_ble_mesh_model_t *m, esp_ble_mesh_msg_ctx_t *c, uint32_t op, int err) {
    ++legacy_calls;
}
static void esp_ble_mesh_tracked_send_event(uint32_t ticket, uint8_t type, int error) {
    assert(ticket && event_count<ARRAY_SIZE(events));
    events[event_count].ticket=ticket; events[event_count].type=type; events[event_count++].error=error;
}
static void seg_tx_reset(struct seg_tx *tx) { memset(tx,0,sizeof(*tx)); }
static int bt_mesh_model_send(struct bt_mesh_model *, struct bt_mesh_msg_ctx *, struct net_buf_simple *, const struct bt_mesh_send_cb *, void *);
static int btc_transfer_context(btc_msg_t *, void *, size_t, void (*)(btc_msg_t *,void *,void *), void (*)(btc_msg_t *));

#include "sdk_extracted.inc"

static int btc_transfer_context(btc_msg_t *msg, void *arg, size_t size,
                               void (*copy)(btc_msg_t *,void *,void *), void (*release)(btc_msg_t *)) {
    if (transfer_error) return transfer_error;
    assert(!pending && size==sizeof(pending_arg));
    pending_msg=*msg; memcpy(&pending_arg,arg,size); pending_msg.arg=&pending_arg;
    copy(&pending_msg,&pending_arg,arg); pending=true;
    return BT_STATUS_SUCCESS;
}
static int bt_mesh_model_send(struct bt_mesh_model *model, struct bt_mesh_msg_ctx *ctx,
                              struct net_buf_simple *buf, const struct bt_mesh_send_cb *cb, void *data) {
    ++core_calls; assert(ctx && model && buf->len==15 && buf->data[3]==0xa5);
    if (core_error) return core_error;
    seg_tx[0]=(struct seg_tx){.cb=cb,.cb_data=data,.dst=ctx->addr};
    if (early_end && cb) seg_tx_complete(&seg_tx[0],0);
    return 0;
}
static void deliver(void) {
    assert(pending); pending=false;
    handle_server(&pending_arg); btc_ble_mesh_model_arg_deep_free(&pending_msg);
    assert(live_allocations==0 && buffer_allocations==0);
}
static void reset(void) {
    assert(!pending && live_allocations==0 && buffer_allocations==0);
    allocations=fail_allocation=core_error=core_calls=legacy_calls=transfer_error=0;
    buffer_fail=early_end=false; host_enabled=true; event_count=0; bt_mesh_tx_reset();
}
static esp_ble_mesh_model_t model;
static esp_ble_mesh_msg_ctx_t context={.addr=5,.net_idx=0,.app_idx=0};
static uint8_t payload[12]={0xa5};
static int send_tracked(uint32_t ticket) {
    return esp_ble_mesh_server_model_send_msg_tracked(&model,&context,0xc10001,sizeof(payload),payload,ticket);
}
static void event(unsigned i,uint32_t ticket,uint8_t type,int error) {
    if (i>=event_count || events[i].ticket!=ticket || events[i].type!=type || events[i].error!=error) {
        if (i<event_count) {
            fprintf(stderr, "event[%u] expected ticket=%u type=%u error=%d, got ticket=%u type=%u error=%d (count=%u)\n",
                    i, ticket, type, error, events[i].ticket, events[i].type, events[i].error, event_count);
        } else {
            fprintf(stderr, "event[%u] expected ticket=%u type=%u error=%d, got count=%u\n",
                    i, ticket, type, error, event_count);
        }
        assert(0);
    }
}

int main(void) {
    reset(); assert(send_tracked(0)==ESP_ERR_INVALID_ARG && !pending);
    context.net_idx=ESP_BLE_MESH_KEY_UNUSED; assert(send_tracked(1)==ESP_ERR_INVALID_ARG); context.net_idx=0;
    assert(esp_ble_mesh_server_model_send_msg_tracked(NULL,&context,1,0,NULL,1)==ESP_ERR_INVALID_ARG);
    assert(esp_ble_mesh_server_model_send_msg_tracked(&model,&context,1,1,NULL,1)==ESP_ERR_INVALID_ARG);
    context.addr=0; assert(send_tracked(1)==ESP_ERR_INVALID_ARG); context.addr=5;
    host_enabled=false; assert(send_tracked(1)==ESP_FAIL && !pending);
    reset(); fail_allocation=1; assert(send_tracked(1)==ESP_ERR_NO_MEM && !pending && live_allocations==0);
    reset(); transfer_error=1; assert(send_tracked(1)==ESP_FAIL && !pending && live_allocations==0);
    puts("PASS extracted SDK API invalid args/host disabled/allocation/BTC queue rejection");

    for (int fail=2;fail<=3;++fail) {
        reset(); fail_allocation=fail; assert(send_tracked(17)==ESP_OK); deliver();
        assert(core_calls==0 && event_count==1); event(0,17,ESP_BLE_MESH_TRACKED_REJECTED,-ENOMEM);
    }
    reset(); buffer_fail=true; assert(send_tracked(18)==ESP_OK); deliver();
    assert(core_calls==0 && event_count==1); event(0,18,ESP_BLE_MESH_TRACKED_REJECTED,-ENOMEM);
    reset(); core_error=-EBUSY; assert(send_tracked(19)==ESP_OK); deliver();
    assert(core_calls==1 && event_count==1); event(0,19,ESP_BLE_MESH_TRACKED_REJECTED,-EBUSY);
    puts("PASS extracted SDK BTC data/ctx copy failure, buffer failure, core EBUSY (no leaks)");

    reset(); assert(send_tracked(20)==ESP_OK); assert(event_count==0); deliver();
    event(0,20,ESP_BLE_MESH_TRACKED_ACCEPTED,0); assert(seg_tx[0].cb);
    seg_tx_complete(&seg_tx[0],0); assert(!seg_tx[0].cb); event(1,20,ESP_BLE_MESH_TRACKED_SAR_ENDED,0);
    seg_tx_complete(&seg_tx[0],0); assert(event_count==2);
    reset(); early_end=true; assert(send_tracked(UINT32_MAX)==ESP_OK); deliver();
    event(0,UINT32_MAX,ESP_BLE_MESH_TRACKED_SAR_ENDED,0); event(1,UINT32_MAX,ESP_BLE_MESH_TRACKED_ACCEPTED,0);
    reset(); assert(send_tracked(21)==ESP_OK); deliver(); seg_tx_complete(&seg_tx[0],-ETIMEDOUT);
    event(1,21,ESP_BLE_MESH_TRACKED_SAR_TIMEOUT,-ETIMEDOUT);
    reset(); assert(send_tracked(22)==ESP_OK); deliver(); seg_tx_complete(&seg_tx[0],-EIO);
    assert(event_count==2); event(0,22,ESP_BLE_MESH_TRACKED_ACCEPTED,0);
    event(1,22,ESP_BLE_MESH_TRACKED_SAR_ENDED,-EIO);
    puts("PASS extracted SDK true end/duplicate complete/end-before-accepted/timeout/ticket UINT32_MAX");

    reset(); assert(send_tracked(22)==ESP_OK); deliver(); bt_mesh_tx_reset(); assert(event_count==1 && !seg_tx[0].cb);
    reset(); assert(send_tracked(23)==ESP_OK); deliver(); bt_mesh_tx_reset_single(context.addr); assert(event_count==1 && !seg_tx[0].cb);
    puts("PASS extracted SDK reset/reset_single omit end (governor must freeze; cannot assume release)");

    reset(); assert(esp_ble_mesh_server_model_send_msg(&model,&context,0xc10001,sizeof(payload),payload)==ESP_OK);
    deliver(); assert(legacy_calls==1 && event_count==0 && !seg_tx[0].cb);
    puts("PASS extracted SDK legacy send uses ticket zero and old completion callback");
    return 0;
}
