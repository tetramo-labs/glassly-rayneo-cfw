#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cfw.h"
static customCfwContext context, *active;
static int stock_calls, pushes, flushes, writes, applies, available = 1, fail_key;
static int32_t prefs[0x30];
static uint8_t packet[7];
customCfwContext *cfw_ctx_peek(void) { return active; }
static void test_notify(void *monitor, uint32_t direction) { assert(monitor == &context); (void)direction; stock_calls++; }
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
#define fw_notify_head test_notify
#define fw_tick test_tick
#define fw_sysstate_monitor test_monitor
#define fw_pref_read test_read
#define fw_pref_write test_write
#define fw_apply_wakeup test_apply
#include "input.c"
#include "head.c"
int main(void) {
    cfw_head_hook(&context, 2); assert(stock_calls == 1 && pushes == 0);
    active = &context;
    cfw_head_hook(&context, 2); assert(stock_calls == 2 && pushes == 0);
    context.queried = 1; /* page remains closed: looking up must still open it */
    cfw_head_hook(&context, 2);
    assert(pushes == 1 && flushes == 1);
    const uint8_t up[] = {7, 12, 2, 0x78, 0x56, 0x34, 0x12};
    assert(!memcmp(packet, up, sizeof(up)));
    cfw_head_hook(&context, 1); assert(pushes == 2 && packet[1] == 15 && flushes == 2);
    cfw_head_hook(&context, 0); cfw_head_hook(&context, 3);
    assert(stock_calls == 6 && pushes == 2 && flushes == 2);
    context.uplink_tail = UPLINK_QUEUE - 2; /* protect reply reserve */
    cfw_head_hook(&context, 2); assert(stock_calls == 7 && pushes == 2 && flushes == 3);
    prefs[0x2f] = 73; /* crown must survive head configuration */
    cfw_head_angle(45); assert(prefs[0x2d] == 1 && prefs[0x2e] == 45 && writes == 2 && applies == 1);
    cfw_head_angle(45); assert(writes == 2 && applies == 2);
    cfw_head_angle(255); assert(prefs[0x2e] == 90 && writes == 3 && applies == 3);
    cfw_head_angle(0); assert(prefs[0x2e] == 0 && writes == 4 && applies == 4);
    fail_key = 0x2e; cfw_head_angle(40); assert(prefs[0x2e] == 0 && applies == 4);
    fail_key = 0x2d; prefs[0x2d] = 0; cfw_head_angle(0); assert(applies == 4);
    available = 0; int count = writes; cfw_head_angle(50); assert(writes == count && applies == 4);
    assert(prefs[0x2f] == 73);
    puts("head: stock forwarding, query gating, closed-page uplink, reply reserve, angle bounds and preference failures passed");
}
