/* The stock "not connected" screen. Every app shows it through the one
 * rayneo::app::common::DisconnectView, whose ensureHintLabel asks
 * I18nManager::GetText for the key `launcher.disonnect_hint` (sic) and then
 * registers the label so a language change rewrites it. build.py retargets
 * those two calls here: the label reads our text and is never registered, so
 * nothing puts the stock hint back. The image above the label is untouched. */
#include "cfw.h"

/* In place of I18nManager::GetText(this, key): the text the label shows. */
const char *cfw_disconnect_hint(void *i18n, const char *key) {
    static const char hint[] = "Glassly CFW v" REV_TEXT "\nOpen the Glassly App to connect";
    (void)i18n; (void)key;
    return hint;
}

/* In place of I18nManager::RegisterLabel(this, label, key): nothing. */
void cfw_hint_unregistered(void *i18n, void *label, const char *key) {
    (void)i18n; (void)label; (void)key;
}
