/* The glasses' own "pair / not connected" screens. The launcher's SetupView
 * (pairing: hint1/2/3 and a "connect to finish setup" line, cycled through
 * every language while unpaired) and the common DisconnectView (paired but
 * no phone) build their labels alike: I18nManager::GetText(key) and then
 * RegisterLabel so a language change rewrites the label. build.py retargets
 * those calls here: the keys below get our text, in English, and the labels
 * are never registered, so the cycling cannot put the stock hints back. Any
 * other key falls through to the stock lookup. The images and the Bluetooth
 * name shown above the hints are untouched (the app pairs by that name). */
#include "cfw.h"

static int hint_key_is(const char *key, const char *want) {
    while (*want) { if (*key != *want) return 0; key++; want++; }
    return *key == 0;
}

/* In place of I18nManager::GetText(this, key): the text the label shows. */
const char *cfw_disconnect_hint(void *i18n, const char *key) {
    static const char hint[]  = "Glassly CFW v" REV_TEXT "\nOpen the Glassly App to connect";
    static const char hint1[] = "Glassly CFW v" REV_TEXT "\nInstall the Glassly App";
    static const char hint2[] = "Follow the app's guide to pair";
    static const char hint3[] = "Hold the Crown for 5 seconds to exit pairing";
    static const char setup[] = "Connect to the Glassly App to finish setup";
    if (!key) return "";
    if (hint_key_is(key, "launcher.disonnect_hint")) return hint;
    if (hint_key_is(key, "launcher.setupview_hint1")) return hint1;
    if (hint_key_is(key, "launcher.setupview_hint2")) return hint2;
    if (hint_key_is(key, "launcher.setupview_hint3")) return hint3;
    if (hint_key_is(key, "launcher.setupview_disconnect")) return setup;
    return fw_i18n_get_text(i18n, key);
}

/* The pairing view (BondView) cycles its texts through every language while
 * unpaired: getLocalizedText asks getActiveTextLanguage for the carousel's
 * current language and fetches the key for it. With no carousel language it
 * falls back to GetText, which we answer in English (the device language is
 * not set before pairing) with Glassly wording. English only, for now. */
const char *cfw_bond_language(void *self) {
    (void)self;
    return 0;
}

const char *cfw_bond_text(void *i18n, const char *key) {
    static const char step1[] = "Pair with the Glassly App";
    static const char hint1[] = "Install the Glassly App";
    static const char hint2[] = "Follow the app's guide to pair";
    static const char leave[] = "Hold the Crown for 5 seconds to exit pairing";
    if (!key) return "";
    if (hint_key_is(key, "launcher.bondview_step1")) return step1;
    if (hint_key_is(key, "launcher.bondview_step1_hint1")) return hint1;
    if (hint_key_is(key, "launcher.bondview_step1_hint2")) return hint2;
    if (hint_key_is(key, "launcher.bondview_rebond_exit_hint")) return leave;
    const char *t = fw_i18n_text_for_language(i18n, "en-US", key);
    return t && *t ? t : fw_i18n_get_text(i18n, key);
}

/* In place of lv_label_set_text(label, bluetooth name) in the pairing view
 * (BondView::updateDeviceNameLabel / updateStep1GlassName): the title line.
 * The Bluetooth name itself is unchanged; the app still pairs by it. */
void cfw_bond_name_label(void *label, const char *name) {
    (void)name;
    fw_label_set_text(label, "RayNeo iO Glassly CFW v" REV_TEXT);
}

/* In place of I18nManager::RegisterLabel(this, label, key): nothing. */
void cfw_hint_unregistered(void *i18n, void *label, const char *key) {
    (void)i18n; (void)label; (void)key;
}
