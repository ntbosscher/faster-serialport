// node-addon-api bridge between lib/bindings.js and the Go c-archive
// (go-serial). One-shot binding functions dispatch straight into the Go
// c-archive (fsp_*_async) and return immediately; Go reports the result back
// through fsp_complete, which resolves the JS promise on the loop thread via a
// per-call Napi::ThreadSafeFunction. The callback-last contract that
// lib/bindings.js expects is preserved: cb(err, result).
//
// bufferedRead follows the same dispatch: it runs synchronously in Go and
// reports completion through fsp_complete, but additionally streams received
// chunks up through a separate ThreadSafeFunction (fsp_emit_chunk) while it runs.

#include <napi.h>

#include <map>
#include <mutex>
#include <string>

#include "libserial.h"

namespace {

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

// ---- async completion plumbing -------------------------------------------
//
// A PendingOp holds everything needed to resolve one in-flight operation: the
// per-call ThreadSafeFunction wrapping the JS callback, an optional buffer
// reference kept alive while Go reads/writes it, and the result filled in by
// fsp_complete.

struct PendingOp {
  Napi::ThreadSafeFunction tsfn;
  Napi::Reference<Napi::Buffer<char>> bufferRef;
  std::string err;
  std::string result;  // JSON-encoded success value
  bool isError = false;
  bool hasResult = false;
  int streamCbId = 0;  // bufferedRead's streaming TSFN, released on completion
};

std::mutex g_pending_mu;
std::map<int, PendingOp*> g_pending;
int g_next_pending_id = 1;

int registerPending(Napi::ThreadSafeFunction tsfn,
                    Napi::Reference<Napi::Buffer<char>> bufferRef,
                    int streamCbId = 0) {
  auto* op = new PendingOp();
  op->tsfn = tsfn;
  op->bufferRef = std::move(bufferRef);
  op->streamCbId = streamCbId;

  std::lock_guard<std::mutex> lock(g_pending_mu);
  int id = g_next_pending_id++;
  g_pending[id] = op;
  return id;
}

// makeCbId wires the JS completion callback to a fresh pending slot and returns
// its id for handing to the Go *_async call.
int makeCbId(Napi::Env env, Napi::Function cb, const char* name) {
  auto tsfn = Napi::ThreadSafeFunction::New(env, cb, name, 0, 1);
  return registerPending(tsfn, Napi::Reference<Napi::Buffer<char>>());
}

// buf is held alive (via a persistent reference) until the operation completes,
// so Go can safely read from / write into its memory.
int makeCbId(Napi::Env env, Napi::Function cb, const char* name,
             Napi::Buffer<char> buf) {
  auto tsfn = Napi::ThreadSafeFunction::New(env, cb, name, 0, 1);
  return registerPending(tsfn, Napi::Persistent(buf));
}

// ---- bufferedRead chunk plumbing -----------------------------------------

// Chunk carries one received batch across the ThreadSafeFunction queue. data is
// a Go-malloc'd buffer owned by us until it is wrapped in a JS Buffer (whose
// finalizer then frees it).
struct Chunk {
  void* data;
  size_t len;
};

// Bounded depth for a bufferedRead stream's chunk queue. When the JS consumer
// falls behind, fsp_emit_chunk's BlockingCall parks the Go reader thread until
// the loop drains a slot, so native memory (one malloc'd batch per queued chunk)
// can't grow without bound. 64 * batchSize (default 8192) ≈ 512 KiB ceiling.
constexpr size_t kChunkQueueDepth = 64;

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

// ---- JS-facing functions -------------------------------------------------

long long asHandle(Napi::Value v) {
  return static_cast<long long>(v.As<Napi::Number>().DoubleValue());
}

Napi::Value List(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int cbId = makeCbId(env, info[0].As<Napi::Function>(), "fsp_list");
  fsp_list_async(cbId);
  return env.Undefined();
}

Napi::Value Open(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  std::string path = info[0].As<Napi::String>();
  std::string opts = jsonStringify(env, info[1]);
  int cbId = makeCbId(env, info[2].As<Napi::Function>(), "fsp_open");
  fsp_open_async(&path[0], &opts[0], cbId);
  return env.Undefined();
}

Napi::Value Close(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int cbId = makeCbId(env, info[1].As<Napi::Function>(), "fsp_close");
  fsp_close_async(asHandle(info[0]), cbId);
  return env.Undefined();
}

Napi::Value Read(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  long long handle = asHandle(info[0]);
  auto buffer = info[1].As<Napi::Buffer<char>>();
  int offset = info[2].As<Napi::Number>().Int32Value();
  int length = info[3].As<Napi::Number>().Int32Value();
  int timeout = info[4].As<Napi::Number>().Int32Value();
  int cbId = makeCbId(env, info[5].As<Napi::Function>(), "fsp_read", buffer);
  fsp_read_async(handle, buffer.Data() + offset, length, timeout, cbId);
  return env.Undefined();
}

Napi::Value Write(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  long long handle = asHandle(info[0]);
  auto buffer = info[1].As<Napi::Buffer<char>>();
  int timeout = info[2].As<Napi::Number>().Int32Value();
  int echoMode = info[3].As<Napi::Boolean>().Value() ? 1 : 0;
  int cbId = makeCbId(env, info[4].As<Napi::Function>(), "fsp_write", buffer);
  fsp_write_async(handle, buffer.Data(), static_cast<int>(buffer.Length()),
                  timeout, echoMode, cbId);
  return env.Undefined();
}

Napi::Value BufferedRead(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  long long handle = asHandle(info[0]);
  int timeout = info[1].As<Napi::Number>().Int32Value();
  auto dataCb = info[2].As<Napi::Function>();
  auto doneCb = info[3].As<Napi::Function>();

  // Streaming callback for received chunks. Created before the done slot so its
  // queued chunks are processed ahead of the done resolution. Bounded queue so a
  // slow consumer applies backpressure to the Go reader (see fsp_emit_chunk).
  auto dataTsfn = Napi::ThreadSafeFunction::New(env, dataCb, "fsp_buffered_read",
                                                kChunkQueueDepth, 1);
  int dataCbId = registerTsfn(dataTsfn);

  // Done callback resolves via fsp_complete, which also releases the streaming
  // callback (dataCbId) once its queued chunks have drained.
  auto doneTsfn = Napi::ThreadSafeFunction::New(env, doneCb,
                                                "fsp_buffered_read_done", 0, 1);
  int doneCbId =
      registerPending(doneTsfn, Napi::Reference<Napi::Buffer<char>>(), dataCbId);

  fsp_buffered_read_async(handle, timeout, dataCbId, doneCbId);
  return env.Undefined();
}

// BufferedReadExt is BufferedRead with the loop tuning passed through as a JSON
// options object (info[1]) instead of a single timeout.
Napi::Value BufferedReadExt(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  long long handle = asHandle(info[0]);
  std::string opts = jsonStringify(env, info[1]);
  auto dataCb = info[2].As<Napi::Function>();
  auto doneCb = info[3].As<Napi::Function>();

  auto dataTsfn = Napi::ThreadSafeFunction::New(
      env, dataCb, "fsp_buffered_read_ext", kChunkQueueDepth, 1);
  int dataCbId = registerTsfn(dataTsfn);

  auto doneTsfn = Napi::ThreadSafeFunction::New(
      env, doneCb, "fsp_buffered_read_ext_done", 0, 1);
  int doneCbId =
      registerPending(doneTsfn, Napi::Reference<Napi::Buffer<char>>(), dataCbId);

  fsp_buffered_read_ext_async(handle, &opts[0], dataCbId, doneCbId);
  return env.Undefined();
}

Napi::Value Update(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  long long handle = asHandle(info[0]);
  std::string opts = jsonStringify(env, info[1]);
  int cbId = makeCbId(env, info[2].As<Napi::Function>(), "fsp_update");
  fsp_update_async(handle, &opts[0], cbId);
  return env.Undefined();
}

Napi::Value Set(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  long long handle = asHandle(info[0]);
  std::string opts = jsonStringify(env, info[1]);
  int cbId = makeCbId(env, info[2].As<Napi::Function>(), "fsp_set");
  fsp_set_async(handle, &opts[0], cbId);
  return env.Undefined();
}

Napi::Value Get(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int cbId = makeCbId(env, info[1].As<Napi::Function>(), "fsp_get");
  fsp_get_async(asHandle(info[0]), cbId);
  return env.Undefined();
}

Napi::Value GetBaudRate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int cbId = makeCbId(env, info[1].As<Napi::Function>(), "fsp_get_baud_rate");
  fsp_get_baud_rate_async(asHandle(info[0]), cbId);
  return env.Undefined();
}

Napi::Value Drain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int cbId = makeCbId(env, info[1].As<Napi::Function>(), "fsp_drain");
  fsp_drain_async(asHandle(info[0]), cbId);
  return env.Undefined();
}

Napi::Value Flush(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int cbId = makeCbId(env, info[1].As<Napi::Function>(), "fsp_flush");
  fsp_flush_async(asHandle(info[0]), cbId);
  return env.Undefined();
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
  exports.Set("bufferedReadExt", Napi::Function::New(env, BufferedReadExt));
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

// fsp_complete is called from a Go fsp_*_async wrapper to report an operation's
// outcome. It resolves the JS promise for cbId via that call's ThreadSafeFunction
// (NonBlockingCall so it can't deadlock when invoked on the loop thread). It
// takes ownership of the err / resultJson strings and frees them.
extern "C" void fsp_complete(int cbId, char* err, char* resultJson) {
  PendingOp* op = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_pending_mu);
    auto it = g_pending.find(cbId);
    if (it != g_pending.end()) {
      op = it->second;
      g_pending.erase(it);
    }
  }

  if (op == nullptr) {
    // Unknown id: free the Go-allocated strings so they don't leak.
    if (err != nullptr) {
      fsp_free(err);
    }
    if (resultJson != nullptr) {
      fsp_free(resultJson);
    }
    return;
  }

  if (err != nullptr) {
    op->err = err;
    op->isError = true;
    fsp_free(err);
  }
  if (resultJson != nullptr) {
    op->result = resultJson;
    op->hasResult = true;
    fsp_free(resultJson);
  }

  Napi::ThreadSafeFunction tsfn = op->tsfn;
  napi_status status = tsfn.NonBlockingCall(
      op, [](Napi::Env env, Napi::Function cb, PendingOp* op) {
        if (op->isError) {
          cb.Call({Napi::Error::New(env, op->err).Value()});
        } else if (op->hasResult) {
          cb.Call({env.Null(), jsonParse(env, op->result)});
        } else {
          cb.Call({env.Null()});
        }
        // bufferedRead: release its streaming callback now that all queued
        // chunks (processed ahead of this done resolution) have drained.
        if (op->streamCbId != 0) {
          releaseTsfn(op->streamCbId);
        }
        delete op;  // releases bufferRef on the loop thread
      });

  if (status != napi_ok) {
    delete op;
  }
  tsfn.Release();
}

// fsp_emit_chunk is called from the Go bufferedRead loop once per received
// chunk. data is a Go-malloc'd buffer whose ownership passes to us: it is wrapped
// in a JS Buffer without copying and freed (via fsp_free) when V8 collects that
// Buffer. BlockingCall applies backpressure: when the bounded queue
// (kChunkQueueDepth) is full it parks the caller until the loop drains a slot.
// This is safe from deadlock because BufferedRead — the only caller — always runs
// on a Go goroutine thread, never the libuv loop thread (unlike fsp_complete,
// which can, and so stays non-blocking).
extern "C" void fsp_emit_chunk(int cbId, void* data, int len) {
  Napi::ThreadSafeFunction tsfn;
  {
    std::lock_guard<std::mutex> lock(g_tsfn_mu);
    auto it = g_tsfns.find(cbId);
    if (it == g_tsfns.end()) {
      // No consumer for this id: we own the buffer, so free it.
      fsp_free(data);
      return;
    }
    tsfn = it->second;
  }

  auto* chunk = new Chunk{data, static_cast<size_t>(len)};

  napi_status status = tsfn.BlockingCall(
      chunk, [](Napi::Env env, Napi::Function jsCallback, Chunk* c) {
        // Wrap the Go-allocated buffer without copying; V8 frees it via the
        // finalizer when the JS Buffer is garbage-collected.
        Napi::Buffer<char> buf = Napi::Buffer<char>::New(
            env, static_cast<char*>(c->data), c->len,
            [](Napi::Env, char* d) { fsp_free(d); });
        jsCallback.Call({buf});
        delete c;
      });

  if (status != napi_ok) {
    fsp_free(data);  // couldn't enqueue; free the buffer we own
    delete chunk;
  }
}

NODE_API_MODULE(faster_serialport, Init)
