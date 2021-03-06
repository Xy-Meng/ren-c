REBOL [
    System: "REBOL [R3] Language Interpreter and Run-time Environment"
    Title: "REBOL 3 HTTP protocol scheme"
    Rights: {
        Copyright 2012 REBOL Technologies
        REBOL is a trademark of REBOL Technologies
    }
    License: {
        Licensed under the Apache License, Version 2.0
        See: http://www.apache.org/licenses/LICENSE-2.0
    }
    Name: http
    Type: module
    File: %prot-http.r
    Version: 0.1.48
    Purpose: {
        This program defines the HTTP protocol scheme for REBOL 3.
    }
    Author: ["Gabriele Santilli" "Richard Smolak"]
    Date: 26-Nov-2012
    History: [
        8-Oct-2015 {Modified by @GrahamChiu to return an error object with
        the info object when manual redirect required}
    ]
]

digit: charset [#"0" - #"9"]
alpha: charset [#"a" - #"z" #"A" - #"Z"]
idate-to-date: function [date [text!]] [
    either parse date [
        5 skip
        copy day: 2 digit
        space
        copy month: 3 alpha
        space
        copy year: 4 digit
        space
        copy time: to space
        space
        copy zone: to end
    ][
        if zone = "GMT" [zone: copy "+0"]
        to date! unspaced [day "-" month "-" year "/" time zone]
    ][
        blank
    ]
]

sync-op: function [port body] [
    if not port/state [
        open port
        port/state/close?: yes
    ]

    state: port/state
    state/awake: :read-sync-awake

    do body

    if state/state = 'ready [do-request port]

    ; Wait in a WHILE loop so the timeout cannot occur during 'reading-data
    ; state.  The timeout should be triggered only when the response from
    ; the other side exceeds the timeout value.
    ;
    while-not [find [ready close] state/state] [
        if not port? wait [state/connection port/spec/timeout] [
            fail make-http-error "Timeout"
        ]
        if state/state = 'reading-data [
            read state/connection
        ]
    ]

    body: copy port

    if state/close? [close port]

    either port/spec/debug [
        state/connection/locals
    ][
        body
    ]
]

read-sync-awake: function [event [event!]] [
    false unless switch event/type [
        'connect
        'ready [
            do-request event/port
            false
        ]
        'done [
            true
        ]
        'close [
            true
        ]
        'error [
            error: event/port/state/error
            event/port/state/error: _
            fail error
        ]
    ]
]

http-awake: function [event] [
    port: event/port
    http-port: port/locals
    state: http-port/state
    if action? :http-port/awake [state/awake: :http-port/awake]
    awake: :state/awake

    true unless switch event/type [
        'read [
            awake make event! [type: 'read port: http-port]
            check-response http-port
        ]
        'wrote [
            awake make event! [type: 'wrote port: http-port]
            state/state: 'reading-headers
            read port
            false
        ]
        'lookup [open port false]
        'connect [
            state/state: 'ready
            awake make event! [type: 'connect port: http-port]
        ]
        'close [
            res: try switch state/state [
                'ready [
                    awake make event! [type: 'close port: http-port]
                ]
                'doing-request 'reading-headers [
                    state/error: make-http-error "Server closed connection"
                    awake make event! [type: 'error port: http-port]
                ]
                'reading-data [
                    either any [
                        integer? state/info/headers/content-length
                        state/info/headers/transfer-encoding = "chunked"
                    ][
                        state/error: make-http-error "Server closed connection"
                        awake make event! [type: 'error port: http-port]
                    ] [
                        ;set state to CLOSE so the WAIT loop in 'sync-op can be interrupted --Richard
                        state/state: 'close
                        any [
                            awake make event! [type: 'done port: http-port]
                            awake make event! [type: 'close port: http-port]
                        ]
                    ]
                ]
            ]
            close http-port
            res
        ]
    ]
]

make-http-error: func [
    {Make an error for the HTTP protocol}

    msg [text! block!]
    /inf obj
    /otherhost new-url [url!] headers
] [
    ; cannot call it "message" because message is the error template.  :-/
    ; hence when the error is created it has message defined as blank, and
    ; you have to overwrite it if you're doing a custom template, e.g.
    ;
    ;     make error! [message: ["the" :animal "has claws"] animal: "cat"]
    ;
    ; A less keyword-y solution is being pursued, however this error template
    ; name of "message" existed before.  It's just that the object creation
    ; with derived fields in the usual way wasn't working, so you didn't
    ; know.  Once it was fixed, the `message` variable name here caused
    ; a conflict where the error had no message.

    if block? msg [msg: unspaced msg]
    case [
        inf [
            make error! [
                type: 'Access
                id: 'Protocol
                arg1: msg
                arg2: obj
            ]
        ]
        otherhost [
            make error! [
                type: 'Access
                id: 'Protocol
                arg1: msg
                arg2: headers
                arg3: new-url
            ]
        ]
    ] else [
        make error! [
            type: 'Access
            id: 'Protocol
            arg1: msg
        ]
    ]
]

make-http-request: func [
    return: [binary!]
    method [word! text!] "E.g. GET, HEAD, POST etc."
    target [file! text!]
        {In case of text!, no escaping is performed.}
        {(eg. useful to override escaping etc.). Careful!}
    headers [block!] "Request headers (set-word! text! pairs)"
    content [any-string! binary! blank!]
        {Request contents (Content-Length is created automatically).}
        {Empty string not exactly like blank.}
    <local> result
] [
    result: unspaced [
        uppercase form method space
        either file? target [next mold target] [target]
        space "HTTP/1.0" CR LF
    ]
    for-each [word string] headers [
        join result [mold word space string CR LF]
    ]
    if content [
        content: to binary! content
        join result ["Content-Length:" space (length of content) CR LF]
    ]
    append result unspaced [CR LF]
    result: to binary! result
    if content [append result content]
    result
]
do-request: func [
    "Perform an HTTP request"
    port [port!]
    <local> spec info req
] [
    spec: port/spec
    info: port/state/info
    spec/headers: body-of construct has [
        Accept: "*/*"
        Accept-Charset: "utf-8"
        Host: either not find [80 443] spec/port-id [
            unspaced [form spec/host ":" spec/port-id]
        ] [
            form spec/host
        ]
        User-Agent: "REBOL"
    ] spec/headers
    port/state/state: 'doing-request
    info/headers: info/response-line: info/response-parsed: port/data:
    info/size: info/date: info/name: blank
    write port/state/connection
    req: make-http-request spec/method any [spec/path %/]
    spec/headers spec/content
    net-log/C to text! req
]

; if a no-redirect keyword is found in the write dialect after 'headers then 302 redirects will not be followed
parse-write-dialect: func [port block <local> spec debug] [
    spec: port/spec
    parse block [
        opt ['headers (spec/debug: true)]
        opt ['no-redirect (spec/follow: 'ok)]
        [set block word! (spec/method: block) | (spec/method: 'post)]
        opt [set block [file! | url!] (spec/path: block)]
        [set block block! (spec/headers: block) | (spec/headers: [])]
        [
            set block [any-string! | binary!] (spec/content: block)
            | (spec/content: blank)
        ]
    ]
]

check-response: function [port] [
    state: port/state
    conn: state/connection
    info: state/info
    headers: info/headers
    line: info/response-line
    awake: :state/awake
    spec: port/spec

    ; dump spec
    all [
        not headers
        d1: find conn/data crlfbin
        d2: find/tail d1 crlf2bin
    ] then [
        info/response-line: line: to text! copy/part conn/data d1

        ; !!! In R3-Alpha, CONSTRUCT/WITH allowed passing in data that could
        ; be a STRING! or a BINARY! which would be interpreted as an HTTP/SMTP
        ; header.  The code that did it was in a function Scan_Net_Header(),
        ; that has been extracted into a completely separate native.  It
        ; should really be rewritten as user code with PARSE here.
        ;
        assert [binary? d1]
        d1: scan-net-header d1

        info/headers: headers: construct/only http-response-headers d1
        info/name: to file! any [spec/path %/]
        if headers/content-length [
            info/size:
                <- headers/content-length:
                <- to-integer/unsigned headers/content-length
        ]
        if headers/last-modified [
            info/date: attempt [idate-to-date headers/last-modified]
        ]
        remove/part conn/data d2
        state/state: 'reading-data
        if quote (txt) <> last body-of :net-log [ ; net-log is in active state
            print "Dumping Webserver headers and body"
            net-log/S info
            trap/with [
                body: to text! conn/data
                dump body
            ][
                print unspaced [
                    "S: " length of conn/data " binary bytes in buffer ..."
                ]
            ]
        ]
    ]

    if not headers [
        read conn
        return false
    ]

    res: false

    info/response-parsed: default [
        catch [
            parse line [
                "HTTP/1." [#"0" | #"1"] some #" " [
                    #"1" (throw 'info)
                    |
                    #"2" [["04" | "05"] (throw 'no-content)
                        | (throw 'ok)
                    ]
                    |
                    #"3" [
                        (if spec/follow = 'ok [throw 'ok])

                        "02" (throw spec/follow)
                        |
                        "03" (throw 'see-other)
                        |
                        "04" (throw 'not-modified)
                        |
                        "05" (throw 'use-proxy)
                        | (throw 'redirect)
                    ]
                    |
                    #"4" [
                        "01" (throw 'unauthorized)
                        |
                        "07" (throw 'proxy-auth)
                        | (throw 'client-error)
                    ]
                    |
                    #"5" (throw 'server-error)
                ]
                | (throw 'version-not-supported)
            ]
        ]
    ]

    if spec/debug = true [
        spec/debug: info
    ]

    switch/all info/response-parsed [
        'ok [
            if spec/method = 'HEAD [
                state/state: 'ready
                res: any [
                    awake make event! [type: 'done port: port]
                    awake make event! [type: 'ready port: port]
                ]
            ] else [
                res: check-data port
                if not res and (state/state = 'ready) [
                    res: any [
                        awake make event! [type: 'done port: port]
                        awake make event! [type: 'ready port: port]
                    ]
                ]
            ]
        ]
        'redirect
        'see-other [
            if spec/method = 'HEAD [
                state/state: 'ready
                res: awake make event! [type: 'custom port: port code: 0]
            ] else [
                res: check-data port
                if not open? port [
                    ;
                    ; !!! comment said: "some servers(e.g. yahoo.com) don't
                    ; supply content-data in the redirect header so the
                    ; state/state can be left in 'reading-data after
                    ; check-data call.  I think it is better to check if port
                    ; has been closed here and set the state so redirect
                    ; sequence can happen."
                    ;
                    state/state: 'ready
                ]
            ]
            if not res and (state/state = 'ready) [
                all [
                    find [get head] spec/method or (all [
                        info/response-parsed = 'see-other
                        spec/method: 'get
                    ])
                    in headers 'Location
                ] then [
                    res: do-redirect port headers/location headers
                ] else [
                    state/error: make-http-error/inf
                        <- "Redirect requires manual intervention" info
                    res: awake make event! [type: 'error port: port]
                ]
            ]
        ]
        'unauthorized
        'client-error
        'server-error
        'proxy-auth [
            if spec/method = 'HEAD [
                state/state: 'ready
            ] else [
                check-data port
            ]
        ]
        'unauthorized [
            state/error: make-http-error "Authentication not supported yet"
            res: awake make event! [type: 'error port: port]
        ]
        'client-error
        'server-error [
            state/error: make-http-error ["Server error: " line]
            res: awake make event! [type: 'error port: port]
        ]
        'not-modified [
            state/state: 'ready
            res: any [
                awake make event! [type: 'done port: port]
                awake make event! [type: 'ready port: port]
            ]
        ]
        'use-proxy [
            state/state: 'ready
            state/error: make-http-error "Proxies not supported yet"
            res: awake make event! [type: 'error port: port]
        ]
        'proxy-auth [
            state/error: make-http-error
                <- "Authentication and proxies not supported yet"
            res: awake make event! [type: 'error port: port]
        ]
        'no-content [
            state/state: 'ready
            res: any [
                awake make event! [type: 'done port: port]
                awake make event! [type: 'ready port: port]
            ]
        ]
        'info [
            info/headers: _
            info/response-line: _
            info/response-parsed: _
            port/data: _
            state/state: 'reading-headers
            read conn
        ]
        'version-not-supported [
            state/error: make-http-error "HTTP response version not supported"
            res: awake make event! [type: 'error port: port]
            close port
        ]
    ]
    res
]
crlfbin: #{0D0A}
crlf2bin: #{0D0A0D0A}
crlf2: to text! crlf2bin
http-response-headers: context [
    Content-Length: _
    Transfer-Encoding: _
    Last-Modified: _
]

do-redirect: func [
    port [port!]
    new-uri [url! text! file!]
    headers
    <local> spec state
][
    spec: port/spec
    state: port/state
    if #"/" = first new-uri [
        new-uri: as url! unspaced [spec/scheme "://" spec/host new-uri]
    ]
    new-uri: decode-url new-uri
    if not find new-uri 'port-id [
        switch new-uri/scheme [
            'https [append new-uri [port-id: 443]]
            'http [append new-uri [port-id: 80]]
        ] else [
            fail ["Unknown scheme:" new-uri/scheme]
        ]
    ]
    new-uri: construct/only port/scheme/spec new-uri
    if not find [http https] new-uri/scheme [
        state/error: make-http-error {Redirect to a protocol different from HTTP or HTTPS not supported}
        return state/awake make event! [type: 'error port: port]
    ]
    either all [
        new-uri/host = spec/host
        new-uri/port-id = spec/port-id
    ] [
        spec/path: new-uri/path
        ;we need to reset tcp connection here before doing a redirect
        close port/state/connection
        open port/state/connection
        do-request port
        false
    ] [
        state/error: make-http-error/otherhost
            "Redirect to other host - requires custom handling"
            as url! unspaced [new-uri/scheme "://" new-uri/host new-uri/path] headers
        state/awake make event! [type: 'error port: port]
    ]
]

check-data: function [port] [
    state: port/state
    headers: state/info/headers
    conn: state/connection
    res: false
    case [
        headers/transfer-encoding = "chunked" [
            data: conn/data
            ;clear the port data only at the beginning of the request --Richard
            port/data: default [make binary! length of data]
            out: port/data
            until [
                either parse data [
                    copy chunk-size some hex-digits thru crlfbin mk1: to end
                ] [
                    ; The chunk size is in the byte stream as ASCII chars
                    ; forming a hex string.  ISSUE! can decode that.
                    chunk-size: (
                        to-integer/unsigned to issue! to text! chunk-size
                    )

                    either chunk-size = 0 [
                        if parse mk1 [
                            crlfbin (trailer: "") to end | copy trailer to crlf2bin to end
                        ] [
                            trailer: has/only trailer
                            append headers body-of trailer
                            state/state: 'ready
                            res: state/awake make event! [type: 'custom port: port code: 0]
                            clear data
                        ]
                        true
                    ] [
                        either parse mk1 [
                            chunk-size skip mk2: crlfbin to end
                        ] [
                            insert/part tail of out mk1 mk2
                            remove/part data skip mk2 2
                            empty? data
                        ] [
                            true
                        ]
                    ]
                ] [
                    true
                ]
            ]
            if state/state <> 'ready [
                ;
                ; Awaken WAIT loop to prevent timeout when reading big data.
                ;
                res: true
            ]
        ]
        integer? headers/content-length [
            port/data: conn/data
            either headers/content-length <= length of port/data [
                state/state: 'ready
                conn/data: make binary! 32000
                res: state/awake make event! [
                    type: 'custom
                    port: port
                    code: 0
                ]
            ][
                ; Awaken WAIT loop to prevent timeout when reading big data.
                ;
                res: true
            ]
        ]
    ] else [
        port/data: conn/data
        either state/info/response-parsed = 'ok [
            ;
            ; Awaken WAIT loop to prevent timeout when reading big data.
            ;
            res: true
        ][
            ; On other response than OK read all data asynchronously
            ; (assuming the data are small).
            ;
            read conn
        ]
    ]

    res
]

hex-digits: charset "1234567890abcdefABCDEF"
sys/make-scheme [
    name: 'http
    title: "HyperText Transport Protocol v1.1"

    spec: construct system/standard/port-spec-net [
        path: %/
        method: 'get
        headers: []
        content: _
        timeout: 15
        debug: _
        follow: 'redirect
    ]

    info: construct system/standard/file-info [
        response-line:
        response-parsed:
        headers: _
    ]

    actor: [
        read: func [
            port [port!]
            /lines
            /string
            <local> foo
        ][
            foo: if action? :port/awake [
                if not open? port [
                    cause-error 'Access 'not-open port/spec/ref
                ]
                if port/state/state <> 'ready [
                    fail make-http-error "Port not ready"
                ]
                port/state/awake: :port/awake
                do-request port
            ] else [
                sync-op port []
            ]
            if lines or (string) [
                ; !!! When READ is called on an http PORT! (directly or
                ; indirectly) it bounces its parameters to this routine.  To
                ; avoid making an error this tolerates the refinements but the
                ; actual work of breaking the buffer into lines is done in the
                ; generic code so it will apply to all ports.  The design
                ; from R3-Alpha for ports (and "actions" in general), was
                ; rather half-baked, so this should all be rethought.
            ]
            return foo
        ]

        write: func [
            port [port!]
            value
        ][
            if not match [block! binary! text!] :value [
                value: form :value
            ]
            if not block? value [
                value: reduce [
                    [Content-Type:
                        "application/x-www-form-urlencoded; charset=utf-8"
                    ]
                    value
                ]
            ]
            if action? :port/awake [
                unless open? port [
                    cause-error 'Access 'not-open port/spec/ref
                ]
                if port/state/state <> 'ready [
                    fail make-http-error "Port not ready"
                ]
                port/state/awake: :port/awake
                parse-write-dialect port value
                do-request port
                port
            ] else [
                sync-op port [parse-write-dialect port value]
            ]
        ]

        open: func [
            port [port!]
            <local> conn
        ][
            if port/state [return port]
            if not port/spec/host [
                fail make-http-error "Missing host address"
            ]
            port/state: has [
                state: 'inited
                connection: _
                error: _
                close?: no
                info: construct port/scheme/info [type: 'file]
                awake: :port/awake
            ]
            port/state/connection: conn: make port! compose [
                scheme: (
                    to lit-word! either port/spec/scheme = 'http ['tcp]['tls]
                )
                host: port/spec/host
                port-id: port/spec/port-id
                ref: join-all [tcp:// host ":" port-id]
            ]
            conn/awake: :http-awake
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
                    if port/data [length of port/data] else [0]
                ]
            ]
        ]

        close: func [
            port [port!]
        ][
            if port/state [
                close port/state/connection
                port/state/connection/awake: _
                port/state: _
            ]
            port
        ]

        copy: func [
            port [port!]
        ][
            either all [port/spec/method = 'HEAD | port/state] [
                reduce bind [name size date] port/state/info
            ][
                if port/data [copy port/data]
            ]
        ]

        query: func [
            port [port!]
            <local> error state
        ][
            if state: port/state [
                either error? error: state/error [
                    state/error: _
                    error
                ][
                    state/info
                ]
            ]
        ]
    ]
]

sys/make-scheme/with [
    name: 'https
    title: "Secure HyperText Transport Protocol v1.1"
    spec: construct spec [
        port-id: 443
    ]
] 'http
