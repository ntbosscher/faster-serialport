
#ifndef v8argdecoder_h
#define v8argdecoder_h

#include <v8.h>
#include <iostream>
#include <nan.h>

using namespace std;
using namespace v8;

struct Buffer {
    char* buffer;
    size_t length;
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

    bool hasKey(string key);
};

class V8ArgDecoder {
    const Nan::FunctionCallbackInfo<Value> *args;
    int position;
    string error;

    void setError(string value);
    bool checkLengthAndError();
    Local<Value> V8ArgDecoder::getNext();
public:
    V8ArgDecoder(const Nan::FunctionCallbackInfo<Value>* args);
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