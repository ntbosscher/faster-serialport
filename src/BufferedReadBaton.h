#ifndef BufferedReadBaton_h
#define BufferedReadBaton_h

#include <nan.h>
#include "BatonBase.h"
#include <thread>
#include <mutex>
#include <queue>

struct BufferItem {
    char* buffer;
    int length;
};

class BufferedReadBaton : public Nan::AsyncResource {
public:
    BufferedReadBaton()
        : Nan::AsyncResource("buffered-read-baton")
    {}

    int fd = 0;
    int noDataTimeoutMs;

    char errorString[ERROR_STRING_SIZE];

    Nan::Persistent<v8::Function> onData;
    Nan::Persistent<v8::Function> onDone;

    std::mutex syncMutex;
    std::queue<BufferItem*> queue;
    bool readThreadIsRunning = false;
    bool readerDone = false;

    uv_async_t async;

    void push(char* buffer, int length);
    void drain();

    static void onAsync(uv_async_t* handle);
    static void onAsyncClose(uv_handle_t* handle);

    ~BufferedReadBaton() {
        onData.Reset();
        onDone.Reset();
    }
};

NAN_METHOD(BufferedRead);

#endif