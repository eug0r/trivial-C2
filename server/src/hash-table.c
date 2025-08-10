
#include <stdlib.h>
#include <string.h>

#include "hash-table.h"

static unsigned long hash_generate(const char *key) { //djb2 hash
    unsigned long hash = 5381;
    int c;
    while ((c = (unsigned char)*key++))
        hash = ((hash << 5) + hash) + c;  // hash * 33 + c
    return hash % HASH_TABLE_SIZE;
}
//user should malloc node, set up key, set up value, set next to NULL.
//pass a proper freeing function if value is allocated on the heap;

struct ht_node **hash_init_table(void) { //error checking left for user
    return calloc(HASH_TABLE_SIZE, sizeof(struct ht_node *));
}

void hash_add_node(struct ht_node **table, struct ht_node *node) {
    unsigned long hash = hash_generate(node->key);
    node->next = table[hash];
    table[hash] = node;
}
struct ht_node *hash_lookup_node(struct ht_node **table, const char *key) {
    unsigned long hash = hash_generate(key);
    const struct ht_node *curr = table[hash];
    while (curr) {
        if (strcmp(curr->key, key) == 0)
            return (struct ht_node *)curr;
        curr = curr->next;
    }
    return NULL;
}

// struct ht_node *hash_case_lookup_node(const struct ht_node **table, const char *key) {
//     //case insensitive lookup
//     unsigned long hash = hash_generate(key);
//     const struct ht_node *curr = table[hash];
//     while (curr) {
//         if (strcmp(curr->key, key) == 0)
//             return (struct ht_node *)curr;
//         curr = curr->next;
//     }
//     return NULL;
// }

void hash_free_table(struct ht_node **table, void (*free_value)(void *)) {
    for (size_t i = 0; i < HASH_TABLE_SIZE; i++) {
        struct ht_node *curr = table[i];
        while (curr) {
            struct ht_node *temp = curr;
            curr = curr->next;
            free(temp->key);
            if (temp->value)
                free_value(temp->value);
            free(temp);
        }
    }
    free(table);
}
struct ht_node *hash_pop_node();
