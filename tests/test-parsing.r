Rebol [
    Title: "Test parsing"
    File: %test-parsing.r
    Copyright: [2012 "Saphirion AG"]
    License: {
        Licensed under the Apache License, Version 2.0 (the "License");
        you may not use this file except in compliance with the License.
        You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0
    }
    Author: "Ladislav Mecir"
    Purpose: "Test framework"
]

do %line-numberq.r
do %../make/tools/parsing-tools.reb
do %../make/tools/text-lines.reb

whitespace: charset [#"^A" - #" " "^(7F)^(A0)"]
digit: charset {0123456789}


read-binary: :read

make object! [

    position: _
    success: _

    ;; TEST-SOURCE-RULE matches the internal text of a test
    ;; even if that text is invalid rebol syntax.

    set 'test-source-rule [
        any [
            position:

            ["{" | {"}] (
                ; handle string using TRANSCODE
                success: either error? trap [
                    position: second transcode/next position
                ] [
                    [end skip]
                ] [
                    [:position]
                ]
            ) success
                |
            ["{" | {"}] :position break
                |
            "[" test-source-rule "]" ;-- plain BLOCK! in code for a test
                |
            "(" test-source-rule ")" ;-- plain GROUP! in code for a test
                |
            ";" [thru newline | to end]
                |
            ;
            ; If we see a closing bracket out of turn, that means we've "gone
            ; too far".  It's either a syntax error, or the closing bracket of
            ; a multi-test block.
            ;
            "]" :position break
                |
            ")" :position break
                |
            skip
        ]
    ]

    set 'load-testfile function [
        {Read the test source, preprocessing if necessary.}
        test-file [file!]
    ][
        test-source: context [
            filepath: test-file
            contents: read test-file
        ]
        test-source
    ]

    set 'collect-tests procedure [
        collected-tests [block!]
            {collect the tests here (modified)}
        test-file [file!]
    ][
        current-dir: what-dir
        print ["file:" mold test-file]

        either error? err: trap [
            if file? test-file [
                test-file: clean-path test-file
                change-dir first split-path test-file
            ]
            test-sources: get in load-testfile test-file 'contents
        ][
            ; probe err
            append collected-tests reduce [
                test-file 'dialect {^/"failed, cannot read the file"^/}
            ]
            change-dir current-dir
            leave
        ][
            change-dir current-dir
            append collected-tests test-file
        ]

        types: context [
            wsp: cmt: val: tst: grpb: grpe: flg: fil: isu: str: end: _
        ]

        flags: copy []

        wsp: [
            [
                some whitespace (type: in types 'wsp)
                |
                ";" [thru newline | to end] (type: in types 'cmt)
            ]
        ]

        any-wsp: [any [wsp emit-token]]

        single-value: parsing-at x [
            if not error? trap [
                set [value: next-position:] transcode/next x
            ][
                type: in types 'val
                next-position
            ]
        ]

        single-test: [
            copy vector ["(" test-source-rule ")"] (
                type: in types 'tst
                append/only collected-tests flags
                append collected-tests vector
            )
        ]

        grouped-tests: [
            "[" (type: in types 'grpb) emit-token
            any [
                any-wsp single-value
                [
                    if (tag? value) (
                        type: in types 'flag
                        append flags value
                    )
                        |
                    if (issue? value) (type: in types 'isu)
                ]
                emit-token
            ]
            opt [
                any-wsp single-value
                if (text? value) (type: in types 'str)
                emit-token
            ]
            any [any-wsp single-test emit-token]
            any-wsp "]" (type: in types 'grpe) emit-token
        ]

        token: [
            position:

            (type: value: _)

            wsp emit-token
                |
            single-test (flags: copy []) emit-token
                |
            grouped-tests (flags: copy [])
                |
            end (type: in types 'end) emit-token break
                |
            single-value
            [
                if (tag? get 'value) (
                    type: in types 'flg
                    append flags value
                )
                |
                if (file? get 'value) (
                    type: in types 'fil
                    collect-tests collected-tests value
                    print ["file:" mold test-file]
                    append collected-tests test-file
                )
            ]
        ]

        emit-token: [
            token-end: (
                comment [
                    prin "emit: " probe compose [
                        (type) (to text! copy/part position token-end)
                    ]
                ]
            )
            position: (type: value: _)
        ]

        rule: [any token]

        parse test-sources rule or [
            append collected-tests reduce [
                'dialect
                spaced [
                    newline
                    {"failed, line/col:} (text-location-of position) {"}
                    newline
                ]
            ]
        ]
    ]

    set 'collect-logs function [
        collected-logs [block!]
            {collect the logged results here (modified)}
        log-file [file!]
    ][
        if error? trap [log-contents: read log-file] [
            fail ["Unable to read " mold log-file]
        ]

        parse log-contents [
            (guard: [end skip])
            any [
                any whitespace
                [
                    position: "%"
                    (set [value: next-position:] transcode/next position)
                    :next-position
                        |
                    ; dialect failure?
                    some whitespace
                    {"} thru {"}
                        |
                    copy last-vector ["(" test-source-rule ")"]
                    any whitespace
                    [
                        end (
                            ; crash found
                            fail "log incomplete!"
                        )
                            |
                        {"} copy value to {"} skip
                        ; test result found
                        (
                            parse value [
                                "succeeded" (value: 'succeeded)
                                    |
                                "failed" (value: 'failed)
                                    |
                                "crashed" (value: 'crashed)
                                    |
                                "skipped" (value: 'skipped)
                                    |
                                (fail "invalid test result")
                            ]
                            append collected-logs reduce [
                                last-vector
                                value
                            ]
                        )
                    ]
                        |
                    "system/version:" to end (guard: _)
                        |
                    (fail "collect-logs - log file parsing problem")
                ] position: guard break ; Break when error detected.
                    |
                :position
            ]
        ]
    ]
]
