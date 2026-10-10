#!/usr/bin/env python3
"""
Compile the Glassly CFW patch module to Thumb-2 machine code for the RayNeo iO
AP core (Bestechnic BES2800, Cortex-M33 / Armv8-M Mainline), lay it out as a
single absolutely-linked blob with no cross linker, resolve its internal
relocations, and splice it into the stock `nuttx_ap.bin`.

This is the clang half of the build. It is imported by gen_patches.py, which
turns its output into the committed, compiler-free patches/cfw_patches.json.

Honesty contract
----------------
Every firmware address the patch code calls is resolved BY NAME from the AP's
own symbol table (patches/symbols.json: pairs of (name_ptr, address) stored at a
tableOffset in the image) and then verified byte-for-byte against the stock
image: the (name_ptr, address) pair must be present at tableOffset, and the name
string must be present at the address name_ptr maps to. A mismatch fails the
build. The resolved addresses are emitted into generated/fw_syms.h, which the C
includes; the C never hardcodes a firmware address.

Linking model
-------------
The module is appended after the stock image, 32-byte aligned, and absolutely
linked at that load address (module_va). clang emits:
  * R_ARM_THM_CALL / R_ARM_THM_JUMP24  intra-module BL/B.W  -> blob-relative
  * R_ARM_THM_MOVW_ABS_NC / MOVT_ABS   absolute movw/movt    -> module_va + off
  * R_ARM_ABS32                        absolute data word    -> module_va + off
All targets must be symbols defined inside the module; an undefined/external
reference (e.g. a firmware function referenced by name instead of through a
generated fw_syms.h literal) is a hard error. Firmware functions are called
through absolute function-pointer constants, never linked.
"""
import os, sys, json, struct, subprocess, hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
GEN_DIR = os.path.join(HERE, "generated")
OBJ_DIR = os.path.join(os.path.dirname(HERE), "obj")

BASE = 0x10190000
CAPACITY = 0x9F0000
SIZE_LIMIT = 9_600_000            # decimal 9.6 MB user ceiling
FOOTER = bytes.fromhex("1d3457be00001930")
AP_SHA = "53afdf5298815849eafca6f315a70606d2a605aff2e563f79050f797d615a988"

# ---- firmware API: (alias, symbol name, expected address or None) -----------
# alias -> generated macro FWA_<alias>. `expected` disambiguates names that occur
# more than once in the table (e.g. two zlib copies); the chosen zlib cluster is
# 0x10835xxx (inflateInit2_ / inflate / inflateEnd sit together there).
FW_SYMBOLS = [
    ("memalign", "memalign", None),
    ("free", "free", None),
    ("rnlink_if_send_payload", "rnlink_if_send_payload", None),
    ("rnlink_if_register_callback", "rnlink_if_register_callback", None),
    ("message_processor_send", "message_processor_send", None),
    ("message_processor_register_c_handler", "message_processor_register_c_handler", None),
    ("os_get_tick_count", "os_get_tick_count", None),
    ("lv_timer_create", "lv_timer_create", None),
    ("lv_timer_delete", "lv_timer_delete", None),
    ("lv_display_get_default", "lv_display_get_default", None),
    ("lv_display_get_screen_active", "lv_display_get_screen_active", None),
    ("lv_obj_create_ex", "lv_obj_create_ex", None),
    ("lv_obj_delete", "lv_obj_delete", None),
    ("lv_obj_set_size", "lv_obj_set_size", None),
    ("lv_obj_set_pos", "lv_obj_set_pos", None),
    ("lv_obj_add_flag", "lv_obj_add_flag", None),
    ("lv_obj_remove_flag", "lv_obj_remove_flag", None),
    ("lv_obj_remove_style_all", "lv_obj_remove_style_all", None),
    ("lv_obj_set_scrollbar_mode", "lv_obj_set_scrollbar_mode", None),
    ("lv_obj_invalidate", "lv_obj_invalidate", None),
    ("lv_obj_align", "lv_obj_align", None),
    ("lv_obj_set_style_bg_color", "lv_obj_set_style_bg_color", None),
    ("lv_obj_set_style_bg_opa", "lv_obj_set_style_bg_opa", None),
    ("lv_canvas_create", "lv_canvas_create", None),
    ("lv_canvas_set_buffer", "lv_canvas_set_buffer", None),
    ("lv_draw_buf_width_to_stride", "lv_draw_buf_width_to_stride", None),
    ("lv_label_create", "lv_label_create", None),
    ("lv_label_set_text", "lv_label_set_text", None),
    ("lv_obj_set_style_text_color", "lv_obj_set_style_text_color", None),
    ("lv_obj_add_event_cb", "lv_obj_add_event_cb", None),
    ("lv_event_get_code", "lv_event_get_code", None),
    ("lv_event_get_key", "lv_event_get_key", None),
    ("lv_event_get_user_data", "lv_event_get_user_data", None),
    ("inflateInit2_", "inflateInit2_", 0x10835AD1),
    ("inflate", "inflate", 0x10835B6D),
    ("inflateEnd", "inflateEnd", 0x10836B4D),
    ("launcher_is_business_idle", "rayneo::service::launcher::LauncherAPI::isBussinessIdle", None),
    ("monitor_manager_instance", "rayneo::service::launcher::MonitorManager::getInstance", None),
    ("monitor_manager_input", "rayneo::service::launcher::MonitorManager::getInputEventMonitor", None),
    ("input_register_wheel", "rayneo::service::launcher::InputEventMonitor::registerRawWheelListener", None),
    ("input_unregister_wheel", "rayneo::service::launcher::InputEventMonitor::unregisterRawWheelListener", None),
    ("btn_register", "rayneo_register_btn_event_interceptor", None),
    ("display_reg_screen_cb", "rayneo_dispay_register_screen_state_callback", None),   # sic
    ("screen_power_force_off", "screen_power_force_off", None),   # ignores wakelocks (the phone link holds one)
    ("sensor_register_sync_imu_mag", "sensor_rpmsg_register_sync_imu_mag_data_handler", None),
    ("sensor_start_sync_imu_mag", "sensor_rpmsg_start_report_sync_imu_mag", None),
    ("sensor_stop_sync_imu_mag", "sensor_rpmsg_stop_report_sync_imu_mag", None),
    ("display_is_screen_on", "rayneo_display_is_screen_on", None),
    ("i18n_get_text", "rayneo::manager::I18nManager::GetText", 0x107D9FF1),   # (this, key) overload
    ("i18n_text_for_language", "rayneo::manager::I18nManager::GetTextForLanguage", None),
    ("ancs_notification_handler",
     "rayneo::service::notification::NotificationMessageHandler::handleAncsNotificationReceived", 0x107029D1),
    ("rnlink_paired_status", "real_get_rnlink_paired_status", None),
    ("display_report_activity", "lv_rayneo_display_report_activity", None),
    # revision 5 display probe (disp.c): the fbdev driver data, the fb device
    # ioctls flush_cb issues (libc ioctl, not the fb driver's own `ioctl` op at
    # 0x107566fd), the LCDC dual update and the parallax/depth getters.
    ("lv_display_get_driver_data", "lv_display_get_driver_data", None),
    ("ioctl", "ioctl", 0x108630EB),
    ("lcdc_dual_update_full", "rayneo_lcdc_layer_dual_update_full", None),
    ("lcdc_cache_sync", "bes_lcdc_cache_sync", None),
    ("lcdc_layer_update", "rayneo_lcdc_layer_update", None),
    ("lcdc_trywaitforirq_timeout", "rayneo_lcdc_trywaitforirq_timeout", None),
    ("lcdc_update_self", "rayneo_lcdc_update_self", None),
    ("fb_get_power", "rayneo_fb_get_power", None),
    ("fb_power_done", "rayneo_fb_power_done", None),
    ("fb_left_parallax", "rayneo_fb_get_left_parallax", None),
    ("fb_right_parallax", "rayneo_fb_get_right_parallax", None),
    ("fb_depth_mode", "rayneo_fb_get_depth_mode", None),
    ("fb_disp_mode", "rayneo_fb_get_disp_mode", None),
    ("logger_log", "logger_log", None),
]

# ---- hook site (byte-verified against the stock image) ----------------------
# LinkMonitor::registerLinkCallback is hooked where it fills the launcher's
# RNLink callback table, so the module can register its own table for business
# 0xF0 through the same rnlink_if_register_callback (the interface vtable slot
# +8 at 0x182d33f4 is that very function).
SHIM_SITE = 0x106D9D1A           # 10-byte inline sequence -> bl cfw_register_shim
SHIM_STOCK = bytes.fromhex("00241f48204b84610360")
# Pointer constants the shim relies on; verified so a rebase that moves them fails.
# ANCS relay: the one `bl handleAncsNotificationReceived` inside
# NotificationMessageHandler::handleMessage -> bl cfw_ancs_hook (ancs.c).
ANCS_SITE = 0x10702E7E
ANCS_STOCK_TARGET = 0x107029D1
# Display present: the two `bl rayneo_lcdc_layer_update` inside rayneo_pandisp_exec
# (the display thread's refresh step) -> bl cfw_lcdc_hook (disp.c), which owns the
# present while the module holds a frame and forwards to the stock otherwise.
LCDC_SITES = [0x10755DB2, 0x10755E68]
LCDC_STOCK_TARGET = 0x10754035
# Connect screen (hint.c): inside DisconnectView::ensureHintLabel, the
# `bl I18nManager::GetText` that fetches the hint text and the
# `bl I18nManager::RegisterLabel` that would let a language change rewrite it.
I18N_GET_TEXT = 0x107D9FF1
I18N_REGISTER_LABEL = 0x107DA601
HINT_SITES = [
    (0x10794C2A, I18N_GET_TEXT, "cfw_disconnect_hint", "DisconnectView::ensureHintLabel: bl I18nManager::GetText -> cfw_disconnect_hint (connect screen text)"),
    (0x10794C7E, I18N_REGISTER_LABEL, "cfw_hint_unregistered", "DisconnectView::ensureHintLabel: bl I18nManager::RegisterLabel -> cfw_hint_unregistered (keep our text)"),
    # SetupView::init: the pairing screen's four labels (hint1, hint2, hint3, disconnect)
    (0x10841EE6, I18N_GET_TEXT, "cfw_disconnect_hint", "SetupView::init hint1: bl I18nManager::GetText -> cfw_disconnect_hint"),
    (0x10841F42, I18N_REGISTER_LABEL, "cfw_hint_unregistered", "SetupView::init hint1: bl I18nManager::RegisterLabel -> cfw_hint_unregistered"),
    (0x10841FE4, I18N_GET_TEXT, "cfw_disconnect_hint", "SetupView::init hint2: bl I18nManager::GetText -> cfw_disconnect_hint"),
    (0x1084203C, I18N_REGISTER_LABEL, "cfw_hint_unregistered", "SetupView::init hint2: bl I18nManager::RegisterLabel -> cfw_hint_unregistered"),
    (0x10842050, I18N_GET_TEXT, "cfw_disconnect_hint", "SetupView::init hint3: bl I18nManager::GetText -> cfw_disconnect_hint"),
    (0x108420A8, I18N_REGISTER_LABEL, "cfw_hint_unregistered", "SetupView::init hint3: bl I18nManager::RegisterLabel -> cfw_hint_unregistered"),
    (0x1084216A, I18N_GET_TEXT, "cfw_disconnect_hint", "SetupView::init disconnect: bl I18nManager::GetText -> cfw_disconnect_hint"),
    (0x108421C4, I18N_REGISTER_LABEL, "cfw_hint_unregistered", "SetupView::init disconnect: bl I18nManager::RegisterLabel -> cfw_hint_unregistered"),
    # BondView: the pairing screen's title shows the Bluetooth name; both label updates become ours
    (0x1083F87C, 0x10600D01, "cfw_bond_name_label", "BondView::updateDeviceNameLabel: bl lv_label_set_text -> cfw_bond_name_label (title)"),
    (0x1083F83C, 0x10600D01, "cfw_bond_name_label", "BondView::updateStep1GlassName: bl lv_label_set_text -> cfw_bond_name_label (title)"),
    # BondView::getLocalizedText: no carousel language, English Glassly texts
    (0x1083F904, 0x1083F8BD, "cfw_bond_language", "BondView::getLocalizedText: bl getActiveTextLanguage -> cfw_bond_language (no language carousel)"),
    (0x1083F910, I18N_GET_TEXT, "cfw_bond_text", "BondView::getLocalizedText: bl I18nManager::GetText -> cfw_bond_text (English, Glassly wording)"),
]
SHIM_ASSERTS = [
    (0x1075D03C, 0x1980FCD8),    # rayneo_intercept_btn_event's literal: the button interceptor list head (cfw.h BTN_LIST_HEAD)
    (0x106D9D9C, 0x19750D2C),    # &launcher business-15 callback table
    (0x106D9DA0, 0x106D97F1),    # its stock slot-0 data callback (preserved at [0])
]
# RNLink callback registry (24 entries x 0x2c): the literal loaded by both
# rnlink_if_register_callback (0x107763dc: ldr r3,[pc,#0x80]) and
# rnlink_if_on_data_recv (0x10776384: ldr r4,[pc,#0x50]). The module keeps its
# heap-context pointer in the `sub` word (+0x20) of its own entry (ctx.c).
RN_REGISTRY = 0x1987C780
RN_REGISTRY_LITERALS = [0x10776460, 0x107763D8]


class BuildError(Exception):
    pass


def _sha256(b):
    return hashlib.sha256(b).hexdigest()


# ========================= symbol resolution / verify =========================
def load_symbols():
    s = json.load(open(os.path.join(HERE, "symbols.json")))
    by_name = {}
    for e in s["entries"]:
        by_name.setdefault(e["name"], []).append(e)
    return s, by_name


def _locate(segments, addr):
    """Map a runtime address to a file offset in nuttx_ap.bin."""
    for low, high, at in segments:
        if low <= addr < high:
            return at + addr - low
    if BASE <= addr < BASE + SIZE_LIMIT + 0x100000:
        return addr - BASE
    return None


def resolve_symbol(ap, segments, by_name, name, expected):
    matches = [e for e in by_name.get(name, []) if expected is None or e["address"] == expected]
    addrs = {e["address"] for e in matches}
    if len(addrs) != 1:
        raise BuildError(f"symbol {name!r}: expected exactly one address, got {sorted(hex(a) for a in addrs)}")
    addr = next(iter(addrs))
    if not (addr & 1):
        raise BuildError(f"symbol {name!r} @ {addr:#x} is not a Thumb function (bit0 clear)")
    for e in matches:
        off = e["tableOffset"]
        p, a = struct.unpack_from("<II", ap, off)
        if a != e["address"]:
            raise BuildError(f"symbol {name!r}: table address {a:#x} != {e['address']:#x} at offset {off}")
        nat = _locate(segments, p)
        if nat is None or ap[nat:nat + len(name) + 1] != name.encode() + b"\0":
            raise BuildError(f"symbol {name!r}: name string absent at name_ptr {p:#x} (file {nat})")
    return addr


def resolve_all(ap):
    s, by_name = load_symbols()
    segments = s["segments"]
    resolved = []
    for alias, name, expected in FW_SYMBOLS:
        addr = resolve_symbol(ap, segments, by_name, name, expected)
        resolved.append({"alias": alias, "name": name, "address": addr})
    return resolved


def write_fw_header(resolved):
    os.makedirs(GEN_DIR, exist_ok=True)
    lines = [
        "/* GENERATED by patches/build.py — do not edit.",
        " * RayNeo iO Strix OS 1.0.4.12 AP firmware addresses, resolved by name from",
        " * the AP symbol table and verified byte-for-byte against the stock image.",
        " */",
        "#pragma once",
    ]
    for r in resolved:
        lines.append(f"#define FWA_{r['alias']} 0x{r['address']:08x}u")
    lines.append("/* data: RNLink callback registry (24 x 0x2c), literal-verified in build_ap */")
    lines.append(f"#define FWA_rnlink_registry 0x{RN_REGISTRY:08x}u")
    open(os.path.join(GEN_DIR, "fw_syms.h"), "w").write("\n".join(lines) + "\n")


# ============================== ELF32 LE parser ==============================
R_ARM_ABS32 = 2
R_ARM_THM_CALL = 10
R_ARM_THM_JUMP24 = 30
R_ARM_THM_MOVW_ABS_NC = 47
R_ARM_THM_MOVT_ABS = 48

SHT_PROGBITS = 1
SHT_REL = 9
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4

CLANG = "clang"
CFLAGS = [
    "--target=thumbv8m.main-none-eabi", "-mcpu=cortex-m33", "-mthumb", "-Os",
    "-ffreestanding", "-fno-builtin", "-fno-jump-tables", "-mno-unaligned-access",
    "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
    "-fno-stack-protector", "-std=c99", "-Wall", "-Wextra", "-Wno-unused-parameter",
    "-I", HERE, "-I", GEN_DIR,
]


def _parse_elf(path):
    d = open(path, "rb").read()
    if not (d[:4] == b"\x7fELF" and d[4] == 1 and d[5] == 1):
        raise BuildError("not ELF32-LE")
    (e_shoff,) = struct.unpack_from("<I", d, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name, typ, flags, addr, offset, size, link, info, align, entsz = \
            struct.unpack_from("<IIIIIIIIII", d, off)
        secs.append(dict(name=name, type=typ, flags=flags, offset=offset, size=size,
                         link=link, info=info, align=align, entsize=entsz))
    shstr = secs[e_shstrndx]
    for sec in secs:
        s = d[shstr["offset"] + sec["name"]:]
        sec["sname"] = s[:s.index(b"\0")].decode()
    return d, secs


def _section(secs, name):
    for s in secs:
        if s["sname"] == name:
            return s
    return None


def _is_rodata(sec):
    return (sec["type"] == SHT_PROGBITS and (sec["flags"] & SHF_ALLOC)
            and not (sec["flags"] & (SHF_WRITE | SHF_EXECINSTR)))


def _thumb_branch(blob, off, target):
    """Rewrite the Thumb-2 BL/B.W at blob[off:off+4] to reach blob offset target."""
    hw2_old = blob[off + 2] | (blob[off + 3] << 8)
    disp = target - (off + 4)
    if disp % 2 or not (-(1 << 24) <= disp < (1 << 24)):
        raise BuildError(f"branch at {off:#x} -> {target:#x} out of Thumb range")
    imm = (disp >> 1) & 0xFFFFFF
    S = (imm >> 23) & 1
    i1 = (imm >> 22) & 1
    i2 = (imm >> 21) & 1
    imm10 = (imm >> 11) & 0x3FF
    imm11 = imm & 0x7FF
    j1 = (~(i1 ^ S)) & 1
    j2 = (~(i2 ^ S)) & 1
    hw1 = 0xF000 | (S << 10) | imm10
    hw2 = (hw2_old & 0xD000) | (j1 << 13) | (j2 << 11) | imm11
    blob[off:off + 4] = bytes([hw1 & 0xFF, hw1 >> 8, hw2 & 0xFF, hw2 >> 8])


def _movwt_get_imm(hw1, hw2):
    imm4 = hw1 & 0xF
    i = (hw1 >> 10) & 1
    imm3 = (hw2 >> 12) & 7
    imm8 = hw2 & 0xFF
    return (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8


def _movwt_set_imm(hw1, hw2, val):
    val &= 0xFFFF
    imm4 = (val >> 12) & 0xF
    i = (val >> 11) & 1
    imm3 = (val >> 8) & 7
    imm8 = val & 0xFF
    hw1 = (hw1 & ~((1 << 10) | 0xF)) | (i << 10) | imm4
    hw2 = (hw2 & ~((7 << 12) | 0xFF)) | (imm3 << 12) | imm8
    return hw1, hw2


def _resolve_movwt_abs(blob, off, sym_abs, high):
    hw1 = blob[off] | (blob[off + 1] << 8)
    hw2 = blob[off + 2] | (blob[off + 3] << 8)
    addend = _movwt_get_imm(hw1, hw2)
    if addend:
        raise BuildError(f"movw/movt at {off:#x} has non-zero addend {addend:#x}; "
                         "only plain symbol addresses are supported")
    result = sym_abs & 0xFFFFFFFF
    val = (result >> 16) & 0xFFFF if high else result & 0xFFFF
    hw1, hw2 = _movwt_set_imm(hw1, hw2, val)
    blob[off:off + 4] = bytes([hw1 & 0xFF, hw1 >> 8, hw2 & 0xFF, hw2 >> 8])


def compile_blob(src, module_va):
    """Compile `src` and return (blob, funcs) where blob is the absolutely-linked
    module image (.text then .rodata) and funcs maps name -> blob offset (Thumb)."""
    os.makedirs(OBJ_DIR, exist_ok=True)
    obj = os.path.join(OBJ_DIR, "patches_main.o")
    subprocess.run([CLANG, *CFLAGS, "-c", src, "-o", obj], check=True)
    d, secs = _parse_elf(obj)

    text = _section(secs, ".text")
    if text is None:
        raise BuildError("no .text section")
    text_idx = secs.index(text)

    # Reject writable/zero-init data: all mutable state must live on the heap.
    for s in secs:
        if s["type"] == SHT_PROGBITS and (s["flags"] & SHF_WRITE) and s["size"]:
            raise BuildError(f"section {s['sname']} is writable with {s['size']} bytes; "
                             "the module must have no .data (use the heap singleton)")
        if s["sname"].startswith(".bss") and s["size"]:
            raise BuildError(f"section {s['sname']} has {s['size']} BSS bytes; no zero-init globals allowed")

    blob = bytearray(d[text["offset"]:text["offset"] + text["size"]])
    base = {text_idx: 0}
    for i, s in enumerate(secs):
        if i == text_idx or not _is_rodata(s):
            continue
        align = max(s["align"], 1)
        while len(blob) % align:
            blob.append(0)
        base[i] = len(blob)
        blob += d[s["offset"]:s["offset"] + s["size"]]

    symtab = _section(secs, ".symtab")
    strtab = secs[symtab["link"]]
    syms = []
    for i in range(symtab["size"] // 16):
        o = symtab["offset"] + i * 16
        st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from("<IIIBBH", d, o)
        nm = d[strtab["offset"] + st_name:]
        nm = nm[:nm.index(b"\0")].decode()
        syms.append(dict(name=nm, value=st_value, size=st_size, typ=st_info & 0xF, shndx=st_shndx))

    bad = []
    for rs in secs:
        if rs["type"] != SHT_REL and not rs["sname"].startswith(".rela"):
            continue
        tgt = rs["info"]
        if tgt not in base:
            continue  # relocs for discarded sections (.ARM.exidx, debug) — ignore
        ent = 12 if rs["sname"].startswith(".rela") else 8
        for i in range(rs["size"] // ent):
            r_offset, r_info = struct.unpack_from("<II", d, rs["offset"] + i * ent)
            r_type = r_info & 0xFF
            sym = syms[r_info >> 8]
            wpos = base[tgt] + r_offset
            if sym["shndx"] not in base:
                bad.append(f"  {rs['sname']}+{r_offset:#x} type={r_type} -> {sym['name']!r}: "
                           "external/undefined (call firmware via fw_syms.h literals)")
                continue
            # A Thumb STT_FUNC symbol already carries bit 0 in st_value (ARM ELF
            # ABI); strip it before computing the address and re-add it once for
            # absolute references, or every callback pointer lands even and one
            # byte past the function start.
            is_func = sym["typ"] == 2
            sym_blob = base[sym["shndx"]] + (sym["value"] & ~1 if is_func else sym["value"])
            thumb_bit = 1 if is_func else 0
            if r_type in (R_ARM_THM_CALL, R_ARM_THM_JUMP24) and tgt == text_idx:
                _thumb_branch(blob, wpos, sym_blob)
            elif r_type in (R_ARM_THM_MOVW_ABS_NC, R_ARM_THM_MOVT_ABS):
                _resolve_movwt_abs(blob, wpos, module_va + sym_blob + thumb_bit, high=(r_type == R_ARM_THM_MOVT_ABS))
            elif r_type == R_ARM_ABS32:
                addend = struct.unpack_from("<I", blob, wpos)[0]
                struct.pack_into("<I", blob, wpos, (module_va + sym_blob + thumb_bit + addend) & 0xFFFFFFFF)
            else:
                bad.append(f"  {rs['sname']}+{r_offset:#x} type={r_type} -> {sym['name']!r}: unsupported")
    if bad:
        raise BuildError(f"{src}: unresolvable relocation(s):\n" + "\n".join(bad))

    funcs = {}
    for s in syms:
        if s["typ"] == 2 and s["shndx"] == text_idx and s["name"]:
            funcs[s["name"]] = s["value"] & ~1
    return bytes(blob), funcs


# ================================ AP assembly ================================
def _branch_bytes(site_va, target_va, link):
    """Encode a Thumb-2 BL (link) or B.W to target_va at site_va."""
    delta = (target_va & ~1) - (site_va + 4)
    if delta % 2 or not (-(1 << 24) <= delta < (1 << 24)):
        raise BuildError("hook branch out of range")
    bits = delta & 0x1FFFFFF
    s = (bits >> 24) & 1
    i1 = (bits >> 23) & 1
    i2 = (bits >> 22) & 1
    j1 = 1 ^ (i1 ^ s)
    j2 = 1 ^ (i2 ^ s)
    return struct.pack("<HH", 0xF000 | (s << 10) | ((bits >> 12) & 0x3FF),
                       (0xD000 if link else 0x9000) | (j1 << 13) | (j2 << 11) | ((bits >> 1) & 0x7FF))


def build_ap(ap, src=None):
    """Produce the patched AP and the (offset, old, new, desc) op list that
    reproduces it from the stock image. `ap` is the stock nuttx_ap.bin bytes."""
    if _sha256(ap) != AP_SHA:
        raise BuildError("stock nuttx_ap.bin sha256 mismatch")
    if len(ap) != 9466560 or ap[-8:] != FOOTER:
        raise BuildError("stock AP length/footer unexpected")
    if src is None:
        src = os.path.join(HERE, "patches_main.c")

    resolved = resolve_all(ap)
    write_fw_header(resolved)

    footer_at = len(ap) - 8
    module_at = (footer_at + 31) & ~31
    module_va = BASE + module_at

    blob, funcs = compile_blob(src, module_va)
    for need in ("cfw_register_shim", "cfw_rx_data"):
        if need not in funcs:
            raise BuildError(f"module does not export {need}")

    # --- verify + build the hook patches ---
    ops = []

    def off(va):
        return va - BASE

    # 1. The RNLink registry address the C relies on (FWA_rnlink_registry).
    for va in RN_REGISTRY_LITERALS:
        got = struct.unpack_from("<I", ap, off(va))[0]
        if got != RN_REGISTRY:
            raise BuildError(f"RNLink registry literal at {va:#x} is {got:#x}, expected {RN_REGISTRY:#x}")

    # 2. Launcher registration shim: inline sequence -> bl cfw_register_shim.
    if ap[off(SHIM_SITE):off(SHIM_SITE) + len(SHIM_STOCK)] != SHIM_STOCK:
        raise BuildError("registration shim stock bytes mismatch")
    for va, want in SHIM_ASSERTS:
        got = struct.unpack_from("<I", ap, off(va))[0]
        if got != want:
            raise BuildError(f"shim pointer constant at {va:#x} is {got:#x}, expected {want:#x}")
    shim_va = (module_va + funcs["cfw_register_shim"]) | 1
    shim_new = _branch_bytes(SHIM_SITE, shim_va, link=True) + bytes.fromhex("00bf") * 3
    # The RNLink callback table must hold the Thumb address of cfw_rx_data; an
    # even value faults the RNLink thread on the first business-0xF0 frame.
    want_rx = (module_va + funcs["cfw_rx_data"]) | 1
    if struct.pack("<I", want_rx) not in blob:
        raise BuildError(f"module does not hold the Thumb address of cfw_rx_data ({want_rx:#x})")
    ops.append({"offset": off(SHIM_SITE), "old": SHIM_STOCK.hex(), "new": shim_new.hex(),
                "desc": "LinkMonitor::registerLinkCallback -> cfw_register_shim (registers the 0xF0 receiver)"})

    # 2b. ANCS relay: retarget the stock bl to cfw_ancs_hook.
    ancs_stock = _branch_bytes(ANCS_SITE, ANCS_STOCK_TARGET, link=True)
    if ap[off(ANCS_SITE):off(ANCS_SITE) + 4] != ancs_stock:
        raise BuildError("ANCS hook site is not `bl handleAncsNotificationReceived`")
    ancs_new = _branch_bytes(ANCS_SITE, (module_va + funcs["cfw_ancs_hook"]) | 1, link=True)
    ops.append({"offset": off(ANCS_SITE), "old": ancs_stock.hex(), "new": ancs_new.hex(),
                "desc": "NotificationMessageHandler::handleMessage: bl handleAncsNotificationReceived -> cfw_ancs_hook (ANCS relay)"})

    # 2c. Display present: retarget both layer_update calls in pandisp_exec.
    lcdc_hooks = []
    for site in LCDC_SITES:
        stock = _branch_bytes(site, LCDC_STOCK_TARGET, link=True)
        if ap[off(site):off(site) + 4] != stock:
            raise BuildError(f"LCDC hook site {site:#x} is not `bl rayneo_lcdc_layer_update`")
        new = _branch_bytes(site, (module_va + funcs["cfw_lcdc_hook"]) | 1, link=True)
        ops.append({"offset": off(site), "old": stock.hex(), "new": new.hex(),
                    "desc": f"rayneo_pandisp_exec @{site:#x}: bl rayneo_lcdc_layer_update -> cfw_lcdc_hook (display present)"})
        lcdc_hooks.append({"site": hex(site), "len": 4, "old": stock.hex(), "new": new.hex(),
                           "purpose": "call cfw_lcdc_hook instead of rayneo_lcdc_layer_update (forwards unless the module holds a frame)"})

    # 2d. Connect screen: the hint label's text and its i18n registration.
    for site, stock_target, func, desc in HINT_SITES:
        stock = _branch_bytes(site, stock_target, link=True)
        if ap[off(site):off(site) + 4] != stock:
            raise BuildError(f"hint site {site:#x} is not the expected bl")
        new = _branch_bytes(site, (module_va + funcs[func]) | 1, link=True)
        ops.append({"offset": off(site), "old": stock.hex(), "new": new.hex(), "desc": desc})
        lcdc_hooks.append({"site": hex(site), "len": 4, "old": stock.hex(), "new": new.hex(), "purpose": desc})

    # 3. Relocate the build-info footer and append the module 32-byte aligned.
    pad = module_at - footer_at           # old footer bytes become zero padding
    ops.append({"offset": footer_at, "old": FOOTER.hex(), "new": ("00" * pad),
                "desc": "zero the old build-info footer slot before the appended module"})
    tail = bytearray(blob)
    while len(tail) % 4:
        tail.append(0)
    tail += FOOTER
    ops.append({"offset": module_at, "old": "", "new": bytes(tail).hex(),
                "desc": f"append CFW module ({len(blob)} B) 32-aligned at {module_va:#x} + relocated footer"})

    # --- materialize the patched image and sanity-check it ---
    patched = _apply(ap, ops)
    if len(patched) > SIZE_LIMIT or len(patched) > CAPACITY:
        raise BuildError(f"patched AP {len(patched)} bytes exceeds the size ceiling")
    if patched[:16] != ap[:16] or patched[-8:] != FOOTER:
        raise BuildError("header or relocated footer wrong after assembly")

    info = {
        "module_va": module_va, "module_at": module_at, "blob_len": len(blob),
        "patched_len": len(patched), "resolved": resolved,
        "hooks": [{"site": hex(SHIM_SITE), "len": len(SHIM_STOCK),
                   "old": SHIM_STOCK.hex(), "new": shim_new.hex(),
                   "purpose": "call cfw_register_shim from LinkMonitor::registerLinkCallback"},
                  {"site": hex(ANCS_SITE), "len": 4, "old": ancs_stock.hex(), "new": ancs_new.hex(),
                   "purpose": "call cfw_ancs_hook instead of handleAncsNotificationReceived (tail-calls it)"},
                  *lcdc_hooks],
    }
    return patched, ops, info


def _apply(img, ops):
    """Local copy of the apply logic so build_ap can self-check (apply_patches.py
    is the committed, standalone replayer)."""
    buf = bytearray(img)
    for op in ops:
        o = op["offset"]
        old = bytes.fromhex(op.get("old", ""))
        new = bytes.fromhex(op["new"])
        if old:
            if bytes(buf[o:o + len(old)]) != old:
                raise BuildError(f"apply: stale bytes at {o:#x}")
            buf[o:o + len(new)] = new
        else:
            if o != len(buf):
                raise BuildError(f"apply: append offset {o:#x} != length {len(buf):#x}")
            buf.extend(new)
    return bytes(buf)


def main():
    import argparse
    ap_arg = argparse.ArgumentParser()
    ap_arg.add_argument("stock_ap")
    ap_arg.add_argument("-o", "--out")
    a = ap_arg.parse_args()
    ap = open(a.stock_ap, "rb").read()
    try:
        patched, ops, info = build_ap(ap)
    except BuildError as e:
        print("FAIL:", e, file=sys.stderr)
        sys.exit(1)
    print(f"OK: module {info['blob_len']} B @ {info['module_va']:#x}, "
          f"patched AP {info['patched_len']} B, {len(info['resolved'])} symbols verified")
    if a.out:
        open(a.out, "wb").write(patched)
        print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
