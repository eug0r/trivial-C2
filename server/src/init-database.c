#include "init-database.h"
#include <stdlib.h>
#include <stdio.h>
#include "sqlite3.h"
int init_database(const char *db_path) {
    sqlite3 *db;
    char *err_msg = NULL;

    const char *schema_sql =
        "CREATE TABLE IF NOT EXISTS agents (\n"
        "   uuid    TEXT NOT NULL UNIQUE,\n"
        "   handle	TEXT,\n"
        "   hostname	TEXT,\n"
        "   PRIMARY KEY(uuid)\n"
        ");"
        "CREATE TABLE IF NOT EXISTS tasks (\n"
        "   uuid	TEXT NOT NULL UNIQUE,\n"
        "   category	TEXT NOT NULL,\n"
        "   agent_id	TEXT NOT NULL,\n"
        "   status	INTEGER NOT NULL,\n"
        "   queue_no	INTEGER,\n"
        "   options	TEXT,\n"
        "   result	TEXT,\n"
        "   PRIMARY KEY(\"queue_no\" AUTOINCREMENT)\n"
        "   FOREIGN KEY(\"agent_id\") REFERENCES agents(\"uuid\") ON DELETE CASCADE\n"
        ");";

    // create db if it doesn't exist
    int rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[-] Cannot open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    // execute schema
    rc = sqlite3_exec(db, schema_sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[-] SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return 1;
    }

    sqlite3_close(db);
    printf("[+] Database initialized at %s\n", db_path);
    return 0;
}