{
  "targets": [
    {
      "target_name": "speakerflow_pipewire",
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
    }
  ]
}
