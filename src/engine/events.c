#include "engine/events.h"

#include <stdlib.h>

typedef struct NcEventNode {
    NcEngineEvent event;
    struct NcEventNode *next;
} NcEventNode;

struct NcEngineEvents {
    SRWLOCK lock;
    NcEventNode *head;
    NcEventNode *tail;
    HWND wake;
    UINT message;
};

NcEngineEvents *nc_engine_events_new(HWND wake_window, UINT wake_message) {
    NcEngineEvents *events = (NcEngineEvents *)calloc(1, sizeof(NcEngineEvents));
    if (!events) {
        return NULL;
    }
    InitializeSRWLock(&events->lock);
    events->wake = wake_window;
    events->message = wake_message;
    return events;
}

void nc_engine_events_free(NcEngineEvents *events) {
    NcEventNode *node;
    if (!events) {
        return;
    }
    AcquireSRWLockExclusive(&events->lock);
    node = events->head;
    events->head = NULL;
    events->tail = NULL;
    ReleaseSRWLockExclusive(&events->lock);
    while (node) {
        NcEventNode *next = node->next;
        free(node);
        node = next;
    }
    free(events);
}

void nc_engine_events_send(NcEngineEvents *events, const NcEngineEvent *event) {
    NcEventNode *node;
    if (!events || !event) {
        return;
    }
    node = (NcEventNode *)malloc(sizeof(NcEventNode));
    if (!node) {
        return;
    }
    node->event = *event;
    node->next = NULL;
    AcquireSRWLockExclusive(&events->lock);
    if (events->tail) {
        events->tail->next = node;
    } else {
        events->head = node;
    }
    events->tail = node;
    ReleaseSRWLockExclusive(&events->lock);
    if (events->wake) {
        PostMessageW(events->wake, events->message, 0, 0);
    }
}

bool nc_engine_events_pop(NcEngineEvents *events, NcEngineEvent *out) {
    NcEventNode *node;
    if (!events || !out) {
        return false;
    }
    AcquireSRWLockExclusive(&events->lock);
    node = events->head;
    if (node) {
        events->head = node->next;
        if (!events->head) {
            events->tail = NULL;
        }
    }
    ReleaseSRWLockExclusive(&events->lock);
    if (!node) {
        return false;
    }
    *out = node->event;
    free(node);
    return true;
}
