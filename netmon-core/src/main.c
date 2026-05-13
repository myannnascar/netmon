#include <libubox/uloop.h>
#include <libubox/runqueue.h>
#include <libubox/blobmsg_json.h>
#include <libubus.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>

#include "nft_monitor.h"
#include "database.h"

static struct ubus_context *ctx;
static struct uloop_timeout traffic_timer;
static struct uloop_timeout cleanup_timer;

#define UPDATE_INTERVAL 1000
#define SNAPSHOT_INTERVAL 300
#define CLEANUP_INTERVAL (24 * 3600 * 1000)

static void handle_signal(int sig)
{
    (void)sig;
    uloop_end();
}

static void traffic_timer_cb(struct uloop_timeout *t)
{
    static int save_counter = SNAPSHOT_INTERVAL - 30;

    update_nft_stats();

    if (++save_counter >= SNAPSHOT_INTERVAL) {
        save_stats();
        save_counter = 0;
    }

    uloop_timeout_set(t, UPDATE_INTERVAL);
}

static void cleanup_timer_cb(struct uloop_timeout *t)
{
    db_cleanup_old_data(30);
    uloop_timeout_set(t, CLEANUP_INTERVAL);
}

static int netmon_traffic(struct ubus_context *ctx, struct ubus_object *obj,
                              struct ubus_request_data *req, const char *method,
                              struct blob_attr *msg)
{
    (void)obj; (void)method; (void)msg;
    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    dump_traffic_json(&b);
    ubus_send_reply(ctx, req, b.head);
    blob_buf_free(&b);
    return 0;
}

enum {
    HIST_START,
    __HIST_MAX
};

static const struct blobmsg_policy hist_policy[__HIST_MAX] = {
    [HIST_START] = { .name = "start", .type = BLOBMSG_TYPE_INT32 },
};

static int netmon_history(struct ubus_context *ctx, struct ubus_object *obj,
                               struct ubus_request_data *req, const char *method,
                               struct blob_attr *msg)
{
    (void)obj; (void)method;
    struct blob_attr *tb[__HIST_MAX];
    uint32_t start_time = 0;

    blobmsg_parse(hist_policy, __HIST_MAX, tb, blobmsg_data(msg), blobmsg_len(msg));
    if (tb[HIST_START])
        start_time = blobmsg_get_u32(tb[HIST_START]);

    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    db_get_history(start_time, &b);
    ubus_send_reply(ctx, req, b.head);
    blob_buf_free(&b);
    return 0;
}

enum {
    BASE_START,
    __BASE_MAX
};

static const struct blobmsg_policy base_policy[__BASE_MAX] = {
    [BASE_START] = { .name = "start", .type = BLOBMSG_TYPE_INT32 },
};

static int netmon_baseline(struct ubus_context *ctx, struct ubus_object *obj,
                               struct ubus_request_data *req, const char *method,
                               struct blob_attr *msg)
{
    (void)obj; (void)method;
    struct blob_attr *tb[__BASE_MAX];
    uint32_t start_time = 0;

    blobmsg_parse(base_policy, __BASE_MAX, tb, blobmsg_data(msg), blobmsg_len(msg));
    if (tb[BASE_START])
        start_time = blobmsg_get_u32(tb[BASE_START]);

    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    if (db_get_baseline(start_time, &b) != 0) {
        blobmsg_add_u8(&b, "found", 0);
    } else {
        blobmsg_add_u8(&b, "found", 1);
    }
    ubus_send_reply(ctx, req, b.head);
    blob_buf_free(&b);
    return 0;
}

static int netmon_clear(struct ubus_context *ctx, struct ubus_object *obj,
                            struct ubus_request_data *req, const char *method,
                            struct blob_attr *msg)
{
    (void)ctx; (void)obj; (void)req; (void)method; (void)msg;
    clear_stats();
    return 0;
}

static const struct ubus_method netmon_methods[] = {
    UBUS_METHOD_NOARG("traffic", netmon_traffic),
    UBUS_METHOD("history", netmon_history, hist_policy),
    UBUS_METHOD("baseline", netmon_baseline, base_policy),
    UBUS_METHOD_NOARG("clear", netmon_clear),
};

static struct ubus_object_type netmon_object_type =
    UBUS_OBJECT_TYPE("netmon", netmon_methods);

static struct ubus_object netmon_object = {
    .name = "netmon",
    .type = &netmon_object_type,
    .methods = netmon_methods,
    .n_methods = ARRAY_SIZE(netmon_methods),
};

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uloop_init();

    ctx = ubus_connect(NULL);
    if (!ctx) {
        fprintf(stderr, "Failed to connect to ubus\n");
        return -1;
    }

    ubus_add_uloop(ctx);
    if (ubus_add_object(ctx, &netmon_object) != 0) {
        fprintf(stderr, "Failed to add ubus object\n");
        return -1;
    }

    if (init_nft_rules() < 0) {
        fprintf(stderr, "netmon: [ERROR] Failed to initialize nft rules. Traffic monitoring will not work.\n");
    }

    if (db_init() < 0) {
        fprintf(stderr, "netmon: [ERROR] Failed to initialize database. Persistence will not work.\n");
    }

    load_stats();

    traffic_timer.cb = traffic_timer_cb;
    traffic_timer_cb(&traffic_timer);

    cleanup_timer.cb = cleanup_timer_cb;
    cleanup_timer_cb(&cleanup_timer);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    uloop_run();

    save_stats();

    db_close();
    ubus_free(ctx);
    uloop_done();
    cleanup_nft_rules();

    return 0;
}
