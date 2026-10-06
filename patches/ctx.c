/* Heap singleton for all CFW state.
 *
 * The module has no .data/.bss, so the one pointer to the heap context must live
 * somewhere the stock image already owns. We reuse the AppListView tail: build.py
 * bumps the launcher's AppListView allocation from 0xdc to 0xfc bytes, giving a
 * 32-byte tail at app+0xdc that the stock code never touches. The first 8 bytes
 * hold {magic, ctx*}; the context itself is a separate heap block.
 *
 * UNVERIFIED (no on-device disassembly was possible in this build environment):
 * the launcher/app pointer chain below (g_launcher @ 0x19a1f954 -> +0x3c manager
 * -> +0x10 AppListView) is taken from the prior firmware analysis. It is only
 * ever dereferenced after a null check, and a null anywhere yields "no context"
 * rather than a fault. See docs/HOOKS.md.
 */
#include "cfw.h"

#define G_LAUNCHER_PTR 0x19A1F954u
#define OFF_MANAGER 0x3Cu
#define OFF_APP 0x10u
#define TAIL_OFFSET 0xDCu

typedef struct { uint32_t magic; customCfwContext *ctx; } cfw_tail;

void cfw_zero(void *p, uint32_t n) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) b[i] = 0;
}

static cfw_tail *cfw_tail_ptr(void) {
    void *launcher = *(void **)(uintptr_t)G_LAUNCHER_PTR;
    if (!launcher) return 0;
    void *manager = *(void **)((uint8_t *)launcher + OFF_MANAGER);
    if (!manager) return 0;
    void *app = *(void **)((uint8_t *)manager + OFF_APP);
    if (!app) return 0;
    return (cfw_tail *)((uint8_t *)app + TAIL_OFFSET);
}

customCfwContext *cfw_ctx_peek(void) {
    cfw_tail *t = cfw_tail_ptr();
    if (!t || t->magic != CFW_CTX_MAGIC) return 0;
    customCfwContext *c = t->ctx;
    if (!c || c->magic != CFW_CTX_MAGIC || c->ctx_size != (uint32_t)sizeof(*c)) return 0;
    return c;
}

customCfwContext *cfw_ctx(void) {
    customCfwContext *c = cfw_ctx_peek();
    if (c) return c;
    cfw_tail *t = cfw_tail_ptr();
    if (!t) return 0;
    c = (customCfwContext *)fw_memalign(8, (uint32_t)sizeof(*c));
    if (!c) return 0;
    cfw_zero(c, (uint32_t)sizeof(*c));
    c->magic = CFW_CTX_MAGIC;
    c->ctx_size = (uint32_t)sizeof(*c);
    c->store = (uint8_t *)fw_memalign(16, CFW_STORE_BYTES);
    if (!c->store) { fw_free(c); return 0; }
    t->ctx = c;
    t->magic = CFW_CTX_MAGIC;   /* publish last: readers key on this */
    return c;
}
