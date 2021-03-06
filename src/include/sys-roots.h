//
//  File: %sys-roots.h
//  Summary: {Definitions for allocating REBVAL* API handles}
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Rebol Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information
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
// API REBVALs live in singular arrays (which fit inside a REBSER node, that
// is the size of 2 REBVALs).  But they aren't kept alive by references from
// other values, like the way that a REBARR used by a BLOCK! is kept alive.
// They are kept alive by being roots (currently implemented with a flag
// NODE_FLAG_ROOT, but it could also mean living in a distinct pool from
// other series nodes).
//
// The API value content is in the single cell, with LINK().owner holding
// a REBCTX* of the FRAME! that controls its lifetime, or EMPTY_ARRAY.  This
// link field exists in the pointer immediately prior to the REBVAL*, which
// means it can be sniffed as a REBNOD* and distinguished from handles that
// were given back with rebMalloc(), so routines can discern them.
//
// MISC() is currently unused, but could serve as a reference count or other
// purpose.  It's not particularly necessary to have API handles use REBSER
// nodes--though the 2*sizeof(REBVAL) provides some optimality, and it
// means that REBSER nodes can be recycled for more purposes.  But it would
// potentially be better to have them in their own pools, because being
// roots could be discovered without a "pre-pass" in the GC.
//

// What distinguishes an API value is that it has both the NODE_FLAG_CELL and
// NODE_FLAG_ROOT bits set.
//
inline static REBOOL Is_Api_Value(const RELVAL *v) {
    assert(v->header.bits & NODE_FLAG_CELL);
    return did (v->header.bits & NODE_FLAG_ROOT);
}

// !!! The return cell from this allocation is a trash cell which has had some
// additional bits set.  This means it is not "canonized" trash that can be
// detected as distinct from UTF-8 strings, so don't call IS_TRASH_DEBUG() or
// Detect_Rebol_Pointer() on it until it has been further initialized.
//
// Ren-C manages by default.
//
inline static REBVAL *Alloc_Value(void)
{
    REBARR *a = Alloc_Singular_Array_Core(
        NODE_FLAG_ROOT
        | NODE_FLAG_MANAGED
        | SERIES_FLAG_FIXED_SIZE
    );

    // Giving the cell itself NODE_FLAG_ROOT lets a REBVAL* be discerned as
    // either an API handle or not.  The flag is not copied by Move_Value().
    //
    REBVAL *v = SINK(ARR_SINGLE(a));
    v->header.bits |= NODE_FLAG_ROOT; // it's trash (can't use SET_VAL_FLAGS)

    assert(UNBOUND != NULL);
    LINK(a).owner = (FS_TOP == NULL)
        ? UNBOUND
        : NOD(CTX_VARLIST(Context_For_Frame_May_Reify_Managed(FS_TOP)));
    return v;
}

inline static void Free_Value(REBVAL *v)
{
    assert(Is_Api_Value(v));

    REBARR *a = Singular_From_Cell(v);
    GC_Kill_Series(SER(a));
}

