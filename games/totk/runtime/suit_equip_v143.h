/* Tears of the Kingdom 1.4.3: suit auto-equip (local mod, port of the BOTW 1.9.0 exefs mod suit_auto_equip).
   When the player equips one armor piece in the pouch screen, the other pieces of the same set that the pouch holds
   are equipped too. TOTK_SUIT_EQUIP=0 turns it off; not on iOS.

   Hook: the pouch screen action handler 0x2173d18 calls PouchMgr::equip(mgr, cat, idx, -1) 0x20a1128 at 0x2175174
   (cat 4 = armor tail-calls equipArmor 0x20a8e54(mgr, idx, owner)). The call goes through a veneer in the text padding
   to suit_equip(), which calls the original and then equips the rest of the set with equipArmor(mgr, i, -1). The
   handler reads the three slot indices again after the call, so the screen shows the new pieces.

   Game data, as equipArmor 0x20a8e54 reads it: gdm = *(main+0x3ad66b0); armor handle = mgr+8+owner*0xc0+0x40
   (owner = *(u32 *)(mgr+0x188), 0 when >= 2); element i = gdGetArrayElem(gdm, &h, armor, 0x4290322e, i), its name =
   gdGetString(gdm, &s, &h, 0x25efa387); worn indices = gdGetInt on gdGetStruct(armor, 0x7d8b094) with the keys head
   0x5777012, upper 0xedb97ebf, lower 0xeecbbe92. Set key: the number of the last actor of the ArmorNextRankActor chain
   (PouchActorInfo row 0x63307c(tbl, &name), char * at row+0x18, "Work/Actor/<name>.engine__actor__ActorParam.gyml"),
   so every upgrade rank of a set has the same key; the slot is the name's suffix (Head, Upper, Lower). Items with a
   part list (row+0x68 count > 0, the multi-slot armors) are left alone, as equipArmor treats them on its own. */
#pragma once
#include "runtime.h"
#include "text_padding_v143.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !TARGET_OS_IPHONE
typedef struct { uint64_t a; uint32_t b; uint8_t c; uint8_t pad[3]; } SuitHandle;   /* game data handle, zeroed */
static struct {
    uint8_t *base;
    uint32_t (*equip)(void *mgr, uint32_t cat, int32_t idx, int32_t owner);
    uint32_t (*equip_armor)(void *mgr, int32_t idx, int32_t owner);
    int32_t (*category_count)(void *mgr, uint32_t cat);
    uint32_t (*array_elem)(void *gdm, SuitHandle *out, SuitHandle *parent, uint32_t hash, int32_t i);
    uint32_t (*get_struct)(void *gdm, SuitHandle *out, SuitHandle *parent, uint32_t hash);
    uint32_t (*get_int)(void *gdm, int32_t *out, SuitHandle *parent, uint32_t hash);
    uint32_t (*get_string)(void *gdm, const char **out, SuitHandle *parent, uint32_t hash);
    void *(*actor_info)(void *table, const char **name);
    void (*list_init)(void *list);
    uint32_t (*list_valid)(void *list);
    int32_t (*list_count)(void *list);
} suit;

enum { SUIT_ARMOR_ARRAY = 0x4290322e, SUIT_NAME = 0x25efa387, SUIT_EQUIP_STRUCT = 0x7d8b094 };
static const uint32_t suit_slot_key[3] = {0x5777012, 0xedb97ebf, 0xeecbbe92};   /* head, upper, lower */

static void *suit_ptr(uint64_t off) { void *p; memcpy(&p, suit.base + off, sizeof p); return p; }

static const char *suit_name(void *gdm, SuitHandle *armor, int32_t i) {
    SuitHandle h = {0};
    const char *s = "";
    if (i < 0 || !(suit.array_elem(gdm, &h, armor, SUIT_ARMOR_ARRAY, i) & 1)) return "";
    if (!(suit.get_string(gdm, &s, &h, SUIT_NAME) & 1) || !s) return "";
    return s;
}

/* Set key of an armor name, or -1: not an Armor_NNN_<slot> name, a part-list armor, or no PouchActorInfo row. */
static int suit_key(void *table, const char *name, int *slot) {
    if (strncmp(name, "Armor_", 6)) return -1;
    const char *suffix = strchr(name + 6, '_');
    if (!suffix) return -1;
    if (!strcmp(suffix, "_Head")) *slot = 0;
    else if (!strcmp(suffix, "_Upper")) *slot = 1;
    else if (!strcmp(suffix, "_Lower")) *slot = 2;
    else return -1;
    char cur[64];
    snprintf(cur, sizeof cur, "%s", name);
    for (int step = 0; step < 8; step++) {
        const char *p = cur;
        uint8_t *row = suit.actor_info(table, &p);
        if (!row) return -1;
        if (step == 0) {                                    /* part list, as equipArmor 0x20a8fb4..0x20a8fe8 */
            uint8_t list[16] = {0};
            suit.list_init(list);
            if (suit.list_valid(row + 0x68) & 1) { memcpy(list, row + 0x68, 8); memcpy(list + 5, row + 0x68 + 5, 8); }
            if (suit.list_count(list) > 0) return -1;
        }
        const char *next;
        memcpy(&next, row + 0x18, sizeof next);
        if (!next || strncmp(next, "Work/Actor/", 11)) break;
        const char *dot = strchr(next + 11, '.');
        size_t n = dot ? (size_t)(dot - (next + 11)) : strlen(next + 11);
        if (!n || n >= sizeof cur) break;
        memcpy(cur, next + 11, n); cur[n] = 0;
    }
    int number;
    if (sscanf(cur, "Armor_%d_", &number) != 1) return -1;
    return number;
}

static void suit_apply(void *mgr, int32_t idx) {
    void *gdm = suit_ptr(0x3ad66b0), *info = suit_ptr(0x3a6f678);
    void *table = info ? *(void **)((uint8_t *)info + 0x60) : NULL;
    if (!gdm || !table) return;
    uint32_t owner; memcpy(&owner, (uint8_t *)mgr + 0x188, sizeof owner);
    SuitHandle *armor = (SuitHandle *)((uint8_t *)mgr + 8 + (owner < 2 ? owner * 0xc0 : 0) + 0x40);
    SuitHandle es = {0};
    int32_t worn[3] = {-1, -1, -1};
    if (!(suit.get_struct(gdm, &es, armor, SUIT_EQUIP_STRUCT) & 1)) return;
    for (int s = 0; s < 3; s++)
        if (!(suit.get_int(gdm, &worn[s], &es, suit_slot_key[s]) & 1)) worn[s] = -1;
    int slot = 0, key = suit_key(table, suit_name(gdm, armor, idx), &slot);
    if (key < 0 || worn[slot] != idx) return;               /* not a set piece, or the call took it off */
    int done[3] = {0, 0, 0}, added = 0;
    done[slot] = 1;
    for (int s = 0; s < 3; s++) {                           /* slots that already wear a piece of this set */
        int ws = -1;
        if (s != slot && worn[s] >= 0 && suit_key(table, suit_name(gdm, armor, worn[s]), &ws) == key && ws == s) done[s] = 1;
    }
    int32_t count = suit.category_count(mgr, 4);
    for (int32_t i = 0; i < count && !(done[0] && done[1] && done[2]); i++) {
        int s = 0;
        if (i == idx || suit_key(table, suit_name(gdm, armor, i), &s) != key || done[s]) continue;
        if (suit.equip_armor(mgr, i, -1) & 1) { done[s] = 1; added++; }
    }
    if (added) fprintf(stderr, "[totk-mod] suit auto-equip: set %d, %d more piece(s) equipped\n", key, added);
}

static uint32_t suit_equip(void *mgr, uint32_t cat, int32_t idx, int32_t owner) {
    uint32_t r = suit.equip(mgr, cat, idx, owner);
    if ((r & 1) && cat == 4 && mgr) suit_apply(mgr, idx);
    return r;
}

static void suit_equip_install(Module *m) {
    const char *enabled = getenv("TOTK_SUIT_EQUIP");
    if ((enabled && !strcmp(enabled, "0")) || getenv("TOTK_EXPORT_IOS")) return;
    const size_t call = 0x2175174, equip = 0x20a1128;
    const size_t stub = ((m->text_end + 15) & ~15ULL) + PAD_V143_SUIT;   /* text_padding_v143.h */
    if (stub + PAD_V143_SUIT_LEN > m->ro_off) { fprintf(stderr, "[totk-mod] suit auto-equip: no veneer space, off\n"); return; }
    uint32_t word, expected = 0x94000000u | ((uint32_t)(((int64_t)equip - (int64_t)call) / 4) & 0x03ffffff);
    memcpy(&word, m->base + call, sizeof word);
    if (word != expected) { fprintf(stderr, "[totk-mod] suit auto-equip: unexpected word at main+%#zx, off\n", call); return; }
    for (unsigned i = 0; i < PAD_V143_SUIT_LEN; i++)
        if (m->base[stub + i]) { fprintf(stderr, "[totk-mod] suit auto-equip: veneer space occupied, off\n"); return; }
    uint8_t *b = m->base;
    suit.base = b;
    suit.equip = (void *)(b + equip);
    suit.equip_armor = (void *)(b + 0x20a8e54);
    suit.category_count = (void *)(b + 0xd61d6c);
    suit.array_elem = (void *)(b + 0x282ed8);
    suit.get_struct = (void *)(b + 0x16fac0);
    suit.get_int = (void *)(b + 0x710a20);
    suit.get_string = (void *)(b + 0x6e76d0);
    suit.actor_info = (void *)(b + 0x63307c);
    suit.list_init = (void *)(b + 0xa6bea8);
    suit.list_valid = (void *)(b + 0x75e934);
    suit.list_count = (void *)(b + 0x7379e0);
    uint32_t code[4] = {0x58000050, 0xd61f0200, 0, 0};      /* ldr x16, #8 ; br x16 ; .quad hook */
    void *hook = runtime_hle_wrap_named(suit_equip, "suit auto-equip");
    memcpy(code + 2, &hook, sizeof hook); memcpy(b + stub, code, sizeof code);
    uint32_t branch = 0x94000000u | ((uint32_t)(((int64_t)stub - (int64_t)call) / 4) & 0x03ffffff);
    memcpy(b + call, &branch, sizeof branch);
    fprintf(stderr, "[totk-mod] suit auto-equip hook enabled (1.4.3, TOTK_SUIT_EQUIP=0 turns it off)\n");
}
#else
static void suit_equip_install(Module *m) { (void)m; }
#endif
