{
    "targets": [
        {
            "target_name": "osr_readback",
            "sources": ["src/osr_readback.cc", "src/convert.cc"],
            "include_dirs": ["<!@(node -p \"require('node-addon-api').include\")"],
            "defines": ["NAPI_VERSION=8"],
            "conditions": [
                [
                    "OS=='win'",
                    {
                        "sources": ["src/readback_win.cc"],
                        "libraries": ["d3d11.lib", "d3d12.lib", "dxgi.lib", "d3dcompiler.lib"],
                        "msvs_settings": {
                            "VCCLCompilerTool": {
                                "ExceptionHandling": 1,
                                "AdditionalOptions": ["/std:c++17"]
                            }
                        }
                    }
                ],
                [
                    "OS=='mac'",
                    {
                        "sources": ["src/readback_mac.mm"],
                        "xcode_settings": {
                            "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
                            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
                            "OTHER_LDFLAGS": ["-framework IOSurface", "-framework CoreFoundation"]
                        }
                    }
                ],
                [
                    "OS=='linux'",
                    {
                        "sources": ["src/readback_linux.cc"],
                        "cflags_cc": ["-std=c++17", "-fexceptions"]
                    }
                ]
            ]
        }
    ],
    "conditions": [
        [
            "OS=='win'",
            {
                "targets": [
                    {
                        # Stage-2 sub-step A harness (plan §10): standalone exe, no Electron/node at
                        # runtime. #includes src/readback_win.cc so the convert code under test is the
                        # addon's own, unforked. Built by the same `node-gyp rebuild` as the addon;
                        # output: build/Release/on12_harness.exe. See test/on12_harness.cc header.
                        "target_name": "on12_harness",
                        "type": "executable",
                        "win_delay_load_hook": "false",
                        "sources": ["test/on12_harness.cc"],
                        "defines": ["NOMINMAX"],
                        "libraries": ["d3d11.lib", "d3d12.lib", "dxgi.lib", "d3dcompiler.lib", "winmm.lib"],
                        "msvs_settings": {
                            "VCCLCompilerTool": {
                                "ExceptionHandling": 1,
                                "AdditionalOptions": ["/std:c++17"]
                            },
                            "VCLinkerTool": {
                                "SubSystem": 1
                            }
                        }
                    }
                ]
            }
        ]
    ]
}
