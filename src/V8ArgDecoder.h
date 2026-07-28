
#ifndef v8argdecoder_h
#define v8argdecoder_h

#include <v8.h>
#include <iostream>
#include <nan.h>

using namespace std;
using namespace v8;
using namespace Nan;

struct Buffer {
    char* buffer;
    size_t length;
    // The owning JS Buffer object. Callers running async work must pin this
    // (via a Nan::Persistent) so V8 can't collect the backing store that
    // `buffer` points into while a worker thread is still using it.
    Local<Object> object;
};

class DecodeObject {
    Local<Object> object;
public:
    DecodeObject(Local<Object> value);

    Local<v8::String> getV8String(string key);
    bool getBool(string key);
    int getInt(string key);
    double getDouble(string key);
    Local<Value> getValue(string key);
    Local<Function> getFunction(string key);

    bool hasKey(string key);
};

class V8ArgDecoder {
    const Nan::FunctionCallbackInfo<v8::Value> *args;
    int position;
    string error;

    void setError(string value);
    bool checkLengthAndError();
    Local<Value> getNext();
public:
    V8ArgDecoder(const Nan::FunctionCallbackInfo<v8::Value>* args)
    {
        this->args = args;
        this->position = 0;
    }

    int takeInt32();
    Buffer takeBuffer();
    Local<Function> takeFunction();
    string takeString();
    DecodeObject takeObject();
    bool takeBool();

    string getError();
    bool hasError();
};


#endif