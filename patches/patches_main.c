/*
 * Single translation unit for the Glassly CFW injected code (RayNeo iO).
 *
 * Compiling every module as ONE unit yields a SINGLE absolutely-linked blob:
 * build.py's mini-linker resolves cross-module calls and absolute references, so
 * each injected entry point is just module_va + its offset in the one blob. The
 * modules share no typedef/macro/function names, so the plain #include chain
 * compiles cleanly. There are no mutable globals (no .data/.bss): all state lives
 * in one heap block whose pointer sits in the module's RNLink registry entry (ctx.c).
 */
#include "cfw.h"

#include "mem.c"
#include "rle.c"
#include "ctx.c"
#include "geom.c"
#include "scene.c"
#include "anim.c"
#include "ident.c"
#include "input.c"
#include "page.c"
#include "link.c"
#include "ancs.c"
