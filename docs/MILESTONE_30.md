# Milestone 30: Generation-scoped controlled stack memory

## Milestone 30 status

M30 replaces append-only controlled-stack retention with exact, generation-
scoped ownership. A successful controlled execution maps one zero-filled,
read/write dynamic region and receives an opaque whole-region ownership token.
The token is released when the `ExecutionSession` generation is destroyed,
after `run-entry` has finished using that generation's result for candidate
assessment and refinement. Static loader mappings remain ordinary permanent
`GuestMemory` mappings.

The existing 512 MiB guest-memory bound is unchanged. The bound measures live
mapped backing bytes, while cumulative mapping activity and the virtual stack
address high-water mark are separate finite accounting dimensions.

## Base and M29 reproduction

The required base SHA was:

`529704c6546868faf7c03840f96776746fd7ff6f`

The branch is stacked directly on `milestone-29-resumable-ir-execution`; M29
was not merged. Before production edits, the ordinary private four-module
configuration reproduced the M29 report exactly:

- size: `37,770,905` bytes;
- SHA-256: `0702b7fd69d16d0b63e47a888fb2af7746c1d6921c0ca527f3a447cb0aea3789`;
- schema: `15`;
- stop: `guest_memory_resource_limit_exceeded`;
- attempt/generation: `422/421`;
- current function: `main:0x7200000030`;
- guest PC: `main:0x7200000148`;
- instruction: `blr x8`;
- target: `main:0x72001334c0`;
- pointer provenance: `guest load at main:0x720456b860`;
- pending refinement work: `0`.

The completed M29 frontier counters were guest instructions `17,139`, guest
blocks `3,335`, IR operations `100,716`, execution slices `1,833`,
resumable yields/resumes `1/1`, mid-block resumes `1`, function transitions
`65`, maximum call depth `3`, direct calls `468`, indirect calls `416`,
returns `883`, runtime fallbacks `0`, productive rounds `421`, stagnant
rounds `0`, unique observations `1,336`, unique candidates `421`, candidate
assessments `421`, successful promotions `421`, and pending work `0`.

## Guest-memory forensic analysis

The pre-edit report and the loader/execution call-site audit establish that
the growth was controlled-stack retention, not a growing loader or refinement
mapping class:

| Quantity at the failed next map | Value |
| --- | ---: |
| configured maximum live mapped bytes | `536,870,912` |
| stable non-stack live bytes | `95,322,368` |
| live controlled stacks | `421` |
| synthetic stack size | `1,048,576` |
| live controlled-stack bytes | `441,450,496` |
| live mapped bytes before attempted map | `536,772,864` |
| next requested live mapped bytes | `537,821,440` |
| live region count | `437` (`16` static + `421` stacks) |
| maximum region count | `1,024` |
| guard gap | `0x10000` |
| first controlled-stack base | `0x7205b2c000` |
| last successful stack base | `0x722196c000` |
| attempted next stack base | `0x7221a7c000` |

The old allocator advanced by `0x110000` (`1 MiB` stack plus `0x10000`
guard gap). All dynamic mapping call sites were inspected: the NSO loader
maps the four static segments per module, and `ExecutionSession::map_stack`
was the only refinement-time dynamic mapper. The process image, function-map
refinement, indirect-target records, and runtime ABI retain guest scalar
addresses and provenance, not host pointers or `GuestMemory` regions. Thus
the linear live-byte increase was exactly the set of still-live synthetic
stack regions.

## Stack ownership and lifetime

The lifecycle of one generation is:

1. `map_stack` reads the checked `GuestMemory` virtual high-water cursor,
   adds the checked guard gap, aligns the result to `0x1000`, and checks the
   stack end.
2. `GuestMemory::map_owned` allocates a zero-filled host backing vector and
   records a private identity in the sorted region. Only after the complete
   map succeeds does it return the exact ownership token.
3. The session initializes SP at the checked aligned stack end. Guest loads,
   stores, runtime ABI stack-argument reads, and event reporting can use the
   mapping while the session is alive.
4. `run-entry` keeps the loop-local session alive while it consumes the run
   result, records observations, assesses candidates, and attempts an
   immutable refinement. No candidate assessment dereferences the stack after
   `run` returns; it consumes already materialized scalar observation and
   provenance records.
5. On `continue`, `break`, typed stop, unsupported boundary, normal return,
   or an infrastructure error after mapping, C++ scope destruction invokes
   the session destructor. The destructor releases the exact owned region.
   The next generation is created only after that destruction.

Calling `run` again on the same session is also a replacement boundary: any
previous token is released before a new generation can map its stack. A failed
release terminates the process rather than silently leaving an ambiguous
mapping state, because failure at that point indicates a broken ownership
invariant.

The current controlled-entry contract does not publish a host reference to a
stack. Guest code can represent arbitrary guest addresses as integer values,
but no process-image, refinement, candidate-identity, or post-run assessment
object retains a stack region or performs a later stack dereference. A future
runtime ABI that deliberately publishes a stack pointer beyond the session
would need an explicit retaining lifetime contract; M30 does not classify
such an escape as safe by convention.

## Ownership and safety model

`GuestMemoryMappingToken` is an opaque capability containing a memory-domain
identity, a per-mapping identity, and exact base/size metadata. `release_owned`
authenticates the domain, exact base, exact size, and identity against one
currently live owned region. It cannot release a static loader mapping, a
mapping in another `GuestMemory`, a replaced mapping at the same address, a
middle region by containment, or an already released region.

Static `map` callers retain their existing API and behavior. The NSO loader's
staging copy is still transactional; copying a `GuestMemory` with live owned
mappings is rejected because a dynamic token cannot be copied into a second
memory object without an ownership transfer. This state is not used by the
process loader, which stages static mappings before execution begins.

Every new size, count, cumulative-byte, range-end, alignment, guard-gap, and
high-water update uses checked arithmetic. A map failure occurs before region
insertion or accounting mutation. A release authenticates the complete region
and computes all checked subtractions before erasing it. Region ordering stays
sorted after exact removal of a middle or top dynamic region. Host backing
allocation and container length failures remain typed resource failures.

Every successful owned map starts with a newly allocated vector explicitly
filled with zero bytes. Releasing and mapping the same virtual address in a
synthetic fixture therefore cannot expose the prior generation's contents.

## Live versus cumulative accounting

`GuestMemory` now reports configured limits, current and peak live mapped bytes,
current and peak region count, cumulative mapped bytes, owned stack create and
reclaim counts, current and peak owned mapping/byte counts, cumulative owned
bytes, and the virtual allocation high-water address. The execution report
schema increments from `15` to `16` and serializes these fields in stable
order. `live_mapped_bytes` is the resource quantity. Cumulative mapping bytes
describe churn and are not charged against the live limit. The high-water
cursor consumes virtual address space conceptually but does not retain host
backing storage.

The old append-only model would have required `473 MiB` of controlled-stack
backing for 473 generations. M30 retains one stack at peak in ordinary
sequential refinement: live stack storage is `O(1)` in generation count.

## Guest-visible stack addresses

M30 intentionally does not reset the virtual allocator after reclamation.
`GuestMemory` retains a checked virtual high-water mark after an owned region
is released. New stacks therefore use the same deterministic progression as
the pre-M30 append-only allocator: the old guard gap, alignment, stack size,
and monotonic address sequence remain visible to the guest. Only host backing
storage is reclaimed. Any 64-bit guard-gap, alignment, stack-end, or
high-water overflow fails closed before mapping.

## Synthetic validation

`tests/milestone30_stack_lifetime_tests.cpp` provides a dedicated small-memory
fixture whose old append-only behavior would exceed the live limit after one
additional generation. It runs `2,000` sequential controlled generations
with an `0x1000` stack and a `0x1008` total live-byte limit. The fixture
remains at one static region and `8` live bytes after every generation, while
the owned cumulative mapping activity reaches `0x2000000` bytes. It verifies
the exact `2,000/2,000` create/reclaim counts, peak one live stack, bounded
peak live bytes, monotonic stack addresses, region ordering, zero reset,
normal return cleanup, typed event-stop cleanup, and cleanup after an error
following successful stack mapping.

The same focused file verifies exact whole-region release, middle-region
ordering, stale/double/cross-memory tokens, static-map protection, failed-map
transactionality for both region and total limits, copy rejection for live
owned mappings, persistent-cursor overflow, alignment overflow, and checked
accounting. The low-level guest-memory suite also covers authenticated owned
release. The unchanged M29 focused suite remains separate and was rerun
without changing its tests.

## Real private execution

The ordinary four-module local configuration was run without an execution
override, forced PC or entry, seed, target seed, memory patch, instruction
skip, fake return, raised guest-memory/refinement/structural limit, or
explicit IR hard limit. The former generation `421` memory failure was crossed
naturally. The final local run created and reclaimed `473` controlled stacks,
completed `472` promotions/map rebuilds after the initial generation, and
stopped at a later finite function-transition boundary.

The final run recorded:

- schema: `16`;
- report size: `38,587,538` bytes;
- report SHA-256: `79367631238e1c4243012e28141d5cf519067c8067b69588dd58a9aa8f89e144`;
- stop: `function_transition_limit_exceeded`;
- stop/current function PC: `main:0x72000c353c`;
- diagnostic: `function transition limit exhausted`;
- guest instructions/blocks/IR operations: `17,850/3,538/105,451`;
- execution slices: `1,934`;
- resumable yields/resumes/mid-block resumes: `1/1/1`;
- function transitions/call depth: `1,000/3`;
- direct calls/indirect calls/returns: `468/467/933`;
- runtime fallbacks: `0`;
- total attempts/productive rounds/stagnant rounds: `473/472/1`;
- unique observations/candidates/assessments: `1,438/472/472`;
- successful promotions: `472`;
- pending refinement work: `0`.

This is downstream of M29: the former attempt `422`, generation `421`, and
`100,716`-IR-operation memory frontier is no longer the stop. No indirect
target was seeded or uncertified; candidate identity, assessment order,
promotion order, map generations, provenance, and function-entry
certification remain on the existing M29 path. The one stagnant round versus
M29's zero is a later natural no-new-promotion round after the former
frontier was crossed; it did not change candidate ordering, certification, or
any finite limit.

## Guest-memory resource accounting

The final private report proves the simultaneous-resource invariant:

| Quantity | Final value |
| --- | ---: |
| configured max total live bytes | `536,870,912` |
| non-stack live baseline | `95,322,368` |
| peak live mapped bytes | `96,370,944` |
| final live mapped bytes | `95,322,368` |
| peak live controlled stacks | `1` |
| peak live controlled-stack bytes | `1,048,576` |
| controlled-stack generations created | `473` |
| controlled-stack generations reclaimed | `473` |
| controlled stacks still live at terminal report point | `0` |
| live region count at terminal report point | `16` |
| cumulative controlled-stack bytes | `495,976,448` |
| cumulative mapped bytes | `591,298,816` |
| virtual stack high-water | `0x72251ac000` |

At 473 generations, append-only accounting would have required
`95,322,368 + 495,976,448 = 591,298,816` live bytes and would have crossed
the old 512 MiB limit. M30's final live backing is the static baseline only;
the machine's available RAM is not used as evidence.

## Refinement aggregate resources

The final run consumed `471/512` immutable refinement transactions,
`1,012/200,000` functions analyzed, `6/100,000` functions reanalyzed,
`26,594/8,000,000` instructions, `5,436/2,000,000` blocks,
`7,746/4,000,000` edges, `106,376/268,435,456` bytes,
`472/2,048` boundary-finalization passes, and `3/100,000` invalidated
records. It had `472` module maps rebuilt and `472` reused, with no pending
candidate work. These resources were not raised or bypassed.

## New exact frontier

The genuine new stop is `function_transition_limit_exceeded` at
`main:0x72000c353c`, consuming `1,000/1,000` function transitions. It occurs
after the former memory-blocked generation has been replaced by successful
generation-scoped stack lifetimes and later immutable refinement progress.
There is no new instruction, indirect target, or pointer provenance attached
to the budget boundary itself; those fields are correctly empty in the
typed report. M31 should investigate this finite transition frontier and
the semantic path that consumes it, without changing the transition budget
merely to extend the run.

## Determinism, validation, and privacy

Two independent ordinary private runs from equivalent starting state were
performed after the implementation stabilized. Both reports were
`38,587,538` bytes with SHA-256
`79367631238e1c4243012e28141d5cf519067c8067b69588dd58a9aa8f89e144`, and a
binary comparison was identical. The reports, prepared modules, keys, local
configuration, private addresses outside the milestone evidence, and
machine-specific paths remain uncommitted local artifacts.

The complete standard CTest suite passed `317/317`; the M30 focused suite
passed `12,120` assertions in `10` cases; and the unchanged M29 focused suite
passed `307` assertions in `10` cases. Local GCC passed `317/317`, ASan/UBSan
passed `317/317`, and TSan passed `317/317`. The final PR validation matrix
was also terminal and successful: all `10/10` configured push/PR checks
passed, including Linux/GCC, LLVM 18, ASan/UBSan, TSan, and Windows/MSVC.
No new third-party dependency was added.

## Known limitations and M31 recommendation

M30 is a whole-region ownership mechanism for generation-local controlled
stacks, not a general guest virtual-memory manager. It does not provide page
protection, heap allocation, thread-stack ownership, snapshots, or a general
escaped-stack-pointer protocol. A future runtime boundary that needs a stack
object after its creating session must introduce an explicit owner rather
than relying on a guest address alone.

M31 should measure and address the named `1,000/1,000` function-transition
frontier, preserving the existing finite transition accounting and all
indirect-target/refinement certification rules. The next architectural
question is whether that path represents a legitimate transition-resource
boundary or an additional missing execution contract; it is not a reason to
raise the guest-memory limit.
