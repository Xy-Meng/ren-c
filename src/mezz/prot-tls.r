REBOL [
    Title: "REBOL 3 TLSv1.0 protocol scheme"
    Name: tls
    Type: module
    Author: "Richard 'Cyphre' Smolak"
    Version: 0.6.1
    Todo: {
        -cached sessions
        -automagic cert data lookup
        -add more cipher suites
        -server role support
        -SSL3.0, TLS1.1/1.2 compatibility
        -cert validation
    }
]

; These are the currently supported cipher suites.  (Additional possibilities
; would be DSA, 3DES, ECDH, ECDHE, ECDSA, SHA256, SHA384...)
;
; https://testssl.sh/openssl-rfc.mapping.html
;
; If you want to get a report on what suites a particular site has:
;
; https://www.ssllabs.com/ssltest/analyze.html
;

cipher-suites: [
    ; <key> crypt@ #hash
    ; !!! Using terminal-@ because bootstrap older Rebols can't have leading @

    #{00 04} [
        TLS_RSA_WITH_RC4_128_MD5
        <rsa> rc4@ [size 16] #md5 [size 16]
    ]
    #{00 05} [
        TLS_RSA_WITH_RC4_128_SHA
        <rsa> rc4@ [size 16] #sha1 [size 20]
    ]
    #{00 2F} [
        TLS_RSA_WITH_AES_128_CBC_SHA
        <rsa> aes@ [size 16 block 16 iv 16] #sha1 [size 20]
    ]
    #{00 35} [
        TLS_RSA_WITH_AES_256_CBC_SHA
        <rsa> aes@ [size 32 block 16 iv 16] #sha1 [size 20]
    ]
    #{00 32} [
        TLS_DHE_DSS_WITH_AES_128_CBC_SHA
        <dhe-dss> aes@ [size 16 block 16 iv 16] #sha1 [size 20]
    ]
    #{00 38} [
        TLS_DHE_DSS_WITH_AES_256_CBC_SHA
        <dhe-dss> aes@ [size 32 block 16 iv 16] #sha1 [size 20]
    ]
    #{00 33} [
        TLS_DHE_RSA_WITH_AES_128_CBC_SHA
        <dhe-rsa> aes@ [size 16 block 16 iv 16] #sha1 [size 20]
    ]
    #{00 39} [
        TLS_DHE_RSA_WITH_AES_256_CBC_SHA
        <dhe-rsa> aes@ [size 32 block 16 iv 16] #sha1 [size 20]
    ]
]


;
; SUPPORT FUNCTIONS
;

debug: (comment [:print] blank)

emit: function [
    {Emits binary data, optionally marking positions with SET-WORD!}

    ctx [object!]
    code [block! binary!]
][
    if block? code [
        while-not [tail? code] [
            if set-word? code/1 [
                set code/1 tail ctx/msg ;-- save position
                code: my next
            ] else [
                append ctx/msg ensure binary! do/next code 'code
            ]
        ]
    ] else [
        join ctx/msg code
    ]
]

to-bin: func [
    val [integer!]
    width [integer!]
][
    skip tail of to binary! val negate width
]

make-tls-error: func [
    message [text! block!]
][
    if block? message [message: unspaced message]
    make error! [
        type: 'Access
        id: 'Protocol
        arg1: message
    ]
]


;
; ASN.1 FORMAT PARSER CODE
;
; ASN.1 is similar in purpose and use to protocol buffers and Apache Thrift,
; which are also interface description languages for cross-platform data
; serialization. Like those languages, it has a schema (in ASN.1, called a
; "module"), and a set of encodings, typically type-length-value encodings.
;
; https://en.wikipedia.org/wiki/Abstract_Syntax_Notation_One
;
; The only use of it here is to extract certificates, so it's rather heavy
; handed to do a complete ASN parse:
;
; https://security.stackexchange.com/a/31057
;
; Yet it's a good, short, real-world case to look at through a Rebol lens.

parse-asn: function [
    {Create a legible Rebol-structured BLOCK! from an ASN.1 BINARY! encoding}

    return: [block!]
    data [binary!]

    <static>

    universal-tags ([
        <eoc>
        <boolean>
        <integer>
        <bit-string>
        <octet-string>
        <null>
        <object-identifier>
        <object-descriptor>
        <external>
        <real>
        <enumerated>
        <embedded-pdv>
        <utf8string>
        <relative-oid>
        <undefined>
        <undefined>
        <sequence>
        <set>
        <numeric-string>
        <printable-string>
        <t61-string>
        <videotex-string>
        <ia5-string>
        <utc-time>
        <generalized-time>
        <graphic-string>
        <visible-string>
        <general-string>
        <universal-string>
        <character-string>
        <bmp-string>
    ])

    ; !!! Older Rebols (used for bootstrap) dont' support leading @, but
    ; that is what these should be.  Trailing @ in the meantime.
    ;
    class-types ([universal@ application@ context-specific@ private@])
][
    data-start: data ;-- may not be at head
    index: does [1 + offset-of data-start data] ;-- calculates effective index

    mode: #type
    class: _
    tag: _

    return collect [ for-next data [
        byte: data/1

        switch mode [
            #type [
                constructed: not zero? (byte and+ 32)
                class: pick class-types 1 + shift byte -6

                switch class [
                    universal@ [
                        tag: pick universal-tags 1 + (byte and+ 31)
                    ]
                    context-specific@ [
                        tag: <context-specific>
                        val: byte and+ 31
                    ]
                ]
                mode: #size
            ]

            #size [
                size: byte and+ 127
                if not zero? (byte and+ 128) [ ;-- long form
                    old-size: size
                    size: to-integer/unsigned copy/part next data old-size
                    data: skip data old-size
                ]
                if zero? size [
                    keep/only/line compose/deep [
                        (tag) [
                            (constructed ?? "constructed" !! "primitive")
                            (index)
                            (size)
                            _
                        ]
                    ]
                    mode: #type
                ] else [
                    mode: #value
                ]
            ]

            #value [
                switch class [
                    universal@ [
                        val: copy/part data size
                        keep/only/line compose/deep/only [
                            (tag) [
                                (constructed ?? "constructed" !! "primitive")
                                (index)
                                (size)
                                (either constructed [parse-asn val] [val])
                            ]
                        ]
                    ]

                    context-specific@ [
                        keep/only/line compose/deep [(tag) [(val) (size)]]
                        parse-asn copy/part data size ;-- !!! ensures valid?
                    ]
                ]

                data: skip data size - 1
                mode: #type
            ]
        ]
    ] ]
]


;
; PROTOCOL STATE HANDLING
;

update-state: procedure [
    ctx [object!]
    new [tag! issue!] "new state, ISSUE! is a (potentially) terminal state"
    direction [word!] "READ or WRITE"
    transitions [block!] "maps from state to a BLOCK! of legal next states"
][
    old: ensure [blank! issue! tag!] ctx/mode
    debug [old unspaced ["=" direction "=>"] new]

    if old and (not find (legal: select transitions old) new) [
        fail ["Invalid write state transition, expected one of:" mold legal]
    ]

    ctx/mode: new
]

update-read-state: specialize 'update-state [
    direction: 'read
    transitions: [
        <client-hello> [<server-hello>]
        <server-hello> [<certificate>]
        <certificate> [#server-hello-done <server-key-exchange>]
        <server-key-exchange> [#server-hello-done]
        <finished> [<change-cipher-spec> #alert]
        <change-cipher-spec> [#encrypted-handshake]
        #encrypted-handshake [#application]
        #application [#application #alert]
        #alert []
        <close-notify> [#alert]
    ]
]

update-write-state: specialize 'update-state [
    direction: 'write
    transitions: [
        #server-hello-done [<client-key-exchange>]
        <client-key-exchange> [<change-cipher-spec>]
        <change-cipher-spec> [<finished>]
        #encrypted-handshake [#application]
        #application [#application #alert]
        #alert [<close-notify>]
        <close-notify> []
    ]
]


;
; TLS PROTOCOL CODE
;

client-hello: procedure [
    ctx [object!]
][
    clear ctx/handshake-messages

    ; generate client random struct
    ;
    ctx/client-random: to-bin to-integer difference now/precise 1-Jan-1970 4
    random/seed now/time/precise
    loop 28 [append ctx/client-random (random/secure 256) - 1]

    cs-data: join-all map-each item cipher-suites [
        if binary? item [item]
    ]

    emit ctx [
        #{16}                       ; protocol type (22=Handshake)
        ctx/version                 ; protocol version (3|1 = TLS1.0)

        ssl-record-length:
        #{00 00}                    ; length of SSL record data

        ssl-record:
        #{01}                       ; protocol message type (1=ClientHello)

        message-length:
        #{00 00 00}                 ; protocol message length (updated after)

        message:
        ctx/version                 ; max supported version by client (TLS1.0)
        ctx/client-random           ; 4 bytes gmt unix time + 28 random bytes
        #{00}                       ; session ID length
        to-bin length of cs-data 2  ; cipher suites length
        cs-data                     ; cipher suites list
        #{01}                       ; compression method length
        #{00}                       ; no compression
    ]

    ; update the embedded lengths to correct values
    ;
    change ssl-record-length (to-bin (length of ssl-record) 2)
    change message-length (to-bin (length of message) 3)
    
    append ctx/handshake-messages ssl-record
]


client-key-exchange: procedure [
    ctx [object!]
][
    switch ctx/key-method [
        <rsa> [
            ; generate pre-master-secret
            ctx/pre-master-secret: copy ctx/version
            random/seed now/time/precise
            loop 46 [append ctx/pre-master-secret (random/secure 256) - 1]

            ; encrypt pre-master-secret
            rsa-key: rsa-make-key
            rsa-key/e: ctx/pub-exp
            rsa-key/n: ctx/pub-key

            ; supply encrypted pre-master-secret to server
            key-data: rsa ctx/pre-master-secret rsa-key
        ]

        <dhe-dss>
        <dhe-rsa> [
            ; generate public/private keypair
            dh-generate-key ctx/dh-key

            ; supply the client's public key to server
            key-data: ctx/dh-key/pub-key

            ; generate pre-master-secret
            ctx/pre-master-secret: dh-compute-key ctx/dh-key ctx/dh-pub
        ]
    ]

    emit ctx [
        #{16}                       ; protocol type (22=Handshake)
        ctx/version                 ; protocol version (3|1 = TLS1.0)

        ssl-record-length:
        #{00 00}                    ; length of SSL record data

        ssl-record:
        #{10}                       ; message type (16=ClientKeyExchange)

        message-length:
        #{00 00 00}                 ; protocol message length

        message:
        to-bin length of key-data 2 ; length of the key (2 bytes)
        key-data
    ]

    ; update the embedded lengths to correct values
    ;
    change ssl-record-length (to-bin (length of ssl-record) 2)
    change message-length (to-bin (length of message) 3)

    ; make all secure data
    ;
    make-master-secret ctx ctx/pre-master-secret

    make-key-block ctx

    ; update keys
    ctx/client-mac-key: copy/part ctx/key-block ctx/hash-size
    ctx/server-mac-key: copy/part skip ctx/key-block ctx/hash-size ctx/hash-size
    ctx/client-crypt-key: copy/part skip ctx/key-block 2 * ctx/hash-size ctx/crypt-size
    ctx/server-crypt-key: copy/part skip ctx/key-block (2 * ctx/hash-size) + ctx/crypt-size ctx/crypt-size

    if ctx/block-size [
        ctx/client-iv: copy/part skip ctx/key-block 2 * (ctx/hash-size + ctx/crypt-size) ctx/block-size
        ctx/server-iv: copy/part skip ctx/key-block (2 * (ctx/hash-size + ctx/crypt-size)) + ctx/block-size ctx/block-size
    ]

    append ctx/handshake-messages ssl-record
]


change-cipher-spec: procedure [
    ctx [object!]
][
    emit ctx [
        #{14}           ; protocol type (20=ChangeCipherSpec)
        ctx/version     ; protocol version (3|1 = TLS1.0)
        #{00 01}        ; length of SSL record data
        #{01}           ; CCS protocol type
    ]
]


encrypted-handshake-msg: procedure [
    ctx [object!]
    message [binary!]
][
    plain-msg: message
    message: encrypt-data/type ctx message #{16}
    emit ctx [
        #{16}                       ; protocol type (22=Handshake)
        ctx/version                 ; protocol version (3|1 = TLS1.0)
        to-bin length of message 2  ; length of SSL record data
        message
    ]
    append ctx/handshake-messages plain-msg
]


application-data: procedure [
    ctx [object!]
    message [binary! text!]
][
    message: encrypt-data ctx to binary! message
    emit ctx [
        #{17}                       ; protocol type (23=Application)
        ctx/version                 ; protocol version (3|1 = TLS1.0)
        to-bin length of message 2  ; length of SSL record data
        message
    ]
]


alert-close-notify: function [
    ctx [object!]
][
    message: encrypt-data ctx #{0100} ; close notify
    emit ctx [
        #{15}                       ; protocol type (21=Alert)
        ctx/version                 ; protocol version (3|1 = TLS1.0)
        to-bin length of message 2  ; length of SSL record data
        message
    ]
]


finished: function [
    return: [binary!]
    ctx [object!]
][
    ctx/seq-num-w: 0
    who-finished: ctx/server? ?? "server finished" !! "client finished"

    return join-all [
        #{14}       ; protocol message type (20=Finished)
        #{00 00 0c} ; protocol message length (12 bytes)

        prf ctx/master-secret who-finished join-all [
            checksum/method ctx/handshake-messages 'md5
            checksum/method ctx/handshake-messages 'sha1
        ] 12
    ]
]


encrypt-data: function [
    return: [binary!]
    ctx [object!]
    data [binary!]
    /type
    msg-type [binary!] "application data is default"
][
    data: join-all [
        data
        ; MAC code
        mac: checksum/method/key join-all [
            to-bin ctx/seq-num-w 8              ; sequence number (64-bit int)
            if type [msg-type] !! #{17}         ; msg type
            ctx/version                         ; version
            to-bin length of data 2             ; msg content length
            data                                ; msg content
        ] (to word! ctx/hash-method) ctx/client-mac-key
    ]

    if ctx/block-size [
        ; add the padding data in CBC mode
        padding: ctx/block-size - (
            remainder (1 + (length of data)) ctx/block-size
        )
        len: 1 + padding
        append data head of insert/dup make binary! len to-bin padding 1 len
    ]

    switch ctx/crypt-method [
        rc4@ [
            ctx/encrypt-stream: default [rc4/key ctx/client-crypt-key]
            rc4/stream ctx/encrypt-stream data
        ]
        aes@ [
            ctx/encrypt-stream: default [
                aes/key ctx/client-crypt-key ctx/client-iv
            ]
            data: aes/stream ctx/encrypt-stream data
        ]
    ] else [
        fail ["Unsupported TLS crypt-method:" ctx/crypt-method]
    ]

    return data
]


decrypt-data: function [
    return: [binary!]
    ctx [object!]
    data [binary!]
][
    switch ctx/crypt-method [
        rc4@ [
            ctx/decrypt-stream: default [rc4/key ctx/server-crypt-key]
            rc4/stream ctx/decrypt-stream data
        ]
        aes@ [
            ctx/decrypt-stream: default [
                aes/key/decrypt ctx/server-crypt-key ctx/server-iv
            ]
            data: aes/stream ctx/decrypt-stream data
        ]
    ] else [
        fail ["Unsupported TLS crypt-method:" ctx/crypt-method]
    ]

    return data
]


parse-protocol: function [
    return: [object!]
    data [binary!]

    <static>

    protocol-types ([
        20 <change-cipher-spec>
        21 #alert
        22 #handshake
        23 #application
    ])
][
    return make object! [
        type: select protocol-types data/1 else [
            fail ["unknown/invalid protocol type:" data/1]
        ]
        version: pick [ssl-v3 tls-v1.0 tls-v1.1 tls-v1.2 tls-v1.3] data/3 + 1
        size: to-integer/unsigned copy/part at data 4 2
        messages: copy/part at data 6 size
    ]
]


parse-messages: function [
    ctx [object!]
    proto [object!]

    <static>

    message-types ([
        0 #hello-request
        1 <client-hello>
        2 <server-hello>
        11 <certificate>
        12 <server-key-exchange>
        13 certificate-request@ ;-- not yet implemented
        14 #server-hello-done
        15 certificate-verify@ ;-- note yet implemented
        16 <client-key-exchange>
        20 <finished>
    ])

    alert-descriptions ([
        0 "Close notify"
        10 "Unexpected message"
        20 "Bad record MAC"
        21 "Decryption failed"
        22 "Record overflow"
        30 "Decompression failure"
        40 "Handshake failure - no supported cipher suite available on server"
        41 "No certificate"
        42 "Bad certificate"
        43 "Unsupported certificate"
        44 "Certificate revoked"
        45 "Certificate expired"
        46 "Certificate unknown"
        47 "Illegal parameter"
        48 "Unknown CA"
        49 "Access denied"
        50 "Decode error"
        51 "Decrypt error"
        60 "Export restriction"
        70 "Protocol version"
        71 "Insufficient security"
        80 "Internal error"
        90 "User cancelled"
       100 "No renegotiation"
       110 "Unsupported extension"
    ])

    ; The structure has a field called LENGTH, so when an ACTION! is used
    ; that field is picked up.
    ;
    <with> length
][
    result: make block! 8
    data: proto/messages

    if ctx/encrypted? [
        change data decrypt-data ctx data
        debug ["decrypting..."]
        if ctx/block-size [
            ; deal with padding in CBC mode
            data: copy/part data (
                ((length of data) - 1) - (to-integer/unsigned last data)
            )
            debug ["depadding..."]
        ]
        debug ["data:" data]
    ]
    debug [ctx/seq-num-r ctx/seq-num-w "READ <--" proto/type]

    if proto/type <> #handshake [
        if proto/type = #alert [
            if proto/messages/1 > 1 [
                ; fatal alert level
                fail [select alert-descriptions data/2 !! "unknown"]
            ]
        ]
        update-read-state ctx proto/type
    ]

    switch proto/type [
        #alert [
            append result reduce [
                context [
                    level: pick [warning fatal] data/1 !! 'unknown
                    description: select alert-descriptions data/2 !! "unknown"
                ]
            ]
        ]

        #handshake [
            while-not [tail? data] [
                msg-type: try select message-types data/1

                update-read-state ctx (
                    ctx/encrypted? ?? #encrypted-handshake !! msg-type
                )

                len: to-integer/unsigned copy/part at data 2 3
                append result switch msg-type [
                    <server-hello> [
                        msg-content: copy/part at data 7 len

                        msg-obj: context [
                            type: msg-type
                            version: pick [ssl-v3 tls-v1.0 tls-v1.1] data/6 + 1
                            length: len
                            server-random: copy/part msg-content 32
                            session-id: copy/part at msg-content 34 msg-content/33
                            suite-id: copy/part at msg-content 34 + msg-content/33 2
                            compression-method-length: first at msg-content 36 + msg-content/33
                            compression-method:
                                either compression-method-length = 0 [blank] [
                                    copy/part
                                        at msg-content 37 + msg-content/33
                                        compression-method-length
                                ]
                        ]

                        ctx/suite: select cipher-suites msg-obj/suite-id else [
                            fail [
                                "This TLS scheme doesn't support ciphersuite:"
                                (mold suite)
                            ]
                        ]

                        ctx/server-random: msg-obj/server-random
                        msg-obj
                    ]

                    <certificate> [
                        msg-content: copy/part at data 5 len
                        msg-obj: context [
                            type: msg-type
                            length: len
                            certificates-length: to-integer/unsigned copy/part msg-content 3
                            certificate-list: make block! 4
                            while-not [tail? msg-content] [
                                if 0 < clen: to-integer/unsigned copy/part skip msg-content 3 3 [
                                    append certificate-list copy/part at msg-content 7 clen
                                ]
                                msg-content: skip msg-content 3 + clen
                            ]
                        ]
                        ; no cert validation - just set it to be used
                        ctx/certificate: parse-asn msg-obj/certificate-list/1

                        switch ctx/key-method [
                            <rsa> [
                                ; get the public key and exponent (hardcoded for now)
                                ctx/pub-key: parse-asn (next
                                    comment [ctx/certificate/1/<sequence>/4/1/<sequence>/4/6/<sequence>/4/2/<bit-string>/4]
                                    ctx/certificate/1/<sequence>/4/1/<sequence>/4/7/<sequence>/4/2/<bit-string>/4
                                )
                                ctx/pub-exp: ctx/pub-key/1/<sequence>/4/2/<integer>/4
                                ctx/pub-key: next ctx/pub-key/1/<sequence>/4/1/<integer>/4
                            ]
                            <dhe-dss>
                            <dhe-rsa> [
                                ; for DH cipher suites the certificate is used
                                ; just for signing the key exchange data
                            ]
                        ] else [
                            fail "Unsupported TLS key-method"
                        ]
                        msg-obj
                    ]

                    <server-key-exchange> [
                        switch ctx/key-method [
                            'dhe-dss 'dhe-rsa [
                                msg-content: copy/part at data 5 len
                                msg-obj: context [
                                    type: msg-type
                                    length: len
                                    p-length: to-integer/unsigned copy/part msg-content 2
                                    p: copy/part at msg-content 3 p-length
                                    g-length: to-integer/unsigned copy/part at msg-content 3 + p-length 2
                                    g: copy/part at msg-content 3 + p-length + 2 g-length
                                    ys-length: to-integer/unsigned copy/part at msg-content 3 + p-length + 2 + g-length 2
                                    ys: copy/part at msg-content 3 + p-length + 2 + g-length + 2 ys-length
                                    signature-length: to-integer/unsigned copy/part at msg-content 3 + p-length + 2 + g-length + 2 + ys-length 2
                                    signature: copy/part at msg-content 3 + p-length + 2 + g-length + 2 + ys-length + 2 signature-length
                                ]

                                ctx/dh-key: dh-make-key
                                ctx/dh-key/p: msg-obj/p
                                ctx/dh-key/g: msg-obj/g
                                ctx/dh-pub: msg-obj/ys

                                ; TODO: the signature sent by server should be verified using DSA or RSA algorithm to be sure the dh-key params are safe
                                msg-obj
                            ]
                        ] else [
                            fail "Server-key-exchange message sent illegally."
                        ]
                    ]

                    #server-hello-done [
                        context [
                            type: msg-type
                            length: len
                        ]
                    ]

                    <client-hello> [
                        msg-content: copy/part at data 7 len
                        context [
                            type: msg-type
                            version: pick [ssl-v3 tls-v1.0 tls-v1.1] data/6 + 1
                            length: len
                            content: msg-content
                        ]
                    ]

                    <finished> [
                        ctx/seq-num-r: 0
                        msg-content: copy/part at data 5 len
                        who-finished: either ctx/server? [
                            "client finished"
                        ][
                            "server finished"
                        ]
                        if (msg-content <>
                            prf ctx/master-secret who-finished join-all [
                                checksum/method
                                ctx/handshake-messages 'md5
                                checksum/method ctx/handshake-messages 'sha1
                            ] 12
                        )[
                            fail "Bad 'finished' MAC"
                        ]

                        debug "FINISHED MAC verify: OK"

                        context [
                            type: msg-type
                            length: len
                            content: msg-content
                        ]
                    ]
                ]

                append ctx/handshake-messages copy/part data len + 4

                skip-amount: either ctx/encrypted? [
                    mac: copy/part skip data len + 4 ctx/hash-size

                    mac-check: checksum/method/key join-all [
                        to-bin ctx/seq-num-r 8  ; 64-bit sequence number
                        #{16}                   ; msg type
                        ctx/version             ; version
                        to-bin len + 4 2        ; msg content length
                        copy/part data len + 4
                    ] (to word! ctx/hash-method) ctx/server-mac-key

                    if mac <> mac-check [
                        fail "Bad handshake record MAC"
                    ]

                    4 + ctx/hash-size
                ][
                    4
                ]

                data: skip data (len + skip-amount)
            ]
        ]

        <change-cipher-spec> [
            ctx/encrypted?: true
            append result context [
                type: 'ccs-message-type
            ]
        ]

        #application [
            append result msg-obj: context [
                type: 'app-data
                content: copy/part data (length of data) - ctx/hash-size
            ]
            len: length of msg-obj/content
            mac: copy/part skip data len ctx/hash-size
            mac-check: checksum/method/key join-all [
                to-bin ctx/seq-num-r 8  ; sequence number (64-bit int in R3)
                #{17}                   ; msg type
                ctx/version             ; version
                to-bin len 2            ; msg content length
                msg-obj/content         ; content
            ] (to word! ctx/hash-method) ctx/server-mac-key

            if mac <> mac-check [
                fail "Bad application record MAC"
            ]
        ]
    ]

    ctx/seq-num-r: ctx/seq-num-r + 1
    return result
]


parse-response: function [
    return: [object!]
    ctx [object!]
    msg [binary!]
][
    proto: parse-protocol msg
    messages: parse-messages ctx proto

    if empty? messages [
        fail "unknown/invalid protocol message"
    ]

    proto/messages: messages

    debug [
        "processed protocol type:" proto/type
        "messages:" length of proto/messages
    ]

    if not tail? skip msg proto/size + 5 [
        fail "invalid length of response fragment"
    ]

    return proto
]


prf: function [
    {(P)suedo-(R)andom (F)unction, generates arbitrarily long random binaries}

    return: [binary!]
    secret [binary!]
    label [text! binary!]
    seed [binary!]
    output-length [integer!]
][
    len: length of secret
    mid: to integer! (.5 * (len + either odd? len [1] [0]))

    s-1: copy/part secret mid
    s-2: copy at secret mid + either odd? len [0] [1]

    seed: join-all [#{} label seed]

    p-md5: copy #{}
    a: seed ; A(0)
    while [output-length > length of p-md5] [
        a: checksum/method/key a 'md5 s-1 ; A(n)
        append p-md5 checksum/method/key join-all [a seed] 'md5 s-1

    ]

    p-sha1: copy #{}
    a: seed ; A(0)
    while [output-length > length of p-sha1] [
        a: checksum/method/key a 'sha1 s-2 ; A(n)
        append p-sha1 checksum/method/key join-all [a seed] 'sha1 s-2
    ]
    return ((copy/part p-md5 output-length) xor+ copy/part p-sha1 output-length)
]


make-key-block: function [
    return: [binary!]
    ctx [object!]
][
    ctx/key-block: prf
        ctx/master-secret
        "key expansion"
        join-all [ctx/server-random ctx/client-random]
        (
            (ctx/hash-size + ctx/crypt-size)
            + (either ctx/block-size [ctx/iv-size] [0])
        ) * 2
]


make-master-secret: function [
    return: [binary!]
    ctx [object!]
    pre-master-secret [binary!]
][
    ctx/master-secret: prf
        pre-master-secret
        "master secret"
        join-all [ctx/client-random ctx/server-random]
        48
]


do-commands: procedure [
    ctx [object!]
    commands [block!]
    /no-wait
][
    clear ctx/msg
    parse commands [
        some [
            set cmd: [
                <client-hello> (
                    client-hello ctx
                )
                | <client-key-exchange> (
                    client-key-exchange ctx
                )
                | <change-cipher-spec> (
                    change-cipher-spec ctx
                )
                | <finished> (
                    encrypted-handshake-msg ctx finished ctx
                )
                | #application set arg: [text! | binary!] (
                    application-data ctx arg
                )
                | <close-notify> (
                    alert-close-notify ctx
                )
            ] (
                debug [ctx/seq-num-r ctx/seq-num-w "WRITE -->" cmd]
                ctx/seq-num-w: ctx/seq-num-w + 1
                update-write-state ctx cmd
            )
        ]
    ]
    debug ["writing bytes:" length of ctx/msg]
    ctx/resp: copy []
    write ctx/connection ctx/msg

    any [
        no-wait
        port? wait [ctx/connection 30]
    ] or [
        fail "port timeout"
    ]
]


;
; TLS SCHEME
;


tls-init: procedure [
    ctx [object!]
][
    ctx/seq-num-r: 0
    ctx/seq-num-w: 0
    ctx/mode: _
    ctx/encrypted?: false

    if not ctx/suite [
        ;-- Seems to always be blank?
    ] else [
        print "** Tell @HostileFork if you see this, ever **"
        wait 5
        switch ctx/crypt-method [
            rc4@ [
                ctx/encrypt-stream: default [
                    rc4/stream ctx/encrypt-stream blank
                ]
                ctx/decrypt-stream: default [
                    rc4/stream ctx/decrypt-stream blank
                ]
            ]

            aes@ [
                ;-- nothing was here
            ]
        ] else [
            fail ["Unsupported TLS crypt-method" ctx/crypt-method]
        ]
    ]
]


tls-read-data: function [
    return: [logic!]
    ctx [object!]
    port-data [binary!]
][
    debug ["tls-read-data:" length of port-data "bytes"]
    data: append ctx/data-buffer port-data
    clear port-data

    ; !!! Why is this making a copy (5 = length of copy...) when just trying
    ; to test a size?
    ;
    while [5 = length of copy/part data 5] [
        len: 5 + to-integer/unsigned copy/part at data 4 2

        debug ["reading bytes:" len]

        fragment: copy/part data len

        if len > length of fragment [
            debug [
                "incomplete fragment:"
                "read" length of fragment "of" len "bytes"
            ]
            break
        ]

        debug ["received bytes:" length of fragment | "parsing response..."]

        append ctx/resp parse-response ctx fragment

        data: skip data len

        if tail? data and (issue? ctx/mode) [
            debug [
                "READING FINISHED"
                length of head of ctx/data-buffer
                index of data
                same? tail of ctx/data-buffer data
            ]
            clear ctx/data-buffer
            return true
        ]
    ]

    debug ["CONTINUE READING..."]
    clear change ctx/data-buffer data
    return false
]


tls-awake: function [
    return: [logic!]
    event [event!]
][
    debug ["TLS Awake-event:" event/type]
    port: event/port
    tls-port: port/locals
    tls-awake: :tls-port/awake

    all [
        tls-port/state/mode = #application
        not port/data
    ] then [
        ; reset the data field when interleaving port r/w states
        tls-port/data: _
    ]

    switch event/type [
        'lookup [
            open port
            tls-init tls-port/state
            insert system/ports/system make event! [
                type: 'lookup
                port: tls-port
            ]
            return false
        ]

        'connect [
            do-commands tls-port/state [<client-hello>]

            if tls-port/state/resp/1/type = #handshake [
                do-commands tls-port/state [
                    <client-key-exchange>
                    <change-cipher-spec>
                    <finished>
                ]
            ]
            insert system/ports/system make event! [
                type: 'connect
                port: tls-port
            ]
            return false
        ]

        'wrote [
            switch tls-port/state/mode [
                <close-notify> [
                    return true
                ]
                #application [
                    insert system/ports/system make event! [
                        type: 'wrote
                        port: tls-port
                    ]
                    return false
                ]
            ]
            read port
            return false
        ]

        'read [
            debug [
                "Read" length of port/data
                "bytes in mode:" tls-port/state/mode
            ]

            complete?: tls-read-data tls-port/state port/data
            application?: false

            for-each proto tls-port/state/resp [
                switch proto/type [
                    #application [
                        for-each msg proto/messages [
                            if msg/type = 'app-data [
                                tls-port/data: default [
                                    clear tls-port/state/port-data
                                ]
                                append tls-port/data msg/content
                                application?: true
                                msg/type: _
                            ]
                        ]
                    ]
                    #alert [
                        for-each msg proto/messages [
                            if msg/description = "Close notify" [
                                do-commands tls-port/state [<close-notify>]
                                insert system/ports/system make event! [
                                    type: 'read
                                    port: tls-port
                                ]
                                return true
                            ]
                        ]
                    ]
                ]
            ]

            debug ["data complete?:" complete? "application?:" application?]

            if application? [
                insert system/ports/system make event! [
                    type: 'read
                    port: tls-port
                ]
            ] else [
                read port
            ]
            return complete?
        ]

        'close [
            insert system/ports/system make event! [
                type: 'close
                port: tls-port
            ]
            return true
        ]
    ]

    close port
    fail ["Unexpected TLS event:" (event/type)]
]


sys/make-scheme [
    name: 'tls
    title: "TLS protocol v1.0"
    spec: construct system/standard/port-spec-net []
    actor: [
        read: func [
            port [port!]
            <local>
                resp data msg
        ][
            debug ["READ" open? port/state/connection]
            read port/state/connection
            return port
        ]

        write: func [port [port!] value [<opt> any-value!]] [
            if find [#encrypted-handshake #application] port/state/mode [
                do-commands/no-wait port/state compose [
                    #application (value)
                ]
                return port
            ]
            return blank
        ]

        open: func [port [port!] <local> conn] [
            if port/state [return port]

            if not port/spec/host [
                fail make-tls-error "Missing host address"
            ]

            port/state: context [
                data-buffer: make binary! 32000
                port-data: make binary! 32000
                resp: _

                version: #{03 01} ; protocol version used

                server?: false

                mode: _

                suite: _

                cipher-suite: does [first find suite word!]

                key-method: does [first find suite tag!]

                hash-method: does [first find suite issue!]
                hash-size: does [
                    select (ensure block! second find suite issue!) 'size
                ]

                crypt-method: does [first find suite email!]
                crypt-size: does [
                    select (ensure block! second find suite email!) 'size
                ]
                block-size: does [
                    try select (ensure block! second find suite email!) 'block
                ]
                iv-size: does [
                    try select (ensure block! second find suite email!) 'iv
                ]

                client-crypt-key: _
                client-mac-key: _
                client-iv: _
                server-crypt-key: _
                server-mac-key: _
                server-iv: _

                seq-num-r: 0
                seq-num-w: 0

                msg: make binary! 4096

                ; all messages from Handshake records except "HelloRequest"
                ;
                handshake-messages: make binary! 4096

                encrypted?: false

                client-random: _
                server-random: _
                pre-master-secret: _
                master-secret: _

                key-block: _

                certificate: _
                pub-key: _
                pub-exp: _

                dh-key: _
                dh-pub: _

                encrypt-stream: _
                decrypt-stream: _

                connection: _
            ]

            port/state/connection: conn: make port! [
                scheme: 'tcp
                host: port/spec/host
                port-id: port/spec/port-id
                ref: join-all [tcp:// host ":" port-id]
            ]

            port/data: port/state/port-data

            conn/awake: :tls-awake
            conn/locals: port
            open conn
            port
        ]

        reflect: func [port [port!] property [word!]] [
            switch property [
                'open? [
                    port/state and (open? port/state/connection)
                ]

                'length [
                    ; actor is not an object!, so this isn't a recursive call
                    ;
                    either port/data [length of port/data] [0]
                ]
            ]
        ]

        close: func [port [port!] <local> ctx] [
            if not port/state [return port]

            close port/state/connection

            ; The symmetric ciphers used by TLS are able to encrypt chunks of
            ; data one at a time.  It keeps the progressive state of the
            ; encryption process in the -stream variables, which under the
            ; hood are memory-allocated items stored as a HANDLE!.
            ;
            ; Calling the encryption functions with BLANK! as the data to
            ; input will assume you are done, and will free the handle.
            ;
            ; !!! Is there a good reason for not doing this with an ordinary
            ; OBJECT! containing a BINARY! ?
            ;
            if port/state/suite [
                switch port/state/crypt-method [
                    rc4@ [
                        if port/state/encrypt-stream [
                            port/state/encrypt-stream: _ ;-- will be GC'd
                        ]
                        if port/state/decrypt-stream [
                            port/state/decrypt-stream: _ ;-- will be GC'd
                        ]
                    ]
                    aes@ [
                        if port/state/encrypt-stream [
                            port/state/encrypt-stream: _ ;-- will be GC'd
                        ]
                        if port/state/decrypt-stream [
                            port/state/decrypt-stream: _ ;-- will be GC'd
                        ]
                    ]
                ] else [
                    fail ["Unknown TLS crypt-method" port/state/crypt-method]
                ]
            ]

            debug "TLS/TCP port closed"
            port/state/connection/awake: blank
            port/state: blank
            port
        ]

        copy: func [port [port!]] [
            if port/data [copy port/data]
        ]

        query: func [port [port!]] [
            all [port/state query port/state/connection]
        ]
    ]
]
