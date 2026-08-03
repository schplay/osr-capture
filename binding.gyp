{
    "targets": [
        {
            "target_name": "osr_readback",
            "sources": ["src/osr_readback.cc"],
            "include_dirs": ["<!@(node -p \"require('node-addon-api').include\")"],
            "defines": ["NAPI_VERSION=8"],
            "conditions": [
                [
                    "OS=='win'",
                    {
                        "sources": ["src/readback_win.cc"],
                        "libraries": ["d3d11.lib", "dxgi.lib"],
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
    ]
}
