#ifndef WriteBaton_h
#define WriteBaton_h

#include <nan.h>
#include "BatonBase.h"
#include <string>
#include <v8.h>
#include <nan.h>

class WriteBaton : public BatonBase {
private:
    void writeEcho(int deadline);
    void writeNormal(int deadline);
public:
    WriteBaton(std::string name, v8::Local<v8::Function> callback_) : BatonBase(name, callback_)
    {
        request.data = this;
    }

    // Release the pinned JS buffer. Runs on the main loop thread (via
    // AfterAction) after the worker is done touching bufferData.
    ~WriteBaton() override {
        buffer.Reset();
    }

    int fd = 0;
    int timeout = -1;
    char *bufferData = nullptr;
    size_t bufferLength = 0;
    size_t offset = 0;
    size_t bytesWritten = 0;
    bool complete = false;
    Nan::Persistent<v8::Object> buffer;
    int result = 0;
    bool echoMode = false;

    v8::Local<v8::Value> getReturnValue() override;
    void run() override;
};

NAN_METHOD(Write);
int writeToSerial(int fd, char* buffer, int length, bool blocking, char* error);

#endif