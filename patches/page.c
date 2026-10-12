/* The Glassly page: while open, the module owns the panel.
 *
 * Revision 5 draws nothing through LVGL. Each lens has an L8 canvas the scene
 * renders into (scene.c); the display thread converts both into the LCDC's
 * per-eye 4-bit buffers and presents them (disp.c, cfw_lcdc_hook) whenever
 * the page asks for a present (a pan request on the framebuffer device, the
 * same ioctls LVGL's flush issues). The stock LVGL flush is parked with the
 * driver's refr_pass switch for as long as the page is open, so the
 * launcher's repaints never reach the panel; on close the launcher screen is
 * invalidated and the stock path resumes. A black placeholder object on the
 * launcher screen remains only to take key focus (input.c).
 *
 * Everything here runs on the launcher UI thread (reached from the message
 * pump, never from the RX callback). A phone-initiated open is gated on the
 * launcher being idle; the carousel-item entry ("Glassly") is deferred to a
 * device-validated build (see docs/HOOKS.md).
 */
#include "cfw.h"

static void cfw_timer_cb(void *timer) {
    (void)timer;
    customCfwContext *ctx = cfw_ctx_peek();
    if (ctx) cfw_page_tick(ctx);
}

void cfw_page_ensure_ui(customCfwContext *ctx) {
    if (!ctx->page_root) {
        void *disp = fw_display_default();
        void *screen = disp ? fw_screen_active(disp) : 0;
        if (!screen) return;
        void *root = fw_obj_create(screen);
        if (!root) return;
        /* a bare container: the theme's border, padding, radius and background
         * drew a frame around the page; it is never presented now, but it
         * holds the key focus and hides the launcher from LVGL's own view */
        fw_obj_remove_style_all(root);
        fw_obj_remove_flag(root, LV_FLAG_SCROLLABLE);
        fw_obj_set_scrollbar_mode(root, LV_SCROLLBAR_OFF);
        fw_obj_set_size(root, CFW_W, CFW_H);
        fw_obj_set_pos(root, 0, 0);
        fw_obj_add_flag(root, LV_FLAG_HIDDEN);
        ctx->page_root = root;
    }
    for (unsigned e = 0; e < 2; e++) {
        cfw_lens *L = &ctx->lens[e];
        L->bit = e ? LENS_RIGHT : LENS_LEFT;
        if (L->canvas) continue;
        uint8_t *buf = (uint8_t *)fw_memalign(16, (uint32_t)CFW_W * CFW_H);
        if (!buf) continue;        /* disp.c shows the other lens's canvas in both eyes */
        cfw_zero(buf, (uint32_t)CFW_W * CFW_H);
        L->canvas = buf;
        L->base = (uint8_t *)fw_memalign(16, (uint32_t)CFW_W * CFW_H);   /* optional: NULL = full renders */
        L->base_valid = 0;
    }
    cfw_page_ensure_timer(ctx);
    if (!ctx->screen_cb_registered) {
        ctx->screen_cb_registered = 1;
        fw_screen_state_register((void *)cfw_screen_state_cb, "glassly", 0, ctx);
    }
}

/* Display manager thread: the screen went on. Just flag it; the page timer
 * (LVGL thread) re-presents or reopens the page. */
void cfw_screen_state_cb(int old_state, int new_state, int reason) {
    (void)reason;
    customCfwContext *ctx = cfw_ctx_peek();
    if (ctx && !old_state && new_state) ctx->wake_pending = 1;
}

/* The 25 ms service timer also drains the uplink queue, so it must exist as
 * soon as the first packet is handled (IDENT is answered before any page). */
void cfw_page_ensure_timer(customCfwContext *ctx) {
    if (!ctx->timer) ctx->timer = fw_timer_create((void *)cfw_timer_cb, 25, 0);
}

int cfw_page_open(customCfwContext *ctx, uint8_t *reason) {
    /* No paired() gate: real_get_rnlink_paired_status is true only for a
     * device-info pair state of 2, which a guest-authenticated phone never
     * reaches (every PAGE_OPEN was refused with reason 2 on hardware,
     * 2026-10-09). The handshake that delivered this packet is the proof. */
    if (!fw_business_idle()) { *reason = PAGE_BUSY; return 0; }
    cfw_page_ensure_ui(ctx);
    if (!ctx->page_root || (!ctx->lens[0].canvas && !ctx->lens[1].canvas)) { *reason = PAGE_BUSY; return 0; }
    fw_obj_remove_flag(ctx->page_root, LV_FLAG_HIDDEN);
    ctx->page_open = 1;
    ctx->crown_accum = 0;
    ctx->keyq_r = ctx->keyq_w = 0;
    cfw_input_wheel_listen(ctx, 1);
    cfw_input_btn_listen(ctx);
    ctx->lease_deadline = fw_tick() + PAGE_LEASE_MS;
    fw_report_activity();
    /* take the panel: park the stock flush, present our (black) canvases */
    cfw_disp_pass(1);
    ctx->disp_pattern = 0;
    ctx->disp_own = 1;
    ctx->disp_pan_pending = 0;
    cfw_page_present(ctx);
    *reason = PAGE_OK;
    return 1;
}

void cfw_page_close(customCfwContext *ctx, uint8_t reason) {
    if (reason == PAGE_LEASE) cfw_compass_stop(ctx);    /* the phone is gone: no stream for nobody */
    ctx->disp_own = 0;
    ctx->disp_pattern = 0;
    cfw_anim_stop(ctx);
    cfw_input_wheel_listen(ctx, 0);
    if (ctx->page_root) fw_obj_add_flag(ctx->page_root, LV_FLAG_HIDDEN);
    /* A lease that ran out while the glasses slept (no keepalives reach a
     * dozing link) leaves the scene retained: the next wake reopens the page
     * on it until the phone is back (page on wake, as the G2 takeover). A
     * close the phone or the user asked for means it. */
    ctx->wake_takeover = reason == PAGE_LEASE && !ctx->page_by_takeover && (ctx->lens[0].last_show_valid || ctx->lens[1].last_show_valid);
    ctx->page_by_takeover = 0;
    for (unsigned e = 0; e < 2; e++) {
        cfw_lens *L = &ctx->lens[e];
        if (L->canvas) cfw_zero(L->canvas, (uint32_t)CFW_W * CFW_H);
    }
    ctx->page_open = 0;
    /* give the panel back: the stock flush resumes and repaints the launcher */
    cfw_disp_pass(0);
    void *disp = fw_display_default();
    void *screen = disp ? fw_screen_active(disp) : 0;
    if (screen) fw_obj_invalidate(screen);
    cfw_send_page_state(ctx, 0, reason);
}

/* Screen on: an open page is presented again (the stock's power-on sequence
 * may have left its own frame on the panel); a page the lease closed during
 * sleep is reopened on its retained scene and announced to the phone, whose
 * session resumes keepalives or replaces the scene, and the lease closes it
 * again if nobody comes. */
static void cfw_page_wake(customCfwContext *ctx) {
    if (ctx->page_open) {
        ctx->disp_dirty = 1;
        cfw_page_present(ctx);
        return;
    }
    if (!ctx->wake_takeover) return;
    ctx->wake_takeover = 0;
    uint8_t reason;
    if (!cfw_page_open(ctx, &reason)) return;
    ctx->page_by_takeover = 1;
    for (unsigned e = 0; e < 2; e++) {
        cfw_lens *L = &ctx->lens[e];
        if (L->last_show_valid) cfw_scene_render(ctx, L);
    }
    cfw_send_page_state(ctx, 1, PAGE_OK);
}

/* The page timer does not run while the glasses are link-idle, so a lease
 * that ran out during sleep is only noticed on wake: that is the takeover
 * case too, kept in place (no close, no flash of the stock home) for one
 * more lease, once per phone session. */
#define LEASE_SLEPT_MS 5000u

void cfw_page_tick(customCfwContext *ctx) {
    if (ctx->wake_pending) { ctx->wake_pending = 0; cfw_page_wake(ctx); }
    cfw_compass_tick(ctx);
    cfw_uplink_flush(ctx);
    if (!ctx->page_open) return;
    cfw_input_poll(ctx);
    cfw_anim_tick(ctx);
    uint32_t now = fw_tick();
    if ((int32_t)(now - ctx->lease_deadline) >= 0) {
        int scene = ctx->lens[0].last_show_valid || ctx->lens[1].last_show_valid;
        if (now - ctx->lease_deadline > LEASE_SLEPT_MS && scene && !ctx->page_by_takeover) {
            ctx->page_by_takeover = 1;
            ctx->lease_deadline = now + PAGE_LEASE_MS;
            ctx->disp_dirty = 1;
            cfw_page_present(ctx);
            cfw_send_page_state(ctx, 1, PAGE_OK);
        } else {
            cfw_page_close(ctx, PAGE_LEASE);
            return;
        }
    }
    if (ctx->press_start_tick && (now - ctx->press_start_tick) >= LONG_CLOSE_MS) {
        ctx->press_start_tick = 0;
        cfw_page_close(ctx, PAGE_USER);
    }
    /* a present request the display thread never served (screen off) must
     * not block the next one forever */
    if (ctx->disp_pan_pending && (now - ctx->disp_pan_tick) > PRESENT_RETRY_MS) ctx->disp_pan_pending = 0;
    /* a frame composed while the panel was off (screen timeout, wake) is
     * presented once the panel reports power again */
    if (ctx->page_open && ctx->disp_own && ctx->disp_dirty && !ctx->disp_pan_pending && fw_fb_get_power()) cfw_page_present(ctx);
    if (ctx->disp_dirty && !ctx->disp_pan_pending) cfw_page_present(ctx);
}
