#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef REMOTE_PAD_COMMON_DATA_H
#define REMOTE_PAD_COMMON_DATA_H

typedef struct circularBuf {
    uint32_t head;
    uint32_t tail;
    uint32_t dataSize;
    uint32_t maxHistory;

    void (*clearData)(void *);

    char data[];
} circularBuf;

void *getDatePtr(circularBuf *buf, uint32_t index);

int initData(circularBuf **buf, uint32_t dataSize, uint32_t maxHistory, void (*emptyData)(void *));

void termData(circularBuf *buf);

// Drop all queued items; the latest item becomes the empty item
void resetData(circularBuf *buf);

// Queue an item. When the queue is full the oldest queued item is dropped.
void pushData(circularBuf *buf, const void *data);

// The newest queued item (not read yet), to update in place; NULL when nothing is queued
void *peekNewestData(circularBuf *buf);

void getLatestData(circularBuf *buf, void *data);

// Copy up to `count` queued items (oldest first) and consume them. Returns the number copied, 0 when nothing is queued.
int32_t getData(circularBuf *buf, void *data, int32_t count);

#endif //REMOTE_PAD_COMMON_DATA_H
