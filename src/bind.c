#include "bind.h"
#include "command.h"
#include "common.h"
#include "error.h"
#include "util/ascii.h"
#include "util/ptr-array.h"
#include "util/xmalloc.h"

typedef struct {
    KeyCode keys[3];
    size_t count;
} KeyChain;

typedef struct {
    char *command;
    KeyChain chain;
} Binding;

static KeyChain pressed_keys;

// Fast lookup table for most common key combinations (Ctrl or Meta
// with ASCII keys or any combination of modifiers with special keys)
static char *bindings_lookup_table[(2 * 128) + (8 * NR_SPECIAL_KEYS)];

// Fallback for all other keys (Unicode combos, multi-chord chains etc.)
static PointerArray bindings_ptr_array = PTR_ARRAY_INIT;

static bool parse_keys(KeyChain *chain, const char *str)
{
    char *keys = xstrdup(str);
    size_t len = strlen(keys);

    // Convert all whitespace to \0
    for (size_t i = 0; i < len; i++) {
        if (ascii_isspace(keys[i])) {
            keys[i] = '\0';
        }
    }

    memzero(chain);
    for (size_t i = 0; i < len; ) {
        while (i < len && keys[i] == 0) {
            i++;
        }
        const char *key = keys + i;
        while (i < len && keys[i] != 0) {
            i++;
        }
        if (key == keys + i) {
            break;
        }

        if (chain->count >= ARRAY_COUNT(chain->keys)) {
            error_msg("Too many keys.");
            free(keys);
            return false;
        }
        if (!parse_key(&chain->keys[chain->count], key)) {
            error_msg("Invalid key %s", key);
            free(keys);
            return false;
        }
        chain->count++;
    }
    free(keys);
    if (chain->count == 0) {
        error_msg("Empty key not allowed.");
        return false;
    }
    return true;
}

static CONST_FN ssize_t key_lookup_index(KeyCode k)
{
    const KeyCode modifiers = keycode_get_modifiers(k);
    const KeyCode key = keycode_get_key(k);

    static_assert(MOD_MASK >> 24 == (1 | 2 | 4));

    if (key >= KEY_SPECIAL_MIN && key <= KEY_SPECIAL_MAX) {
        const size_t mod_offset = (modifiers >> 24) * NR_SPECIAL_KEYS;
        return (2 * 128) + mod_offset + (key - KEY_SPECIAL_MIN);
    }

    if (key >= 0x20 && key <= 0x7E) {
        switch (modifiers) {
        case MOD_CTRL:
            return key;
        case MOD_META:
            return key + 128;
        default:
            break;
        }
    }

    return -1;
}

UNITTEST {
    BUG_ON(key_lookup_index(MOD_MASK | KEY_SPECIAL_MAX) != 256 + (8 * NR_SPECIAL_KEYS) - 1);
    BUG_ON(key_lookup_index(KEY_SPECIAL_MIN) != 256);
    BUG_ON(key_lookup_index(KEY_SPECIAL_MAX) != 256 + NR_SPECIAL_KEYS - 1);
    BUG_ON(key_lookup_index(MOD_CTRL | KEY_SPECIAL_MIN) != 256 + NR_SPECIAL_KEYS);
    BUG_ON(key_lookup_index(MOD_SHIFT | KEY_SPECIAL_MAX) != 256 + (5 * NR_SPECIAL_KEYS) - 1);

    BUG_ON(key_lookup_index(MOD_CTRL | ' ') != 32);
    BUG_ON(key_lookup_index(MOD_META | ' ') != 32 + 128);
    BUG_ON(key_lookup_index(MOD_CTRL | '~') != 126);
    BUG_ON(key_lookup_index(MOD_META | '~') != 126 + 128);

    BUG_ON(key_lookup_index(MOD_CTRL | MOD_META | 'a') != -1);
    BUG_ON(key_lookup_index(MOD_META | 0x0E01) != -1);
}

void add_binding(const char *keys, const char *command)
{
    KeyChain chain;
    if (!parse_keys(&chain, keys)) {
        return;
    }

    if (chain.count == 1) {
        const ssize_t idx = key_lookup_index(chain.keys[0]);
        if (idx >= 0) {
            char *old_command = bindings_lookup_table[idx];
            if (old_command) {
                free(old_command);
            }
            bindings_lookup_table[idx] = xstrdup(command);
            return;
        }
    }

    Binding *b = xnew(Binding, 1);
    b->chain = chain;
    b->command = xstrdup(command);
    ptr_array_add(&bindings_ptr_array, b);
}

void remove_binding(const char *keys)
{
    KeyChain chain;
    if (!parse_keys(&chain, keys)) {
        return;
    }

    if (chain.count == 1) {
        const ssize_t idx = key_lookup_index(chain.keys[0]);
        if (idx >= 0) {
            char *command = bindings_lookup_table[idx];
            if (command) {
                free(command);
                bindings_lookup_table[idx] = NULL;
            }
            return;
        }
    }

    size_t i = bindings_ptr_array.count;
    while (i > 0) {
        Binding *b = bindings_ptr_array.ptrs[--i];

        if (memcmp(&b->chain, &chain, sizeof(chain)) == 0) {
            ptr_array_remove_idx(&bindings_ptr_array, i);
            free(b->command);
            free(b);
            return;
        }
    }
}

void handle_binding(KeyCode key)
{
    pressed_keys.keys[pressed_keys.count] = key;
    pressed_keys.count++;

    if (pressed_keys.count == 1) {
        const ssize_t idx = key_lookup_index(key);
        if (idx >= 0) {
            const char *command = bindings_lookup_table[idx];
            if (command) {
                handle_command(commands, command);
                pressed_keys.count = 0;
                return;
            }
        }
    }

    for (size_t i = bindings_ptr_array.count; i > 0; i--) {
        Binding *b = bindings_ptr_array.ptrs[i - 1];
        KeyChain *c = &b->chain;

        if (c->count < pressed_keys.count) {
            continue;
        }

        if (memcmp (
            c->keys,
            pressed_keys.keys,
            pressed_keys.count * sizeof(pressed_keys.keys[0])
        )) {
            continue;
        }

        if (c->count > pressed_keys.count) {
            return;
        }

        handle_command(commands, b->command);
        break;
    }
    pressed_keys.count = 0;
}

size_t nr_pressed_keys(void)
{
    return pressed_keys.count;
}

String dump_bindings(void)
{
    String buf = STRING_INIT;

    for (KeyCode k = 0x20; k < 0x7E; k++) {
        const char *command = bindings_lookup_table[k];
        if (command) {
            char *keystr = key_to_string(MOD_CTRL | k);
            string_sprintf(&buf, "   %-10s  %s\n", keystr, command);
            free(keystr);
        }
    }

    for (KeyCode k = 0x20; k < 0x7E; k++) {
        const char *command = bindings_lookup_table[k + 128];
        if (command) {
            char *keystr = key_to_string(MOD_META | k);
            string_sprintf(&buf, "   %-10s  %s\n", keystr, command);
            free(keystr);
        }
    }

    static_assert(MOD_CTRL == (1 << 24));
    for (size_t m = 0; m <= 7; m++) {
        const KeyCode modifiers = m << 24;
        for (size_t k = KEY_SPECIAL_MIN; k <= KEY_SPECIAL_MAX; k++) {
            const size_t mod_offset = m * NR_SPECIAL_KEYS;
            const size_t i = (2 * 128) + mod_offset + (k - KEY_SPECIAL_MIN);
            const char *command = bindings_lookup_table[i];
            if (command) {
                char *keystr = key_to_string(modifiers | k);
                string_sprintf(&buf, "   %-10s  %s\n", keystr, command);
                free(keystr);
            }
        }
    }

    for (size_t i = 0, nbinds = bindings_ptr_array.count; i < nbinds; i++) {
        const Binding *b = bindings_ptr_array.ptrs[i];
        const KeyChain *c = &b->chain;
        String tmp = STRING_INIT;
        for (size_t j = 0, nkeys = c->count; j < nkeys; j++) {
            char *keystr = key_to_string(c->keys[j]);
            string_add_str(&tmp, keystr);
            free(keystr);
            string_add_byte(&tmp, ' ');
        }
        char *chainstr = string_steal_cstring(&tmp);
        string_sprintf(&buf, "   %-10s  %s\n", chainstr, b->command);
        free(chainstr);
    }

    return buf;
}
