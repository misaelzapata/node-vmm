{
  "targets": [
    {
      "target_name": "node_vmm_native",
      "sources": [],
      "conditions": [
        ["OS=='linux'", {
          "sources": ["native/kvm/backend.cc"],
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
          "ldflags": ["-Wl,-z,relro", "-Wl,-z,now", "-Wl,-z,noexecstack"],
          "conditions": [
            ["\"<!(node -e \"process.stdout.write(process.env.NODE_VMM_HAVE_LIBSLIRP || '')\")\"=='1'", {
              "defines+": ["NODE_VMM_HAVE_LIBSLIRP"],
              "cflags_cc+": ["<!@(node -e \"process.stdout.write(process.env.NODE_VMM_LIBSLIRP_CFLAGS || '')\")"],
              "libraries+": ["<!@(node -e \"process.stdout.write(process.env.NODE_VMM_LIBSLIRP_LIBS || '')\")"]
            }]
          ]
        }],
        ["OS=='win'", {
          "sources": [
            "native/whp/backend.cc",
            "native/whp/boot_params.cc",
            "native/whp/devices/acpi_pm_timer.cc",
            "native/whp/devices/cmos.cc",
            "native/whp/devices/hpet.cc",
            "native/whp/devices/pic.cc",
            "native/whp/devices/pit.cc",
            "native/whp/devices/uart.cc",
            "native/whp/elf_loader.cc",
            "native/whp/irq.cc",
            "native/whp/page_tables.cc",
            "native/whp/virtio/blk.cc",
            "native/whp/virtio/rng.cc",
            "native/whp/win_console_ctrl.cc"
          ],
          "include_dirs": ["native/whp"],
          "defines": ["NOMINMAX", "WIN32_LEAN_AND_MEAN"],
          "libraries": ["winmm.lib", "ws2_32.lib", "iphlpapi.lib", "bcrypt.lib"],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "ExceptionHandling": 1,
              "AdditionalOptions": ["/std:c++17", "/GS", "/guard:cf"]
            },
            "VCLinkerTool": {
              "AdditionalOptions": ["/guard:cf", "/DYNAMICBASE", "/NXCOMPAT"]
            }
          },
          "conditions": [
            ["\"<!(echo %NODE_VMM_HAVE_LIBSLIRP%)\"=='1'", {
              "defines+": ["NODE_VMM_HAVE_LIBSLIRP"],
              "include_dirs": [
                "<!(echo %NODE_VMM_LIBSLIRP_INCLUDE%)"
              ],
              "libraries+": [
                "<!(echo %NODE_VMM_LIBSLIRP_LIB%)/libslirp.lib"
              ]
            }]
          ]
        }],
        ["OS=='mac'", {
          "sources": ["native/hvf/backend.cc"],
          "cflags_cc!": ["-fno-exceptions"],
          "cflags_cc": [
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Wno-missing-field-initializers",
            "-fexceptions",
            "<!@(node scripts/slirp-flags.mjs cflags)"
          ],
          "xcode_settings": {
            "OTHER_CFLAGS": [
              "-mmacosx-version-min=15.0",
              "-std=c++17",
              "-fexceptions",
              "-O2",
              "<!@(node scripts/slirp-flags.mjs cflags)"
            ],
            "MACOSX_DEPLOYMENT_TARGET": "15.0",
            "OTHER_LDFLAGS": [
              "-framework Hypervisor",
              "-framework vmnet",
              "-framework CoreFoundation",
              "<!@(node scripts/slirp-flags.mjs libs)"
            ]
          }
        }]
      ]
    }
  ]
}
