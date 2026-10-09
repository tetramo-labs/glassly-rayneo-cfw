/* Heap singleton for all CFW state.
 *
 * The module has no .data/.bss, so the one pointer to the heap context must
 * live in RAM the stock image already owns. It lives in the module's OWN entry
 * of RNLink's callback registry: rnlink_if_register_callback copies the 32-byte
 * callback table into a 24-entry table (0x2c bytes each) and stores the caller's
 * `sub` word at +0x20 and the table pointer at +0x24. Verified by disassembly
 * (2026-10-08): the `sub` word is read only by rnlink_if_register_callback (the
 * duplicate check) and rnlink_if_unregister_callback (a match key); every
 * dispatcher (data, conn/bond/multipoint state, device info, send done, file)
 * keys on +0x24 and calls a slot only when it is non-NULL. So the entry whose
 * +0x24 is &cfw_rn_cbs is ours, and its +0x20 word is a free pointer-sized cell
 * that nothing ever calls. The registry address is the literal both
 * rnlink_if_register_callback and rnlink_if_on_data_recv load (asserted by
 * build.py and emitted as FWA_rnlink_registry).
 *
 * Only the RNLink RX thread creates the context; the LVGL thread only peeks.
 */
#include "cfw.h"

#define RN_ENTRIES 24u
#define RN_STRIDE  0x2Cu
#define RN_OFF_SUB 0x20u
#define RN_OFF_CBS 0x24u

void cfw_zero(void *p, uint32_t n) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) b[i] = 0;
}

/* Our registry entry, or NULL before cfw_boot_register ran. */
uint8_t *cfw_rn_entry(void) {
    uint8_t *e = (uint8_t *)(uintptr_t)FWA_rnlink_registry;
    for (uint32_t i = 0; i < RN_ENTRIES; i++, e += RN_STRIDE) {
        if (*(const void *volatile *)(e + RN_OFF_CBS) == (const void *)&cfw_rn_cbs) return e;
    }
    return 0;
}

static customCfwContext **cfw_ctx_cell(void) {
    uint8_t *e = cfw_rn_entry();
    return e ? (customCfwContext **)(e + RN_OFF_SUB) : 0;
}

customCfwContext *cfw_ctx_peek(void) {
    customCfwContext **cell = cfw_ctx_cell();
    if (!cell) return 0;
    customCfwContext *c = *cell;
    if (!c || c->magic != CFW_CTX_MAGIC || c->ctx_size != (uint32_t)sizeof(*c)) return 0;
    return c;
}

customCfwContext *cfw_ctx(void) {
    customCfwContext *c = cfw_ctx_peek();
    if (c) return c;
    customCfwContext **cell = cfw_ctx_cell();
    if (!cell) return 0;
    c = (customCfwContext *)fw_memalign(8, (uint32_t)sizeof(*c));
    if (!c) return 0;
    cfw_zero(c, (uint32_t)sizeof(*c));
    c->magic = CFW_CTX_MAGIC;
    c->ctx_size = (uint32_t)sizeof(*c);
    c->store = (uint8_t *)fw_memalign(16, CFW_STORE_BYTES);
    if (!c->store) { fw_free(c); return 0; }
    *cell = c;   /* publish last: readers validate magic + size */
    return c;
}
