
#include "./ReadBaton.h"
#include "./V8ArgDecoder.h"
#include <algorithm>
#include <chrono>
#include <thread>

v8::Local<v8::Value> ReadBaton::getReturnValue()
{
    return Nan::New<v8::Integer>(static_cast<int>(bytesRead));
}

NAN_METHOD(Read)
{
        V8ArgDecoder args(&info);

        auto fd = args.takeInt32();
        auto buffer = args.takeBuffer();
        auto offset = (size_t)args.takeInt32();
        auto bytesToRead = (size_t)args.takeInt32();
        auto timeout = args.takeInt32();
        auto cb = args.takeFunction();

        if(args.hasError()) return;

        if ((bytesToRead + offset) > buffer.length)
        {
            Nan::ThrowTypeError("'bytesToRead' + 'offset' cannot be larger than the buffer's length");
            return;
        }

        ReadBaton *baton = new ReadBaton("read-baton", cb);

        baton->fd = fd;
        baton->offset = offset;
        baton->bytesToRead = bytesToRead;
        baton->bufferLength = buffer.length;
        baton->bufferData = buffer.buffer;
        // Pin the JS buffer so V8 can't free bufferData while the worker reads into it.
        baton->bufferRef.Reset(buffer.object);
        baton->timeout = timeout;
        baton->complete = false;

        baton->start();
}

void ReadBaton::run()
{

    // Use a wall-clock (steady) deadline. currentMs() is derived from clock(),
    // which measures CPU time, not elapsed time, so it can't be used to bound a
    // read that spends its time waiting on the device.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(this->timeout);

    if(verbose) {
        muLogger.lock();
        auto out = defaultLogger();
        out << currentMs() << " " << debugName << " expecting=" << bytesToRead << " have=" << bytesRead << " blocking=" << false << " \n";
        out.close();
        muLogger.unlock();
    }

    do
    {
        char *buffer = bufferData + offset;

        // Non-blocking read: a blocking read() ignores the deadline entirely and
        // hangs forever when the device goes quiet, pinning a libuv threadpool
        // thread and leaking this baton (and every read queued behind it).
        auto bytesTransferred = readFromSerial(fd, buffer, bytesToRead, false, errorString);
        if(bytesTransferred < 0) {
            complete = true;
            break;
        }

        if(bytesTransferred == 0) {
            // no data ready yet; wait briefly, then re-check the deadline
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        bytesToRead -= bytesTransferred;
        bytesRead += bytesTransferred;
        offset += bytesTransferred;
        complete = bytesToRead == 0;

    } while (!complete && std::chrono::steady_clock::now() < deadline);

    if(verbose) {
        muLogger.lock();
        auto out = defaultLogger();
        auto hex = bufferToHex(bufferData, bytesRead);

        out << hex << "\n";
        out.close();
        muLogger.unlock();
    }
}