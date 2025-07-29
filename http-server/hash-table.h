

#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#define HASH_TABLE_SIZE 17
struct ht_node {
    char *key;
    void *value;
    struct ht_node *next;
};

struct ht_node **hash_init_table(void);
void hash_add_node(struct ht_node **table, struct ht_node *node);
struct ht_node *hash_lookup_node(const struct ht_node **table, const char *key);
void hash_free_table(struct ht_node **table, void (*free_value)(void *));
//not sure how to make a reenterant, thread-safe hash table traversal function
//that returns a single node at each call until all nodes are returned.
//so I will implement it from inside the code.
#endif //HASH_TABLE_H
