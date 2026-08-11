// node-addon-api bridge between lib/bindings.js and the Go backend (go-serial),
// reached through goApi — a static c-archive on macOS/Linux, or a runtime-loaded
// libserial.dll on Windows (see the Go backend binding section below). One-shot
// binding functions dispatch straight into Go (goApi.*_async) and return
// immediately; Go reports the result back through fsp_complete, which resolves
// the JS promise on the loop thread via a per-call Napi::ThreadSafeFunction. The
// callback-last contract that lib/bindings.js expects is preserved: cb(err, result).
//
// bufferedRead follows the same dispatch: it runs synchronously in Go and
// reports completion through fsp_complete, but additionally streams received
// chunks up through a separate ThreadSafeFunction (fsp_emit_chunk) while it runs.

#include <napi.h>

#include <map>
#include <mutex>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "libserial.h"

// Completion/streaming callbacks the Go side invokes (defined at the bottom of
// this file). Their addresses are handed to Go via goApi.set_callbacks in Init.
extern "C" void fsp_complete(int cbId, char* err, char* resultJson);
extern "C" void fsp_complete_num(int cbId, long long value);
extern "C" void fsp_emit_chunk(int cbId, void* data, int len);
extern "C" void fsp_emit_event(int cbId, char* err, int errorCode, int event);
extern "C" void fsp_release_event(int cbId);

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
  std::string result;    // JSON-encoded success value
  long long numResult = 0;  // numeric success value (see fsp_complete_num)
  bool isError = false;
  bool hasResult = false;
  bool hasNum = false;
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

// ---- Go backend binding --------------------------------------------------
//
// The Go backend ships as libserial: a static c-archive linked straight into
// this addon on macOS/Linux, or a c-shared libserial.dll loaded at runtime on
// Windows. The runtime load is deliberate — it runs the Go runtime's DLL
// initialisation, which never happened when a mingw c-archive was linked with
// MSVC, leaving every cgo call hung on an uninitialised runtime. All Go entry
// points are called through goApi so the call sites don't care which form it is.

struct GoApi {
  void (*free)(void*);
  void (*configure_logging)(int, char*);
  void (*list_async)(int);
  void (*open_async)(char*, char*, int, int);
  void (*close_async)(long long, int);
  void (*read_async)(long long, void*, int, int, int);
  void (*write_async)(long long, void*, int, int, int, int);
  void (*buffered_read_async)(long long, int, int, int);
  void (*buffered_read_ext_async)(long long, char*, int, int);
  void (*update_async)(long long, char*, int);
  void (*set_async)(long long, char*, int);
  void (*get_async)(long long, int);
  void (*get_baud_rate_async)(long long, int);
  void (*drain_async)(long long, int);
  void (*flush_async)(long long, int);
  void (*set_callbacks)(void*, void*, void*, void*, void*);
};

GoApi goApi;

#ifdef _WIN32
// bindGoApi loads libserial.dll from beside this .node (so the Go runtime's DLL
// init runs) and resolves every export. Returns an error string on failure.
const char* bindGoApi() {
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&bindGoApi), &self)) {
    return "could not locate the addon module";
  }

  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return "could not resolve the addon path";
  }

  // Load the DLL sitting next to this .node by full path, so it resolves
  // regardless of the process's working directory or DLL search path.
  std::wstring full(buf, n);
  size_t sep = full.find_last_of(L"\\/");
  std::wstring dll =
      (sep == std::wstring::npos ? L"" : full.substr(0, sep + 1)) +
      L"libserial.dll";

  HMODULE h =
      LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (h == nullptr) {
    return "could not load libserial.dll";
  }

#define FSP_BIND(field, name)                                             \
  goApi.field = reinterpret_cast<decltype(goApi.field)>(                  \
      reinterpret_cast<void*>(GetProcAddress(h, name)));                  \
  if (goApi.field == nullptr) return "missing export " name;

  FSP_BIND(free, "fsp_free")
  FSP_BIND(configure_logging, "fsp_configure_logging")
  FSP_BIND(list_async, "fsp_list_async")
  FSP_BIND(open_async, "fsp_open_async")
  FSP_BIND(close_async, "fsp_close_async")
  FSP_BIND(read_async, "fsp_read_async")
  FSP_BIND(write_async, "fsp_write_async")
  FSP_BIND(buffered_read_async, "fsp_buffered_read_async")
  FSP_BIND(buffered_read_ext_async, "fsp_buffered_read_ext_async")
  FSP_BIND(update_async, "fsp_update_async")
  FSP_BIND(set_async, "fsp_set_async")
  FSP_BIND(get_async, "fsp_get_async")
  FSP_BIND(get_baud_rate_async, "fsp_get_baud_rate_async")
  FSP_BIND(drain_async, "fsp_drain_async")
  FSP_BIND(flush_async, "fsp_flush_async")
  FSP_BIND(set_callbacks, "fsp_set_callbacks")
#undef FSP_BIND

  return nullptr;
}
#else
// The c-archive is statically linked, so the exports are ordinary symbols.
const char* bindGoApi() {
  goApi.free = fsp_free;
  goApi.configure_logging = fsp_configure_logging;
  goApi.list_async = fsp_list_async;
  goApi.open_async = fsp_open_async;
  goApi.close_async = fsp_close_async;
  goApi.read_async = fsp_read_async;
  goApi.write_async = fsp_write_async;
  goApi.buffered_read_async = fsp_buffered_read_async;
  goApi.buffered_read_ext_async = fsp_buffered_read_ext_async;
  goApi.update_async = fsp_update_async;
  goApi.set_async = fsp_set_async;
  goApi.get_async = fsp_get_async;
  goApi.get_baud_rate_async = fsp_get_baud_rate_async;
  goApi.drain_async = fsp_drain_async;
  goApi.flush_async = fsp_flush_async;
  goApi.set_callbacks = fsp_set_callbacks;
  return nullptr;
}
#endif

// ---- JS-facing functions -------------------------------------------------

long long asHandle(Napi::Value v) {
  return static_cast<long long>(v.As<Napi::Number>().DoubleValue());
}

Napi::Value List(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int cbId = makeCbId(env, info[0].As<Napi::Function>(), "fsp_list");
  goApi.list_async(cbId);
  return env.Undefined();
}

// Bounded depth for a port's events callback queue. Events are infrequent, but a
// bounded queue applies backpressure to the Go watcher (fsp_emit_event uses
// BlockingCall) instead of growing without bound if JS falls behind.
constexpr size_t kEventQueueDepth = 16;

Napi::Value Open(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  std::string path = info[0].As<Napi::String>();
  std::string opts = jsonStringify(env, info[1]);

  // Persistent events callback, alive for the port's lifetime and released when
  // Go reports the close (fsp_release_event). Created before the open completes
  // so the watcher has a valid slot as soon as the port opens.
  auto eventsTsfn = Napi::ThreadSafeFunction::New(
      env, info[2].As<Napi::Function>(), "fsp_events", kEventQueueDepth, 1);
  int eventCbId = registerTsfn(eventsTsfn);

  int cbId = makeCbId(env, info[3].As<Napi::Function>(), "fsp_open");
  goApi.open_async(&path[0], &opts[0], eventCbId, cbId);
  return env.Undefined();
}

Napi::Value Close(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int cbId = makeCbId(env, info[1].As<Napi::Function>(), "fsp_close");
  goApi.close_async(asHandle(info[0]), cbId);
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
  goApi.read_async(handle, buffer.Data() + offset, length, timeout, cbId);
  return env.Undefined();
}

Napi::Value Write(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  long long handle = asHandle(info[0]);
  auto buffer = info[1].As<Napi::Buffer<char>>();
  int timeout = info[2].As<Napi::Number>().Int32Value();
  int echoMode = info[3].As<Napi::Boolean>().Value() ? 1 : 0;
  int cbId = makeCbId(env, info[4].As<Napi::Function>(), "fsp_write", buffer);
  goApi.write_async(handle, buffer.Data(), static_cast<int>(buffer.Length()),
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

  goApi.buffered_read_async(handle, timeout, dataCbId, doneCbId);
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

  goApi.buffered_read_ext_async(handle, &opts[0], dataCbId, doneCbId);
  return env.Undefined();
}

Napi::Value Update(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  long long handle = asHandle(info[0]);
  std::string opts = jsonStringify(env, info[1]);
  int cbId = makeCbId(env, info[2].As<Napi::Function>(), "fsp_update");
  goApi.update_async(handle, &opts[0], cbId);
  return env.Undefined();
}

Napi::Value Set(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  long long handle = asHandle(info[0]);
  std::string opts = jsonStringify(env, info[1]);
  int cbId = makeCbId(env, info[2].As<Napi::Function>(), "fsp_set");
  goApi.set_async(handle, &opts[0], cbId);
  return env.Undefined();
}

Napi::Value Get(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int cbId = makeCbId(env, info[1].As<Napi::Function>(), "fsp_get");
  goApi.get_async(asHandle(info[0]), cbId);
  return env.Undefined();
}

Napi::Value GetBaudRate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int cbId = makeCbId(env, info[1].As<Napi::Function>(), "fsp_get_baud_rate");
  goApi.get_baud_rate_async(asHandle(info[0]), cbId);
  return env.Undefined();
}

Napi::Value Drain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int cbId = makeCbId(env, info[1].As<Napi::Function>(), "fsp_drain");
  goApi.drain_async(asHandle(info[0]), cbId);
  return env.Undefined();
}

Napi::Value Flush(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int cbId = makeCbId(env, info[1].As<Napi::Function>(), "fsp_flush");
  goApi.flush_async(asHandle(info[0]), cbId);
  return env.Undefined();
}

Napi::Value ConfigureLogging(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int enabled = info[0].As<Napi::Boolean>().Value() ? 1 : 0;
  std::string dir = info[1].As<Napi::String>();
  goApi.configure_logging(enabled, &dir[0]);
  return env.Undefined();
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  // Bind the Go backend before exposing anything — on Windows this loads
  // libserial.dll and initialises the Go runtime.
  if (const char* err = bindGoApi()) {
    Napi::Error::New(env, std::string("faster-serialport: ") + err)
        .ThrowAsJavaScriptException();
    return exports;
  }

  // Hand Go the addresses of our completion/streaming callbacks so it can call
  // back into this module (order matches fsp_set_callbacks in api.go).
  goApi.set_callbacks(reinterpret_cast<void*>(fsp_emit_chunk),
                      reinterpret_cast<void*>(fsp_complete),
                      reinterpret_cast<void*>(fsp_complete_num),
                      reinterpret_cast<void*>(fsp_emit_event),
                      reinterpret_cast<void*>(fsp_release_event));

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

// takePending removes and returns the pending op for id, or nullptr if the id is
// unknown (e.g. already completed).
PendingOp* takePending(int cbId) {
  std::lock_guard<std::mutex> lock(g_pending_mu);
  auto it = g_pending.find(cbId);
  if (it == g_pending.end()) {
    return nullptr;
  }
  PendingOp* op = it->second;
  g_pending.erase(it);
  return op;
}

// resolvePending hands op's filled-in outcome to its JS callback on the loop
// thread (NonBlockingCall so it can't deadlock when invoked on the loop thread)
// and releases the op's resources.
void resolvePending(PendingOp* op) {
  Napi::ThreadSafeFunction tsfn = op->tsfn;
  napi_status status = tsfn.NonBlockingCall(
      op, [](Napi::Env env, Napi::Function cb, PendingOp* op) {
        if (op->isError) {
          cb.Call({Napi::Error::New(env, op->err).Value()});
        } else if (op->hasNum) {
          cb.Call({env.Null(),
                   Napi::Number::New(env, static_cast<double>(op->numResult))});
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
    // The callback won't run, so release bufferedRead's streaming TSFN here
    // instead (it is otherwise released from inside that callback).
    if (op->streamCbId != 0) {
      releaseTsfn(op->streamCbId);
    }
    delete op;
  }
  tsfn.Release();
}

}  // namespace

// fsp_complete is called from a Go fsp_*_async wrapper to report an operation's
// outcome with an error or a JSON-encoded success value. It takes ownership of
// the err / resultJson strings and frees them.
extern "C" void fsp_complete(int cbId, char* err, char* resultJson) {
  PendingOp* op = takePending(cbId);
  if (op == nullptr) {
    // Unknown id: free the Go-allocated strings so they don't leak.
    if (err != nullptr) {
      goApi.free(err);
    }
    if (resultJson != nullptr) {
      goApi.free(resultJson);
    }
    return;
  }

  if (err != nullptr) {
    op->err = err;
    op->isError = true;
    goApi.free(err);
  }
  if (resultJson != nullptr) {
    op->result = resultJson;
    op->hasResult = true;
    goApi.free(resultJson);
  }

  resolvePending(op);
}

// fsp_complete_num is fsp_complete's fast path for a numeric success value: it
// resolves the JS promise with a JS number directly, skipping the JSON encode on
// the Go side and the JSON.parse here. Errors still report through fsp_complete.
extern "C" void fsp_complete_num(int cbId, long long value) {
  PendingOp* op = takePending(cbId);
  if (op == nullptr) {
    return;
  }
  op->numResult = value;
  op->hasNum = true;
  resolvePending(op);
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
      goApi.free(data);
      return;
    }
    tsfn = it->second;
  }

  auto* chunk = new Chunk{data, static_cast<size_t>(len)};

  napi_status status = tsfn.BlockingCall(
      chunk, [](Napi::Env env, Napi::Function jsCallback, Chunk* c) {
        // Copy into a V8-owned buffer. External (zero-copy) buffers via
        // napi_create_external_buffer are rejected (napi_no_external_buffers_allowed)
        // under Electron's V8 sandbox, which threw here and dropped every chunk.
        Napi::Buffer<char> buf =
            Napi::Buffer<char>::Copy(env, static_cast<char*>(c->data), c->len);
        goApi.free(c->data);  // Go buffer is copied out; free it now
        jsCallback.Call({buf});
        delete c;
      });

  if (status != napi_ok) {
    goApi.free(data);  // couldn't enqueue; free the buffer we own
    delete chunk;
  }
}

// Event carries one comm event across the ThreadSafeFunction queue: a failure
// (err set, with errorCode) or a Win32 EV_* mask (event). err is a Go-malloc'd
// string owned by us until the JS callback runs, then freed via fsp_free.
struct Event {
  char* err;
  int errorCode;
  int event;
};

// fsp_emit_event is called from the Go comm-event watcher (a goroutine thread,
// so BlockingCall is safe) to invoke a port's eventsCallback. It mirrors the
// callback-last (err, arg) shape callers expect: on failure cb(Error, {errorCode}),
// otherwise cb(null, {event}).
extern "C" void fsp_emit_event(int cbId, char* err, int errorCode, int event) {
  Napi::ThreadSafeFunction tsfn;
  {
    std::lock_guard<std::mutex> lock(g_tsfn_mu);
    auto it = g_tsfns.find(cbId);
    if (it == g_tsfns.end()) {
      if (err != nullptr) {
        goApi.free(err);
      }
      return;
    }
    tsfn = it->second;
  }

  auto* ev = new Event{err, errorCode, event};

  napi_status status = tsfn.BlockingCall(
      ev, [](Napi::Env env, Napi::Function jsCallback, Event* e) {
        Napi::Object arg = Napi::Object::New(env);
        if (e->err != nullptr) {
          arg.Set("errorCode", Napi::Number::New(env, e->errorCode));
          jsCallback.Call({Napi::Error::New(env, e->err).Value(), arg});
          goApi.free(e->err);
        } else {
          arg.Set("event", Napi::Number::New(env, e->event));
          jsCallback.Call({env.Null(), arg});
        }
        delete e;
      });

  if (status != napi_ok) {
    if (err != nullptr) {
      goApi.free(err);
    }
    delete ev;
  }
}

// fsp_release_event releases a port's persistent events callback slot. Called
// from Go once the port has closed (watcher stopped, no further emits) or when no
// watcher was started.
extern "C" void fsp_release_event(int cbId) { releaseTsfn(cbId); }

NODE_API_MODULE(faster_serialport, Init)
