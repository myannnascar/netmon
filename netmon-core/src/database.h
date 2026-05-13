#ifndef NETMON_DATABASE_H
#define NETMON_DATABASE_H

#include <stdint.h>
#include <sqlite3.h>
#include <libubox/blobmsg.h>

#define NETMON_DB_PATH "/etc/netmon/netmon.db"

int db_init(void);
void db_close(void);
int db_save_snapshot(const char *json_data);
int db_checkpoint(void);
int db_get_history(uint32_t start_time, struct blob_buf *b);
int db_get_baseline(uint32_t start_time, struct blob_buf *b);
int db_get_latest_snapshot(struct blob_buf *b);
void db_cleanup_old_data(int days);
void db_clear_history(void);

#endif
