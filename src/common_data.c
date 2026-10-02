#include "common_data.h"

inline void *getDatePtr(circularBuf *buf, uint32_t index) {
    if (index >= buf->maxHistory) {
        return NULL;
    }
    return &buf->data[index * buf->dataSize];
}

static inline void clearData(circularBuf *buf, void *data) {
    if (buf->clearData == NULL) {
        memset(data, 0, buf->dataSize);
    } else {
        buf->clearData(data);
    }
}

int initData(circularBuf **buf, uint32_t dataSize, uint32_t maxHistory, void (*clearDataFn)(void *)) {
    *buf = malloc(sizeof(circularBuf) + dataSize * maxHistory);
    if (*buf == NULL)
        return -1;
    memset(*buf, 0, sizeof(circularBuf) + dataSize * maxHistory);
    (*buf)->dataSize = dataSize;
    (*buf)->maxHistory = maxHistory;
    (*buf)->clearData = clearDataFn;
    resetData(*buf);
    return 0;
}

void termData(circularBuf *buf) {
    if (buf == NULL)
        return;
    free(buf);
}

void resetData(circularBuf *buf) {
    if (buf == NULL)
        return;
    buf->head = 0;
    buf->tail = 0;
    // getLatestData() reads the slot before head, so this empty item is returned until new data arrives
    clearData(buf, getDatePtr(buf, buf->maxHistory - 1));
}

void pushData(circularBuf *buf, const void *data) {
    if (buf == NULL) {
        return;
    }
    memcpy(getDatePtr(buf, buf->head), data, buf->dataSize);
    buf->head = (buf->head + 1) % buf->maxHistory;
    if (buf->head == buf->tail)
        buf->tail = (buf->tail + 1) % buf->maxHistory;
}

void *peekNewestData(circularBuf *buf) {
    if (buf == NULL || buf->head == buf->tail) {
        return NULL;
    }
    return getDatePtr(buf, (buf->head + buf->maxHistory - 1) % buf->maxHistory);
}

void getLatestData(circularBuf *buf, void *data) {
    if (buf == NULL) {
        return;
    }
    uint32_t lastDataIndex = (buf->head + buf->maxHistory - 1) % buf->maxHistory;
    memcpy(data, getDatePtr(buf, lastDataIndex), buf->dataSize);
}

int32_t getData(circularBuf *buf, void *data, int32_t count) {
    int32_t ret = 0;
    char *out = data;
    if (buf == NULL || count <= 0) {
        return 0;
    }
    if ((uint32_t) count > buf->maxHistory)
        count = (int32_t) buf->maxHistory;
    while (count > 0 && buf->tail != buf->head) {
        memcpy(out, getDatePtr(buf, buf->tail), buf->dataSize);
        buf->tail = (buf->tail + 1) % buf->maxHistory;
        out += buf->dataSize;
        count--;
        ret++;
    }
    return ret;
}
