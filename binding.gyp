{
    "targets": [
        {
            "target_name": "gi",
            "sources": [
                "src/gi.cc",
            ],
            "cflags": [
                "<!@(pkg-config --cflags gobject-introspection-1.0) -Wall -Werror",
            ],
            "ldflags": [
                "<!@(pkg-config --libs gobject-introspection-1.0)",
            ],
        }
    ]
}
