REBOL []

name: 'FFI
source: %ffi/ext-ffi.c
includes: cfg-ffi/includes
cflags: cfg-ffi/cflags
definitions: cfg-ffi/definitions
init: %ffi/ext-ffi-init.reb
modules: [
    [
        name: 'FFI
        source: %ffi/mod-ffi.c
        depends: [
            %ffi/t-struct.c
            %ffi/t-routine.c
        ]
        includes: join-of [%prep/extensions/ffi] opt cfg-ffi/includes
        definitions: cfg-ffi/definitions
        cflags: cfg-ffi/cflags
        searches: cfg-ffi/searches
        ldflags: cfg-ffi/ldflags
        libraries: cfg-ffi/libraries
        ; Currently the libraries are specified by the USER-CONFIG/WITH-FFI
        ; until that logic is moved to something here.  So if you are going
        ; to build the FFI module, you need to also set WITH-FFI (though
        ; setting WITH-FFI alone will not get you the module)
    ]
]

options: [
    with-ffi [block! word! logic! blank!] (
        cfg-ffi: make object! [
            cflags: [
                ; ffi_closure has an alignment specifier, which causes
                ; padding, and MSVC warns about that.
                ;
                <msc:/wd4324>
            ]
            includes: _
            definitions: _
            ldflags: _
            libraries: _
            searches: _
        ]
        either block? user-config/with-ffi [
            cfg-ffi: make cfg-ffi user-config/with-ffi
            cfg-ffi/libraries: map-each lib cfg-ffi/libraries [
                case [
                    file? lib [
                        make rebmake/ext-dynamic-class [
                            output: lib
                        ]
                    ]
                    all [
                            object? lib
                            find [ext-dynamic-class ext-static-class] lib/class-name
                    ][
                        lib
                    ]
                    true [
                        fail ["Libraries can only be file! or static/dynamic library object, not" lib]
                    ]
                ]
            ]
        ][
            switch/default user-config/with-ffi [
                'static 'dynamic [
                    for-each var [includes cflags searches ldflags][
                        x: rebmake/pkg-config
                            any [user-config/pkg-config {pkg-config}]
                            var
                            %libffi
                        if not empty? x [
                            set (in cfg-ffi var) x
                        ]
                    ]

                    libs: rebmake/pkg-config
                        any [user-config/pkg-config {pkg-config}]
                        'libraries
                        %libffi

                    cfg-ffi/libraries: map-each lib libs [
                        make rebmake/ext-dynamic-class [
                            output: lib
                            flags: either user-config/with-ffi = 'static [[static]][_]
                        ]
                    ]
                ]
                _ 'no 'off 'false #[false] [
                    ;pass
                ]
            ][
                fail [
                    "WITH-FFI should be one of [dynamic static no]"
                    "not" (user-config/with-ffi)
                ]
            ]
        ]
    )
]
