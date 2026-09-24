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
    }
  ]
}