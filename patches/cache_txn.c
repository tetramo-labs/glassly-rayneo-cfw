/* PUT / inline SHOW rollback. Metadata costs about 6 KiB per transaction;
 * objects and overwritten live store granules are saved only when touched.
 * Unused store bytes and append-only upload tails need no backup. No canvas,
 * timer or uplink side effects run until validation has committed. */
#include "cfw.h"

typedef struct cfw_object_undo {
    struct cfw_object_undo *next;
    cfw_object *target;
    cfw_object value;
} cfw_object_undo;
typedef struct cfw_store_undo {
    struct cfw_store_undo *next;
    uint32_t offset, length;
    uint8_t data[];
} cfw_store_undo;
typedef struct {
    cfw_journal_entry entries[CACHE_JOURNAL];
    uint32_t revision, lost;
    uint8_t head, count, mutated;
} cfw_journal_undo;
struct cfw_transaction {
    cfw_asset assets[CACHE_ASSETS];
    uint8_t map[STORE_GRANULES_MAX / 8];
    uint8_t saved[STORE_GRANULES_MAX / 8];
    uint32_t used, clock, evicted;
    cfw_journal_undo journal[2];
    cfw_object_undo *objects;
    cfw_store_undo *store;
    uint8_t failed;
};

static int txn_begin(customCfwContext *ctx) {
    struct cfw_transaction *t = fw_memalign(8, sizeof(*t));
    if (!t) return 0;
    cfw_zero(t, sizeof(*t));
    memcpy(t->assets, ctx->assets, sizeof(t->assets));
    memcpy(t->map, ctx->store_map, sizeof(t->map));
    t->used = ctx->store_used; t->clock = ctx->use_clock; t->evicted = ctx->evicted;
    for (unsigned e = 0; e < 2; e++) {
        cfw_lens *L = &ctx->lens[e];
        cfw_journal_undo *j = &t->journal[e];
        memcpy(j->entries, L->journal, sizeof(j->entries));
        j->revision = L->revision; j->lost = L->journal_lost_rev;
        j->head = L->journal_head; j->count = L->journal_count; j->mutated = L->mutated;
    }
    ctx->transaction = t;
    return 1;
}

static int txn_save_object(customCfwContext *ctx, cfw_object *o) {
    struct cfw_transaction *t = ctx->transaction;
    if (!t) return 1;
    if (t->failed) return 0;
    for (cfw_object_undo *u = t->objects; u; u = u->next) if (u->target == o) return 1;
    cfw_object_undo *u = fw_memalign(8, sizeof(*u));
    if (!u) { t->failed = 1; return 0; }
    u->target = o; u->value = *o; u->next = t->objects; t->objects = u;
    return 1;
}

/* Before reusing a freed allocation, preserve any bytes live at entry. The
 * saved bitmap bounds backup storage to the original store's used size. */
static int txn_save_store(customCfwContext *ctx, uint32_t first, uint32_t count) {
    struct cfw_transaction *t = ctx->transaction;
    if (!t) return 1;
    if (t->failed) return 0;
    uint32_t end = first + count;
    for (uint32_t g = first; g < end;) {
        uint8_t bit = (uint8_t)(1u << (g & 7u));
        if (!(t->map[g >> 3] & bit) || (t->saved[g >> 3] & bit)) { g++; continue; }
        uint32_t start = g++;
        while (g < end && (t->map[g >> 3] & (1u << (g & 7u))) && !(t->saved[g >> 3] & (1u << (g & 7u)))) g++;
        uint32_t bytes = (g - start) * STORE_GRANULE;
        cfw_store_undo *u = fw_memalign(8, sizeof(*u) + bytes);
        if (!u) { t->failed = 1; return 0; }
        u->offset = start * STORE_GRANULE; u->length = bytes;
        memcpy(u->data, ctx->store + u->offset, bytes);
        u->next = t->store; t->store = u;
        for (uint32_t k = start; k < g; k++) t->saved[k >> 3] |= (uint8_t)(1u << (k & 7u));
    }
    return 1;
}

static void txn_end(customCfwContext *ctx, int commit) {
    struct cfw_transaction *t = ctx->transaction;
    if (!t) return;
    if (!commit) {
        memcpy(ctx->assets, t->assets, sizeof(t->assets));
        memcpy(ctx->store_map, t->map, sizeof(t->map));
        ctx->store_used = t->used; ctx->use_clock = t->clock; ctx->evicted = t->evicted;
        for (unsigned e = 0; e < 2; e++) {
            cfw_lens *L = &ctx->lens[e]; cfw_journal_undo *j = &t->journal[e];
            memcpy(L->journal, j->entries, sizeof(j->entries));
            L->revision = j->revision; L->journal_lost_rev = j->lost;
            L->journal_head = j->head; L->journal_count = j->count; L->mutated = j->mutated;
        }
    }
    while (t->objects) {
        cfw_object_undo *u = t->objects; t->objects = u->next;
        if (!commit) *u->target = u->value;
        else u->target->pinned = u->value.pinned;
        fw_free(u);
    }
    while (t->store) {
        cfw_store_undo *u = t->store; t->store = u->next;
        if (!commit) memcpy(ctx->store + u->offset, u->data, u->length);
        fw_free(u);
    }
    if (commit) for (unsigned i = 0; i < CACHE_ASSETS; i++) ctx->assets[i].pinned = 0;
    ctx->transaction = 0;
    fw_free(t);
}
