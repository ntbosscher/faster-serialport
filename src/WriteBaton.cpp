
#include "./WriteBaton.h"
#include "./V8ArgDecoder.h"
#include <sstream>
#include <iomanip>
#include "./ReadBaton.h"

v8::Local<v8::Value> WriteBaton::getReturnValue()
{
    if(verbose) {
        muLogger.lock();
        auto out = defaultLogger();
        out << currentMs() << " " << debugName << " get-return \n";
        out.close();
        muLogger.unlock();
    }

    return Nan::Null();
}

string char2hex(char c)
{
    std::ostringstream oss (std::ostringstream::out);
    oss << std::setw(2) << std::setfill('0') << std::hex << (((int)c) & 0xff);
    return oss.str();
}

void WriteBaton::writeEcho(int deadline) {
    if(verbose) {
        muLogger.lock();
        auto out = defaultLogger();
        out << currentMs() << " " << debugName << " to-write=" << (bufferLength - offset) << " blocking=" << true << " echoMode=" << echoMode << "\n";
        out << currentMs() << " " << debugName << " buffer-contents: " << bufferToHex(bufferData, (bufferLength - offset)) << "\n";
        out.close();
        muLogger.unlock();
    }

    char readBuffer[1];
    char writeBuffer[1];

    for(size_t i = 0; i < bufferLength; i++) {
        writeBuffer[0] = bufferData[i];
        int len = 1;
        int result = 0;

        do {
            result = writeToSerial(fd, writeBuffer, len, true, errorString);
            if(result < 0) {
                complete = true;
                return;
            }

            if(currentMs() > deadline) {
                logVerbose("timeout " + std::to_string(deadline));
                return;
            }

        } while(result != len);

        logVerbose("wrote " + char2hex(writeBuffer[0]) + " index=" + std::to_string(i));

        do {
            result = readFromSerial(fd, readBuffer, len, true, errorString);
            if(result < 0) {
                complete = true;
                return;
            }

            if(currentMs() > deadline) {
                logVerbose("timeout " + std::to_string(deadline));
                return;
            }

            if(result == 1) {
                if(readBuffer[0] != writeBuffer[0]) {
                    result = 0;
                    logVerbose("read unexpected value " + char2hex(readBuffer[0]));
                } else {
                    logVerbose("read " + char2hex(readBuffer[0]));
                }
            }

        } while(result != len);
    }

    logVerbose("complete");
    complete = true;
}

void WriteBaton::writeNormal(int deadline) {
    do
    {
        if(verbose) {
            muLogger.lock();
            auto out = defaultLogger();
            out << currentMs() << " " << debugName << " to-write=" << (bufferLength - offset) << " blocking=" << true << " echoMode=" << echoMode << "\n";
            out << currentMs() << " " << debugName << " buffer-contents: " << bufferToHex(bufferData, (bufferLength - offset)) << "\n";
            out.close();
            muLogger.unlock();
        }

        char *buffer = bufferData + offset;
        int len = bufferLength - offset;

        auto bytesTransferred = writeToSerial(fd, buffer, len, true, errorString);
        if(bytesTransferred < 0) {
            complete = true;
            return;
        }

        if(verbose) {
            muLogger.lock();
            auto out = defaultLogger();
            out << currentMs() << " " << debugName << " wrote-len=" << bytesTransferred << " \n";
            out.close();
            muLogger.unlock();
        }

        offset += bytesTransferred;
        complete = offset == bufferLength;

    } while (!complete && currentMs() > deadline);

    if(verbose) {
        muLogger.lock();
        auto out = defaultLogger();
        out << currentMs() << " " << debugName << " done-loop \n";
        out.close();
        muLogger.unlock();
    }
}

void WriteBaton::run()
{

    int start = currentMs();
    int deadline = start + this->timeout;

    if(echoMode) {
        this->writeEcho(deadline);
    } else {
        this->writeNormal(deadline);
    }

    if(!complete) {
        sprintf((char*)&errorString, "Timeout writing to port: %lu of %zu bytes written", (this->bufferLength-this->offset), this->bufferLength);
    }
}

NAN_METHOD(Write)
{
    V8ArgDecoder args(&info);

    auto fd = args.takeInt32();
    auto buffer = args.takeBuffer();
    auto timeout = args.takeInt32();
    auto echoMode = args.takeBool();
    auto cb = args.takeFunction();

    if(args.hasError()) return;

    WriteBaton *baton = new WriteBaton("write-baton", cb);

    baton->fd = fd;
    baton->timeout = timeout;
    baton->echoMode = echoMode;
    baton->bufferData = buffer.buffer;
    baton->bufferLength = buffer.length;
    // Pin the JS buffer so V8 can't free bufferData while the worker writes it.
    baton->buffer.Reset(buffer.object);
    baton->offset = 0;
    baton->complete = false;

    baton->start();
}