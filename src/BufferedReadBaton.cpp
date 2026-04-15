
#include "./BufferedReadBaton.h"
#include "./V8ArgDecoder.h"
#include <algorithm>
#include "./ReadBaton.h"

#ifdef WIN32
#include <Windows.h>
#endif

#include <chrono>
#include <thread>

void reader(BufferedReadBaton* baton) {

    int length = 10000;
    int highWater = 9000;
    int lowWater = 8000;

    char* buffer = (char*)malloc(sizeof(char) * length);
    char errorString[ERROR_STRING_SIZE];

    int noDataDeadlineMs = 10 * 1000;
    int received = 0;
    int noDataForMs = 0;
    long totalCount = 0;

    for(;;) {

        if(noDataForMs > noDataDeadlineMs) {
            break;
        }

        auto got = readFromSerial(baton->fd, (char*)(buffer + received), length - received, false, errorString);
        if(got == 0) {
            if(received > lowWater) {
                baton->push(buffer, received);
                buffer = (char*)malloc(sizeof(char) * length);
                received = 0;
            } else {
                std::this_thread::sleep_for (std::chrono::milliseconds(1));
                noDataForMs += 1;
            }

            continue;
        }

        if(got < 0) {
            std::this_thread::sleep_for (std::chrono::milliseconds(1));
            noDataForMs += 1;

            free(buffer);

            {
                std::unique_lock<std::mutex> lck(baton->syncMutex);
                strcpy(baton->errorString, errorString);
                baton->readerDone = true;
            }

            uv_async_send(&baton->async);
            return;
        }

        totalCount += got;
        received += got;

        if(received > highWater) {
            baton->push(buffer, received);
            buffer = (char*)malloc(sizeof(char) * length);
            received = 0;
        }

        noDataForMs = 0;
        noDataDeadlineMs = baton->noDataTimeoutMs;
    }

    if(received > 0) {
        baton->push(buffer, received);
    } else {
        free(buffer);
    }

    {
        std::unique_lock<std::mutex> lck(baton->syncMutex);
        baton->readerDone = true;
    }

    uv_async_send(&baton->async);
}


void BufferedReadBaton::push(char* buffer, int length) {
    std::unique_lock<std::mutex> lck(syncMutex);

    auto item = new BufferItem;
    item->buffer = buffer;
    item->length = length;
    queue.push(item);

    lck.unlock();
    uv_async_send(&async);
}

void BufferedReadBaton::onAsync(uv_async_t* handle) {
    auto baton = static_cast<BufferedReadBaton*>(handle->data);
    baton->drain();
}

void BufferedReadBaton::drain() {
    Nan::HandleScope scope;

    for(;;) {
        BufferItem* item = nullptr;
        bool done = false;

        {
            std::unique_lock<std::mutex> lck(syncMutex);
            if(!queue.empty()) {
                item = queue.front();
                queue.pop();
            } else {
                done = readerDone;
            }
        }

        if(item) {
            auto buf = Nan::CopyBuffer(item->buffer, item->length);
            free(item->buffer);
            delete item;

            v8::Local<v8::Value> argv[1] = { buf.ToLocalChecked() };
            v8::Local<v8::Object> target = Nan::New<v8::Object>();
            v8::Local<v8::Function> cb = Nan::New(onData);
            runInAsyncScope(target, cb, 1, argv);
        } else if(done) {
            v8::Local<v8::Value> argv[2];

            if(errorString[0]) {
                argv[0] = v8::Exception::Error(Nan::New<v8::String>(errorString).ToLocalChecked());
                argv[1] = Nan::Undefined();
            } else {
                argv[0] = Nan::Null();
                argv[1] = Nan::False();
            }

            v8::Local<v8::Object> target = Nan::New<v8::Object>();
            v8::Local<v8::Function> cb = Nan::New(onDone);
            runInAsyncScope(target, cb, 2, argv);

            uv_close(reinterpret_cast<uv_handle_t*>(&async), onAsyncClose);
            return;
        } else {
            // queue empty but reader still running, wait for next signal
            return;
        }
    }
}

void BufferedReadBaton::onAsyncClose(uv_handle_t* handle) {
    auto baton = static_cast<BufferedReadBaton*>(
        reinterpret_cast<uv_async_t*>(handle)->data
    );
    delete baton;
}

NAN_METHOD(BufferedRead)
{
    V8ArgDecoder args(&info);

    auto fd = args.takeInt32();
    auto noDataTimeoutMs = args.takeInt32();
    auto cb = args.takeFunction();
    auto done = args.takeFunction();

    if(args.hasError()) return;

    BufferedReadBaton *baton = new BufferedReadBaton();

    baton->fd = fd;
    baton->noDataTimeoutMs = noDataTimeoutMs;
    baton->onData.Reset(cb);
    baton->onDone.Reset(done);
    baton->readThreadIsRunning = true;
    snprintf(baton->errorString, sizeof(baton->errorString), "");

    uv_async_init(Nan::GetCurrentEventLoop(), &baton->async, BufferedReadBaton::onAsync);
    baton->async.data = baton;

    std::thread t1(reader, baton);
    t1.detach();
}
