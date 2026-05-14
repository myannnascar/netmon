#define _GNU_SOURCE
#include "database.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libubox/blobmsg_json.h>

static sqlite3 *db = NULL;

struct aggregated_device {
    char key[80];
    char ip[64];
    char mac[18];
    uint8_t has_online;
    uint8_t online;
    uint64_t up_speed;
    uint64_t down_speed;
    uint64_t total_up;
    uint64_t total_down;
};

static uint64_t get_blob_u64(struct blob_attr *attr)
{
    if (!attr)
        return 0;

    return blobmsg_type(attr) == BLOBMSG_TYPE_INT64 ? blobmsg_get_u64(attr) : blobmsg_get_u32(attr);
}

static int prefer_ip(const char *current_ip, const char *candidate_ip)
{
    if (!candidate_ip || !*candidate_ip)
        return 0;

    if (!current_ip || !*current_ip)
        return 1;

    if (strchr(current_ip, ':') && strchr(candidate_ip, '.'))
        return 1;

    return 0;
}

static struct aggregated_device *find_aggregated_device(struct aggregated_device *items, size_t count, const char *key)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (strcmp(items[i].key, key) == 0)
            return &items[i];
    }

    return NULL;
}

static struct blob_attr *find_devices_attr(struct blob_attr *attr)
{
    struct blob_attr *cur;
    int rem;

    if (!attr)
        return NULL;

    if (blobmsg_type(attr) == BLOBMSG_TYPE_ARRAY && blobmsg_name(attr) && strcmp(blobmsg_name(attr), "devices") == 0)
        return attr;

    if (blobmsg_type(attr) != BLOBMSG_TYPE_TABLE)
        return NULL;

    blobmsg_for_each_attr(cur, attr, rem) {
        struct blob_attr *found = find_devices_attr(cur);
        if (found)
            return found;
    }

    return NULL;
}

static int aggregate_devices_attr(struct blob_attr *devices_attr, struct blob_buf *b)
{
    static const struct blobmsg_policy policy[7] = {
        { .name = "ip", .type = BLOBMSG_TYPE_STRING },
        { .name = "mac", .type = BLOBMSG_TYPE_STRING },
        { .name = "up_speed", .type = BLOBMSG_TYPE_UNSPEC },
        { .name = "down_speed", .type = BLOBMSG_TYPE_UNSPEC },
        { .name = "total_up", .type = BLOBMSG_TYPE_UNSPEC },
        { .name = "total_down", .type = BLOBMSG_TYPE_UNSPEC },
        { .name = "online", .type = BLOBMSG_TYPE_UNSPEC },
    };
    struct aggregated_device *items = NULL;
    size_t count = 0;
    size_t cap = 0;
    struct blob_attr *cur;
    int rem;
    void *arr;

    if (!devices_attr)
        return -1;

    blobmsg_for_each_attr(cur, devices_attr, rem) {
        struct blob_attr *tb[7] = {0};
        const char *ip;
        const char *mac = NULL;
        char key[80];
        struct aggregated_device *item;

        if (blobmsg_parse(policy, 7, tb, blobmsg_data(cur), blobmsg_data_len(cur)) != 0 || !tb[0])
            continue;

        ip = blobmsg_get_string(tb[0]);
        if (!ip || !*ip)
            continue;

        if (tb[1] && blobmsg_get_string(tb[1]) && *blobmsg_get_string(tb[1])) {
            mac = blobmsg_get_string(tb[1]);
            snprintf(key, sizeof(key), "mac:%s", mac);
        } else {
            snprintf(key, sizeof(key), "ip:%s", ip);
        }

        item = find_aggregated_device(items, count, key);
        if (!item) {
            struct aggregated_device *next;

            if (count == cap) {
                size_t next_cap = cap ? cap * 2 : 64;
                next = realloc(items, next_cap * sizeof(*items));
                if (!next) {
                    free(items);
                    return -1;
                }
                items = next;
                cap = next_cap;
            }

            item = &items[count++];
            memset(item, 0, sizeof(*item));
            strncpy(item->key, key, sizeof(item->key) - 1);
            strncpy(item->ip, ip, sizeof(item->ip) - 1);
            if (mac)
                strncpy(item->mac, mac, sizeof(item->mac) - 1);
        } else if (prefer_ip(item->ip, ip)) {
            strncpy(item->ip, ip, sizeof(item->ip) - 1);
            item->ip[sizeof(item->ip) - 1] = '\0';
        }

        item->up_speed += get_blob_u64(tb[2]);
        item->down_speed += get_blob_u64(tb[3]);
        item->total_up += get_blob_u64(tb[4]);
        item->total_down += get_blob_u64(tb[5]);

        if (tb[6]) {
            item->has_online = 1;
            if (get_blob_u64(tb[6]) != 0)
                item->online = 1;
        }
    }

    arr = blobmsg_open_array(b, "devices");
    for (size_t i = 0; i < count; i++) {
        void *tbl = blobmsg_open_table(b, NULL);
        blobmsg_add_string(b, "ip", items[i].ip);
        if (items[i].mac[0] != '\0')
            blobmsg_add_string(b, "mac", items[i].mac);
        blobmsg_add_u64(b, "up_speed", items[i].up_speed);
        blobmsg_add_u64(b, "down_speed", items[i].down_speed);
        blobmsg_add_u64(b, "total_up", items[i].total_up);
        blobmsg_add_u64(b, "total_down", items[i].total_down);
        if (items[i].has_online)
            blobmsg_add_u8(b, "online", items[i].online ? 1 : 0);
        blobmsg_close_table(b, tbl);
    }
    blobmsg_close_array(b, arr);

    free(items);
    return 0;
}

int db_init(void) {
    int rc = sqlite3_open(NETMON_DB_PATH, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_exec(db, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous = FULL;", NULL, NULL, NULL);

    char *err_msg = NULL;
    const char *sql = "CREATE TABLE IF NOT EXISTS traffic_history ("
                      "timestamp INTEGER PRIMARY KEY, "
                      "data TEXT NOT NULL);";

    rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    return 0;
}

void db_close(void) {
    if (db) {
        sqlite3_close(db);
        db = NULL;
    }
}

int db_save_snapshot(const char *json_data) {
    if (!db) return -1;

    const char *sql = "INSERT OR REPLACE INTO traffic_history (timestamp, data) VALUES (?, ?);";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)time(NULL));
    sqlite3_bind_text(stmt, 2, json_data, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_checkpoint(void) {
    if (!db) return -1;

    int rc = sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_FULL, NULL, NULL);
    return (rc == SQLITE_OK) ? 0 : -1;
}

int db_get_history(uint32_t start_time, struct blob_buf *b) {
    if (!db) return -1;

    const char *sql = "SELECT timestamp, data FROM traffic_history WHERE timestamp >= ? ORDER BY timestamp ASC LIMIT 100;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)start_time);

    void *arr = blobmsg_open_array(b, "history");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        struct blob_buf parsed = {0};
        struct blob_attr *devices_attr = NULL;
        void *tbl = blobmsg_open_table(b, NULL);
        blobmsg_add_u32(b, "timestamp", (uint32_t)sqlite3_column_int64(stmt, 0));
        
        const char *json_str = (const char *)sqlite3_column_text(stmt, 1);
        if (json_str) {
            const char *json_start = strchr(json_str, '{');
            if (!json_start) json_start = strchr(json_str, '[');
            if (json_start) {
                blobmsg_buf_init(&parsed);
                if (!blobmsg_add_json_from_string(&parsed, json_start)) {
                    fprintf(stderr, "netmon: [DB] Failed to parse history JSON at timestamp %llu\n", 
                            (unsigned long long)sqlite3_column_int64(stmt, 0));
                } else {
                    devices_attr = find_devices_attr(parsed.head);
                    aggregate_devices_attr(devices_attr, b);
                }
            }
        }
        
        blobmsg_close_table(b, tbl);
        blob_buf_free(&parsed);
    }
    blobmsg_close_array(b, arr);

    sqlite3_finalize(stmt);
    return 0;
}

static int db_add_parsed_json(struct blob_buf *b, const char *json_str, uint32_t ts)
{
    struct blob_buf parsed = {0};
    struct blob_attr *devices_attr = NULL;
    if (!b || !json_str) return -1;

    void *tbl = blobmsg_open_table(b, "baseline");
    blobmsg_add_u32(b, "timestamp", ts);

    const char *json_start = strchr(json_str, '{');
    if (!json_start) json_start = strchr(json_str, '[');
    if (json_start) {
        blobmsg_buf_init(&parsed);
        if (!blobmsg_add_json_from_string(&parsed, json_start)) {
            fprintf(stderr, "netmon: [DB] Failed to parse baseline JSON at timestamp %u, deleting corrupted record\n", ts);
            char sql[128];
            snprintf(sql, sizeof(sql), "DELETE FROM traffic_history WHERE timestamp = %u;", ts);
            sqlite3_exec(db, sql, NULL, NULL, NULL);
            blobmsg_close_table(b, tbl);
            blob_buf_free(&parsed);
            return -1;
        }

        devices_attr = find_devices_attr(parsed.head);
        if (aggregate_devices_attr(devices_attr, b) != 0) {
            blobmsg_close_table(b, tbl);
            blob_buf_free(&parsed);
            return -1;
        }
    } else {
        fprintf(stderr, "netmon: [DB] No valid JSON start found for baseline at timestamp %u, deleting corrupted record\n", ts);
        char sql[128];
        snprintf(sql, sizeof(sql), "DELETE FROM traffic_history WHERE timestamp = %u;", ts);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
        blobmsg_close_table(b, tbl);
        return -1;
    }

    blobmsg_close_table(b, tbl);
    blob_buf_free(&parsed);
    return 0;
}

int db_get_baseline(uint32_t start_time, struct blob_buf *b)
{
    if (!db) return -1;
    if (!b) return -1;

    sqlite3_stmt *stmt = NULL;
    const char *json_str = NULL;
    uint32_t ts = 0;

    const char *sql1 = "SELECT timestamp, data FROM traffic_history WHERE timestamp <= ? ORDER BY timestamp DESC LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql1, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)start_time);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        ts = (uint32_t)sqlite3_column_int64(stmt, 0);
        json_str = (const char *)sqlite3_column_text(stmt, 1);
        
        int ret = -1;
        if (json_str)
            ret = db_add_parsed_json(b, json_str, ts);
            
        sqlite3_finalize(stmt);
        return ret;
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    const char *sql2 = "SELECT timestamp, data FROM traffic_history WHERE timestamp >= ? ORDER BY timestamp ASC LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql2, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)start_time);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        ts = (uint32_t)sqlite3_column_int64(stmt, 0);
        json_str = (const char *)sqlite3_column_text(stmt, 1);
        
        int ret = -1;
        if (json_str)
            ret = db_add_parsed_json(b, json_str, ts);
            
        sqlite3_finalize(stmt);
        return ret;
    }

    sqlite3_finalize(stmt);
    return -1;
}

void db_cleanup_old_data(int days) {
    if (!db) return;
    const char *sql = "DELETE FROM traffic_history WHERE timestamp < strftime('%s', 'now', ?);";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        char day_str[32];
        snprintf(day_str, sizeof(day_str), "-%d days", days);
        sqlite3_bind_text(stmt, 1, day_str, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void db_clear_history(void) {
    if (!db) return;
    sqlite3_exec(db, "DELETE FROM traffic_history;", NULL, NULL, NULL);
    sqlite3_exec(db, "VACUUM;", NULL, NULL, NULL);
}

int db_get_latest_snapshot(struct blob_buf *b) {
    if (!db) return -1;
    const char *sql = "SELECT data FROM traffic_history ORDER BY timestamp DESC LIMIT 1;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    int found = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *json_str = (const char *)sqlite3_column_text(stmt, 0);
        if (json_str) {
            const char *json_start = strchr(json_str, '{');
            if (!json_start) json_start = strchr(json_str, '[');
            
            if (json_start) {
                if (json_start != json_str) {
                    fprintf(stderr, "netmon: [DB] Warning: skipping %ld bytes of garbage before JSON\n", (long)(json_start - json_str));
                }
                fprintf(stderr, "netmon: [DB] Parsing JSON: %.100s...\n", json_start);
                
                if (blobmsg_add_json_from_string(b, json_start)) {
                    found = 1;
                } else {
                    fprintf(stderr, "netmon: [DB] Failed to parse JSON from DB\n");
                }
            } else {
                fprintf(stderr, "netmon: [DB] No valid JSON start found in DB data\n");
            }
        }
    }

    sqlite3_finalize(stmt);
    return found ? 0 : -1;
}
