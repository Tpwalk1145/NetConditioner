#include "engine/scheduler.h"

#include <stdlib.h>
#include <string.h>

uint64_t nc_sat_add_u64(uint64_t value, uint64_t delta) {
    return value > UINT64_MAX - delta ? UINT64_MAX : value + delta;
}

void nc_burst_loss_init(NcBurstLossState *state, int32_t allow_ms, int32_t drop_ms, uint64_t now_ns) {
    if (!state) {
        return;
    }
    state->allow_ns = allow_ms > 0 ? (uint64_t)allow_ms * 1000000ull : 0;
    state->drop_ns = drop_ms > 0 ? (uint64_t)drop_ms * 1000000ull : 0;
    state->phase_start_ns = now_ns;
}

bool nc_burst_loss_should_drop(const NcBurstLossState *state, uint64_t now_ns) {
    uint64_t cycle;
    uint64_t elapsed;
    if (!state) {
        return false;
    }
    if (state->allow_ns == 0 && state->drop_ns == 0) {
        return false;
    }
    if (state->allow_ns == 0) {
        return true;
    }
    if (state->drop_ns == 0) {
        return false;
    }
    cycle = nc_sat_add_u64(state->allow_ns, state->drop_ns);
    elapsed = now_ns > state->phase_start_ns ? now_ns - state->phase_start_ns : 0;
    return (elapsed % cycle) >= state->allow_ns;
}

uint64_t nc_packet_service_ns(size_t length, uint64_t bandwidth_kbps) {
    uint64_t service;
    if (bandwidth_kbps == 0 || length == 0) {
        return 0;
    }
    service = ((uint64_t)length * 8ull * 1000000000ull) / (bandwidth_kbps * 1000ull);
    return service == 0 ? 1 : service;
}

void nc_bandwidth_scheduler_init(NcBandwidthScheduler *scheduler) {
    if (scheduler) {
        scheduler->next_send_ns = 0;
    }
}

bool nc_bandwidth_scheduler_plan(const NcBandwidthScheduler *scheduler, uint64_t now_ns,
                                 uint64_t intended_delay_ns, size_t length,
                                 uint64_t bandwidth_kbps, uint64_t max_queue_ns,
                                 NcSendPlan *out) {
    uint64_t intended_due_ns;
    uint64_t packet_ns;
    uint64_t start_ns;
    uint64_t due_ns;
    uint64_t extra_ns;
    uint64_t expires_ns;
    if (!scheduler || !out) {
        return false;
    }
    intended_due_ns = nc_sat_add_u64(now_ns, intended_delay_ns);
    packet_ns = nc_packet_service_ns(length, bandwidth_kbps);
    start_ns = intended_due_ns > scheduler->next_send_ns ? intended_due_ns : scheduler->next_send_ns;
    due_ns = nc_sat_add_u64(start_ns, packet_ns);
    extra_ns = due_ns - intended_due_ns;
    if (max_queue_ns > 0 && extra_ns > max_queue_ns) {
        return false;
    }
    expires_ns = max_queue_ns > 0 ? nc_sat_add_u64(intended_due_ns, max_queue_ns) : 0;
    out->due_ns = due_ns;
    out->next_send_ns = due_ns;
    out->expires_ns = expires_ns;
    return true;
}

void nc_bandwidth_scheduler_commit(NcBandwidthScheduler *scheduler, const NcSendPlan *plan) {
    if (!scheduler || !plan) {
        return;
    }
    if (plan->next_send_ns > scheduler->next_send_ns) {
        scheduler->next_send_ns = plan->next_send_ns;
    }
}

uint64_t nc_paced_send_time(uint64_t due_ns, bool has_last, uint64_t last_send_end_ns,
                            uint64_t now_ns) {
    uint64_t send_at = due_ns;
    if (has_last && last_send_end_ns > send_at) {
        send_at = last_send_end_ns;
    }
    if (send_at < now_ns) {
        send_at = now_ns;
    }
    return send_at;
}

static bool packet_before(const NcQueuedPacket *left, const NcQueuedPacket *right) {
    if (left->due_ns != right->due_ns) {
        return left->due_ns < right->due_ns;
    }
    return left->sequence < right->sequence;
}

void nc_packet_queue_init(NcPacketQueue *queue) {
    if (queue) {
        memset(queue, 0, sizeof(*queue));
    }
}

void nc_packet_queue_free(NcPacketQueue *queue) {
    size_t index;
    if (!queue) {
        return;
    }
    for (index = 0; index < queue->len; index++) {
        if (queue->items[index]) {
            free(queue->items[index]->data);
            free(queue->items[index]);
        }
    }
    free(queue->items);
    memset(queue, 0, sizeof(*queue));
}

bool nc_packet_queue_push(NcPacketQueue *queue, NcQueuedPacket *item) {
    size_t index;
    if (!queue || !item) {
        return false;
    }
    if (queue->len == queue->cap) {
        size_t cap = queue->cap ? queue->cap * 2 : 64;
        NcQueuedPacket **items = (NcQueuedPacket **)realloc(queue->items, cap * sizeof(NcQueuedPacket *));
        if (!items) {
            return false;
        }
        queue->items = items;
        queue->cap = cap;
    }
    index = queue->len++;
    queue->items[index] = item;
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (!packet_before(queue->items[index], queue->items[parent])) {
            break;
        }
        {
            NcQueuedPacket *temp = queue->items[parent];
            queue->items[parent] = queue->items[index];
            queue->items[index] = temp;
        }
        index = parent;
    }
    queue->bytes += item->length;
    return true;
}

NcQueuedPacket *nc_packet_queue_peek(const NcPacketQueue *queue) {
    return (queue && queue->len > 0) ? queue->items[0] : NULL;
}

static void sift_down(NcPacketQueue *queue, size_t index) {
    for (;;) {
        size_t left = index * 2 + 1;
        size_t right = left + 1;
        size_t smallest = index;
        NcQueuedPacket *temp;
        if (left < queue->len && packet_before(queue->items[left], queue->items[smallest])) {
            smallest = left;
        }
        if (right < queue->len && packet_before(queue->items[right], queue->items[smallest])) {
            smallest = right;
        }
        if (smallest == index) {
            break;
        }
        temp = queue->items[index];
        queue->items[index] = queue->items[smallest];
        queue->items[smallest] = temp;
        index = smallest;
    }
}

NcQueuedPacket *nc_packet_queue_pop(NcPacketQueue *queue) {
    NcQueuedPacket *top;
    if (!queue || queue->len == 0) {
        return NULL;
    }
    top = queue->items[0];
    queue->len--;
    if (queue->len > 0) {
        queue->items[0] = queue->items[queue->len];
        sift_down(queue, 0);
    }
    queue->bytes = queue->bytes >= top->length ? queue->bytes - top->length : 0;
    return top;
}

void nc_packet_pool_init(NcPacketPool *pool) {
    if (pool) {
        memset(pool, 0, sizeof(*pool));
    }
}

void nc_packet_pool_free(NcPacketPool *pool) {
    size_t index;
    if (!pool) {
        return;
    }
    for (index = 0; index < pool->len; index++) {
        if (pool->items[index]) {
            free(pool->items[index]->data);
            free(pool->items[index]);
        }
    }
    free(pool->items);
    memset(pool, 0, sizeof(*pool));
}

NcQueuedPacket *nc_packet_pool_rent(NcPacketPool *pool) {
    NcQueuedPacket *item = NULL;
    if (pool && pool->len > 0) {
        item = pool->items[--pool->len];
    }
    if (!item) {
        item = (NcQueuedPacket *)calloc(1, sizeof(NcQueuedPacket));
    }
    if (item) {
        item->length = 0;
        item->due_ns = 0;
        item->expires_ns = 0;
        item->sequence = 0;
    }
    return item;
}

void nc_packet_pool_return(NcPacketPool *pool, NcQueuedPacket *item) {
    if (!item) {
        return;
    }
    item->length = 0;
    item->due_ns = 0;
    item->expires_ns = 0;
    item->sequence = 0;
    if (pool && pool->len < NC_POOL_LIMIT) {
        if (pool->len == pool->cap) {
            size_t cap = pool->cap ? pool->cap * 2 : 64;
            NcQueuedPacket **items = (NcQueuedPacket **)realloc(pool->items, cap * sizeof(NcQueuedPacket *));
            if (items) {
                pool->items = items;
                pool->cap = cap;
            }
        }
        if (pool->len < pool->cap) {
            pool->items[pool->len++] = item;
            return;
        }
    }
    free(item->data);
    free(item);
}

bool nc_packet_reserve(NcQueuedPacket *item, size_t length) {
    unsigned char *data;
    if (!item) {
        return false;
    }
    if (item->cap >= length) {
        return true;
    }
    data = (unsigned char *)realloc(item->data, length);
    if (!data) {
        return false;
    }
    item->data = data;
    item->cap = length;
    return true;
}
