#include "test_harness.h"

#include "engine/direction.h"

#include <string.h>

static void test_packet_queue_and_pool(void) {
    NcPacketQueue queue;
    NcPacketPool pool;
    NcQueuedPacket *a;
    NcQueuedPacket *b;
    NcQueuedPacket *c;
    NcQueuedPacket *popped;
    nc_packet_queue_init(&queue);
    nc_packet_pool_init(&pool);
    a = nc_packet_pool_rent(&pool);
    b = nc_packet_pool_rent(&pool);
    c = nc_packet_pool_rent(&pool);
    CHECK(a != NULL && b != NULL && c != NULL);
    CHECK(nc_packet_reserve(a, 100));
    CHECK(nc_packet_reserve(b, 200));
    CHECK(nc_packet_reserve(c, 50));
    a->length = 100; a->due_ns = 300; a->sequence = 1;
    b->length = 200; b->due_ns = 100; b->sequence = 2;
    c->length = 50;  c->due_ns = 100; c->sequence = 1;
    CHECK(nc_packet_queue_push(&queue, a));
    CHECK(nc_packet_queue_push(&queue, b));
    CHECK(nc_packet_queue_push(&queue, c));
    CHECK_INT(queue.len, 3);
    CHECK_INT(queue.bytes, 350);
    popped = nc_packet_queue_pop(&queue);
    CHECK(popped == c);
    CHECK_INT(queue.bytes, 300);
    nc_packet_pool_return(&pool, popped);
    popped = nc_packet_queue_pop(&queue);
    CHECK(popped == b);
    nc_packet_pool_return(&pool, popped);
    popped = nc_packet_queue_pop(&queue);
    CHECK(popped == a);
    CHECK_INT(queue.len, 0);
    CHECK_INT(queue.bytes, 0);
    nc_packet_pool_return(&pool, popped);
    CHECK_INT(pool.len, 3);
    /* Renting reuses pooled buffer without losing capacity. */
    popped = nc_packet_pool_rent(&pool);
    CHECK(popped == a && popped->cap >= 100 && popped->length == 0);
    nc_packet_pool_return(&pool, popped);
    nc_packet_queue_free(&queue);
    nc_packet_pool_free(&pool);
}

void test_direction_run(void) {
    NcBurstLossState burst;
    NcBandwidthScheduler scheduler;
    NcSendPlan plan;
    NcSendPlan retry;
    uint64_t service;

    nc_burst_loss_init(&burst, 100, 0, 0);
    CHECK(!nc_burst_loss_should_drop(&burst, 0));
    nc_burst_loss_init(&burst, 0, 100, 0);
    CHECK(nc_burst_loss_should_drop(&burst, 0));
    nc_burst_loss_init(&burst, 0, 0, 0);
    CHECK(!nc_burst_loss_should_drop(&burst, UINT64_MAX));
    nc_burst_loss_init(&burst, 100, 100, 0);
    CHECK(!nc_burst_loss_should_drop(&burst, 50000000ull));
    CHECK(nc_burst_loss_should_drop(&burst, 150000000ull));
    CHECK(!nc_burst_loss_should_drop(&burst, 250000000ull));
    CHECK(nc_burst_loss_should_drop(&burst, 1350000000ull));
    CHECK(!nc_burst_loss_should_drop(&burst, 1450000000ull));

    CHECK_INT(nc_packet_service_ns(1000, 8), 1000000000ull);
    CHECK_INT(nc_packet_service_ns(0, 8), 0);
    CHECK_INT(nc_packet_service_ns(1000, 0), 0);
    CHECK_INT(nc_packet_service_ns(1000, 8000000), 1000ull);
    CHECK_INT(nc_packet_service_ns(65791, 1), 526328000000ull);

    nc_bandwidth_scheduler_init(&scheduler);
    CHECK(nc_bandwidth_scheduler_plan(&scheduler, 0, 0, 1000, 8, 0, &plan));
    CHECK(nc_bandwidth_scheduler_plan(&scheduler, 0, 0, 1000, 8, 0, &retry));
    CHECK_INT(plan.due_ns, retry.due_ns);
    nc_bandwidth_scheduler_commit(&scheduler, &retry);
    CHECK(nc_bandwidth_scheduler_plan(&scheduler, 0, 0, 1000, 8, 0, &plan));
    CHECK_INT(plan.due_ns, retry.due_ns + nc_packet_service_ns(1000, 8));

    nc_bandwidth_scheduler_init(&scheduler);
    CHECK(!nc_bandwidth_scheduler_plan(&scheduler, 0, 0, 1000, 1, 1, &plan));
    CHECK(nc_bandwidth_scheduler_plan(&scheduler, 0, 0, 100, 1000000, 1000000, &plan));
    CHECK(plan.expires_ns > 0);

    service = nc_packet_service_ns(1000, 8);
    CHECK_INT(nc_paced_send_time(10, false, 0, 100), 100);
    CHECK_INT(nc_paced_send_time(10, true, 100 + service, 100), 100 + service);
    CHECK_INT(nc_paced_send_time(200, true, 150, 100), 200);
    CHECK_INT(nc_paced_send_time(50, false, 0, 100), 100);

    test_packet_queue_and_pool();
}
