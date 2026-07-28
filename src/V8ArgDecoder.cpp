
#include <v8.h>
#include <nan.h>
#include "./V8ArgDecoder.h"

DecodeObject::DecodeObject(v8::Local<v8::Object> _value) {
    object = _value;
}

v8::Local<v8::Value> DecodeObject::getValue(string key) {
  auto v8str = Nan::New<v8::String>(key).ToLocalChecked();
  return Nan::Get(object, v8str).ToLocalChecked();
}

bool DecodeObject::hasKey(string key) {
    return Nan::Has(object, Nan::New<v8::String>(key).ToLocalChecked()).FromMaybe(false);
}

Local<v8::String> DecodeObject::getV8String(string key) {
    return Nan::To<v8::String>(this->getValue(key)).ToLocalChecked();
}

int DecodeObject::getInt(string key) {
    return Nan::To<Int32>(this->getValue(key)).ToLocalChecked()->Value();
}

bool DecodeObject::getBool(string key) {
    return Nan::To<Boolean>(this->getValue(key)).ToLocalChecked()->Value();
}

double DecodeObject::getDouble(string key) {
    return Nan::To<double>(this->getValue(key)).FromMaybe(0);
}

v8::Local<v8::Function> DecodeObject::getFunction(string key) {
    return Nan::To<v8::Function>(this->getValue(key)).ToLocalChecked();
}


bool V8ArgDecoder::checkLengthAndError() {
    if(!error.empty()) return false; // has existing error
    if(position < args->Length()) return true;
    this->setError("Not enough arguments provided");
    return false;
}

bool V8ArgDecoder::hasError() {
    return !error.empty();
}

void V8ArgDecoder::setError(string value) {
    this->error = value;
    Nan::ThrowTypeError(this->error.c_str());
}

Local<Value> V8ArgDecoder::getNext() {
    return (*this->args)[this->position++];
}

int V8ArgDecoder::takeInt32() {
    if(!this->checkLengthAndError()) return 0;

    auto value = this->getNext();
    if(!value->IsInt32()) {
        this->setError("Expecting argument 'int32' at position " + to_string(this->position-1));
        return 0;
    }

    return Nan::To<Int32>(value).ToLocalChecked()->Value();
}

bool V8ArgDecoder::takeBool() {
    if(!this->checkLengthAndError()) return 0;

    auto value = this->getNext();
    if(!value->IsBoolean()) {
        this->setError("Expecting argument 'bool' at position " + to_string(this->position-1));
        return 0;
    }

    return Nan::To<Boolean>(value).ToLocalChecked()->Value();
}

DecodeObject V8ArgDecoder::takeObject() {
    Local<Object> defaultResult;
    if(!this->checkLengthAndError()) return DecodeObject(defaultResult);

    auto value = this->getNext();
    if(!value->IsObject()) {
        this->setError("Expecting argument 'object' at position " + to_string(this->position-1));
        return DecodeObject(defaultResult);
    }

    return DecodeObject(Nan::To<v8::Object>(value).ToLocalChecked());
}

string V8ArgDecoder::takeString() {
    if(!this->checkLengthAndError()) return 0;

    auto value = this->getNext();
    if(!value->IsString()) {
        this->setError("Expecting argument 'string' at position " + to_string(this->position-1));
        return 0;
    }

    Nan::Utf8String path(value);
    return string(*path);
}

Buffer V8ArgDecoder::takeBuffer() {
    Buffer buffer;

    if(!this->checkLengthAndError()) return buffer;

    auto value = this->getNext();
    if (!value->IsObject() || !node::Buffer::HasInstance(value))
    {
        this->setError("Expecting argument 'buffer' at position " + to_string(this->position-1));
        return buffer;
    }

    v8::Local<v8::Object> buf = Nan::To<v8::Object>(value).ToLocalChecked();
    buffer.length = node::Buffer::Length(buf);
    buffer.buffer = node::Buffer::Data(buf);
    buffer.object = buf;

    return buffer;
}

Local<Function> V8ArgDecoder::takeFunction() {
    Local<Function> function;
    if(!this->checkLengthAndError()) return function;

    auto value = this->getNext();
    if (!value->IsFunction())
    {
        this->setError("Expecting argument 'function' at position " + to_string(this->position-1));
        return function;
    }

    return Nan::To<v8::Function>(value).ToLocalChecked();
}

