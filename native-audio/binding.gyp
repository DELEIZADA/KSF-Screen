{
  "targets": [
    {
      "target_name": "ksf_audio",
      "sources": [
        "ksf_audio.cpp"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS"
      ],
      "libraries": [
        "ole32.lib",
        "uuid.lib",
        "avrt.lib",
        "mmdevapi.lib"
      ],
      "msvs_settings": {
        "VCCLCompilerTool": {
          "ExceptionHandling": 1,
          "AdditionalOptions": [
            "/std:c++20"
          ]
        }
      }
    },
    {
      "target_name": "ksf_video",
      "sources": [
        "ksf_video.cpp"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS",
        "WIN32_LEAN_AND_MEAN",
        "NOMINMAX"
      ],
      "libraries": [
        "ole32.lib",
        "uuid.lib",
        "windowsapp.lib",
        "d3d11.lib",
        "dxgi.lib",
        "dwmapi.lib"
      ],
      "msvs_settings": {
        "VCCLCompilerTool": {
          "ExceptionHandling": 1,
          "AdditionalOptions": [
            "/std:c++20",
            "/permissive-"
          ]
        }
      }
    },
    {
      "target_name": "ksf_hook",
      "type": "shared_library",
      "product_extension": "dll",
      "sources": [
        "ksf-hook/ksf_hook.cpp",
        "ksf-hook/ksf_hook_dxgi.cpp",
        "ksf-hook/minhook/src/buffer.c",
        "ksf-hook/minhook/src/hook.c",
        "ksf-hook/minhook/src/trampoline.c",
        "ksf-hook/minhook/src/hde/hde32.c",
        "ksf-hook/minhook/src/hde/hde64.c"
      ],
      "include_dirs": [
        "ksf-hook",
        "ksf-hook/minhook/include",
        "ksf-hook/minhook/src",
        "ksf-hook/minhook/src/hde"
      ],
      "defines": [
        "WIN32_LEAN_AND_MEAN",
        "NOMINMAX"
      ],
      "libraries": [
        "d3d11.lib",
        "dxgi.lib",
        "user32.lib"
      ],
      "msvs_settings": {
        "VCCLCompilerTool": {
          "ExceptionHandling": 1,
          "AdditionalOptions": [
            "/std:c++20",
            "/permissive-"
          ]
        }
      }
    },
    {
      "target_name": "ksf_injector",
      "type": "executable",
      "sources": [
        "ksf-injector/ksf_injector.cpp"
      ],
      "defines": [
        "WIN32_LEAN_AND_MEAN",
        "NOMINMAX",
        "_UNICODE",
        "UNICODE"
      ],
      "libraries": [
        "kernel32.lib"
      ],
      "msvs_settings": {
        "VCCLCompilerTool": {
          "ExceptionHandling": 1,
          "AdditionalOptions": [
            "/std:c++20",
            "/permissive-"
          ]
        }
      }
    }
  ]
}