REBOL [
    System: "REBOL [R3] Language Interpreter and Run-time Environment"
    Title: "REBOL 3 Mezzanine: Shell-like Command Functions"
    Rights: {
        Copyright 2012 REBOL Technologies
        REBOL is a trademark of REBOL Technologies
    }
    License: {
        Licensed under the Apache License, Version 2.0
        See: http://www.apache.org/licenses/LICENSE-2.0
    }
]

ls:     :list-dir
pwd:    :what-dir

rm: does [
    fail "Use DELETE, not RM (Rebol REMOVE is different, shell dialect coming)"
]

mkdir:  :make-dir

cd: func [
    "Change directory (shell shortcut function)."

    return: [file!]
        {The directory after the change}
    'path [<end> file! word! path! text!]
        "Accepts %file, :variables and just words (as dirs)"
][
    switch type of :path [
        null []
        file! [change-dir path]
        text! [change-dir local-to-file path]
        word! path! [change-dir to-file path]
    ]

    return what-dir
]

more: func [
    {Print file (shell shortcut function).}

    'file "Accepts %file and also just words (as file names)"
        [file! word! path! text!]
][
    print deline to-text read switch type of :file [
        file! [file]
        text! [local-to-file file]
        word! path! [to-file file]
    ]
]
