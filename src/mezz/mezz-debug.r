REBOL [
    System: "REBOL [R3] Language Interpreter and Run-time Environment"
    Title: "REBOL 3 Mezzanine: Debug"
    Rights: {
        Copyright 2012 REBOL Technologies
        REBOL is a trademark of REBOL Technologies
    }
    License: {
        Licensed under the Apache License, Version 2.0
        See: http://www.apache.org/licenses/LICENSE-2.0
    }
]

verify: procedure [
    {Verify all the conditions in the passed-in block are conditionally true}

    conditions [block!]
        {Conditions to check}
][
    while-not [tail? conditions] [
        if not (result: do/next conditions quote pos:) [
            fail [
                "Assertion condition returned"
                 choose [
                    (unset? 'result) "void"
                    (blank? result) "blank"
                    (result = false) "false"
                ]
                ":"
                copy/part conditions pos
            ]
        ]

        conditions: pos ;-- move expression position and continue

        ; including BAR!s in the failure report looks messy
        while [bar? :conditions/1] [conditions: next conditions]
    ]
]


; Set up ASSERT as having a user-mode implementation matching VERIFY.
; This helps show people a pattern for implementing their own assert.
;
; !!! If a debug mode were offered, you'd want to be able to put back ASSERT
; in such a way as to cost basically nothing.
;
; !!! Note there is a layering problem, in that if people make a habit of
; hijacking ASSERT, and it's used in lower layer implementations, it could
; recurse.  e.g. if file I/O writing used ASSERT, and you added a logging
; feature via HIJACK that wrote to a file.  Implications of being able to
; override a system-wide assert in this way should be examined, and perhaps
; copies of the function made at layer boundaries.
;
native-assert: copy :assert
hijack 'assert :verify


delta-time: function [
    {Delta-time - returns the time it takes to evaluate the block.}
    block [block!]
][
    start: stats/timer
    do block
    stats/timer - start
]

delta-profile: func [
    {Delta-profile of running a specific block.}
    block [block!]
    <local> start end
][
    start: values of stats/profile
    do block
    end: values of stats/profile
    for-each num start [
        change end end/1 - num
        end: next end
    ]
    start: construct system/standard/stats []
    set start head of end
    start
]

speed?: function [
    "Returns approximate speed benchmarks [eval cpu memory file-io]."
    /no-io "Skip the I/O test"
    /times "Show time for each test"
][
    result: copy []
    for-each block [
        [
            loop 100'000 [
                ; measure more than just loop func
                ; typical load: 1 set, 2 data, 1 op, 4 trivial funcs
                x: 1 * index of back next "x"
                x: 1 * index of back next "x"
                x: 1 * index of back next "x"
                x: 1 * index of back next "x"
            ]
            calc: [100'000 / secs / 100] ; arbitrary calc
        ][
            tmp: make binary! 500'000
            insert/dup tmp "abcdefghij" 50000
            loop 10 [
                random tmp
                gunzip gzip tmp
            ]
            calc: [(length of tmp) * 10 / secs / 1900]
        ][
            repeat n 40 [
                change/dup tmp to-char n 500'000
            ]
            calc: [(length of tmp) * 40 / secs / 1024 / 1024]
        ][
            if not no-io [
                write file: %tmp-junk.txt "" ; force security request before timer
                tmp: make text! 32000 * 5
                insert/dup tmp "test^/" 32000
                loop 100 [
                    write file tmp
                    read file
                ]
                delete file
                calc: [(length of tmp) * 100 * 2 / secs / 1024 / 1024]
            ]
        ]
    ][
        secs: now/precise
        calc: 0
        recycle
        do block
        secs: to decimal! difference now/precise secs
        append result to integer! do calc
        if times [append result secs]
    ]
    result
]

net-log: func [txt /C /S][txt]

net-trace: procedure [
    "Switch between using a no-op or a print operation for net-tracing"
    val [logic!]
][
    either val [
        hijack 'net-log func [txt /C /S][
            print [
                (c ?? "C:") (s ?? "S:") either block? txt [spaced txt] [txt]
            ]
            txt
        ]
        print "Net-trace is now on"
    ][
        hijack 'net-log func [txt /C /S][txt]
    ]
]
