{
  "targets": [
    {
      "target_name": "node_vmm_native",
      "sources": [],
      "conditions": [
        ["OS=='linux'", {
          "sources": ["native/kvm_backend.cc"],
          "cflags_cc!": ["-fno-exceptions"],
          "cflags_cc": [
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Wno-missing-field-initializers",
            "-fexceptions",
            "-fPIC",
            "-fstack-protector-strong",
            "-D_FORTIFY_SOURCE=2"
          ],
          "ldflags": ["-Wl,-z,relro", "-Wl,-z,now", "-Wl,-z,noexecstack"]
        }],
        ["OS=='win'", {
          "sources": ["native/whp_backend.cc"],
          "defines": ["NOMINMAX", "WIN32_LEAN_AND_MEAN"],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "ExceptionHandling": 1,
              "AdditionalOptions": ["/std:c++17", "/GS", "/guard:cf"]
            },
            "VCLinkerTool": {
              "AdditionalOptions": ["/guard:cf", "/DYNAMICBASE", "/NXCOMPAT"]
            }
          }
        }]
      ]
    }
  ]
}
