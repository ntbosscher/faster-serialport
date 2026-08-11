{
  "targets": [
    {
      "target_name": "faster-serialport",
      "sources": [ "src/addon.cc" ],
      "include_dirs": [
        "<(module_root_dir)/node_modules/node-addon-api",
        "<(module_root_dir)/build-go"
      ],
      "defines": [ "NAPI_VERSION=8", "NODE_ADDON_API_CPP_EXCEPTIONS" ],
      "cflags!": [ "-fno-exceptions" ],
      "cflags_cc!": [ "-fno-exceptions" ],
      "cflags_cc": [ "-std=c++17" ],
      "conditions": [
        [ "OS==\"mac\"", {
          "libraries": [
            "<(module_root_dir)/build-go/libserial.a",
            "-framework CoreFoundation",
            "-framework IOKit"
          ],
          "xcode_settings": {
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "CLANG_CXX_LIBRARY": "libc++",
            "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
            "MACOSX_DEPLOYMENT_TARGET": "10.13"
          }
        } ],
        [ "OS==\"linux\"", {
          "libraries": [
            "<(module_root_dir)/build-go/libserial.a",
            "-lpthread"
          ]
        } ],
        [ "OS==\"win\"", {
          # libserial.dll is loaded at runtime (see src/addon.cc), not linked
          # here; its transitive libs are static-linked into the DLL by build-go.js.
          "msvs_settings": {
            "VCCLCompilerTool": { "ExceptionHandling": 1 },
            "VCLinkerTool": { "ImageHasSafeExceptionHandlers": "false" }
          }
        } ]
      ]
    }
  ]
}
