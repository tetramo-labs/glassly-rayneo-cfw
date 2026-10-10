/* The Glassly page: a full-screen 540x180 overlay on the launcher, created on
 * demand, with a 120 s lease renewed by KEEPALIVE and a 100 ms service timer.
 *
 * Everything here runs on the launcher UI thread (reached from the message pump,
 * never from the RX callback). A phone-initiated open is gated on the
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
    if (ctx->page_root) return;
    void *disp = fw_display_default();
    void *screen = disp ? fw_screen_active(disp) : 0;
    if (!screen) return;
    void *root = fw_obj_create(screen);
    if (!root) return;
    /* a bare container: the theme's border, padding, radius and background
     * drew a frame around the page (thick top/left from the padding, thin
     * bottom/right from the border) and offset the canvas */
    fw_obj_remove_style_all(root);
    fw_obj_remove_flag(root, LV_FLAG_SCROLLABLE);
    fw_obj_set_scrollbar_mode(root, LV_SCROLLBAR_OFF);
    fw_obj_set_size(root, CFW_W, CFW_H);
    fw_obj_set_pos(root, 0, 0);
    fw_obj_add_flag(root, LV_FLAG_HIDDEN);
    void *canvas = fw_canvas_create(root);
    if (!canvas) { fw_obj_delete(root); return; }
    uint8_t *buf = (uint8_t *)fw_memalign(16, (uint32_t)CFW_W * CFW_H);
    if (!buf) { fw_obj_delete(root); return; }
    cfw_zero(buf, (uint32_t)CFW_W * CFW_H);
    fw_canvas_set_buffer(canvas, buf, CFW_W, CFW_H, LV_CF_L8);
    fw_obj_set_pos(canvas, 0, 0);
    ctx->page_root = root;
    ctx->canvas = canvas;
    ctx->canvas_buf = buf;
    cfw_input_attach(ctx, root);
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
    if (!ctx->page_root) { *reason = PAGE_BUSY; return 0; }
    fw_obj_remove_flag(ctx->page_root, LV_FLAG_HIDDEN);
    ctx->page_open = 1;
    ctx->crown_accum = 0;
    cfw_input_wheel_listen(ctx, 1);
    ctx->lease_deadline = fw_tick() + PAGE_LEASE_MS;
    fw_report_activity();
    *reason = PAGE_OK;
    return 1;
}

void cfw_page_close(customCfwContext *ctx, uint8_t reason) {
    ctx->disp_own = 0;
    cfw_anim_stop(ctx);
    cfw_input_wheel_listen(ctx, 0);
    if (ctx->page_root) fw_obj_add_flag(ctx->page_root, LV_FLAG_HIDDEN);
    cfw_scene_hide(ctx);
    ctx->page_open = 0;
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
}
