
#ifdef WIN32
#define strncasecmp strnicmp
  #include "./win.h"
#endif

void EIO_Close(uv_work_t* req) {
    VoidBaton* data = static_cast<VoidBaton*>(req->data);

    g_closingHandles.push_back(data->fd);

    HMODULE hKernel32 = LoadLibrary("kernel32.dll");
    // Look up function address
    CancelIoExType pCancelIoEx = (CancelIoExType)GetProcAddress(hKernel32, "CancelIoEx");
    // Do something with it
    if (pCancelIoEx) {
        // Function exists so call it
        // Cancel all pending IO Requests for the current device
        pCancelIoEx(int2handle(data->fd), NULL);
    }
    if (!CloseHandle(int2handle(data->fd))) {
        ErrorCodeToString("Closing connection (CloseHandle)", GetLastError(), data->errorString);
        return;
    }
}
