//
//  File: %reb-host.h
//  Summary: "Include files for hosting"
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Rebol Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//=////////////////////////////////////////////////////////////////////////=//
//

#include <stddef.h> // size_t and other definitions API use

#include "reb-config.h"

#include "reb-c.h"
#include "reb-ext.h"        // includes reb-defs.h

#include "reb-device.h"
#include "reb-event.h"
#include "reb-evtypes.h"

#include "sys-rebnod.h" // !!! Legacy dependency, REBGOB should not be REBNOD
#include "reb-gob.h"

#include "reb-lib.h"

#include "host-lib.h"
