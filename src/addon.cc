// node-addon-api bridge between lib/bindings.js and the Go c-archive
// (go-serial). Every async binding function runs its blocking Go call on a
// libuv worker thread via Napi::AsyncWorker and reports back through the
// callback-last contract that lib/bindings.js expects: cb(err, result).

#include <napi.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "libserial.h"

namespace {

// takeGoError consumes a char* returned by a Go export: it copies the message
// (if any) into `out` and frees the Go-allocated string. Returns true on error.
bool takeGoError(char* err, std::string& out) {
  if (err == nullptr) {
    return false;
  }
  out = err;
  fsp_free(err);
  return true;
}

std::string jsonStringify(Napi::Env env, Napi::Value value) {
  Napi::Object json = env.Global().Get("JSON").As<Napi::Object>();
  Napi::Function stringify = json.Get("stringify").As<Napi::Function>();
  Napi::Value result = stringify.Call(json, {value});
  if (result.IsString()) {
    return result.As<Napi::String>();
  }
  return "{}";
}

Napi::Value jsonParse(Napi::Env env, const std::string& text) {
  Napi::Object json = env.Global().Get("JSON").As<Napi::Object>();
  Napi::Function parse = json.Get("parse").As<Napi::Function>();
  return parse.Call(json, {Napi::String::New(env, text)});
}

// ---- bufferedRead chunk plumbing -----------------------------------------

struct Chunk {
  std::vector<char> bytes;
};

std::mutex g_tsfn_mu;
std::map<int, Napi::ThreadSafeFunction> g_tsfns;
int g_next_cb_id = 1;

int registerTsfn(Napi::ThreadSafeFunction tsfn) {
  std::lock_guard<std::mutex> lock(g_tsfn_mu);
  int id = g_next_cb_id++;
  g_tsfns[id] = tsfn;
  return id;
}

void releaseTsfn(int id) {
  Napi::ThreadSafeFunction tsfn;
  {
    std::lock_guard<std::mutex> lock(g_tsfn_mu);
    auto it = g_tsfns.find(id);
    if (it == g_tsfns.end()) {
      return;
    }
    tsfn = it->second;
    g_tsfns.erase(it);
  }
  tsfn.Release();
}

// ---- workers -------------------------------------------------------------

class ListWorker : public Napi::AsyncWorker {
 public:
  ListWorker(const Napi::Function& cb) : Napi::AsyncWorker(cb) {}

  void Execute() override {
    char* json = nullptr;
    std::string err;
    if (takeGoError(fsp_list(&json), err)) {
      SetError(err);
      return;
    }
    if (json != nullptr) {
      json_ = json;
      fsp_free(json);
    }
  }

  void OnOK() override {
    Napi::Env env = Env();
    Callback().Call({env.Null(), jsonParse(env, json_)});
  }

 private:
  std::string json_;
};

class OpenWorker : public Napi::AsyncWorker {
 public:
  OpenWorker(const Napi::Function& cb, std::string path, std::string opts)
      : Napi::AsyncWorker(cb), path_(std::move(path)), opts_(std::move(opts)) {}

  void Execute() override {
    std::string err;
    if (takeGoError(fsp_open(&path_[0], &opts_[0], &handle_), err)) {
      SetError(err);
    }
  }

  void OnOK() override {
    Napi::Env env = Env();
    Callback().Call(
        {env.Null(), Napi::Number::New(env, static_cast<double>(handle_))});
  }

 private:
  std::string path_;
  std::string opts_;
  long long handle_ = 0;
};

class CloseWorker : public Napi::AsyncWorker {
 public:
  CloseWorker(const Napi::Function& cb, long long handle)
      : Napi::AsyncWorker(cb), handle_(handle) {}

  void Execute() override {
    std::string err;
    if (takeGoError(fsp_close(handle_), err)) {
      SetError(err);
    }
  }

  void OnOK() override { Callback().Call({Env().Null()}); }

 private:
  long long handle_;
};

class ReadWorker : public Napi::AsyncWorker {
 public:
  ReadWorker(const Napi::Function& cb, long long handle, Napi::Buffer<char> buf,
             int offset, int length, int timeout)
      : Napi::AsyncWorker(cb),
        handle_(handle),
        data_(buf.Data() + offset),
        length_(length),
        timeout_(timeout) {
    bufferRef_ = Napi::Persistent(buf);
  }

  void Execute() override {
    std::string err;
    if (takeGoError(fsp_read(handle_, data_, length_, timeout_, &bytesRead_),
                    err)) {
      SetError(err);
    }
  }

  void OnOK() override {
    Napi::Env env = Env();
    Callback().Call({env.Null(), Napi::Number::New(env, bytesRead_)});
  }

 private:
  long long handle_;
  char* data_;
  int length_;
  int timeout_;
  int bytesRead_ = 0;
  Napi::Reference<Napi::Buffer<char>> bufferRef_;
};

class WriteWorker : public Napi::AsyncWorker {
 public:
  WriteWorker(const Napi::Function& cb, long long handle,
              Napi::Buffer<char> buf, int timeout, int echoMode)
      : Napi::AsyncWorker(cb),
        handle_(handle),
        data_(buf.Data()),
        length_(static_cast<int>(buf.Length())),
        timeout_(timeout),
        echoMode_(echoMode) {
    bufferRef_ = Napi::Persistent(buf);
  }

  void Execute() override {
    std::string err;
    if (takeGoError(fsp_write(handle_, data_, length_, timeout_, echoMode_),
                    err)) {
      SetError(err);
    }
  }

  void OnOK() override { Callback().Call({Env().Null()}); }

 private:
  long long handle_;
  char* data_;
  int length_;
  int timeout_;
  int echoMode_;
  Napi::Reference<Napi::Buffer<char>> bufferRef_;
};

class BufferedReadWorker : public Napi::AsyncWorker {
 public:
  BufferedReadWorker(const Napi::Function& cb, long long handle, int timeout,
                     int cbId)
      : Napi::AsyncWorker(cb),
        handle_(handle),
        timeout_(timeout),
        cbId_(cbId) {}

  void Execute() override {
    std::string err;
    if (takeGoError(fsp_buffered_read(handle_, timeout_, cbId_), err)) {
      SetError(err);
    }
  }

  void OnOK() override {
    releaseTsfn(cbId_);
    Napi::Env env = Env();
    Callback().Call({env.Null(), Napi::Boolean::New(env, false)});
  }

  void OnError(const Napi::Error& e) override {
    releaseTsfn(cbId_);
    Callback().Call({e.Value()});
  }

 private:
  long long handle_;
  int timeout_;
  int cbId_;
};

// SimpleWorker covers the handle-only ops whose success result is null:
// update, set, drain, flush.
class SimpleWorker : public Napi::AsyncWorker {
 public:
  using Fn = std::function<char*()>;

  SimpleWorker(const Napi::Function& cb, Fn fn)
      : Napi::AsyncWorker(cb), fn_(std::move(fn)) {}

  void Execute() override {
    std::string err;
    if (takeGoError(fn_(), err)) {
      SetError(err);
    }
  }

  void OnOK() override { Callback().Call({Env().Null()}); }

 private:
  Fn fn_;
};

class GetWorker : public Napi::AsyncWorker {
 public:
  GetWorker(const Napi::Function& cb, long long handle)
      : Napi::AsyncWorker(cb), handle_(handle) {}

  void Execute() override {
    char* json = nullptr;
    std::string err;
    if (takeGoError(fsp_get(handle_, &json), err)) {
      SetError(err);
      return;
    }
    if (json != nullptr) {
      json_ = json;
      fsp_free(json);
    }
  }

  void OnOK() override {
    Napi::Env env = Env();
    Callback().Call({env.Null(), jsonParse(env, json_)});
  }

 private:
  long long handle_;
  std::string json_;
};

class GetBaudRateWorker : public Napi::AsyncWorker {
 public:
  GetBaudRateWorker(const Napi::Function& cb, long long handle)
      : Napi::AsyncWorker(cb), handle_(handle) {}

  void Execute() override {
    std::string err;
    if (takeGoError(fsp_get_baud_rate(handle_, &baud_), err)) {
      SetError(err);
    }
  }

  void OnOK() override {
    Napi::Env env = Env();
    Callback().Call({env.Null(), Napi::Number::New(env, baud_)});
  }

 private:
  long long handle_;
  int baud_ = 0;
};

// ---- JS-facing functions -------------------------------------------------

long long asHandle(Napi::Value v) {
  return static_cast<long long>(v.As<Napi::Number>().DoubleValue());
}

Napi::Value List(const Napi::CallbackInfo& info) {
  auto cb = info[0].As<Napi::Function>();
  (new ListWorker(cb))->Queue();
  return info.Env().Undefined();
}

Napi::Value Open(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  std::string path = info[0].As<Napi::String>();
  std::string opts = jsonStringify(env, info[1]);
  auto cb = info[2].As<Napi::Function>();
  (new OpenWorker(cb, std::move(path), std::move(opts)))->Queue();
  return env.Undefined();
}

Napi::Value Close(const Napi::CallbackInfo& info) {
  auto cb = info[1].As<Napi::Function>();
  (new CloseWorker(cb, asHandle(info[0])))->Queue();
  return info.Env().Undefined();
}

Napi::Value Read(const Napi::CallbackInfo& info) {
  long long handle = asHandle(info[0]);
  auto buffer = info[1].As<Napi::Buffer<char>>();
  int offset = info[2].As<Napi::Number>().Int32Value();
  int length = info[3].As<Napi::Number>().Int32Value();
  int timeout = info[4].As<Napi::Number>().Int32Value();
  auto cb = info[5].As<Napi::Function>();
  (new ReadWorker(cb, handle, buffer, offset, length, timeout))->Queue();
  return info.Env().Undefined();
}

Napi::Value Write(const Napi::CallbackInfo& info) {
  long long handle = asHandle(info[0]);
  auto buffer = info[1].As<Napi::Buffer<char>>();
  int timeout = info[2].As<Napi::Number>().Int32Value();
  int echoMode = info[3].As<Napi::Boolean>().Value() ? 1 : 0;
  auto cb = info[4].As<Napi::Function>();
  (new WriteWorker(cb, handle, buffer, timeout, echoMode))->Queue();
  return info.Env().Undefined();
}

Napi::Value BufferedRead(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  long long handle = asHandle(info[0]);
  int timeout = info[1].As<Napi::Number>().Int32Value();
  auto dataCb = info[2].As<Napi::Function>();
  auto doneCb = info[3].As<Napi::Function>();

  auto tsfn = Napi::ThreadSafeFunction::New(env, dataCb, "fsp_buffered_read", 0,
                                            1);
  int cbId = registerTsfn(tsfn);
  (new BufferedReadWorker(doneCb, handle, timeout, cbId))->Queue();
  return env.Undefined();
}

Napi::Value Update(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  long long handle = asHandle(info[0]);
  std::string opts = jsonStringify(env, info[1]);
  auto cb = info[2].As<Napi::Function>();
  (new SimpleWorker(cb, [handle, opts]() mutable {
    return fsp_update(handle, &opts[0]);
  }))->Queue();
  return env.Undefined();
}

Napi::Value Set(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  long long handle = asHandle(info[0]);
  std::string opts = jsonStringify(env, info[1]);
  auto cb = info[2].As<Napi::Function>();
  (new SimpleWorker(cb, [handle, opts]() mutable {
    return fsp_set(handle, &opts[0]);
  }))->Queue();
  return env.Undefined();
}

Napi::Value Get(const Napi::CallbackInfo& info) {
  auto cb = info[1].As<Napi::Function>();
  (new GetWorker(cb, asHandle(info[0])))->Queue();
  return info.Env().Undefined();
}

Napi::Value GetBaudRate(const Napi::CallbackInfo& info) {
  auto cb = info[1].As<Napi::Function>();
  (new GetBaudRateWorker(cb, asHandle(info[0])))->Queue();
  return info.Env().Undefined();
}

Napi::Value Drain(const Napi::CallbackInfo& info) {
  long long handle = asHandle(info[0]);
  auto cb = info[1].As<Napi::Function>();
  (new SimpleWorker(cb, [handle]() { return fsp_drain(handle); }))->Queue();
  return info.Env().Undefined();
}

Napi::Value Flush(const Napi::CallbackInfo& info) {
  long long handle = asHandle(info[0]);
  auto cb = info[1].As<Napi::Function>();
  (new SimpleWorker(cb, [handle]() { return fsp_flush(handle); }))->Queue();
  return info.Env().Undefined();
}

Napi::Value ConfigureLogging(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int enabled = info[0].As<Napi::Boolean>().Value() ? 1 : 0;
  std::string dir = info[1].As<Napi::String>();
  fsp_configure_logging(enabled, &dir[0]);
  return env.Undefined();
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("list", Napi::Function::New(env, List));
  exports.Set("open", Napi::Function::New(env, Open));
  exports.Set("close", Napi::Function::New(env, Close));
  exports.Set("read", Napi::Function::New(env, Read));
  exports.Set("bufferedRead", Napi::Function::New(env, BufferedRead));
  exports.Set("write", Napi::Function::New(env, Write));
  exports.Set("update", Napi::Function::New(env, Update));
  exports.Set("set", Napi::Function::New(env, Set));
  exports.Set("get", Napi::Function::New(env, Get));
  exports.Set("getBaudRate", Napi::Function::New(env, GetBaudRate));
  exports.Set("drain", Napi::Function::New(env, Drain));
  exports.Set("flush", Napi::Function::New(env, Flush));
  exports.Set("configureLogging", Napi::Function::New(env, ConfigureLogging));
  return exports;
}

}  // namespace

// fsp_emit_chunk is called from the Go bufferedRead loop (on the worker thread)
// once per received chunk. It copies the bytes and hands them to the JS data
// callback registered under cbId.
extern "C" void fsp_emit_chunk(int cbId, void* data, int len) {
  Napi::ThreadSafeFunction tsfn;
  {
    std::lock_guard<std::mutex> lock(g_tsfn_mu);
    auto it = g_tsfns.find(cbId);
    if (it == g_tsfns.end()) {
      return;
    }
    tsfn = it->second;
  }

  auto* chunk = new Chunk();
  chunk->bytes.assign(static_cast<char*>(data),
                      static_cast<char*>(data) + len);

  napi_status status = tsfn.BlockingCall(
      chunk, [](Napi::Env env, Napi::Function jsCallback, Chunk* c) {
        Napi::Buffer<char> buf =
            Napi::Buffer<char>::Copy(env, c->bytes.data(), c->bytes.size());
        jsCallback.Call({buf});
        delete c;
      });

  if (status != napi_ok) {
    delete chunk;
  }
}

NODE_API_MODULE(faster_serialport, Init)
