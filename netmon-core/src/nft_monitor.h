#ifndef NFT_MONITOR_H
#define NFT_MONITOR_H

#include <libubox/blobmsg_json.h>

#ifdef __cplusplus
extern "C" {
#endif

int init_nft_rules(void);
void cleanup_nft_rules(void);
void update_nft_stats(void);
void dump_traffic_json(struct blob_buf *b);
void save_stats(void);
void load_stats(void);
void clear_stats(void);

#ifdef __cplusplus
}
#endif

#endif
