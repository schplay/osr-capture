{
    "targets": [
        {
            "target_name": "osr_readback",
            "sources": ["src/osr_readback.cc"],
            "include_dirs": ["<!@(node -p \"require('node-addon-api').include\")"],
            "cflags_cc": ["/std:c++17"],
            "defines": ["NAPI_VERSION=8"],
            "conditions": [
                [
                    "OS=='win'",
                    {
                        "libraries": ["d3d11.lib", "dxgi.lib"],
                        "msvs_settings": {
                            "VCCLCompilerTool": {
                                "ExceptionHandling": 1,
                                "AdditionalOptions": ["/std:c++17"]
                            }
                        }
                    }
                ]
            ]
        }
    ]
}
