#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NC_WINDIVERT_ADDRESS_SIZE 80
#define NC_MAX_QUEUE_PACKETS 4096u
#define NC_MAX_QUEUE_BYTES (8u * 1024u * 1024u)
#define NC_POOL_LIMIT 1024u

/* Burst loss state machine (allow -> drop periodic cycle). */
typedef struct {
    uint64_t allow_ns;
    uint64_t drop_ns;
    uint64_t phase_start_ns;
} NcBurstLossState;

/* Token/serialization bandwidth scheduler. */
typedef struct {
    uint64_t next_send_ns;
} NcBandwidthScheduler;

typedef struct {
    uint64_t due_ns;
    uint64_t next_send_ns;
    uint64_t expires_ns;
} NcSendPlan;

uint64_t nc_sat_add_u64(uint64_t value, uint64_t delta);
void nc_burst_loss_init(NcBurstLossState *state, int32_t allow_ms, int32_t drop_ms, uint64_t now_ns);
bool nc_burst_loss_should_drop(const NcBurstLossState *state, uint64_t now_ns);
uint64_t nc_packet_service_ns(size_t length, uint64_t bandwidth_kbps);
void nc_bandwidth_scheduler_init(NcBandwidthScheduler *scheduler);
bool nc_bandwidth_scheduler_plan(const NcBandwidthScheduler *scheduler, uint64_t now_ns,
                                 uint64_t intended_delay_ns, size_t length,
                                 uint64_t bandwidth_kbps, uint64_t max_queue_ns,
                                 NcSendPlan *out);
void nc_bandwidth_scheduler_commit(NcBandwidthScheduler *scheduler, const NcSendPlan *plan);
uint64_t nc_paced_send_time(uint64_t due_ns, bool has_last, uint64_t last_send_end_ns,
                            uint64_t now_ns);

/* Queued packet record, binary min-heap queue, and reusable object pool. */
typedef struct NcQueuedPacket {
    unsigned char *data;
    size_t cap;
    size_t length;
    unsigned char address[NC_WINDIVERT_ADDRESS_SIZE];
    uint64_t due_ns;
    uint64_t expires_ns;
    uint64_t sequence;
} NcQueuedPacket;

typedef struct {
    NcQueuedPacket **items;
    size_t len;
    size_t cap;
    size_t bytes;
} NcPacketQueue;

typedef struct {
    NcQueuedPacket **items;
    size_t len;
    size_t cap;
} NcPacketPool;

void nc_packet_queue_init(NcPacketQueue *queue);
void nc_packet_queue_free(NcPacketQueue *queue);
bool nc_packet_queue_push(NcPacketQueue *queue, NcQueuedPacket *item);
NcQueuedPacket *nc_packet_queue_peek(const NcPacketQueue *queue);
NcQueuedPacket *nc_packet_queue_pop(NcPacketQueue *queue);

void nc_packet_pool_init(NcPacketPool *pool);
void nc_packet_pool_free(NcPacketPool *pool);
NcQueuedPacket *nc_packet_pool_rent(NcPacketPool *pool);
void nc_packet_pool_return(NcPacketPool *pool, NcQueuedPacket *item);
bool nc_packet_reserve(NcQueuedPacket *item, size_t length);
