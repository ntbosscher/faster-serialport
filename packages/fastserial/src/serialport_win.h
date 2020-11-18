#ifndef PACKAGES_SERIALPORT_SRC_SERIALPORT_WIN_H_
#define PACKAGES_SERIALPORT_SRC_SERIALPORT_WIN_H_
// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
#include <nan.h>
#include <list>
#include <string>

#define MAX_BUFFER_SIZE 1000
// As per https://msdn.microsoft.com/en-us/library/windows/desktop/ms724872(v=vs.85).aspx
#define MAX_REGISTRY_KEY_SIZE 255
#define TIMEOUT_PRECISION 10
#define ERROR_STRING_SIZE 1024
#define ARRAY_SIZE(arr)     (sizeof(arr)/sizeof(arr[0]))

void ErrorCodeToString(const char* prefix, int errorCode, char *errorStr);

struct WriteBaton : public Nan::AsyncResource {
  WriteBaton() : AsyncResource("node-serialport:WriteBaton"), bufferData(), errorString() {}
  int fd = 0;
  int timeout = -1;
  char* bufferData = nullptr;
  size_t bufferLength = 0;
  size_t offset = 0;
  size_t bytesWritten = 0;
  void* hThread = nullptr;
  bool complete = false;
  Nan::Persistent<v8::Object> buffer;
  Nan::Callback callback;
  int result = 0;
  char errorString[ERROR_STRING_SIZE];
};

NAN_METHOD(Write);
void EIO_Write(uv_work_t* req);
void EIO_AfterWrite(uv_async_t* req);
DWORD __stdcall WriteThread(LPVOID param);


struct ReadBaton : public Nan::AsyncResource {
  ReadBaton() : AsyncResource("node-serialport:ReadBaton"), errorString() {}
  int fd = 0;
  int timeout = -1;
  char* bufferData = nullptr;
  size_t bufferLength = 0;
  size_t bytesRead = 0;
  size_t bytesToRead = 0;
  size_t offset = 0;
  void* hThread = nullptr;
  bool complete = false;
  char errorString[ERROR_STRING_SIZE];
  Nan::Callback callback;
};

NAN_METHOD(Read);
void EIO_Read(uv_work_t* req);
void EIO_AfterRead(uv_async_t* req);
DWORD __stdcall ReadThread(LPVOID param);


NAN_METHOD(List);
void EIO_List(uv_work_t* req);
void EIO_AfterList(uv_work_t* req);

struct ListResultItem {
  std::string path;
  std::string manufacturer;
  std::string serialNumber;
  std::string pnpId;
  std::string locationId;
  std::string vendorId;
  std::string productId;
};

void internalInit();

#endif  // PACKAGES_SERIALPORT_SRC_SERIALPORT_WIN_H_
