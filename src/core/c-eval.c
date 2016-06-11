//
//  File: %c-eval.c
//  Summary: "Central Interpreter Evaluator"
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2016 Rebol Open Source Contributors
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
// This file contains `Do_Core()`, which is the central evaluator which
// is behind DO.  It can execute single evaluation steps (e.g. a DO/NEXT)
// or it can run the array to the end of its content.  A flag controls that
// behavior, and there are other flags for controlling its other behaviors.
//
// For comprehensive notes on the input parameters, output parameters, and
// internal state variables...see %sys-do.h and `struct Reb_Frame`.
//
// NOTES:
//
// * Do_Core() is a very long routine.  That is largely on purpose, because it
//   doesn't contain repeated portions.  If it were broken into functions that
//   would add overhead for little benefit, and prevent interesting tricks
//   and optimizations.  Note that it is broken down into sections, and
//   the invariants in each section are made clear with comments and asserts.
//
// * The evaluator only moves forward, and it consumes exactly one element
//   from the input at a time.  This input may be a source where the index
//   needs to be tracked and care taken to contain the index within its
//   boundaries in the face of change (e.g. a mutable ARRAY).  Or it may be
//   an entity which tracks its own position on each fetch, where "indexor"
//   is serving as a flag and should be left static.
//

#include "sys-core.h"

#include "tmp-evaltypes.inc"


#if !defined(NDEBUG)
    //
    // Forward declarations for debug-build-only code--routines at end of
    // file.  (Separated into functions to reduce clutter in the main logic.)
    //
    static void Do_Core_Entry_Checks_Debug(struct Reb_Frame *f);
    static REBUPT Do_Core_Expression_Checks_Debug(struct Reb_Frame *f);
    static void Do_Core_Exit_Checks_Debug(struct Reb_Frame *f);

    // The `do_count` should be visible in the C debugger watchlist as a
    // local variable in Do_Core() for each stack level.  So if a fail()
    // happens at a deterministic moment in a run, capture the number from
    // the level of interest and recompile with it here to get a breakpoint
    // at that tick.
    //
    // Notice also that in debug builds, frames carry this value in them.
    // *Plus* you can get the initialization tick for void cells, BLANK!s,
    // LOGIC!s, and most end markers by looking at the `track` payload of
    // the REBVAL cell.  And series contain the do_count where they were
    // created as well.
    //
    //      *** DON'T COMMIT THIS v-- KEEP IT AT ZERO! ***
    #define DO_COUNT_BREAKPOINT    0
    //      *** DON'T COMMIT THIS --^ KEEP IT AT ZERO! ***
    //
    // !!! Taking this number on the command line could be convenient.
#endif


static inline void Start_New_Expression_Core(struct Reb_Frame *f) {
    f->expr_index = f->index; // !!! See FRM_INDEX() for caveats
    if (Trace_Flags)
        Trace_Line(f);
}

#ifdef NDEBUG
    #define START_NEW_EXPRESSION(f) \
        Start_New_Expression_Core(f)
#else
    // Macro is used to mutate local do_count variable in Do_Core (for easier
    // browsing in the watchlist) as well as to not be in a deeper stack level
    // than Do_Core when a DO_COUNT_BREAKPOINT is hit.
    //
    #define START_NEW_EXPRESSION(f) \
        do { \
            Start_New_Expression_Core(f); \
            do_count = Do_Core_Expression_Checks_Debug(f); \
            if (do_count == DO_COUNT_BREAKPOINT) \
                debug_break(); /* see %debug_break.h */ \
        } while (FALSE)
#endif

static inline void Type_Check_Arg_For_Param_May_Fail(struct Reb_Frame * f) {
    if (!TYPE_CHECK(f->param, VAL_TYPE(f->arg)))
        fail (Error_Arg_Type(FRM_LABEL(f), f->param, VAL_TYPE(f->arg)));
}

static inline void Drop_Function_Args_For_Frame(struct Reb_Frame *f) {
    Drop_Function_Args_For_Frame_Core(f, TRUE);
}

static inline void Abort_Function_Args_For_Frame(struct Reb_Frame *f) {
    Drop_Function_Args_For_Frame(f);

    // If a function call is aborted, there may be pending refinements (if
    // in the gathering phase) or functions (if running a chainer) on the
    // data stack.  They must be dropped to balance.
    //
    DS_DROP_TO(f->dsp_orig);
}

static inline REBOOL Specialized_Arg(REBVAL *arg) {
    return NOT_END(arg); // END marker is used to indicate "pending" arg slots
}


//
//  Do_Core: C
//
// While this routine looks very complex, it's actually not that difficult
// to step through.  A lot of it is assertions, debug tracking, and comments.
//
// Whether fields contain usable values upon entry depends on `f->eval_type`
// and a number of conditions.  For instance, if ET_FUNCTION and `f->lookback`
// then `f->out` will contain the first argument to the lookback (e.g. infix)
// function being run.
//
// Comments on the definition of Reb_Frame are a good place to start looking
// to understand what's going on.  See %sys-frame.h
//
void Do_Core(struct Reb_Frame * const f)
{
#if !defined(NDEBUG)
    REBUPT do_count = f->do_count = TG_Do_Count; // snapshot initial state
#endif

    // Establish baseline for whether we are to evaluate function argumentsn
    // according to the flags passed in.  EVAL can change this with EVAL/ONLY
    //
    REBOOL args_evaluate = NOT(f->flags & DO_FLAG_NO_ARGS_EVALUATE);

    // APPLY and a DO of a FRAME! both use this same code path.
    //
    if (f->flags & DO_FLAG_APPLYING) {
        assert(NOT(f->lookback)); // no support ATM for "applying infixedly"
        goto do_function_arglist_in_progress;
    }

    PUSH_CALL(f);

#if !defined(NDEBUG)
    SNAP_STATE(&f->state); // to make sure stack balances, etc.
    Do_Core_Entry_Checks_Debug(f); // run once per Do_Core()
#endif

    // Check just once (stack level would be constant if checked in a loop)
    //
    if (C_STACK_OVERFLOWING(&f)) Trap_Stack_Overflow();

    // Capture the data stack pointer on entry (used by debug checks, but
    // also refinements are pushed to stack and need to be checked if there
    // are any that are not processed)
    //
    f->dsp_orig = DSP;

do_next:

    assert(Eval_Count != 0);

    if (--Eval_Count == 0 || Eval_Signals || TRUE) {
        //
        // Note that Do_Signals_Throws() may do a recycle step of the GC, or
        // it may spawn an entire interactive debugging session via
        // breakpoint before it returns.  It may also FAIL and longjmp out.
        //
        REBET eval_type_saved = f->eval_type;
        f->eval_type = ET_INERT;

        INIT_CELL_WRITABLE_IF_DEBUG(&f->cell.eval);
        if (Do_Signals_Throws(SINK(&f->cell.eval))) {
            *f->out = *KNOWN(&f->cell.eval);
            goto finished;
        }

        f->eval_type = eval_type_saved;

        if (!IS_VOID(&f->cell.eval)) {
            //
            // !!! What to do with something like a Ctrl-C-based breakpoint
            // session that does something like `resume/with 10`?  We are
            // "in-between" evaluations, so that 10 really has no meaning
            // and is just going to get discarded.  FAIL for now to alert
            // the user that something is off, but perhaps the failure
            // should be contained in a sandbox and restart the break?
            //
            fail (Error(RE_MISC));
        }
    }

reevaluate:
    // ^--
    // `reevaluate` is jumped to by EVAL, and must skip the possible Recycle()
    // from the above.  Whenever `eval` holds a REBVAL it is unseen by the GC
    // *by design*.  This avoids having to initialize it or GC-safe null it
    // each time through the evaluator loop.  It will only be protected by
    // the GC indirectly when its properties are extracted during the switch,
    // such as a function that gets stored into `f->func`.
    //
    // (We also want the debugger to consider the triggering EVAL as the
    // start of the expression, and don't want to advance `expr_index`).

    //==////////////////////////////////////////////////////////////////==//
    //
    // BEGIN MAIN SWITCH STATEMENT
    //
    //==////////////////////////////////////////////////////////////////==//

    // This switch is done via ET_XXX and not just switching on the VAL_TYPE()
    // (e.g. REB_XXX).  The reason is due to "jump table" optimizing--because
    // the REB_XXX types are sparse, the switch would be less efficient than
    // when switching on values that are packed consecutively (e.g. ET_XXX).
    //
    // Note that infix ("lookback") functions are dispatched *after* the
    // switch...unless DO_FLAG_NO_LOOKAHEAD is set.

    START_NEW_EXPRESSION(f);

    switch (f->eval_type) { // <-- DO_COUNT_BREAKPOINT landing spot

//==//////////////////////////////////////////////////////////////////////==//
//
// [no evaluation] (REB_BLOCK, REB_INTEGER, REB_STRING, etc.)
//
// Copy the value's bits to f->out and fetch the next value.
//
//==//////////////////////////////////////////////////////////////////////==//

    case ET_INERT:
        QUOTE_NEXT_REFETCH(f->out, f); // clears VALUE_FLAG_EVALUATED
        break;

//==//////////////////////////////////////////////////////////////////////==//
//
// [BAR! and LIT-BAR!]
//
// If an expression barrier is seen in-between expressions (as it will always
// be if hit in this switch), it evaluates to void.  It only errors in argument
// fulfillment during the switch case for ANY-FUNCTION!.
//
// LIT-BAR! decays into an ordinary BAR! if seen here by the evaluator.
//
// Note that natives and dialects frequently do their own interpretation of
// BAR!--rather than just evaluate it and let it mean something equivalent
// to an unset.  For instance:
//
//     case [false [print "F"] | true [print ["T"]]
//
// If CASE did not specially recognize BAR!, it would complain that the
// "second condition" had no value.  So if you are looking for a BAR! behavior
// and it's not passing through here, check the construct you are using.
//
//==//////////////////////////////////////////////////////////////////////==//

    case ET_BAR:
        FETCH_NEXT_ONLY_MAYBE_END(f);
        if (NOT_END(f->value)) {
            f->eval_type = Eval_Table[VAL_TYPE(f->value)];
            goto do_next; // keep feeding BAR!s
        }

        SET_VOID(f->out);
        SET_VAL_FLAG(f->out, VALUE_FLAG_EVALUATED);
        break;

    case ET_LIT_BAR:
        SET_BAR(f->out);
        SET_VAL_FLAG(f->out, VALUE_FLAG_EVALUATED);
        FETCH_NEXT_ONLY_MAYBE_END(f);
        break;

//==//////////////////////////////////////////////////////////////////////==//
//
// [WORD!]
//
// A plain word tries to fetch its value through its binding.  It will fail
// and longjmp out of this stack if the word is unbound (or if the binding is
// to a variable which is not set).  Should the word look up to a function,
// then that function will be called by jumping to the ANY-FUNCTION! case.
//
//==//////////////////////////////////////////////////////////////////////==//

    case ET_WORD:
        if (f->gotten == NULL) // no work to reuse from failed optimization
            f->gotten = Get_Var_Core(
                &f->lookback, f->value, f->specifier, GETVAR_READ_ONLY
            );

        if (IS_FUNCTION(f->gotten)) { // before IS_VOID() speeds common case

            f->eval_type = ET_FUNCTION;
            SET_FRAME_SYM(f, VAL_WORD_SYM(f->value));

            if (NOT(f->lookback)) { // ordinary "prefix" function dispatch
                SET_END(f->out);
                goto do_function_in_gotten;
            }

            // `case ET_WORD` runs at the start of a new evaluation cycle.
            // It could be the very first element evaluated, hence it's not
            // meaningful to say it has a "left hand side" in f->out to give
            // an infix (prefix, etc.) lookback function.
            //
            // However, it can climb the stack and peek at the eval_type of
            // the parent to find SET-WORD! or SET-PATH!s in progress.
            // They are signaled specially as not being products of an
            // evaluation--hence safe to quote.

            if (f->prior)
                switch (f->prior->eval_type) {
                case ET_SET_WORD:
                    COPY_VALUE(f->out, f->prior->param, f->prior->specifier);
                    assert(IS_SET_WORD(f->out));
                    CLEAR_VAL_FLAG(f->out, VALUE_FLAG_EVALUATED);
                    goto do_function_in_gotten;

                case ET_SET_PATH:
                    COPY_VALUE(f->out, f->prior->param, f->prior->specifier);
                    assert(IS_SET_PATH(f->out));
                    CLEAR_VAL_FLAG(f->out, VALUE_FLAG_EVALUATED);
                    goto do_function_in_gotten;
                }

            SET_END(f->out); // some <end> args are able to tolerate absences
            goto do_function_in_gotten;
        }

    do_word_in_value_with_gotten:
        assert(!IS_FUNCTION(f->gotten)); // infix handling needs differ

        if (IS_VOID(f->gotten)) // need `:x` if `x` is unset
            fail (Error_No_Value_Core(f->value, f->specifier));

        *f->out = *f->gotten;
        SET_VAL_FLAG(f->out, VALUE_FLAG_EVALUATED);
        FETCH_NEXT_ONLY_MAYBE_END(f);

    #if !defined(NDEBUG)
        if (LEGACY(OPTIONS_LIT_WORD_DECAY) && IS_LIT_WORD(f->out))
            VAL_SET_TYPE_BITS(f->out, REB_WORD); // don't reset full header!
    #endif
        break;

//==//////////////////////////////////////////////////////////////////////==//
//
// [SET-WORD!]
//
// Does the evaluation into `out`, then gets the variable indicated by the
// word and writes the result there as well.
//
//==//////////////////////////////////////////////////////////////////////==//

    case ET_SET_WORD:
        //
        // fetch writes f->value, so save SET-WORD! ptr.  Note that the nested
        // evaluation here might peek up at it if it contains an infix
        // function that quotes its first argument, e.g. `x: ++ 10`
        //
        assert(IS_SET_WORD(f->value));
        f->param = f->value;

        FETCH_NEXT_ONLY_MAYBE_END(f);
        if (IS_END(f->value))
            fail (Error(RE_NEED_VALUE, f->param)); // e.g. `do [foo:]`

        if (args_evaluate) {
            //
            // A SET-WORD! handles lookahead like a prefix function would;
            // so it uses lookahead on its arguments regardless of f->flags
            //
            DO_NEXT_REFETCH_MAY_THROW(f->out, f, DO_FLAG_LOOKAHEAD);

            if (THROWN(f->out)) goto finished;

            // leave VALUE_FLAG_EVALUATED as is
        }
        else
            QUOTE_NEXT_REFETCH(f->out, f); // clears VALUE_FLAG_EVALUATED

    #if !defined(NDEBUG)
        if (LEGACY(OPTIONS_SET_WORD_VOID_IS_ERROR) && IS_VOID(f->out))
            fail (Error(RE_NEED_VALUE, f->param)); // e.g. `foo: ()`
    #endif

        *GET_MUTABLE_VAR_MAY_FAIL(f->param, f->specifier) = *(f->out);
        break;

//==//////////////////////////////////////////////////////////////////////==//
//
// [GET-WORD!]
//
// A GET-WORD! does no checking for unsets, no dispatch on functions, and
// will return void if the variable is not set.
//
//==//////////////////////////////////////////////////////////////////////==//

    case ET_GET_WORD:
        if (f->gotten == NULL) // no work to reuse from failed optimization
            f->gotten = GET_OPT_VAR_MAY_FAIL(f->value, f->specifier);

        *f->out = *f->gotten;
        SET_VAL_FLAG(f->out, VALUE_FLAG_EVALUATED);
        FETCH_NEXT_ONLY_MAYBE_END(f);
        break;

//==/////////////////////////////////////////////////////////////////////==//
//
// [LIT-WORD!]
//
// Note we only want to reset the type bits in the header, not the whole
// header--because header bits contain information like WORD_FLAG_BOUND.
//
//==//////////////////////////////////////////////////////////////////////==//

    case ET_LIT_WORD:
        QUOTE_NEXT_REFETCH(f->out, f); // we're adding VALUE_FLAG_EVALUATED
        VAL_SET_TYPE_BITS(f->out, REB_WORD);
        SET_VAL_FLAG(f->out, VALUE_FLAG_EVALUATED);
        break;

//==//////////////////////////////////////////////////////////////////////==//
//
// [GROUP!]
//
// If a GROUP! is seen then it generates another call into Do_Core().  The
// resulting value for this step will be the outcome of that evaluation.
//
//==//////////////////////////////////////////////////////////////////////==//

    case ET_GROUP:
        //
        // If the source array we are processing that is yielding values is
        // part of the deep copy of a function body, it's possible that this
        // GROUP! is a "relative ANY-ARRAY!" that needs the specifier to
        // resolve the relative any-words and other any-arrays inside it...
        //
        if (Do_At_Throws(
            f->out,
            VAL_ARRAY(f->value), // the GROUP!'s array
            VAL_INDEX(f->value), // index in group's REBVAL (may not be head)
            IS_RELATIVE(f->value)
                ? f->specifier // if relative, use parent specifier...
                : VAL_SPECIFIER(const_KNOWN(f->value)) // ...else use child's
        )) {
            goto finished;
        }

        SET_VAL_FLAG(f->out, VALUE_FLAG_EVALUATED);
        FETCH_NEXT_ONLY_MAYBE_END(f);
        break;

//==//////////////////////////////////////////////////////////////////////==//
//
// [PATH!]
//
//==//////////////////////////////////////////////////////////////////////==//

    case ET_PATH: {
        REBSYM sym;
        if (Do_Path_Throws_Core(
            f->out,
            &sym, // requesting symbol says we process refinements
            f->value,
            f->specifier,
            NULL // `setval`: null means don't treat as SET-PATH!
        )) {
            goto finished;
        }

        if (IS_VOID(f->out)) // need `:x/y` if `y` is unset
            fail (Error_No_Value_Core(f->value, f->specifier));

        if (IS_FUNCTION(f->out)) {
            f->eval_type = ET_FUNCTION;
            SET_FRAME_SYM(f, sym);

            // object/func or func/refinements or object/func/refinement
            //
            // Because we passed in a label symbol, the path evaluator was
            // willing to assume we are going to invoke a function if it
            // is one.  Hence it left any potential refinements on data stack.
            //
            assert(DSP >= f->dsp_orig);

            // The WORD! dispatch case checks whether the dispatch was via an
            // infix binding at this point, and if so allows the infix function
            // to run only if it has an <end>able left argument.  Paths ignore
            // the infix-or-not status of a binding for several reasons, so
            // this does not come into play here.

            assert(!f->lookback);

            f->cell.eval = *f->out;
            f->gotten = KNOWN(&f->cell.eval);
            SET_END(f->out);
            goto do_function_in_gotten;
        }

        // Path should have been fully processed, no refinements on stack
        //
        assert(DSP == f->dsp_orig);

        SET_VAL_FLAG(f->out, VALUE_FLAG_EVALUATED);
        FETCH_NEXT_ONLY_MAYBE_END(f);
        break;
    }

//==//////////////////////////////////////////////////////////////////////==//
//
// [SET-PATH!]
//
//==//////////////////////////////////////////////////////////////////////==//

    case ET_SET_PATH:
        //
        // fetch writes f->value, so save SET-PATH! ptr.  Note that the nested
        // evaluation here might peek up at it if it contains an infix
        // function that quotes its first argument, e.g. `x/y: ++ 10`
        //
        f->param = f->value;

        // f->out is held between a DO_NEXT and a Do_Path and expected to
        // stay valid.  The GC must therefore protect the f->out slot, so
        // it can't contain garbage.  (Similar issue with ET_FUNCTION)
        //
        SET_END(f->out);

        FETCH_NEXT_ONLY_MAYBE_END(f);

        // `do [a/b/c:]` is not legal
        //
        if (IS_END(f->value))
            fail (Error(RE_NEED_VALUE, f->param));

        // We want the result of the set path to wind up in `out`, so go
        // ahead and put the result of the evaluation there.  Do_Path_Throws
        // will *not* put this value in the output when it is making the
        // variable assignment!
        //
        if (args_evaluate) {
            //
            // A SET-PATH! handles lookahead like a prefix function would;
            // so it uses lookahead on its arguments regardless of f->flags
            //
            DO_NEXT_REFETCH_MAY_THROW(f->out, f, DO_FLAG_LOOKAHEAD);

            if (THROWN(f->out)) goto finished;
        }
        else {
            COPY_VALUE(f->out, f->value, f->specifier);
            FETCH_NEXT_ONLY_MAYBE_END(f);
        }

    #if !defined(NDEBUG)
        if (LEGACY(OPTIONS_SET_WORD_VOID_IS_ERROR) && IS_VOID(f->out))
            fail (Error(RE_NEED_VALUE, f->param)); // e.g. `a/b/c: ()`
    #endif

        // !!! The evaluation ordering of SET-PATH! evaluation seems to break
        // the "left-to-right" nature of the language:
        //
        //     >> foo: make object! [[bar][bar: 10]]
        //
        //     >> foo/(print "left" 'bar): (print "right" 20)
        //     right
        //     left
        //     == 20
        //
        // In addition to seeming "wrong" it also necessitates an extra cell
        // of storage.  This should be reviewed along with Do_Path generally.
        {
            REBVAL temp;
            if (Do_Path_Throws_Core(
                &temp, // output location
                NULL, // not requesting symbol means refinements not allowed
                f->param, // param is currently holding SET-PATH! we got in
                f->specifier, // needed to resolve relative array in path
                f->out // `setval`: non-NULL means do assignment as SET-PATH!
            )) {
                *(f->out) = temp;
                goto finished;
            }

            // leave VALUE_FLAG_EVALUATED as is
        }

        // We did not pass in a symbol, so not a call... hence we cannot
        // process refinements.  Should not get any back.
        //
        assert(DSP == f->dsp_orig);
        break;

//==//////////////////////////////////////////////////////////////////////==//
//
// [GET-PATH!]
//
//==//////////////////////////////////////////////////////////////////////==//

    case ET_GET_PATH:
        //
        // !!! Should a GET-PATH! be able to call into the evaluator, by
        // evaluating GROUP!s in the path?  It's clear that `get path`
        // shouldn't be able to evaluate (a GET should not have side effects).
        // But perhaps source-level GET-PATH!s can be more liberal, as one can
        // visibly see the GROUP!s.
        //
        if (Do_Path_Throws_Core(
            f->out,
            NULL, // not requesting symbol means refinements not allowed
            f->value,
            f->specifier,
            NULL // `setval`: null means don't treat as SET-PATH!
        )) {
            goto finished;
        }

        // We did not pass in a symbol ID
        //
        assert(DSP == f->dsp_orig);
        SET_VAL_FLAG(f->out, VALUE_FLAG_EVALUATED);
        FETCH_NEXT_ONLY_MAYBE_END(f);
        break;

//==//////////////////////////////////////////////////////////////////////==//
//
// [LIT-PATH!]
//
// We only set the type, in order to preserve the header bits... (there
// currently aren't any for ANY-PATH!, but there might be someday.)
//
// !!! Aliases a REBSER under two value types, likely bad, see #2233
//
//==//////////////////////////////////////////////////////////////////////==//

    case ET_LIT_PATH:
        QUOTE_NEXT_REFETCH(f->out, f);
        VAL_SET_TYPE_BITS(f->out, REB_PATH);
        SET_VAL_FLAG(f->out, VALUE_FLAG_EVALUATED);
        break;

//==//////////////////////////////////////////////////////////////////////==//
//
// [FUNCTION!]
//
// If a function makes it to the SWITCH statement, that means it is either
// literally a function value in the array (`do compose [(:+) 1 2]`) or is
// being retriggered via EVAL.  Note that infix functions that are
// encountered in this way will behave as prefix--their infix behavior
// is only triggered when they are looked up from a word.  See #1934.
//
// Most function evaluations are triggered from a SWITCH on a WORD! or PATH!,
// which jumps in at the `do_function_in_gotten` label.
//
//==//////////////////////////////////////////////////////////////////////==//

    case ET_FUNCTION:
        if (f->lookback) {
            assert(NOT_END(f->out)); // !!! for future use
        }
        else {
            //
            // Hitting this case (vs the labels below) means hitting a function
            // literally in a block.  This is relatively uncommon, so the code
            // caters to more common function fetches winding up in f->gotten.
            //
            f->gotten = const_KNOWN(f->value);
            SET_FRAME_SYM(f, SYM___ANONYMOUS__); // literal functions nameless
            SET_END(f->out); // needs GC-safe data
        }

    do_function_in_gotten:

        assert(IS_FUNCTION(f->gotten));

        assert(f->label_sym != SYM_0); // must be something (even "anonymous")
    #if !defined(NDEBUG)
        assert(f->label_str != NULL); // SET_FRAME_SYM sets (for C debugging)
    #endif

        // There may be refinements pushed to the data stack to process, if
        // the call originated from a path dispatch.
        //
        assert(DSP >= f->dsp_orig);

    //==////////////////////////////////////////////////////////////////==//
    //
    // FUNCTION! EVAL HANDLING
    //
    //==////////////////////////////////////////////////////////////////==//

        // The EVAL "native" is unique because it cannot be a function that
        // runs "under the evaluator"...because it *is the evaluator itself*.
        // Hence it is handled in a special way.
        //
        if (VAL_FUNC(f->gotten) == NAT_FUNC(eval)) {
            FETCH_NEXT_ONLY_MAYBE_END(f);

            // The garbage collector expects f->func to be valid during an
            // argument fulfillment, and f->param needs to be a typeset in
            // order to cue Is_Function_Frame_Fulfilling().
            //
            f->func = NAT_FUNC(eval);
            f->param = FUNC_PARAM(NAT_FUNC(eval), 1);

            // "DO/NEXT" full expression into the `eval` REBVAR slot
            // (updates index...).  (There is an /ONLY switch to suppress
            // normal evaluation but it does not apply to the value being
            // retriggered itself, just any arguments it consumes.)
            //
            if (f->lookback) {
                if (IS_END(f->out))
                    fail (Error_No_Arg(FRM_LABEL(f), f->param));

                f->cell.eval = *f->out;
                f->lookback = FALSE;
                SET_END(f->out);
            }
            else {
                if (IS_END(f->value)) // e.g. `do [eval]`
                    fail (Error_No_Arg(FRM_LABEL(f), f->param));

                DO_NEXT_REFETCH_MAY_THROW(
                    SINK(&f->cell.eval), f, DO_FLAG_LOOKAHEAD
                );

                if (THROWN(&f->cell.eval)) goto finished;
            }

            // There's only one refinement to EVAL and that is /ONLY.  It can
            // push one refinement to the stack or none.  The state will
            // twist up the evaluator for the next evaluation only.
            //
            if (DSP > f->dsp_orig) {
                assert(DSP == f->dsp_orig + 1);
                assert(VAL_WORD_SYM(DS_TOP) == SYM_ONLY); // canonized on push
                DS_DROP;
                args_evaluate = FALSE;
            }
            else
                args_evaluate = TRUE;

            CLEAR_FRAME_SYM(f);

            // Jumping to the `reevaluate:` label will skip the fetch from the
            // array to get the next `value`.  So seed it with the address of
            // eval result, and step the index back by one so the next
            // increment will get our position sync'd in the block.
            //
            // If there's any reason to be concerned about the temporary
            // item being GC'd, it should be taken care of by the implicit
            // protection from the Do Stack.  (e.g. if it contains a function
            // that gets evaluated it will wind up in f->func, if it's a
            // GROUP! or PATH!-containing-GROUP! it winds up in f->array...)
            //
            f->pending = f->value; // may be END marker for next fetch

            // Since the evaluation result is a REBVAL and not a RELVAL, it
            // is specific.  This means the `f->specifier` (which can only
            // specify values from the source array) won't ever be applied
            // to it, since it only comes into play for IS_RELATIVE values.
            //
            f->value = const_KNOWN(&f->cell.eval);
            f->eval_type = Eval_Table[VAL_TYPE(f->value)];
            goto reevaluate; // we don't move index!
        }

    //==////////////////////////////////////////////////////////////////==//
    //
    // FUNCTION! NORMAL ARGUMENT FULFILLMENT PROCESS
    //
    //==////////////////////////////////////////////////////////////////==//

        // We assume you can enumerate both the formal parameters (in the
        // spec) and the actual arguments (in the call frame) using pointer
        // incrementation, that they are both terminated by END, and
        // that there are an equal number of values in both.

        Push_Or_Alloc_Args_For_Underlying_Func(f); // sets f's func, param, arg

        FETCH_NEXT_ONLY_MAYBE_END(f); // overwrites f->value

    do_function_arglist_in_progress:

        // Now that we have extracted f->func, we do not have to worry that
        // f->value might have lived in f->cell.eval.  We can't overwrite
        // f->out in case that is holding the first argument to an infix
        // function, so f->cell.eval gets used for temporary evaluations.

        assert(f->eval_type == ET_FUNCTION);

        // The f->out slot is guarded while a function is gathering its
        // arguments.  It cannot contain garbage, so it must either be END
        // or a lookback's first argument (which can also be END).
        //
        assert(IS_END(f->out) || f->lookback);

        // If a function doesn't want to act as an argument to a function
        // call or an assignment (e.g. `x: print "don't do this"`) we can
        // stop it by looking at the frame above.  Note that if a function
        // frame is running but not fulfilling arguments, that just means
        // that this is being used in the implementation.
        //
        // Must be positioned here to apply to infix, and also so that the
        // f->param field is initialized (checked by error machinery)
        //
        if (GET_VAL_FLAG(FUNC_VALUE(f->func), FUNC_FLAG_PUNCTUATES) && f->prior)
            switch (f->prior->eval_type) {
            case ET_FUNCTION:
                if (Is_Function_Frame_Fulfilling(f->prior))
                    fail (Error_Punctuator_Hit(f));
                break;
            case ET_SET_PATH:
            case ET_SET_WORD:
                fail (Error_Punctuator_Hit(f));
            }

        // `10 = add 5 5` is `true`
        // `add 5 5 = 10` is `** Script error: expected logic! not integer!`
        //
        // `5 + 5 = 10` is `true`
        // `10 = 5 + 5` is `** Script error: expected logic! not integer!`
        //
        // We may consume the `lookahead` parameter, but if we *were* looking
        // ahead then it suppresses lookahead on all evaluated arguments.
        // Need a separate variable to track it.
        //
        REBUPT lookahead_flags; // `goto finished` would cross if initialized
        lookahead_flags =
            f->lookback ? DO_FLAG_NO_LOOKAHEAD : DO_FLAG_LOOKAHEAD;

        // "not a refinement arg, evaluate normally", won't be modified
        f->refine = m_cast(REBVAL*, BAR_VALUE);

    //==////////////////////////////////////////////////////////////////==//
    //
    // FUNCTION! NORMAL ARGUMENT FULFILLMENT LOOP
    //
    //==////////////////////////////////////////////////////////////////==//

        // This loop goes through the parameter and argument slots.  Based on
        // the parameter type, it may be necessary to "consume" an expression
        // from values that come after the invokation point.  But not all
        // params will consume arguments for all calls.  See notes below.
        //
        // For this one body of code to be able to handle both function
        // specialization and ordinary invocation, the void type is used as
        // a signal to have "unspecialized" behavior.  Hence a normal call
        // just pre-fills all the args with void--which will be overwritten
        // during the argument fulfillment process (unless they turn out to
        // be optional in the invocation).
        //
        //
        // To get around that, there's a trick.  An out-of-order refinement
        // makes a note in the stack about a parameter and arg position that
        // it sees that it will need to come back to.  It pokes those two
        // pointers into extra space in the refinement's word on the stack,
        // since that word isn't using its binding.  See WORD_FLAG_PICKUP for
        // the type of WORD! that is used to implement this.

        enum Reb_Param_Class pclass; // gotos would cross it if inside loop

        REBOOL doing_pickups; // case label would cross it if initialized
        doing_pickups = FALSE;

        for (; NOT_END(f->param); ++f->param, ++f->arg) {
            pclass = VAL_PARAM_CLASS(f->param);

    //=//// A /REFINEMENT ARG /////////////////////////////////////////////=//

            // Refinements are checked for first for a reason.  This is to
            // short-circuit based on the `doing_pickups` flag before redoing
            // fulfillments on arguments that have already been handled.
            //
            // The reason an argument might have already been handled is
            // because refinements have to reach back and be revisited after
            // the original parameter walk.  They can't be fulfilled in a
            // single pass because these two calls mean different things:
            //
            //     foo: func [a /b c /d e] [...]
            //
            //     foo/b/d (1 + 2) (3 + 4) (5 + 6)
            //     foo/d/b (1 + 2) (3 + 4) (5 + 6)
            //
            // The order of refinements in the definition (b d) might not match
            // what order the refinements are invoked in the path.  This means
            // the "visitation order" of the parameters while walking across
            // parameters in the array might not match the "consumption order"
            // of the expressions that are being fetched from the callsite.
            // Hence refinements are targeted to be revisited by "pickups"
            // after the initial parameter walk.

            if (pclass == PARAM_CLASS_REFINEMENT) {

                if (doing_pickups) {
                    f->param = END_CELL; // !Is_Function_Frame_Fulfilling
                    break;
                }

                if (NOT(Specialized_Arg(f->arg))) {

    //=//// UNSPECIALIZED REFINEMENT SLOT (no consumption) ////////////////=//

                    if (f->dsp_orig == DSP) { // no refinements left on stack
                        SET_FALSE(f->arg);
                        f->refine = BLANK_VALUE; // "don't consume args, ever"
                        goto continue_arg_loop;
                    }

                    f->refine = DS_TOP;

                    if (
                        VAL_WORD_SYM(f->refine)
                        == SYMBOL_TO_CANON(VAL_TYPESET_SYM(f->param)) // #2258
                    ) {
                        DS_DROP; // we're lucky: this was next refinement used

                        SET_TRUE(f->arg); // marks refinement used
                        f->refine = f->arg; // "consume args (can be revoked)"
                        goto continue_arg_loop;
                    }

                    --f->refine; // not lucky: if in use, this is out of order

                    for (; f->refine > DS_AT(f->dsp_orig); --f->refine) {
                        if (
                            VAL_WORD_SYM(f->refine) // canonized when pushed
                            == SYMBOL_TO_CANON(
                                VAL_TYPESET_SYM(f->param) // #2258
                            )
                        ) {
                            // The call uses this refinement but we'll have to
                            // come back to it when the expression index to
                            // consume lines up.  Make a note of the param
                            // and arg and poke them into the stack WORD!.
                            //
                            UNBIND_WORD(f->refine);
                            SET_VAL_FLAG(f->refine, WORD_FLAG_PICKUP);
                            f->refine->payload.any_word.place.pickup.param
                                = f->param;
                            f->refine->payload.any_word.place.pickup.arg
                                = f->arg;

                            SET_TRUE(f->arg); // marks refinement used
                            // "consume args later" (promise not to change)
                            f->refine = m_cast(REBVAL*, VOID_CELL);
                            goto continue_arg_loop;
                        }
                    }

                    // Wasn't in the path and not specialized, so not present
                    //
                    SET_FALSE(f->arg);
                    f->refine = BLANK_VALUE; // "don't consume args, ever"
                    goto continue_arg_loop;
                }

    //=//// SPECIALIZED REFINEMENT SLOT (no consumption) //////////////////=//

                if (args_evaluate && IS_QUOTABLY_SOFT(f->arg)) {
                    //
                    // Needed for `(copy [1 2 3])`, active specializations

                    if (EVAL_VALUE_THROWS(SINK(&f->cell.eval), f->arg)) {
                        *f->out = *KNOWN(&f->cell.eval);
                        Abort_Function_Args_For_Frame(f);
                        goto finished;
                    }

                    *f->arg = *KNOWN(&f->cell.eval);
                }

                if (IS_VOID(f->arg)) {
                    SET_FALSE(f->arg);
                    f->refine = BLANK_VALUE; // handled same as false
                    goto continue_arg_loop;
                }

                if (!IS_LOGIC(f->arg))
                    fail (Error_Non_Logic_Refinement(f));

                if (IS_CONDITIONAL_TRUE(f->arg))
                    f->refine = f->arg; // remember so we can revoke!
                else
                    f->refine = BLANK_VALUE; // (read-only)

                goto continue_arg_loop;
            }

    //=//// "PURE" LOCAL: ARG /////////////////////////////////////////////=//

            // This takes care of locals, including "magic" RETURN and LEAVE
            // cells that need to be pre-filled.  Notice that although the
            // parameter list may have RETURN and LEAVE slots, that parameter
            // list may be reused by an "adapter" or "hijacker" which would
            // technically happen *before* the "magic" (if the user had
            // implemented the definitinal returns themselves inside the
            // function body).  Hence they are not always filled.
            //
            // Also note that while it might seem intuitive to take care of
            // these "easy" fills before refinement checking--checking for
            // refinement pickups ending prevents double-doing this work.

            switch (pclass) {
            case PARAM_CLASS_LOCAL:
                SET_VOID(f->arg); // faster than checking bad specializations
                goto continue_arg_loop;

            case PARAM_CLASS_RETURN:
                assert(VAL_TYPESET_CANON(f->param) == SYM_RETURN);

                if (!GET_VAL_FLAG(FUNC_VALUE(f->func), FUNC_FLAG_RETURN)) {
                    SET_VOID(f->arg);
                    goto continue_arg_loop;
                }

                *f->arg = *NAT_VALUE(return);

                if (f->varlist) // !!! in specific binding, always for Plain
                    f->arg->payload.function.exit_from = f->varlist;
                else
                    f->arg->payload.function.exit_from
                        = FUNC_PARAMLIST(f->func);
                goto continue_arg_loop;

            case PARAM_CLASS_LEAVE:
                assert(VAL_TYPESET_CANON(f->param) == SYM_LEAVE);

                if (!GET_VAL_FLAG(FUNC_VALUE(f->func), FUNC_FLAG_LEAVE)) {
                    SET_VOID(f->arg);
                    goto continue_arg_loop;
                }

                *f->arg = *NAT_VALUE(return);

                if (f->varlist) // !!! in specific binding, always for Plain
                    f->arg->payload.function.exit_from = f->varlist;
                else
                    f->arg->payload.function.exit_from
                        = FUNC_PARAMLIST(f->func);
                goto continue_arg_loop;
            }

    //=//// IF COMING BACK TO REFINEMENT ARGS LATER, MOVE ON FOR NOW //////=//

            if (IS_VOID(f->refine)) goto continue_arg_loop;

    //=//// SPECIALIZED ARG (already filled, so does not consume) /////////=//

            if (Specialized_Arg(f->arg)) {

                // The arg came preloaded with a value to use.  Handle soft
                // quoting first, in case arg needs evaluation.

                if (args_evaluate && IS_QUOTABLY_SOFT(f->arg)) {

                    if (EVAL_VALUE_THROWS(SINK(&f->cell.eval), f->arg)) {
                        *f->out = *KNOWN(&f->cell.eval);
                        Abort_Function_Args_For_Frame(f);
                        goto finished;
                    }

                    *f->arg = *KNOWN(&f->cell.eval);
                }

                // Varargs are special, because the type checking doesn't
                // actually check the type of the parameter--it's always
                // a VARARGS!.  Also since the "types accepted" are a lie
                // (an [integer! <...>] takes VARARGS!, not INTEGER!) then
                // an "honest" parameter has to be made to give the error.
                //
                if (
                    IS_CONDITIONAL_TRUE(f->refine) // not unused or revoking
                    && GET_VAL_FLAG(f->param, TYPESET_FLAG_VARIADIC)
                ) {
                    if (!IS_VARARGS(f->arg)) {
                        REBVAL honest_param;
                        Val_Init_Typeset(
                            &honest_param,
                            FLAGIT_KIND(REB_VARARGS), // *actually* expected...
                            VAL_TYPESET_SYM(f->param)
                        );

                        fail (Error_Arg_Type(
                            FRM_LABEL(f), &honest_param, VAL_TYPE(f->arg))
                        );
                    }

                    // !!! Passing the varargs through directly does not
                    // preserve the type checking or symbol.  This suggests
                    // that even array-based varargs frames should have
                    // an optional frame and parameter.  Consider specializing
                    // variadics to be TBD until the type checking issue
                    // is sorted out.
                    //
                    assert(FALSE);

                    goto continue_arg_loop;
                }

                goto check_arg; // normal checking, handles errors also
            }

    //=//// IF UNSPECIALIZED ARG IS INACTIVE, SET VOID AND MOVE ON ////////=//

            // Unspecialized arguments that do not consume do not need any
            // further processing or checking.  void will always be fine.
            //
            if (IS_BLANK(f->refine)) { // FALSE if revoked, and still evaluates
                assert(NOT(Specialized_Arg(f->arg)));
                SET_VOID(f->arg);
                goto continue_arg_loop;
            }

    //=//// VARIADIC ARG (doesn't consume anything *yet*) /////////////////=//

            // Evaluation argument "hook" parameters (marked in MAKE FUNCTION!
            // by a `[[]]` in the spec, and in FUNC by `<...>`).  They point
            // back to this call through a reified FRAME!, and are able to
            // consume additional arguments during the function run.
            //
            if (GET_VAL_FLAG(f->param, TYPESET_FLAG_VARIADIC)) {
                //
                // !!! Can EVAL/ONLY be supported by variadics?  What would
                // it mean?  It generally means that argument fulfillment will
                // ignore the quoting settings, if that's all it is then
                // the varargs needs to have this flag communicated...but
                // then should it function variadically anyway?
                //
                assert(args_evaluate);

                VAL_RESET_HEADER(f->arg, REB_VARARGS);

                // Note that this varlist is to a context that is not ready
                // to be shared with the GC yet (bad cells in any unfilled
                // arg slots).  To help cue that it's not necessarily a
                // completed context yet, we store it as an array type.
                //
                Context_For_Frame_May_Reify_Core(f);
                f->arg->payload.varargs.feed.varlist = f->varlist;

                f->arg->payload.varargs.param = const_KNOWN(f->param); // check
                f->arg->payload.varargs.arg = f->arg; // linkback, might change
                goto continue_arg_loop;
            }

    //=//// AFTER THIS, PARAMS CONSUME FROM CALLSITE IF NOT APPLY ////////=//

            assert(NOT(Specialized_Arg(f->arg)));

    //=//// ERROR ON END MARKER, BAR! IF APPLICABLE //////////////////////=//

            if (IS_END(f->value)) {
                if (!GET_VAL_FLAG(f->param, TYPESET_FLAG_ENDABLE))
                    fail (Error_No_Arg(FRM_LABEL(f), f->param));

                SET_VOID(f->arg);
                goto continue_arg_loop;
            }

            // Literal expression barriers cannot be consumed in normal
            // evaluation, even if the argument takes a BAR!.  It must come
            // through non-literal means(e.g. `quote '|` or `first [|]`)
            //
            if (args_evaluate && IS_BAR(f->value)) {
                if (!GET_VAL_FLAG(f->param, TYPESET_FLAG_ENDABLE))
                    fail (Error(RE_EXPRESSION_BARRIER));

                SET_VOID(f->arg);
                goto continue_arg_loop;
            }

    //=//// REGULAR ARG-OR-REFINEMENT-ARG (consumes a DO/NEXT's worth) ////=//

            if (pclass == PARAM_CLASS_NORMAL) {
                if (f->lookback) {
                    f->lookback = FALSE;

                    if (IS_END(f->out)) {
                        if (!GET_VAL_FLAG(f->param, TYPESET_FLAG_ENDABLE))
                            fail (Error_No_Arg(FRM_LABEL(f), f->param));

                        SET_VOID(f->out);
                        goto continue_arg_loop;
                    }

                    *f->arg = *f->out;
                    SET_END(f->out);
                }
                else if (args_evaluate) {
                    DO_NEXT_REFETCH_MAY_THROW(f->arg, f, lookahead_flags);

                    if (THROWN(f->arg)) {
                        *f->out = *f->arg;
                        Abort_Function_Args_For_Frame(f);
                        goto finished;
                    }
                }
                else
                    QUOTE_NEXT_REFETCH(f->arg, f); // no VALUE_FLAG_EVALUATED

                goto check_arg;
            }

    //=//// HARD QUOTED ARG-OR-REFINEMENT-ARG /////////////////////////////=//

            if (pclass == PARAM_CLASS_HARD_QUOTE) {
                if (f->lookback) {
                    f->lookback = FALSE;

                    if (IS_END(f->out)) {
                        if (!GET_VAL_FLAG(f->param, TYPESET_FLAG_ENDABLE))
                            fail (Error_No_Arg(FRM_LABEL(f), f->param));

                        SET_VOID(f->out);
                        goto continue_arg_loop;
                    }

                    if (GET_VAL_FLAG(f->out, VALUE_FLAG_EVALUATED))
                        fail (Error_Lookback_Quote_Too_Late(f));

                    *f->arg = *f->out;
                    SET_END(f->out);
                }
                else
                    QUOTE_NEXT_REFETCH(f->arg, f); // non-VALUE_FLAG_EVALUATED

                goto check_arg;
            }

    //=//// SOFT QUOTED ARG-OR-REFINEMENT-ARG  ////////////////////////////=//

            assert(pclass == PARAM_CLASS_SOFT_QUOTE);

            if (f->lookback) {
                f->lookback = FALSE;

                if (IS_END(f->out)) {
                    if (!GET_VAL_FLAG(f->param, TYPESET_FLAG_ENDABLE))
                        fail (Error_No_Arg(FRM_LABEL(f), f->param));

                    SET_VOID(f->out);
                    goto continue_arg_loop;
                }

                if (GET_VAL_FLAG(f->out, VALUE_FLAG_EVALUATED))
                    fail (Error_Lookback_Quote_Too_Late(f));

                if (IS_SET_WORD(f->out) || IS_SET_PATH(f->out))
                    fail (Error_Lookback_Quote_Set_Soft(f));

                *f->arg = *f->out;
                SET_END(f->out);
            }
            else if (args_evaluate && IS_QUOTABLY_SOFT(f->value)) {
                if (EVAL_VALUE_CORE_THROWS(f->arg, f->value, f->specifier)) {
                    *f->out = *f->arg;
                    Abort_Function_Args_For_Frame(f);
                    goto finished;
                }
                FETCH_NEXT_ONLY_MAYBE_END(f);
            }
            else
                QUOTE_NEXT_REFETCH(f->arg, f); // non-VALUE_FLAG_EVALUATED

    //=//// TYPE CHECKING FOR (MOST) ARGS AT END OF ARG LOOP //////////////=//

        check_arg:

            // Some arguments can be fulfilled and skip type checking or
            // take care of it themselves.  But normal args pass through
            // this code which checks the typeset and also handles it when
            // a void arg signals the revocation of a refinement usage.

            ASSERT_VALUE_MANAGED(f->arg);
            assert(pclass != PARAM_CLASS_REFINEMENT);
            assert(pclass != PARAM_CLASS_LOCAL);

            // See notes on `Reb_Frame.refine` in %sys-do.h for more info.
            //
            assert(
                IS_BLANK(f->refine) || // f->arg is arg to never-used refinment
                IS_LOGIC(f->refine) || // F = revoked, T = used refinement slot
                IS_BAR(f->refine) // f->arg is ordinary function argument
            );

            if (IS_VOID(f->arg)) {
                if (IS_BAR(f->refine)) {
                    //
                    // fall through to check ordinary arg for if <opt> is ok
                }
                else if (IS_CONDITIONAL_FALSE(f->refine)) {
                    //
                    // FALSE means the refinement has already been revoked so
                    // the void is okay.  BLANK! means the refinement was
                    // never in use in the first place.  Don't type check.
                    //
                    goto continue_arg_loop;
                }
                else {
                    assert(IS_LOGIC(f->refine));

                    // We can only revoke the refinement if this is the 1st
                    // refinement arg.  If it's a later arg, then the first
                    // didn't trigger revocation, or refine wouldn't be WORD!
                    //
                    if (f->refine + 1 != f->arg)
                        fail (Error_Bad_Refine_Revoke(f));

                    SET_FALSE(f->refine);
                    // won't be modified
                    f->refine = m_cast(REBVAL*, FALSE_VALUE);
                    goto continue_arg_loop; // don't type check for optionality
                }
            }
            else {
                // If the argument is set, then the refinement shouldn't be
                // in a revoked or unused state.
                //
                if (IS_CONDITIONAL_FALSE(f->refine))
                    fail (Error_Bad_Refine_Revoke(f));
            }

            Type_Check_Arg_For_Param_May_Fail(f);

        continue_arg_loop: // `continue` might bind to the wrong scope
            NOOP;
        }

        // There may have been refinements that were skipped because the
        // order of definition did not match the order of usage.  They were
        // left on the stack with a pointer to the `param` and `arg` after
        // them for later fulfillment.
        //
        if (DSP != f->dsp_orig) {
            if (!GET_VAL_FLAG(DS_TOP, WORD_FLAG_PICKUP)) {
                //
                // The walk through the arguments didn't fill in any
                // information for this word, so it was either a duplicate of
                // one that was fulfilled or not a refinement the function
                // has at all.
                //
                fail (Error(RE_BAD_REFINE, DS_TOP));
            }
            f->param = DS_TOP->payload.any_word.place.pickup.param;
            f->refine = f->arg = DS_TOP->payload.any_word.place.pickup.arg;
            assert(IS_LOGIC(f->refine) && VAL_LOGIC(f->refine));
            DS_DROP;
            doing_pickups = TRUE;
            goto continue_arg_loop; // leaves refine, but bumps param+arg
        }

    #if !defined(NDEBUG)
        if (GET_VAL_FLAG(FUNC_VALUE(f->func), FUNC_FLAG_LEGACY))
            Legacy_Convert_Function_Args(f); // BLANK!+NONE! vs. FALSE+UNSET!
    #endif

    //==////////////////////////////////////////////////////////////////==//
    //
    // FUNCTION! ARGUMENTS NOW GATHERED, DISPATCH CALL
    //
    //==////////////////////////////////////////////////////////////////==//

        assert(DSP == f->dsp_orig);

        // Now we reset arg to the head of the argument list.  This provides
        // fast access for the callees, so they don't have to go through an
        // indirection further than just f->arg to get it.
        //
        // !!! When hybrid frames are introduced, review the question of
        // which pointer "wins".  Might more than one be used?
        //
        if (f->varlist) {
            //
            // Technically speaking we would only be *required* at this point
            // to manage the varlist array if we've poked it into a vararg
            // as a context.  But specific binding will always require a
            // context available, so no point in optimizing here.
            //
            Context_For_Frame_May_Reify_Managed(f);

            f->arg = CTX_VARS_HEAD(AS_CONTEXT(f->varlist));
        }
        else {
            // We cache the stackvars data pointer in the stack allocated
            // case.  Note that even if the frame becomes "reified" as a
            // context, the data pointer will be the same over the stack
            // level lifetime.
            //
            f->arg = &f->stackvars[0];
            assert(CHUNK_FROM_VALUES(f->arg) == TG_Top_Chunk);
        }

        if (Trace_Flags) Trace_Func(FRM_LABEL(f), FUNC_VALUE(f->func));

        // The garbage collector may run when we call out to functions, so
        // we have to be sure that the frame fields are something valid.
        // f->param cannot be a typeset while the function is running, because
        // typesets are used as a signal to Is_Function_Frame_Fulfilling.
        //
        f->cell.subfeed = NULL;

    execute_func:
        assert(IS_END(f->param));
        // refine can be anything.
        assert(
            IS_END(f->value)
            || (f->flags & DO_FLAG_VA_LIST)
            || IS_VALUE_IN_ARRAY(f->source.array, f->value)
        );

        if (Trace_Flags) Trace_Func(FRM_LABEL(f), FUNC_VALUE(f->func));

        // The out slot needs initialization for GC safety during the function
        // run.  Choosing an END marker should be legal because places that
        // you can use as output targets can't be visible to the GC (that
        // includes argument arrays being fulfilled).  This offers extra
        // perks, because it means a recycle/torture will catch you if you
        // try to Do_Core into movable memory...*and* a native can tell if it
        // has written the output slot yet or not (e.g. WHILE's /? refinement).
        //
        assert(IS_END(f->out));

        // Any of the below may return f->out as THROWN().  (Note: this used
        // to do `Eval_Natives++` in the native dispatcher, which now fades
        // into the background.)  The dispatcher may also push functions to
        // the data stack which will be used to process the return result.
        //
        REBNAT dispatcher; // goto would cross initialization
        dispatcher = FUNC_DISPATCHER(f->func);
        switch (dispatcher(f)) {
        case R_OUT: // put sequentially in switch() for jump-table optimization
            break;

        case R_OUT_IS_THROWN:
            assert(THROWN(f->out));
            break;

        case R_OUT_TRUE_IF_WRITTEN:
            if (IS_END(f->out))
                SET_FALSE(f->out);
            else
                SET_TRUE(f->out);
            break;

        case R_OUT_VOID_IF_UNWRITTEN:
            if (IS_END(f->out))
                SET_VOID(f->out);
            break;

        case R_BLANK:
            SET_BLANK(f->out);
            break;

        case R_VOID:
            SET_VOID(f->out);
            break;

        case R_TRUE:
            SET_TRUE(f->out);
            break;

        case R_FALSE:
            SET_FALSE(f->out);
            break;

        case R_REDO:
            //
            // This instruction represents the idea that it is desired to
            // run the f->func again.  The dispatcher may have changed the
            // value of what f->func is, for instance.
            //
            SET_END(f->out);
            goto execute_func;

        default:
            assert(FALSE);
        }

        assert(f->eval_type == ET_FUNCTION); // shouldn't have changed
        assert(NOT_END(f->out)); // should have overwritten

    //==////////////////////////////////////////////////////////////////==//
    //
    // FUNCTION! CATCHING OF EXITs (includes catching RETURN + LEAVE)
    //
    //==////////////////////////////////////////////////////////////////==//

        if (THROWN(f->out)) {
            if (!IS_FUNCTION(f->out) || VAL_FUNC(f->out) != NAT_FUNC(exit)) {
                //
                // Do_Core only catches "definitional exits" to current frame
                //
                Abort_Function_Args_For_Frame(f);
                goto finished;
            }

            ASSERT_ARRAY(VAL_FUNC_EXIT_FROM(f->out));

            if (VAL_FUNC_EXIT_FROM(f->out) == FUNC_PARAMLIST(f->func)) {
                //
                // The most recent instance of a function on the stack (if
                // any) will catch a FUNCTION! style exit.
                //
                CATCH_THROWN(f->out, f->out);
            }
            else if (VAL_FUNC_EXIT_FROM(f->out) == f->varlist) {
                //
                // This identifies an exit from a *specific* function
                // invocation.  We'll only match it if we have a reified
                // frame context.  (Note f->varlist may be null here.)
                //
                CATCH_THROWN(f->out, f->out);
            }
            else {
                Abort_Function_Args_For_Frame(f);
                goto finished; // stay THROWN and try to exit frames above...
            }
        }

    //==////////////////////////////////////////////////////////////////==//
    //
    // FUNCTION! CALL COMPLETION (Type Check Result, Throw If Needed)
    //
    //==////////////////////////////////////////////////////////////////==//

        Drop_Function_Args_For_Frame(f);

        // Here we know the function finished and did not throw or exit.  If
        // it has a definitional return we need to type check it--and if it
        // has punctuates we have to squash whatever the last evaluative
        // result was and return no value

        if (GET_VAL_FLAG(FUNC_VALUE(f->func), FUNC_FLAG_PUNCTUATES)) {
            SET_VOID(f->out);
        }
        else if (GET_VAL_FLAG(FUNC_VALUE(f->func), FUNC_FLAG_RETURN)) {
            f->param = FUNC_PARAM(f->func, FUNC_NUM_PARAMS(f->func));
            assert(VAL_TYPESET_CANON(f->param) == SYM_RETURN);

            // The type bits of the definitional return are not applicable
            // to the `return` word being associated with a FUNCTION!
            // vs. an INTEGER! (for instance).  It is where the type
            // information for the non-existent return function specific
            // to this call is hidden.
            //
            if (!TYPE_CHECK(f->param, VAL_TYPE(f->out)))
                fail (Error_Arg_Type(
                    SYM_RETURN, f->param, VAL_TYPE(f->out))
                );
        }

        // Calling a function counts as an evaluation *unless* it's quote or
        // semiquote (the generic means for fooling the semiquote? test)
        //
        if (f->func == NAT_FUNC(semiquote) || f->func == NAT_FUNC(quote))
            CLEAR_VAL_FLAG(f->out, VALUE_FLAG_EVALUATED);
        else
            SET_VAL_FLAG(f->out, VALUE_FLAG_EVALUATED);

        // If we have functions pending to run on the outputs, then do so.
        //
        while (DSP != f->dsp_orig) {
            assert(IS_FUNCTION(DS_TOP));

            f->eval_type = ET_INERT; // function is over, so don't involve GC

            REBVAL temp = *f->out; // better safe than sorry, for now?
            if (Apply_Only_Throws(
                f->out, DS_TOP, &temp, END_CELL
            )) {
                goto finished;
            }

            DS_DROP;
        }

        assert(DSP == f->dsp_orig);

        if (Trace_Flags)
            Trace_Return(FRM_LABEL(f), f->out);

        CLEAR_FRAME_SYM(f);
        break;

//==//////////////////////////////////////////////////////////////////////==//
//
// [ ??? ] => panic
//
// All types must match a case in the switch.  This shouldn't happen.
//
//==//////////////////////////////////////////////////////////////////////==//

    default:
        panic (Error(RE_MISC));
    }

    //==////////////////////////////////////////////////////////////////==//
    //
    // END MAIN SWITCH STATEMENT
    //
    //==////////////////////////////////////////////////////////////////==//

    assert(!THROWN(f->out)); // should have jumped to exit sooner

    if (IS_END(f->value))
        goto finished;

    assert(!IS_END(f->value));

    f->eval_type = Eval_Table[VAL_TYPE(f->value)];

    if (f->flags & DO_FLAG_NO_LOOKAHEAD) {
        //
        // Don't do infix lookahead if asked *not* to look.  It's not typical
        // to be requested by callers (there is already no infix lookahead
        // by using DO_FLAG_EVAL_ONLY, so those cases don't need to ask.)
        //
        // However, recursive cases of DO disable infix dispatch if they are
        // currently processing an infix operation.  The currently processing
        // operation is thus given "higher precedence" by this disablement.

        f->gotten = NULL; // signal to ET_WORD and ET_GET_WORD to do a get
    }
    else if (f->eval_type == ET_WORD) {
        REBOOL lookback_leftover = f->lookback;

        // Don't overwrite f->value (if this just a DO/NEXT and it's not
        // infix, we might need to hold it at its position.)
        //
        f->gotten = Get_Var_Core(
            &f->lookback,
            f->value,
            f->specifier,
            GETVAR_READ_ONLY
        );

    //=//// DO/NEXT WON'T RUN MORE CODE UNLESS IT'S AN INFIX FUNCTION /////=//

        if (NOT(f->lookback) && NOT(f->flags & DO_FLAG_TO_END))
            goto finished;

    //=//// IT'S INFIX OR WE'RE DOING TO THE END...DISPATCH LIKE WORD /////=//

        START_NEW_EXPRESSION(f);

        if (!IS_FUNCTION(f->gotten)) // <-- DO_COUNT_BREAKPOINT landing spot
            goto do_word_in_value_with_gotten;

        f->eval_type = ET_FUNCTION;
        SET_FRAME_SYM(f, VAL_WORD_SYM(f->value));

        // If a previous "infix" call had 0 arguments and didn't consume
        // the value before it, assume that means it's a 0-arg barrier
        // that does not want to be the left hand side of another infix.
        //
        if (f->lookback) {
            if (lookback_leftover)
                fail (Error_Infix_Left_Arg_Prohibited(f));
        }
        else
            SET_END(f->out);

        goto do_function_in_gotten;
    }
    else
        f->gotten = NULL; // signal to ET_GET_WORD it needs to fetch for itself

    // Continue evaluating rest of block if not just a DO/NEXT
    //
    if (f->flags & DO_FLAG_TO_END)
        goto do_next;

finished:

#if !defined(NDEBUG)
    Do_Core_Exit_Checks_Debug(f); // will get called unless a fail() longjmps
#endif

    // Restore the top of stack (if there is a fail() and associated longjmp,
    // this restoration will be done by the Drop_Trap helper.)
    //
    DROP_CALL(f);

    // All callers must inspect for THROWN(f->out), and most should also
    // inspect for IS_END(f->value)
}


//==//////////////////////////////////////////////////////////////////////==//
//
// DEBUG-BUILD ONLY CHECKS
//
//==//////////////////////////////////////////////////////////////////////==//
//
// Due to the length of Do_Core() and how many debug checks it already has,
// three debug-only routines are separated out:
//
// * Do_Core_Entry_Checks_Debug() runs once at the beginning of a Do_Core()
//   call.  It verifies that the fields of the frame the caller has to
//   provide have been pre-filled correctly, and snapshots bits of the
//   interpreter state that are supposed to "balance back to zero" by the
//   end of a run (assuming it completes, and doesn't longjmp from fail()ing)
//
// * Do_Core_Expression_Checks_Debug() runs before each full "expression"
//   is evaluated, e.g. before each DO/NEXT step.  It makes sure the state
//   balanced completely--so no DS_PUSH that wasn't balanced by a DS_POP
//   or DS_DROP (for example).  It also trashes variables in the frame which
//   might accidentally carry over from one step to another, so that there
//   will be a crash instead of a casual reuse.
//
// * Do_Core_Exit_Checks_Debug() runs if the Do_Core() call makes it to the
//   end without a fail() longjmping out from under it.  It also checks to
//   make sure the state has balanced, and that the return result is
//   consistent with the state being returned.
//
// Because none of these routines are in the release build, they cannot have
// any side-effects that affect the interpreter's ordinary operation.
//

#if !defined(NDEBUG)

static void Do_Core_Entry_Checks_Debug(struct Reb_Frame *f)
{
    // Though we can protect the value written into the target pointer 'out'
    // from GC during the course of evaluation, we can't protect the
    // underlying value from relocation.  Technically this would be a problem
    // for any series which might be modified while this call is running, but
    // most notably it applies to the data stack--where output used to always
    // be returned.
    //
    // !!! A non-contiguous data stack which is not a series is a possibility.
    //
#ifdef STRESS_CHECK_DO_OUT_POINTER
    REBSER *containing = Try_Find_Containing_Series_Debug(f->out);

    if (containing) {
        if (GET_SER_FLAG(series, SERIES_FLAG_FIXED_SIZE)) {
            //
            // Currently it's considered OK to be writing into a fixed size
            // series, for instance the durable portion of a function's
            // arg storage.  It's assumed that the memory will not move
            // during the course of the argument evaluation.
            //
        }
        else {
            Debug_Fmt("Request for ->out location in movable series memory");
            assert(FALSE);
        }
    }
#else
    assert(!IN_DATA_STACK_DEBUG(f->out));
#endif

    // The caller must preload ->value with the first value to process.  It
    // may be resident in the array passed that will be used to fetch further
    // values, or it may not.
    //
    assert(f->value);

    if (f->eval_type == ET_FUNCTION && f->gotten != NULL)
        assert(f->label_sym != SYM_0 && f->label_str != NULL);
    else {
        f->label_sym = SYM_0;
        f->label_str = NULL;
    }

    // All callers should ensure that the type isn't an END marker before
    // bothering to invoke Do_Core().
    //
    assert(NOT_END(f->value));

    // The DO_FLAGs were decided to come in pairs for clarity, to make sure
    // that each callsite of the core routines was clear on what it was
    // asking for.  This may or may not be overkill long term, but helps now.
    //
    assert(
        LOGICAL(f->flags & DO_FLAG_NEXT)
        != LOGICAL(f->flags & DO_FLAG_TO_END)
    );
    assert(
        LOGICAL(f->flags & DO_FLAG_LOOKAHEAD)
        != LOGICAL(f->flags & DO_FLAG_NO_LOOKAHEAD)
    );
    assert(
        LOGICAL(f->flags & DO_FLAG_ARGS_EVALUATE)
        != LOGICAL(f->flags & DO_FLAG_NO_ARGS_EVALUATE)
    );
}


//
// The iteration preamble takes care of clearing out variables and preparing
// the state for a new "/NEXT" evaluation.  It's a way of ensuring in the
// debug build that one evaluation does not leak data into the next, and
// making the code shareable allows code paths that jump to later spots
// in the switch (vs. starting at the top) to reuse the work.
//
static REBUPT Do_Core_Expression_Checks_Debug(struct Reb_Frame *f) {
    //
    // There shouldn't have been any "accumulated state", in the sense that
    // we should be back where we started in terms of the data stack, the
    // mold buffer position, the outstanding manual series allocations, etc.
    //
    ASSERT_STATE_BALANCED(&f->state);

    // Once a throw is started, no new expressions may be evaluated until
    // that throw gets handled.
    //
    assert(IS_TRASH_DEBUG(&TG_Thrown_Arg));

    // If running the evaluator, then this frame should be the topmost on the
    // frame stack.
    //
    assert(f == FS_TOP);

    // We checked for END when we entered Do_Core() and short circuited
    // that, but if we're running DO_FLAG_TO_END then the catch for that is
    // an index check.  We shouldn't go back and `do_at_index` on an end!
    //
    // !!! are there more rules for the locations value can't point to?
    //
    assert(f->value && NOT_END(f->value) && f->value != f->out);
    assert(NOT(THROWN(f->value)));

    // The eval_type is expected to be calculated already, because it's an
    // opportunity for the caller to decide pushing a frame is not necessary
    // (e.g. if it's ET_INERT).  Hence it is only set at the end of the loop.
    //
    // Special exemption is made when f->gotten is a function and the symbol
    // has been set from a WORD!, because f->value is still that word.
    //
    assert(
        f->eval_type == Eval_Table[VAL_TYPE(f->value)]
        || (f->lookback && f->eval_type == ET_FUNCTION && IS_WORD(f->value))
    );

    if (f->flags & DO_FLAG_VA_LIST)
        assert(f->index == TRASHED_INDEX);
    else {
        assert(
            f->index != TRASHED_INDEX
            && f->index != END_FLAG
            && f->index != THROWN_FLAG
            && f->index != VA_LIST_FLAG
        ); // END, THROWN, VA_LIST only used by wrappers
    }

    // Make sure `eval` is trash in debug build if not doing a `reevaluate`.
    // It does not have to be GC safe (for reasons explained below).  We
    // also need to reset evaluation to normal vs. a kind of "inline quoting"
    // in case EVAL/ONLY had enabled that.
    //
    // Note that since the cell lives in a union, it cannot have a constructor
    // so the automatic mark of writable that most REBVALs get could not
    // be used.  Since it's a raw RELVAL, we have to explicitly mark writable.
    //
    // Also, the eval's cell bits live in a union that can wind up getting used
    // for other purposes.  Hence the writability must be re-indicated here
    // before the slot is used each time.
    //
    if (f->value != &(f->cell.eval)) {
        INIT_CELL_WRITABLE_IF_DEBUG(&(f->cell.eval));
        SET_TRASH_IF_DEBUG(&(f->cell.eval));
    }

    // The value we are processing should not be THROWN() and any series in
    // it should be under management by the garbage collector.
    //
    // !!! THROWN() bit on individual values is in the process of being
    // deprecated, in favor of the evaluator being in a "throwing state".
    //
    assert(!THROWN(f->value));
    ASSERT_VALUE_MANAGED(f->value);
    assert(f->value != f->out);

    // Trash call variables in debug build to make sure they're not reused.
    // Note that this call frame will *not* be seen by the GC unless it gets
    // chained in via a function execution, so it's okay to put "non-GC safe"
    // trash in at this point...though by the time of that call, they must
    // hold valid values.
    //
    f->func = NULL;

    if (f->eval_type == ET_FUNCTION && f->gotten != NULL)
        assert(f->label_sym != SYM_0 && f->label_str != NULL);
    else
        assert(f->label_sym == SYM_0 && f->label_str == NULL);

    f->param = cast(const RELVAL*, 0xDECAFBAD);
    f->arg = cast(REBVAL*, 0xDECAFBAD);
    f->refine = cast(REBVAL*, 0xDECAFBAD);

    f->exit_from = cast(REBARR*, 0xDECAFBAD);

    f->stackvars = cast(REBVAL*, 0xDECAFBAD);
    f->varlist = cast(REBARR*, 0xDECAFBAD);

    f->func = cast(REBFUN*, 0xDECAFBAD);

    // Mutate va_list sources into arrays at fairly random moments in the
    // debug build.  It should be able to handle it at any time.
    //
    if ((f->flags & DO_FLAG_VA_LIST) && SPORADICALLY(50)) {
        const REBOOL truncated = TRUE;
        Reify_Va_To_Array_In_Frame(f, truncated);
    }

    // We bound the count at the max unsigned 32-bit, since otherwise it would
    // roll over to zero and print a message that wasn't asked for, which
    // is annoying even in a debug build.  (It's actually a REBUPT, so this
    // wastes possible bits in the 64-bit build, but there's no MAX_REBUPT.)
    //
    if (TG_Do_Count < MAX_U32) {
        f->do_count = ++TG_Do_Count;
        if (f->do_count == DO_COUNT_BREAKPOINT) {
            REBVAL dump;
            COPY_VALUE(&dump, f->value, f->specifier);

            PROBE_MSG(&dump, "DO_COUNT_BREAKPOINT hit at...");

            if (f->flags & DO_FLAG_VA_LIST) {
                //
                // NOTE: This reifies the va_list in the frame, and hence has
                // side effects.  It may need to be commented out if the
                // problem you are trapping with DO_COUNT_BREAKPOINT was
                // specifically with va_list frame processing.
                //
                const REBOOL truncated = TRUE;
                Reify_Va_To_Array_In_Frame(f, truncated);
            }

            if (f->pending && NOT_END(f->pending)) {
                assert(IS_SPECIFIC(f->pending));
                PROBE_MSG(
                    const_KNOWN(f->pending),
                    "EVAL in progress, so next will be..."
                );
            }

            if (IS_END(f->value)) {
                Debug_Fmt("...then at end of array");
            }
            else {
                REBVAL dump;
                Val_Init_Series_Index_Core(
                    &dump,
                    REB_BLOCK,
                    ARR_SERIES(f->source.array),
                    cast(REBCNT, f->index),
                    f->specifier
                );

                PROBE_MSG(&dump, "...then this array for the next input");
            }
        }
    }

    return f->do_count;
}


static void Do_Core_Exit_Checks_Debug(struct Reb_Frame *f) {
    //
    // Make sure the data stack, mold stack, and other structures didn't
    // accumulate any state over the course of the run.
    //
    ASSERT_STATE_BALANCED(&f->state);

    if (f->flags & DO_FLAG_VA_LIST)
        assert(f->index == TRASHED_INDEX);
    else {
        assert(
            f->index != TRASHED_INDEX
            && f->index != END_FLAG
            && f->index != THROWN_FLAG
            && f->index != VA_LIST_FLAG
        ); // END, THROWN, VA_LIST only used by wrappers
    }

    if (NOT_END(f->value) && NOT(f->flags & DO_FLAG_VA_LIST)) {
        //
        // If we're at the array's end position, then we've prefetched the
        // last value for processing (and not signaled end) but on the
        // next fetch we *will* signal an end.
        //
        assert(
            (f->index <= ARR_LEN(f->source.array))
            || (
                (
                    (f->pending && IS_END(f->pending))
                    || THROWN(f->out)
                )
                && f->index == ARR_LEN(f->source.array) + 1
            )
        );
    }

    if (f->flags & DO_FLAG_TO_END)
        assert(THROWN(f->out) || IS_END(f->value));

    // Function execution should have written *some* actual output value.
    //
    assert(NOT_END(f->out)); // series END marker shouldn't leak out
    assert(!IS_TRASH_DEBUG(f->out));
    assert(VAL_TYPE(f->out) < REB_MAX); // cheap check

    if (NOT(THROWN(f->out))) {
        assert(f->label_sym == SYM_0);
        ASSERT_VALUE_MANAGED(f->out);
    }
}

#endif
