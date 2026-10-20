#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cfw.h"
static customCfwContext context, *active;
static int stock_calls, pushes, flushes, writes, applies, available = 1, fail_key;
static int32_t prefs[0x30];
static uint8_t packet[7];
customCfwContext *cfw_ctx_peek(void) { return active; }
static int rebonding, stock_wakes;
static void test_handle(void *handler, const void *arg) {
    assert(handler == &context); stock_calls++;
    if (rebonding) return; /* reproduces the stock early return that defeated v12 */
    const TIONativeMessage *message = arg;
    if (message && message->reserved == 2) stock_wakes++;
}
static void deliver(uint32_t direction) {
    TIONativeMessage message = {0}; message.id = 0x1d; message.reserved = direction;
    cfw_head_hook(&context, &message);
}
static uint32_t test_tick(void) { return 0x12345678u; }
static void *test_monitor(void) { return available ? &context : NULL; }
static int test_read(uint32_t key, int32_t *value) { *value = prefs[key]; return 0; }
static int test_write(uint32_t key, int32_t value) {
    assert(key == 0x2d || key == 0x2e); writes++;
    if ((int)key == fail_key) return -1;
    prefs[key] = value; return 0;
}
static void test_apply(void *monitor) { assert(monitor == &context); applies++; }
int cfw_uplink_push(customCfwContext *ctx, const uint8_t *data, uint8_t len) {
    assert(ctx == &context && len == sizeof(packet)); memcpy(packet, data, len); pushes++; return 1;
}
void cfw_uplink_flush(customCfwContext *ctx) { assert(ctx == &context); flushes++; }
#define fw_handle_head test_handle
#define fw_tick test_tick
#define fw_sysstate_monitor test_monitor
#define fw_pref_read test_read
#define fw_pref_write test_write
#define fw_apply_wakeup test_apply
#include "input.c"
#include "head.c"
static void test_mallinfo(cfw_mallinfo *info) { memset(info, 0, sizeof(*info)); }
#define fw_mallinfo test_mallinfo
uint8_t *cfw_rn_entry(void) { return NULL; }
uint8_t cfw_font_line_height(void) { return 16; }
void cfw_zero(void *p, uint32_t n) { memset(p, 0, n); }
#include "ident.c"
int main(void) {
    deliver(2); assert(stock_calls == 1 && pushes == 0);
    active = &context;
    deliver(2); assert(stock_calls == 2 && pushes == 0);
    rebonding = 1; int wakes = stock_wakes;
    context.queried = 1; /* page remains closed: looking up must still open it */
    deliver(2);
    assert(pushes == 1 && flushes == 1);
    const uint8_t up[] = {7, 12, 2, 0x78, 0x56, 0x34, 0x12};
    assert(!memcmp(packet, up, sizeof(up)));
    deliver(1); assert(pushes == 2 && packet[1] == 15 && flushes == 2);
    deliver(0); deliver(3);
    assert(stock_calls == 6 && pushes == 2 && flushes == 2);
    context.uplink_tail = UPLINK_QUEUE - 2; /* protect reply reserve */
    deliver(2); assert(stock_calls == 7 && pushes == 2 && flushes == 3);
    assert(stock_wakes == wakes && context.head_events == 3 && context.head_direction == 2);
    TIONativeMessage wrong = {0}; wrong.id = 0x1c; wrong.reserved = 2;
    cfw_head_hook(&context, &wrong); cfw_head_hook(&context, NULL);
    assert(stock_calls == 9 && pushes == 2 && context.head_events == 3);
    prefs[0x2f] = 73; /* crown must survive head configuration */
    cfw_head_angle(&context, 45); assert(context.head_config_status == 1 && prefs[0x2d] == 1 && prefs[0x2e] == 45 && writes == 2 && applies == 1);
    cfw_head_angle(&context, 45); assert(writes == 2 && applies == 2);
    cfw_head_angle(&context, 255); assert(prefs[0x2e] == 90 && writes == 3 && applies == 3);
    cfw_head_angle(&context, 0); assert(prefs[0x2e] == 0 && writes == 4 && applies == 4);
    fail_key = 0x2e; cfw_head_angle(&context, 40); assert(context.head_config_status == 3 && prefs[0x2e] == 0 && applies == 4);
    fail_key = 0x2d; prefs[0x2d] = 0; cfw_head_angle(&context, 0); assert(context.head_config_status == 4 && applies == 4);
    available = 0; int count = writes; cfw_head_angle(&context, 50); assert(context.head_config_status == 2 && writes == count && applies == 4);
    assert(prefs[0x2f] == 73);
    uint8_t diag[64]; memset(diag, 0xa5, sizeof(diag));
    assert(cfw_diag_fill(diag, DIAG_SRC_UI, &context) == 58);
    assert(diag[0] == RPL_DIAG && diag[51] == 3 && diag[52] == 0 && diag[53] == 0 && diag[54] == 0);
    assert(diag[55] == 2 && diag[56] == 50 && diag[57] == 2 && diag[58] == 0xa5);
    assert(cfw_diag_fill(diag, DIAG_SRC_RX, NULL) == 58);
    for (int i = 51; i < 58; i++) assert(diag[i] == 0);
    puts("head: stock forwarding, query gating, closed-page uplink, reply reserve, angle bounds and preference failures passed");
}
