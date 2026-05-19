#define _GNU_SOURCE
#include "nft_monitor.h"
#include "database.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>
#include <libmnl/libmnl.h>
#include <libnftnl/set.h>
#include <libnftnl/expr.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nf_tables.h>
#include <linux/rtnetlink.h>
#include <linux/neighbour.h>

#define MAX_DEVICES 1024
#define HASH_SIZE 2048
#define RECV_BUF_SIZE 8192
#define PERF_LOG_THRESHOLD_MS 200

struct device_stat {
    uint8_t family;
    uint8_t addr[16];
    uint64_t last_up_bytes;
    uint64_t last_down_bytes;
    uint64_t up_speed;
    uint64_t down_speed;
    uint64_t total_up;
    uint64_t total_down;
    uint32_t last_up_seen_gen;
    uint32_t last_down_seen_gen;
};

static struct device_stat devices[MAX_DEVICES];
static int device_count = 0;
static int device_hash[HASH_SIZE];
static uint32_t current_gen = 0;

static struct mnl_socket *nl = NULL;
static uint32_t portid;

static char req_bufs[4][512];
static int req_lens[4];
static uint64_t last_update_ms = 0;
static uint64_t current_interval_ms = 1000;

static uint64_t monotonic_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;

    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static inline uint32_t hash_ip(const uint8_t *addr, int len) {
    uint32_t h;
    if (len == 4) {
        memcpy(&h, addr, 4);
    } else {
        uint32_t p[4];
        memcpy(p, addr, 16);
        h = p[0] ^ p[1] ^ p[2] ^ p[3];
    }
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h & (HASH_SIZE - 1);
}

static inline int compare_ip(const struct device_stat *dev, const uint8_t *addr, int len) {
    if (dev->family == AF_INET && len == 4)
        return memcmp(dev->addr, addr, 4) == 0;
    if (dev->family == AF_INET6 && len == 16)
        return memcmp(dev->addr, addr, 16) == 0;
    return 0;
}

void clear_stats() {
    memset(devices, 0, sizeof(devices));
    device_count = 0;
    for (int i = 0; i < HASH_SIZE; i++) device_hash[i] = -1;

    system("nft flush set inet netmon v4_up");
    system("nft flush set inet netmon v4_down");
    system("nft flush set inet netmon v6_up");
    system("nft flush set inet netmon v6_down");
    db_clear_history();
}

int init_nft_rules() {
    nl = mnl_socket_open(NETLINK_NETFILTER);
    if (nl == NULL) return -1;
    if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
        mnl_socket_close(nl);
        return -1;
    }
    portid = mnl_socket_get_portid(nl);

    for (int i = 0; i < HASH_SIZE; i++) device_hash[i] = -1;

    const char *sets[] = {"v4_up", "v4_down", "v6_up", "v6_down"};
    for (int i = 0; i < 4; i++) {
        struct nftnl_set *s = nftnl_set_alloc();
        if (!s) continue;
        nftnl_set_set_str(s, NFTNL_SET_TABLE, "netmon");
        nftnl_set_set_str(s, NFTNL_SET_NAME, sets[i]);

        struct nlmsghdr *nlh = nftnl_nlmsg_build_hdr(req_bufs[i], NFT_MSG_GETSETELEM, NFPROTO_INET, NLM_F_DUMP, i + 1);
        nftnl_set_nlmsg_build_payload(nlh, s);
        req_lens[i] = nlh->nlmsg_len;
        nftnl_set_free(s);
    }

    return 0;
}

void cleanup_nft_rules() {
    if (nl) {
        mnl_socket_close(nl);
        nl = NULL;
    }
}

struct parse_ctx {
    int is_download;
};

static inline int parse_expr_cb(struct nftnl_expr *e, void *data) {
    uint64_t *bytes = data;
    const char *name = nftnl_expr_get_str(e, NFTNL_EXPR_NAME);
    if (name && name[0] == 'c' && strcmp(name, "counter") == 0)
        *bytes = nftnl_expr_get_u64(e, NFTNL_EXPR_CTR_BYTES);
    return 0;
}

static inline int find_device(const uint8_t *addr, int len) {
    uint32_t h = hash_ip(addr, len);
    for (int i = 0; i < HASH_SIZE; i++) {
        int pos = (h + i) & (HASH_SIZE - 1);
        int idx = device_hash[pos];
        if (idx == -1) break;
        if (compare_ip(&devices[idx], addr, len)) return idx;
    }
    return -1;
}

static inline int add_device(const uint8_t *addr, int len) {
    if (device_count >= MAX_DEVICES) return -1;

    int idx = device_count++;
    devices[idx].family = (len == 4) ? AF_INET : AF_INET6;
    memcpy(devices[idx].addr, addr, len);

    uint32_t h = hash_ip(addr, len);
    for (int i = 0; i < HASH_SIZE; i++) {
        int pos = (h + i) & (HASH_SIZE - 1);
        if (device_hash[pos] == -1) {
            device_hash[pos] = idx;
            break;
        }
    }
    return idx;
}

static inline uint64_t bytes_to_rate(uint64_t bytes, uint64_t interval_ms) {
    if (interval_ms == 0)
        interval_ms = 1000;
    return (bytes * 1000) / interval_ms;
}

static inline void update_stat(uint64_t bytes, uint64_t *last_bytes, uint64_t *speed, uint64_t *total) {
    if (*last_bytes == 0) {
        *last_bytes = bytes;
        *speed = 0;
        return;
    }

    if (bytes >= *last_bytes) {
        uint64_t diff = bytes - *last_bytes;
        *total += diff;
        *speed = bytes_to_rate(diff, current_interval_ms);
    } else {
        *total += bytes;
        *speed = bytes_to_rate(bytes, current_interval_ms);
    }
    *last_bytes = bytes;
}

static int set_elem_cb(const struct nlmsghdr *nlh, void *data) {
    struct parse_ctx *ctx = data;
    struct nftnl_set *s = nftnl_set_alloc();
    if (!s) return MNL_CB_OK;

    if (nftnl_set_elems_nlmsg_parse(nlh, s) < 0) {
        nftnl_set_free(s);
        return MNL_CB_OK;
    }

    struct nftnl_set_elems_iter *iter = nftnl_set_elems_iter_create(s);
    if (!iter) {
        nftnl_set_free(s);
        return MNL_CB_OK;
    }

    struct nftnl_set_elem *elem;
    while ((elem = nftnl_set_elems_iter_next(iter)) != NULL) {
        uint32_t len;
        const void *val = nftnl_set_elem_get(elem, NFTNL_SET_ELEM_KEY, &len);
        if (!val) continue;

        uint64_t bytes = 0;
        nftnl_set_elem_expr_foreach(elem, parse_expr_cb, &bytes);

        int found = find_device((const uint8_t *)val, len);
        if (found == -1) found = add_device((const uint8_t *)val, len);

        if (found != -1) {
            if (ctx->is_download) {
                devices[found].last_down_seen_gen = current_gen;
                update_stat(bytes, &devices[found].last_down_bytes, &devices[found].down_speed, &devices[found].total_down);
            } else {
                devices[found].last_up_seen_gen = current_gen;
                update_stat(bytes, &devices[found].last_up_bytes, &devices[found].up_speed, &devices[found].total_up);
            }
        }
    }

    nftnl_set_elems_iter_destroy(iter);
    nftnl_set_free(s);
    return MNL_CB_OK;
}

static void fetch_set_stats(int idx, int is_download) {
    static char recv_buf[RECV_BUF_SIZE];
    struct nlmsghdr *nlh = (struct nlmsghdr *)req_bufs[idx];
    uint32_t seq = (uint32_t)time(NULL);
    nlh->nlmsg_seq = seq;

    if (mnl_socket_sendto(nl, nlh, req_lens[idx]) < 0) return;

    struct parse_ctx ctx = { .is_download = is_download };
    int ret;
    while ((ret = mnl_socket_recvfrom(nl, recv_buf, RECV_BUF_SIZE)) > 0) {
        ret = mnl_cb_run(recv_buf, ret, seq, portid, set_elem_cb, &ctx);
        if (ret <= MNL_CB_STOP) break;
    }
}

void update_nft_stats() {
    if (!nl) return;

    uint64_t now_ms = monotonic_ms();
    uint64_t started_ms = now_ms;
    if (last_update_ms != 0 && now_ms > last_update_ms)
        current_interval_ms = now_ms - last_update_ms;
    else
        current_interval_ms = 1000;
    last_update_ms = now_ms;

    current_gen++;

    fetch_set_stats(0, 0);
    fetch_set_stats(1, 1);
    fetch_set_stats(2, 0);
    fetch_set_stats(3, 1);

    for (int i = 0; i < device_count; i++) {
        if (devices[i].last_up_seen_gen != current_gen)
            devices[i].up_speed = 0;
        if (devices[i].last_down_seen_gen != current_gen)
            devices[i].down_speed = 0;
    }

    now_ms = monotonic_ms();
    if (started_ms != 0 && now_ms > started_ms && now_ms - started_ms > PERF_LOG_THRESHOLD_MS) {
        fprintf(stderr, "netmon: [PERF] nft update took %llu ms for %d devices\n",
                (unsigned long long)(now_ms - started_ms), device_count);
    }
}

static char ip_cache_buf[64];
static char mac_cache_buf[18];

struct neigh_entry {
    uint8_t family;
    uint8_t addr[16];
    uint8_t mac[6];
    uint8_t has_mac;
    uint8_t online;
};

struct neigh_ctx {
    struct neigh_entry *entries;
    size_t count;
    size_t cap;
};

static int is_online_state(uint16_t state) {
    return state & (NUD_REACHABLE | NUD_STALE | NUD_DELAY | NUD_PROBE | NUD_PERMANENT | NUD_NOARP);
}

static int neigh_attr_cb(const struct nlattr *attr, void *data) {
    struct nlattr **tb = data;
    int type = mnl_attr_get_type(attr);

    if (mnl_attr_type_valid(attr, NDA_MAX) < 0)
        return MNL_CB_OK;

    tb[type] = (struct nlattr *)attr;
    return MNL_CB_OK;
}

static int neigh_add_entry(struct neigh_ctx *ctx, int family, const void *addr, int len, uint16_t state) {
    if (!ctx || !addr || len <= 0 || len > 16)
        return MNL_CB_OK;

    if (ctx->count == ctx->cap) {
        size_t ncap = ctx->cap ? ctx->cap * 2 : 128;
        struct neigh_entry *n = realloc(ctx->entries, ncap * sizeof(*n));
        if (!n)
            return MNL_CB_ERROR;
        ctx->entries = n;
        ctx->cap = ncap;
    }

    struct neigh_entry *e = &ctx->entries[ctx->count++];
    e->family = (uint8_t)family;
    memset(e->addr, 0, sizeof(e->addr));
    memset(e->mac, 0, sizeof(e->mac));
    memcpy(e->addr, addr, len);
    e->has_mac = 0;
    e->online = is_online_state(state) ? 1 : 0;
    return MNL_CB_OK;
}

static int neigh_parse_cb(const struct nlmsghdr *nlh, void *data) {
    struct neigh_ctx *ctx = data;
    struct ndmsg *ndm = mnl_nlmsg_get_payload(nlh);
    if (ndm->ndm_family != AF_INET && ndm->ndm_family != AF_INET6)
        return MNL_CB_OK;

    struct nlattr *tb[NDA_MAX + 1] = {0};
    mnl_attr_parse(nlh, sizeof(*ndm), neigh_attr_cb, tb);

    if (!tb[NDA_DST])
        return MNL_CB_OK;

    int len = mnl_attr_get_payload_len(tb[NDA_DST]);
    if ((ndm->ndm_family == AF_INET && len != 4) || (ndm->ndm_family == AF_INET6 && len != 16))
        return MNL_CB_OK;

    if (neigh_add_entry(ctx, ndm->ndm_family, mnl_attr_get_payload(tb[NDA_DST]), len, ndm->ndm_state) != MNL_CB_OK)
        return MNL_CB_ERROR;

    if (tb[NDA_LLADDR] && mnl_attr_get_payload_len(tb[NDA_LLADDR]) >= 6) {
        struct neigh_entry *e = &ctx->entries[ctx->count - 1];
        memcpy(e->mac, mnl_attr_get_payload(tb[NDA_LLADDR]), 6);
        e->has_mac = 1;
    }

    return MNL_CB_OK;
}

static int read_neighbors(struct neigh_ctx *ctx) {
    if (!ctx)
        return -1;

    struct mnl_socket *nlrt = mnl_socket_open(NETLINK_ROUTE);
    if (!nlrt)
        return -1;

    if (mnl_socket_bind(nlrt, 0, MNL_SOCKET_AUTOPID) < 0) {
        mnl_socket_close(nlrt);
        return -1;
    }

    char buf[8192];
    struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type = RTM_GETNEIGH;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    nlh->nlmsg_seq = (unsigned)time(NULL);

    struct ndmsg *ndm = mnl_nlmsg_put_extra_header(nlh, sizeof(*ndm));
    memset(ndm, 0, sizeof(*ndm));
    ndm->ndm_family = AF_UNSPEC;

    if (mnl_socket_sendto(nlrt, nlh, nlh->nlmsg_len) < 0) {
        mnl_socket_close(nlrt);
        return -1;
    }

    int ret;
    unsigned int rt_portid = mnl_socket_get_portid(nlrt);
    while ((ret = mnl_socket_recvfrom(nlrt, buf, sizeof(buf))) > 0) {
        ret = mnl_cb_run(buf, ret, nlh->nlmsg_seq, rt_portid, neigh_parse_cb, ctx);
        if (ret <= 0)
            break;
    }

    mnl_socket_close(nlrt);
    if (ret == -1)
        return -1;

    return 0;
}

static int device_online(const struct neigh_ctx *ctx, const struct device_stat *dev) {
    if (!ctx || !dev || !ctx->entries)
        return -1;

    int len = (dev->family == AF_INET) ? 4 : 16;
    for (size_t i = 0; i < ctx->count; i++) {
        if (ctx->entries[i].family == dev->family && memcmp(ctx->entries[i].addr, dev->addr, len) == 0)
            return ctx->entries[i].online ? 1 : 0;
    }

    return -1;
}

static const struct neigh_entry *find_neigh_entry(const struct neigh_ctx *ctx, const struct device_stat *dev) {
    int len;
    size_t i;

    if (!ctx || !dev || !ctx->entries)
        return NULL;

    len = (dev->family == AF_INET) ? 4 : 16;
    for (i = 0; i < ctx->count; i++) {
        if (ctx->entries[i].family == dev->family && memcmp(ctx->entries[i].addr, dev->addr, len) == 0)
            return &ctx->entries[i];
    }

    return NULL;
}

void dump_traffic_json(struct blob_buf *b) {
    uint64_t started_ms = monotonic_ms();
    struct neigh_ctx nctx = {0};
    int neigh_ok = read_neighbors(&nctx);

    void *arr = blobmsg_open_array(b, "devices");
    for (int i = 0; i < device_count; i++) {
        inet_ntop(devices[i].family, devices[i].addr, ip_cache_buf, sizeof(ip_cache_buf));

        void *tbl = blobmsg_open_table(b, NULL);
        const struct neigh_entry *entry = neigh_ok == 0 ? find_neigh_entry(&nctx, &devices[i]) : NULL;
        blobmsg_add_string(b, "ip", ip_cache_buf);
        if (entry && entry->has_mac) {
            snprintf(mac_cache_buf, sizeof(mac_cache_buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                entry->mac[0], entry->mac[1], entry->mac[2],
                entry->mac[3], entry->mac[4], entry->mac[5]);
            blobmsg_add_string(b, "mac", mac_cache_buf);
        }
        blobmsg_add_u64(b, "up_speed", devices[i].up_speed);
        blobmsg_add_u64(b, "down_speed", devices[i].down_speed);
        blobmsg_add_u64(b, "total_up", devices[i].total_up);
        blobmsg_add_u64(b, "total_down", devices[i].total_down);

        if (entry) {
            int online = entry->online ? 1 : 0;
            if (online >= 0)
                blobmsg_add_u8(b, "online", (uint8_t)online);
        }

        blobmsg_close_table(b, tbl);
    }
    blobmsg_close_array(b, arr);

    free(nctx.entries);

    uint64_t finished_ms = monotonic_ms();
    if (started_ms != 0 && finished_ms > started_ms && finished_ms - started_ms > PERF_LOG_THRESHOLD_MS) {
        fprintf(stderr, "netmon: [PERF] traffic dump took %llu ms for %d devices\n",
                (unsigned long long)(finished_ms - started_ms), device_count);
    }
}

static struct blob_attr *find_devices_attr(struct blob_attr *attr) {
    if (!attr) return NULL;
    if (blobmsg_type(attr) == BLOBMSG_TYPE_ARRAY && blobmsg_name(attr) && strcmp(blobmsg_name(attr), "devices") == 0)
        return attr;

    if (blobmsg_type(attr) == BLOBMSG_TYPE_TABLE) {
        struct blob_attr *cur;
        int rem;
        blobmsg_for_each_attr(cur, attr, rem) {
            struct blob_attr *found = find_devices_attr(cur);
            if (found) return found;
        }
    }
    return NULL;
}

void load_stats() {
    struct blob_buf b = {0};
    blobmsg_buf_init(&b);

    if (db_get_latest_snapshot(&b) != 0) {
        fprintf(stderr, "netmon: [LOAD] No snapshot to load or DB error\n");
        blob_buf_free(&b);
        return;
    }

    struct blob_attr *devices_attr = find_devices_attr(b.head);

    if (!devices_attr) {
        fprintf(stderr, "netmon: [LOAD] Could not find valid 'devices' array in snapshot\n");
        blob_buf_free(&b);
        return;
    }

    device_count = 0;
    for (int i = 0; i < HASH_SIZE; i++) device_hash[i] = -1;

    int recovered = 0;
    struct blob_attr *cur;
    int rem;
    blobmsg_for_each_attr(cur, devices_attr, rem) {
        struct blob_attr *dtb[3] = {NULL};
        static const struct blobmsg_policy dp[3] = {
            { .name = "ip", .type = BLOBMSG_TYPE_STRING },
            { .name = "total_up", .type = BLOBMSG_TYPE_UNSPEC },
            { .name = "total_down", .type = BLOBMSG_TYPE_UNSPEC },
        };

        if (blobmsg_parse(dp, 3, dtb, blobmsg_data(cur), blobmsg_data_len(cur)) != 0) {
            fprintf(stderr, "netmon: [LOAD] Failed to parse device object\n");
            continue;
        }

        if (!dtb[0] || !dtb[1] || !dtb[2]) {
            fprintf(stderr, "netmon: [LOAD] Missing required fields in device object (ip: %p, up: %p, down: %p)\n",
                    dtb[0], dtb[1], dtb[2]);
            continue;
        }

        const char *ip_str = blobmsg_get_string(dtb[0]);
        uint8_t addr[16];
        int family = (strchr(ip_str, ':')) ? AF_INET6 : AF_INET;
        if (inet_pton(family, ip_str, addr) != 1) {
            fprintf(stderr, "netmon: [LOAD] Invalid IP address: %s\n", ip_str);
            continue;
        }
        int len = (family == AF_INET ? 4 : 16);

        int idx = add_device(addr, len);
        if (idx != -1) {
            devices[idx].total_up = (blobmsg_type(dtb[1]) == BLOBMSG_TYPE_INT64) ? blobmsg_get_u64(dtb[1]) : blobmsg_get_u32(dtb[1]);
            devices[idx].total_down = (blobmsg_type(dtb[2]) == BLOBMSG_TYPE_INT64) ? blobmsg_get_u64(dtb[2]) : blobmsg_get_u32(dtb[2]);
            devices[idx].last_up_bytes = 0;
            devices[idx].last_down_bytes = 0;
            devices[idx].last_up_seen_gen = current_gen;
            devices[idx].last_down_seen_gen = current_gen;
            recovered++;
        } else {
            fprintf(stderr, "netmon: [LOAD] Failed to add device (MAX_DEVICES reached?)\n");
        }
    }

    fprintf(stderr, "netmon: [LOAD] Successfully recovered %d devices from database\n", recovered);
    blob_buf_free(&b);
}

void save_stats() {
    if (device_count == 0) return;
    struct blob_buf b = {0};
    blobmsg_buf_init(&b);
    dump_traffic_json(&b);

    char *json = blobmsg_format_json(b.head, false);
    if (json) {
        if (db_save_snapshot(json) == 0) {
            if (db_checkpoint() != 0)
                fprintf(stderr, "netmon: [SAVE] SQLite checkpoint failed\n");
        } else {
            fprintf(stderr, "netmon: [SAVE] Failed to save stats to database\n");
        }
        free(json);
    }
    blob_buf_free(&b);
}
