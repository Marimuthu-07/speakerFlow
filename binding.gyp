{
  "targets": [
    {
      "target_name": "speakerflow_pipewire",
      "conditions": [
        ["OS!='linux'", {
          "type": "none"
        }],
        ["OS=='linux'", {
          "sources": [
            "native/pipewire_binding.c"
          ],
          "include_dirs": [
            "<(module_root_dir)/native/include/pipewire-0.3",
            "<(module_root_dir)/native/include/spa-0.2"
          ],
          "libraries": [
            "-L/usr/lib/x86_64-linux-gnu",
            "-L/usr/lib",
            "-l:libpipewire-0.3.so.0",
            "-lm"
          ],
          "cflags": [
            "-O3",
            "-Wall",
            "-Wextra",
            "-fPIC",
            "-D_GNU_SOURCE"
          ]
        }]
      ]
    },
    {
      "target_name": "speakerflow_coreaudio",
      "conditions": [
        ["OS!='win'", {
          "type": "none"
        }],
        ["OS=='win'", {
          "sources": [
            "native/windows/coreaudio_binding.cpp",
            "native/windows/wasapi_capture_client.cpp",
            "native/windows/wasapi_render_client.cpp",
            "native/windows/wasapi_fanout_engine.cpp"
          ],
          "libraries": [
            "-lole32.lib",
            "-luuid.lib",
            "-lpropsys.lib",
            "-lpsapi.lib",
            "-lavrt.lib"
          ],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "ExceptionHandling": 1,
              "AdditionalOptions": [ "/EHsc" ]
            }
          }
        }]
      ]
    }
  ]
}
