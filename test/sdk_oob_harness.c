// Executes the patched SDK's actual Start/authentication branch.
// The crypto/RF/RTOS dependencies are stubs: this is not an RF test.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#define BT_INFO(...) ((void)0)
#define BT_ERR(...) ((void)0)
#define CONFIG_BLE_MESH_PROV_EPA 0
#define PROV_AUTH_SIZE(link) 16
#define PROV_ALG_P256_CMAC_AES128 0
#define PROV_ALG_METHOD_MAX_NUM 2
#define AUTH_METHOD_NO_OOB 0
#define AUTH_METHOD_STATIC 1
#define AUTH_METHOD_OUTPUT 2
#define AUTH_METHOD_INPUT 3
#define PROV_ERR_NVAL_FMT 2
#define PROV_PUB_KEY 3
#define PROV_OOB_PUB_KEY 1
#define BLE_MESH_DISPLAY_STRING 16
#define BLE_MESH_BLINK 1
#define BLE_MESH_BEEP 2
#define BLE_MESH_VIBRATE 4
#define BLE_MESH_ENTER_STRING 8
#define WAIT_STRING 1
#define WAIT_NUMBER 2
typedef unsigned bt_mesh_output_action_t;
typedef unsigned bt_mesh_input_action_t;
static struct { uint8_t algorithm, public_key, auth[32], conf_inputs[145], expect; int flags[4]; } prov_link;
static struct { const uint8_t *static_val; uint8_t static_val_len, oob_pub_key, output_size, input_size;
  unsigned output_actions, input_actions; void (*oob_pub_key_cb)(void);
  int (*output_string)(char *); int (*output_number)(unsigned, uint32_t); int (*input)(unsigned, uint8_t);
} prov;
static int closed;
#define bt_mesh_prov_get() (&prov)
static void close_link(int reason) { closed = reason; }
static unsigned bt_mesh_prov_output_action(uint8_t x) { return 1u << x; }
static unsigned bt_mesh_prov_input_action(uint8_t x) { return 1u << x; }
static void bt_mesh_rand(void *v, unsigned n) { memset(v, 7, n); }
static void bt_mesh_atomic_set_bit(int *v, unsigned b) { v[b] = 1; }
static void sys_put_be32(uint32_t v, uint8_t *p) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
#include "sdk_oob_extracted.inc"
static void reset(const uint8_t *key, uint8_t n) {
  memset(&prov_link, 0, sizeof(prov_link)); memset(&prov, 0, sizeof(prov));
  prov.static_val = key; prov.static_val_len = n; closed = 0;
}
int main(void) {
  uint8_t key[32], start[5] = {0, 0, 1, 0, 0};
  for (unsigned i=0; i<sizeof(key); ++i) key[i] = (uint8_t)(i+1);
  reset(key,16); prov_start(start);
  assert(!closed && prov_link.expect == PROV_PUB_KEY && !memcmp(key,prov_link.auth,16));
  // Exhaust every Start-field mutation: authentication cannot downgrade.
  unsigned cases=0;
  for (unsigned field=0; field<5; ++field) for (unsigned v=0; v<256; ++v) {
    uint8_t trial[5]; memcpy(trial,start,5); if (v == start[field]) continue;
    trial[field]=(uint8_t)v; reset(key,16); prov_start(trial);
    assert(closed == PROV_ERR_NVAL_FMT && prov_link.expect == 0); ++cases;
  }
  for (unsigned n=1; n<=32; ++n) if (n!=16) {
    reset(key,(uint8_t)n); prov_start(start); assert(closed == PROV_ERR_NVAL_FMT);
  }
  reset(NULL,16); prov_start(start); assert(closed == PROV_ERR_NVAL_FMT);
  uint8_t old[5]={0}; reset(NULL,0); prov_start(old); assert(!closed);
  printf("PASS actual SDK Static OOB Start/auth: %u downgrade/malformed mutations rejected; legacy no-OOB unchanged outside recovery\n", cases);
  return 0;
}
