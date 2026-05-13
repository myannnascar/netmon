#define _GNU_SOURCE
#include "database.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <libubox/blobmsg_json.h>

static sqlite3 *db = NULL;

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
        void *tbl = blobmsg_open_table(b, NULL);
        blobmsg_add_u32(b, "timestamp", (uint32_t)sqlite3_column_int64(stmt, 0));
        
        const char *json_str = (const char *)sqlite3_column_text(stmt, 1);
        if (json_str) {
            const char *json_start = strchr(json_str, '{');
            if (!json_start) json_start = strchr(json_str, '[');
            if (json_start) {
                if (!blobmsg_add_json_from_string(b, json_start)) {
                    fprintf(stderr, "netmon: [DB] Failed to parse history JSON at timestamp %llu\n", 
                            (unsigned long long)sqlite3_column_int64(stmt, 0));
                }
            }
        }
        
        blobmsg_close_table(b, tbl);
    }
    blobmsg_close_array(b, arr);

    sqlite3_finalize(stmt);
    return 0;
}

static int db_add_parsed_json(struct blob_buf *b, const char *json_str, uint32_t ts)
{
    if (!b || !json_str) return -1;

    void *tbl = blobmsg_open_table(b, "baseline");
    blobmsg_add_u32(b, "timestamp", ts);

    const char *json_start = strchr(json_str, '{');
    if (!json_start) json_start = strchr(json_str, '[');
    if (json_start) {
        if (!blobmsg_add_json_from_string(b, json_start)) {
            fprintf(stderr, "netmon: [DB] Failed to parse baseline JSON at timestamp %u, deleting corrupted record\n", ts);
            char sql[128];
            snprintf(sql, sizeof(sql), "DELETE FROM traffic_history WHERE timestamp = %u;", ts);
            sqlite3_exec(db, sql, NULL, NULL, NULL);
            blobmsg_close_table(b, tbl);
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
