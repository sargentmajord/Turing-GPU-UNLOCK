# Code review checklist

Read this in full before starting a review pass, and check off each item against what you actually did -
not what you intended to do. Every item below exists because this project missed it at least once and the
miss was caught later, not during the review that should have caught it (see PORTING-PLAN.md's
"Post-broadening review" entry, 2026-11-04, for the concrete case this was written from).

## 1. Grep the whole repo, not just the file you changed

After editing anything referenced elsewhere by name - a table, a struct field, a function, a validation
rule, a status claim ("still a stub", "not yet implemented") - grep the entire repo for that name or
concept before calling the change done. Caught misses this way: a doc comment on a function's
*declaration* (separate file/location from its *definition*, which had already been fixed) still
describing the old behavior; a completely separate Python tool with its own independent copy of matching
logic that needed the identical fix the C code got and did not get it automatically.

## 2. Run the full test suite, every time - don't reason about it, run it

Never conclude "the existing tests should still pass" from reading the diff. Run them. This project found
two tests silently asserting stale behavior (one checking a now-wrong refusal code, one checking a now-wrong
table row count) that a full suite run caught immediately and reading would not have.

## 3. Execute, don't just read

Reading code for correctness misses things reading alone cannot catch. Actually run it, build it, execute
the real logic path against real input. Every serious bug this project has found was found by running
something, never by reading alone: a regex that silently matched nothing (format mismatch), an exact-equality
check that would have rejected every real capture, a hardcoded value from one reference machine, a validation
gap that only showed up against one specific real log out of several.

## 4. Test against every real data sample available, not just the first one that works

A single passing run - synthetic, or the first real fixture that happens to be at hand - proves far less
than testing against everything real this project actually has. The 0xffffffff BAR0-unmapped bug (2026-11-04)
hid inside one of seven real captures; testing against only one or two would have missed it completely, and
it would have shipped as a silent "ok: true" producing a meaningless profile.

## 5. Check for parallel/duplicate implementations of the same logic

Where the same validation, matching rule, or check exists in more than one place - a Python tool mirroring
C firmware logic, a test harness re-implementing a check, a capture tool and an unlock tool both parsing the
same table - changing one without the other is a real, repeat failure mode here. Find every copy before
declaring a change complete, not just the one you were already looking at.

## 6. Look specifically for boundary/sentinel values, not just the normal failure cases

The dangerous bugs are rarely in the ordinary success/failure path - they hide in what happens with an
all-ones, all-zeros, or otherwise degenerate value that nothing currently checks for, especially where an
existing check's own logic (a bitmask AND, a pattern match tuned for a *different* sentinel) can be defeated
by construction. If a register or value can plausibly read as "unmapped" or "nothing here," check for that
specifically - don't assume an existing "is this value sane" check already covers it.

## 7. Rebuild the actual artifact after every change, including fixes made during a review pass

"Should still compile" is not verification. Get the real binary, confirm the exit code, and sanity-check
its properties (import count, size, hash) - a hash that's unexpectedly identical or unexpectedly different
from before is itself a signal worth explaining, not ignoring.

## 8. Write down what you checked, not only what you fixed

A fix with no record of what else was ruled out just moves the next blind spot onto whoever reads this
later. Record the negative results too (what you tested and found clean), the same way PORTING-PLAN.md's
entries already do - that record is what lets the *next* review build on this one instead of re-discovering
the same ground or missing something this pass happened to cover by luck.
