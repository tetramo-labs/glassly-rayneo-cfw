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
        cfw_input_attach(ctx, root);
    }
    for (unsigned e = 0; e < 2; e++) {
        cfw_lens *L = &ctx->lens[e];
        L->bit = e ? LENS_RIGHT : LENS_LEFT;
        if (L->canvas) continue;
        uint8_t *buf = (uint8_t *)fw_memalign(16, (uint32_t)CFW_W * CFW_H);
        if (!buf) continue;        /* disp.c shows the other lens's canvas in both eyes */
        cfw_zero(buf, (uint32_t)CFW_W * CFW_H);
        L->canvas = buf;
    }
    cfw_page_ensure_timer(ctx);
}

/* The 100 ms service timer also drains the uplink queue, so it must exist as
 * soon as the first packet is handled (IDENT is answered before any page). */
void cfw_page_ensure_timer(customCfwContext *ctx) {
    if (!ctx->timer) ctx->timer = fw_timer_create((void *)cfw_timer_cb, 100, 0);
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
    cfw_input_wheel_listen(ctx, 1);
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
    ctx->disp_own = 0;
    ctx->disp_pattern = 0;
    cfw_anim_stop(ctx);
    cfw_input_wheel_listen(ctx, 0);
    if (ctx->page_root) fw_obj_add_flag(ctx->page_root, LV_FLAG_HIDDEN);
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

void cfw_page_tick(customCfwContext *ctx) {
    cfw_uplink_flush(ctx);
    if (!ctx->page_open) return;
    uint32_t now = fw_tick();
    if ((int32_t)(now - ctx->lease_deadline) >= 0) {
        cfw_page_close(ctx, PAGE_LEASE);
        return;
    }
    if (ctx->press_start_tick && (now - ctx->press_start_tick) >= LONG_CLOSE_MS) {
        ctx->press_start_tick = 0;
        cfw_page_close(ctx, PAGE_USER);
    }
    /* a present request the display thread never served (screen off) must
     * not block the next one forever */
    if (ctx->disp_pan_pending && (now - ctx->disp_pan_tick) > 500u) ctx->disp_pan_pending = 0;
    if (ctx->disp_dirty && !ctx->disp_pan_pending) cfw_page_present(ctx);
}
