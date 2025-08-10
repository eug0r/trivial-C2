#include "handlers.h"
#include "http-server.h"
#include "init-database.h"

int main(void) {
    const char *db_path = C2_DB_PATH;
    init_database(db_path);
    http_init_server(&router);
}