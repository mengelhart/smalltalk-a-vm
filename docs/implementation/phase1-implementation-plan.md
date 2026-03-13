# Smalltalk/A — Phase 1 Implementation Plan

## Minimal Live Kernel

*Generated March 2026 — based on bytecode-spec v4, PROJECT_STATUS.md, ADRs 007–013*

---

## Part 1: Open Decision Triage

The 22 open decisions from PROJECT_STATUS.md are sorted below into "must resolve
before Phase 1 code starts" vs "resolve during Phase 1" vs "Phase 2+".

### Must Resolve BEFORE Any Phase 1 Code

These five decisions block the first line of production code.

**#1 — Nil/True/False as immediates vs heap objects.**
The bytecode spec §11.1 explicitly says this must be resolved before the first
dispatch loop. It affects OP_PUSH_NIL/TRUE/FALSE, the special object table layout,
the bootstrap sequence (step 2), the GC (what's a root vs an immediate), and
potentially the image format. Highest priority decision.

**#6 — Stack slab growth policy.**
ADR 010 deferred this. The bytecode spec §4.2 marks the `largeFrame` header field
as provisional pending this decision. The interpreter can't activate a method
without knowing how much slab to reserve. Blocks: interpreter dispatch loop,
method activation, block invocation.

**#16 — Quiescing protocol for live actors.**
The bytecode spec §11.7 explicitly calls this a Phase 1 blocker before image save.
The bootstrap sequence ends with "save the bootstrap image" (step 11). Even in a
single-actor Phase 1 world, this needs a defined protocol.

**#17 — Root table for multi-root images.**
The spike-006 format has a single root object. The bootstrap image needs at minimum:
the special object table, the class table, the global dictionary. Either extend the
format or define a convention for encoding multiple roots through one root object.

**#18 — Class identifier portability.**
The bytecode spec §11.5 assigns fixed class indices 0–31. The image format needs
stable class keys so a saved image can be loaded on a different build. Blocks:
bootstrap image save/load round-trip.

### Resolve DURING Phase 1 Implementation

**#7 — TCO with callee having more locals.** Resolve alongside #6 when building
the interpreter's method activation code.

### Phase 2+ (Not Phase 1 Blockers)

| # | Decision | Why deferred |
|---|---|---|
| 3 | Class table concurrency | Phase 1 is single-threaded |
| 4 | Variant B deque root cause | Investigation, no dependency |
| 5 | Per-actor scheduling fairness | Phase 2 scheduler |
| 8 | Closure and NLR compatibility | Closures are Phase 2 |
| 9 | Deep-copy visited set | Cross-actor copy is Phase 2 |
| 10 | ask: future on mailbox-full | Phase 2 |
| 11 | Transfer buffer allocator | Phase 2 |
| 12 | Resume point for I/O suspension | Phase 2 |
| 13 | Lock-free I/O request queue | Phase 2 |
| 14 | I/O backpressure | Phase 2 |
| 15 | 4-byte density headroom | Track, not a Phase 1 blocker |
| 19 | Growable handle table | Phase 3 |
| 20 | Handle validity after vm_destroy | Phase 3 |
| 21 | sta_inspect_cstring buffer | Phase 3 |
| 22 | Event callback re-entrancy | Phase 3 |

---

## Part 2: Dependency Map

```
Epic 0: Resolve 5 blocking decisions (ADRs) ← design work, not CC
    |
    v
Epic 1: Object memory & allocator
    |
    v
Epic 2: Symbol table, method dictionary
    |
    v
Epic 3: Bootstrap Tier 0 (metaclass circularity)
    |
    v
Epic 4: Bootstrap Tier 1 (kernel classes + primitive methods)
    |
    v
Epic 5: Bytecode interpreter (dispatch + primitives + blocks)
    |
    v
Epic 6: Compiler (Smalltalk source -> bytecodes)
    |
    v
Epic 7: Exception handling (handler stack, on:do:, signal)
    |
    v
Epic 8: Kernel source loading (file-in)
    |
    v
Epic 9: Image save/load (production format)
    |
    v
Epic 10: Eval loop & smoke test ("3 + 4 -> 7")
```

Epics 1-4 are strictly sequential. Epics 5 and 6 have a tight feedback loop
(interpreter needs compiled methods to test; compiler needs interpreter to verify).
Epics 7-8 depend on both. Epic 9 promotes spike-006 to production. Epic 10 is the
integration milestone.

---

## Part 3: Epics with Claude Code Prompts

Each epic below includes a prompt you can copy-paste directly into a Claude Code
session. Start a fresh CC session for each epic. Paste CLAUDE.md + PROJECT_STATUS.md
at the start of each session (CC reads them), then paste the prompt.

---

### Epic 0: Resolve Phase 1 Blocking Decisions

This is design work — do it in this Claude conversation, not CC. Produces ADRs
014-016 (or amendments to existing ADRs) covering decisions #1, #6, #7, #16, #17, #18.

No CC prompt for this epic.

---

### Epic 1: Object Memory & Allocator

**Depends on:** Epic 0 (decision #1 resolved — nil/true/false representation)

Promotes spike-001 structures to production. Allocator handles: shared immutable
region, per-actor heap (Phase 1: single actor), special object table, class table.

#### Claude Code Prompt

```
Read CLAUDE.md, PROJECT_STATUS.md, docs/decisions/007-object-header-layout.md,
docs/decisions/012-image-format.md, and docs/architecture/bytecode-spec.md
section 11.1 (special object table) and section 11.5 (class table).

We are beginning Phase 1 — Minimal Live Kernel. Phase 0 spikes are complete.
All spike code in src/ is exploratory — it will be referenced but not directly
promoted. Production code goes in new files.

This epic: Object memory and allocator.

Create a GitHub milestone "Phase 1 — Minimal Live Kernel" if it does not exist.
Create a GitHub epic issue "Phase 1 Epic 1: Object Memory and Allocator".

Work items (create as child issues, implement in order):

1. Production object header and OOP definitions
   - Create src/vm/oop.h and src/vm/oop.c (production, not spike)
   - STA_ObjHeader (12 bytes, 16-byte allocation unit per ADR 007)
   - OOP typedef, SmallInt tagging macros, IS_SMALLINT/SMALLINT_VAL/SMALLINT_OOP
   - nil/true/false representation per the resolved decision on open item #1
   - gc_flags and obj_flags bit definitions from ADR 007
   - Tests: sizeof/offsetof assertions, tagging round-trip, nil/true/false identity

2. Shared immutable region allocator
   - Create src/vm/immutable_space.h and src/vm/immutable_space.c
   - Bump allocator for bootstrap-time allocation of symbols, methods, class objects
   - Region is marked read-only after bootstrap completes (mprotect)
   - Objects allocated here get STA_OBJ_IMMUTABLE flag set
   - Tests: allocate objects, verify flag, verify region is contiguous

3. Actor-local heap allocator (single-actor Phase 1 variant)
   - Create src/vm/heap.h and src/vm/heap.c
   - Bump allocator with nursery semantics (per ADR 007 density discussion)
   - Phase 1: single heap, no actor isolation yet — but the API surface must
     accept an actor/heap context parameter so Phase 2 can add per-actor heaps
     without API surgery
   - basicNew / basicNew: allocation paths
   - Tests: allocate objects of various sizes, verify alignment, verify headers

4. Special object table
   - Create src/vm/special_objects.h and src/vm/special_objects.c
   - 32-entry OOP array per bytecode spec section 11.1
   - SPC_NIL, SPC_TRUE, SPC_FALSE, SPC_SMALLTALK, etc. as defines
   - Populated during bootstrap (Epic 3), but the table and accessors exist now
   - Tests: table initialized to placeholder, indices match spec

5. Class table
   - Create src/vm/class_table.h and src/vm/class_table.c
   - Array mapping class_index (uint32) to class OOP
   - Reserved indices 0-31 per bytecode spec section 11.5
   - Growth: fixed initial size (256 entries), growable later
   - Phase 1 is single-threaded — no concurrent access yet, but use atomic
     pointer swap for the array so Phase 2 does not require restructuring
   - Tests: register class at known index, lookup by index, SmallInteger
     special-case (index 1, no header)

Create branch: phase1/epic-1-object-memory
Implement each story in order. Run ctest after each. Do not close the epic.
```

---

### Epic 2: Symbol Table & Method Dictionary

**Depends on:** Epic 1 (allocator, class table)

#### Claude Code Prompt

```
Read CLAUDE.md, PROJECT_STATUS.md, docs/decisions/007-object-header-layout.md,
and docs/architecture/bytecode-spec.md section 10.6 (method dictionary),
section 10.7 (special selectors), section 11.4 (symbol table requirements).

Phase 1 Epic 2: Symbol table and method dictionary.

Create GitHub epic "Phase 1 Epic 2: Symbol Table and Method Dictionary".

Work items (implement in order):

1. Symbol table
   - Create src/vm/symbol_table.h and src/vm/symbol_table.c
   - Hash table mapping string content to canonical Symbol OOP
   - Intern operation: lookup-or-create, returns the same OOP for identical strings
   - Phase 1: single-threaded, no lock-free read path yet — but the API must
     separate the read path (sta_symbol_lookup) from the write path
     (sta_symbol_intern) so Phase 2 can make reads lock-free
   - Symbol objects allocated in shared immutable region (Epic 1)
   - Tests: intern same string twice yields same OOP, different strings yield
     different OOPs, pre-intern all special selectors from section 10.7

2. Method dictionary
   - Create src/vm/method_dict.h and src/vm/method_dict.c
   - Hash table mapping Symbol OOP to CompiledMethod OOP
   - Lookup by selector (the dispatch hot path)
   - Insert (addSelector:withMethod: path — primitive 69)
   - Phase 1: insert acquires install_lock (ADR 013), atomically swaps internal
     array pointer. Reads are against the current array pointer (no lock).
   - Cache generation counter (section 10.4): global uint64_t, incremented on
     every insert
   - Tests: insert method, lookup by selector, verify generation increments,
     verify stale lookup still works (returns old method until cache miss)

3. Special selector table
   - Create src/vm/special_selectors.h and src/vm/special_selectors.c
   - Fixed array of 32 pre-interned selector symbols per section 10.7
   - Populated during bootstrap, but the table structure exists now
   - Tests: all 32 entries non-nil after population, arity matches spec

Create branch: phase1/epic-2-symbol-method-dict
Implement in order. Run ctest after each story. Do not close the epic.
```

---

### Epic 3: Bootstrap Tier 0 — Metaclass Circularity

**Depends on:** Epics 1 and 2

This is the hardest single task. The bytecode spec section 11.2 and section 11.7
define the exact sequence.

#### Claude Code Prompt

```
Read CLAUDE.md, PROJECT_STATUS.md, and docs/architecture/bytecode-spec.md
section 11 (all of section 11 — Special Objects and Bootstrap Requirements).
Also read the architecture doc section 11 (The Bootstrapping Problem).

This is the hardest single implementation task in the project. Read the Blue Book
chapters 13 and 16 (class hierarchy and metaclass protocol) if you have not
already. The bytecode spec section 11.2 defines the exact class hierarchy and
section 11.7 defines the bootstrap execution sequence.

Phase 1 Epic 3: Bootstrap Tier 0 — Metaclass Circularity.

Create GitHub epic "Phase 1 Epic 3: Bootstrap Tier 0 (Metaclass Circularity)".

Work items (implement in order — this order is load-bearing):

1. Bootstrap entry point
   - Create src/bootstrap/bootstrap.h and src/bootstrap/bootstrap.c
   - Function: sta_bootstrap_create_image(const char* output_path) returns int
   - This function runs once to create a fresh image from scratch
   - Subsequent launches load the saved image and skip bootstrap entirely

2. Create nil, true, false (bytecode spec section 11.7 steps 1-3)
   - Allocate special object table (32 entries, filled with placeholder)
   - Allocate nil as instance of UndefinedObject (class_index = 3) in immutable
     region
   - Allocate true as instance of True (class_index = 4) in immutable region
   - Allocate false as instance of False (class_index = 5) in immutable region
   - Classes do not exist yet — write class indices directly into headers
   - Populate special_objects[0..2] with real nil, true, false OOPs
   - Tests: special_objects[SPC_NIL] is valid, class_index == 3, etc.

3. Create Tier 0 classes (section 11.7 step 4 — the metaclass circularity)
   - Allocate raw memory for 10 objects: Object, Behavior, ClassDescription,
     Class, Metaclass, and their 5 metaclasses
   - Each class needs: STA_ObjHeader + class-specific instance variables
     (superclass, methodDictionary, format, name, instanceVariableNames, etc.)
   - Wire superclass pointers: Object to nil, Behavior to Object,
     ClassDescription to Behavior, Class to ClassDescription,
     Metaclass to ClassDescription
   - Wire metaclass pointers: each class's class_index points to its metaclass;
     each metaclass is an instance of Metaclass (class_index = 17)
   - Create empty MethodDictionary for each of the 10 classes
   - Register all 10 in the class table at reserved indices (section 11.5)
   - Tests: verify circularity — Metaclass class class == Metaclass,
     Object class superclass == Class, Class class class == Metaclass

4. Wire the Smalltalk global dictionary (section 11.7 step 8 — partial)
   - Create SystemDictionary instance
   - Register all Tier 0 classes as Associations: #Object to Object, etc.
   - Store in special_objects[SPC_SMALLTALK]
   - Tests: lookup #Object returns the Object class OOP

Create branch: phase1/epic-3-bootstrap-tier0
Implement in order. This is meticulous pointer-wiring work — verify each step
before moving to the next. Run ctest after each story. Do not close the epic.
```

---

### Epic 4: Bootstrap Tier 1 — Kernel Classes

**Depends on:** Epic 3

#### Claude Code Prompt

```
Read CLAUDE.md, PROJECT_STATUS.md, and docs/architecture/bytecode-spec.md
section 11.2 (Tier 1 kernel classes), section 11.3 (kernel methods),
section 11.4 (symbol interning), section 11.7 steps 5-9.

Phase 1 Epic 4: Bootstrap Tier 1 — Kernel Classes.

The Tier 0 metaclass circularity is wired (Epic 3). Now we can create classes
using the (still primitive) class-creation machinery. These classes are needed
before the first bytecode can execute.

Create GitHub epic "Phase 1 Epic 4: Bootstrap Tier 1 (Kernel Classes)".

Work items (implement in order):

1. Tier 1 class creation
   - Using Tier 0 machinery, create all Tier 1 classes from section 11.2:
     UndefinedObject, True, False, SmallInteger, Number, Magnitude, Float,
     Character, Symbol, String, Array, ByteArray, ArrayedCollection,
     SequenceableCollection, Collection, Association, MethodDictionary,
     CompiledMethod, BlockClosure, BlockDescriptor, Message,
     MessageNotUnderstood, BlockCannotReturn, Error, Exception,
     SystemDictionary, Boolean
   - Each gets: class table entry at reserved index, method dictionary,
     global dictionary Association
   - Superclass chains per section 11.2 (e.g. SmallInteger to Number to
     Magnitude to Object)
   - Tests: every Tier 1 class has correct superclass, correct class_index,
     is findable in global dictionary

2. Intern kernel symbols (section 11.7 step 6)
   - Intern all symbols from section 11.4: special selectors,
     interpreter-referenced selectors (#doesNotUnderstand:, #cannotReturn:,
     #mustBeBoolean, etc.), control flow selectors (#ifTrue:, #ifFalse:, etc.)
   - Populate the special selector table
     (special_objects[SPC_SPECIAL_SELECTORS])
   - Tests: every symbol from section 11.4 is interned, sta_symbol_lookup
     succeeds for all of them

3. Create character table (section 11.7 step 7)
   - Allocate 256 Character instances for ASCII 0-255
   - Store in special_objects[SPC_CHARACTER_TABLE]
   - Tests: character table[65] has value 65 (Character $A)

4. Install kernel primitive methods (section 11.7 step 9)
   - For each method in section 11.3 "must exist before any bytecode runs":
     create a CompiledMethod object with appropriate header (hasPrimitive=1,
     primitiveIndex, numArgs, numLiterals >= 1 for owner class)
   - Install in the correct class's method dictionary via method_dict_insert
   - Minimum set: Object>>doesNotUnderstand:, Object>>yourself, Object>>class,
     Object>>=, SmallInteger>>+, SmallInteger>>-, SmallInteger>><,
     SmallInteger>>>, SmallInteger>>=, SmallInteger>>*, Array>>at:,
     Array>>at:put:, Array>>size, BlockClosure>>value, BlockClosure>>value:,
     True>>ifTrue:, True>>ifFalse:, True>>ifTrue:ifFalse:,
     False>>ifTrue:, False>>ifFalse:, False>>ifTrue:ifFalse:
   - Each method's literal frame must contain the owner class OOP as the
     last literal (section 4.4)
   - Tests: lookup #+ on SmallInteger returns a CompiledMethod with
     hasPrimitive=1 and primitiveIndex=1

Create branch: phase1/epic-4-bootstrap-tier1
Implement in order. Run ctest after each. Do not close the epic.
```

---

### Epic 5: Bytecode Interpreter

**Depends on:** Epic 4 (bootstrap classes and primitive methods exist)

#### Claude Code Prompt

```
Read CLAUDE.md, PROJECT_STATUS.md, docs/decisions/010-frame-layout.md,
and docs/architecture/bytecode-spec.md — all of section 2 (encoding),
section 3 (opcode table, Phase 1 opcodes only), section 4 (compiled method
format), section 5 (syntax-to-bytecode compilation examples), section 8
(primitive table, Phase 1 primitives), section 10 (dispatch and caching),
and section 12 (quick reference).

Also read docs/decisions/009-scheduler.md for the reduction quota constant
(STA_REDUCTION_QUOTA = 1000).

Phase 1 Epic 5: Bytecode Interpreter.

This is the core execution engine. It executes the 31 Phase 1 opcodes, performs
message dispatch with monomorphic inline caching, handles primitive dispatch
(two-stage protocol per section 4.9), and counts reductions for preemption.

Phase 1 is single-actor, single-threaded. The interpreter runs in a simple
loop — no scheduler integration yet. But the reduction counter must exist
and decrement correctly so Phase 2 does not require restructuring.

Create GitHub epic "Phase 1 Epic 5: Bytecode Interpreter".

Work items (implement in order):

1. Frame layout (production)
   - Create src/vm/frame.h and src/vm/frame.c
   - STA_Frame struct per ADR 010: method, pc, receiver, sender, homeFrame,
     marker, unwindHandler (initialized to nil), stack/temp area
   - Frame size = 48 bytes per bytecode spec section 7.7
     (40 base + 8 unwindHandler)
   - Stack slab: contiguous memory region per actor, initial size per the
     resolved decision on open item #6
   - Frame push, frame pop, stack operations (push OOP, pop OOP, peek)
   - GC root enumeration: sta_frame_gc_roots() walks all OOP slots
   - Tests: push/pop frame, verify marker written/cleared, GC root count

2. Dispatch loop and Phase 1 opcodes
   - Create src/vm/interpreter.h and src/vm/interpreter.c
   - Main dispatch loop: fetch opcode at pc, switch on opcode, advance pc by 2
   - OP_WIDE handling: read high byte, advance, read opcode + low byte,
     compute 16-bit operand
   - Implement all Phase 1 opcodes from section 3 and section 12:
     Push group: PUSH_RECEIVER, PUSH_NIL, PUSH_TRUE, PUSH_FALSE, PUSH_LIT,
       PUSH_TEMP, PUSH_INSTVAR, PUSH_GLOBAL, PUSH_SMALLINT, PUSH_MINUS_ONE,
       PUSH_ZERO, PUSH_ONE, PUSH_TWO
     Store group: STORE_TEMP, STORE_INSTVAR, STORE_GLOBAL, POP_STORE_TEMP,
       POP_STORE_INSTVAR, POP_STORE_GLOBAL
     Send group: SEND, SEND_SUPER
     Return group: RETURN_TOP, RETURN_SELF, RETURN_NIL, RETURN_TRUE,
       RETURN_FALSE
     Jump group: JUMP, JUMP_TRUE, JUMP_FALSE, JUMP_BACK
     Stack group: POP, DUP, PRIMITIVE
     Block group: BLOCK_COPY
   - Phase 2 opcodes (0x16, 0x17, 0x35, 0x61, 0x62, 0x70-0x73): recognize
     in dispatch table, signal NotYetImplemented if executed
   - Reserved opcodes: fatal interpreter error
   - Reduction counter: increment on SEND, SEND_SUPER, JUMP_BACK.
     Check >= 1000. Phase 1: when quota exceeded, just reset counter
     (no scheduler to yield to)
   - Tests: hand-assembled bytecode arrays that exercise each opcode. Verify
     stack effects match section 12. Verify reduction counting.

3. Message dispatch and inline cache
   - Implement section 10.1 dispatch algorithm:
     a. Read receiver class_index from header (SmallInteger: special-case
        tag check)
     b. Check monomorphic inline cache at send site
     c. On miss: walk method dictionaries up superclass chain
     d. On failure: create Message object, send #doesNotUnderstand:
   - Monomorphic inline cache per section 10.3: side table indexed by
     method + PC
   - Cache generation counter per section 10.4: check on every cache hit
   - Super send dispatch per section 10.2: owner class from last literal,
     take superclass
   - mustBeBoolean protocol per section 10.10 for JUMP_TRUE/JUMP_FALSE
     on non-booleans
   - Tests: cache hit on repeated sends, cache miss on class change,
     doesNotUnderstand: fires on unknown selector, generation counter
     invalidates cache

4. Primitive dispatch
   - Create src/vm/primitives.h and src/vm/primitives.c
   - Primitive dispatch protocol per section 4.9: two-stage (header check,
     then bytecode preamble for extended primitives)
   - On success: no frame created, result pushed to caller stack
   - On failure: create frame, store failure code in temp[numArgs],
     fall through to bytecode
   - Implement Phase 1 primitives from section 8:
     Arithmetic: prims 1-17 (SmallInteger), 18-22 (Float)
     Comparison: prims 23-30
     Object/memory: prims 31-42
     Array/string: prims 51-55, 60-64
     Class/method: prims 68-75
     Block: prims 81-87
     Symbol/char: prims 91-95
     System: prims 100-103, 110-114, 120-121
   - SmallInteger overflow: primitive fails, fallback bytecode handles
     promotion
   - Tests: 3 + 4 = 7 via prim 1, Array>>at: via prim 51,
     basicNew via prim 31

5. TCO (tail-call optimization)
   - Implement section 2.4 lookahead: after any SEND, check
     bytecode[pc + 2] == OP_RETURN_TOP
   - If TCO: clear old frame marker, reuse frame for callee
   - Wide send: check bytecode[pc + 4]
   - NLR restriction: compiler responsibility (not interpreter's job to
     check), but frame marker clearing is the runtime safety net per
     section 7.10
   - Tests: tail-recursive method with depth 100,000 — stack depth
     stays at 1

6. Clean block support
   - Implement OP_BLOCK_COPY per section 6.2: read descriptor from literal
     frame, allocate BlockClosure, fill homeMethod/startPC/numArgs/receiver,
     jump past body
   - Block invocation via prims 81-85: create block frame per section 6.7,
     set method=homeMethod, pc=startPC, receiver=block.receiver
   - OP_RETURN_TOP inside block: local return to block caller
   - homeFrame field set on block frame creation (for future NLR support)
   - Frame marker written/cleared correctly for block frames
   - Tests: [42] value = 42, [:x | x + 1] value: 5 = 6, nested blocks

Create branch: phase1/epic-5-interpreter
Implement in order. Each story should have passing tests before the next begins.
Do not close the epic.
```

---

### Epic 6: Compiler

**Depends on:** Epic 5 (interpreter exists to verify compiled output)

#### Claude Code Prompt

```
Read CLAUDE.md, PROJECT_STATUS.md, and docs/architecture/bytecode-spec.md —
all of section 4 (compiled method format), section 5 (syntax-to-bytecode
compilation, every subsection), section 6.1 and 6.2 (clean blocks only —
closures are Phase 2).

Phase 1 Epic 6: Smalltalk Compiler.

The compiler parses Smalltalk source and emits bytecode conforming to the
bytecode spec. Phase 1 scope: standard Smalltalk syntax, clean blocks only
(no capturing closures), no non-local return. The reference compilations in
section 5 are the exact sequences to implement first before any optimization.

Create GitHub epic "Phase 1 Epic 6: Compiler".

Work items (implement in order):

1. Lexer / scanner
   - Create src/compiler/scanner.h and src/compiler/scanner.c
   - Tokenize Smalltalk source: identifiers, keywords, symbols, strings,
     numbers (integer and float), characters, binary selectors, assignment
     (:=), return (^), block delimiters ([ ]), parentheses, period,
     cascade (;)
   - Handle: comments in double quotes, string escaping, negative numbers,
     #(...) literal arrays
   - Tests: scan representative methods, verify token sequence

2. Parser
   - Create src/compiler/parser.h and src/compiler/parser.c
   - Recursive descent parser for Smalltalk method syntax:
     method header (unary, binary, keyword), temporaries, statements,
     expressions (unary/binary/keyword sends with correct precedence),
     cascades, assignments, returns, block literals, literal arrays
   - Output: AST suitable for bytecode generation
   - Tests: parse each example from section 5, verify AST structure

3. Bytecode generator
   - Create src/compiler/codegen.h and src/compiler/codegen.c
   - Walk AST, emit bytecodes per section 5 reference compilations:
     Literals (section 5.1), variable access (section 5.2), assignment
     (section 5.3), message sends (section 5.4), cascades (section 5.5),
     control structure inlining (section 5.6), returns (section 5.7),
     temporaries (section 5.8), clean block literals (section 5.9)
   - Build literal frame: selector symbols, literal constants, global
     Associations, block descriptors, owner class (last literal per
     section 4.4)
   - Build method header word per section 4.2: numArgs, numTemps,
     numLiterals, primitiveIndex, hasPrimitive, largeFrame
   - Tail position tracking per section 5.11: emit SEND + RETURN_TOP
     adjacently
   - OP_WIDE emission for operands > 255
   - Tests: compile each section 5 example, verify bytecode matches
     reference output

4. CompiledMethod object creation
   - Allocate CompiledMethod in shared immutable region
   - Layout per section 4.1: header word + literal frame + bytecode array
   - Method installation path: compile source, create CompiledMethod,
     install via method_dict_insert (increments generation counter)
   - Tests: compile "Rectangle >> area", verify layout matches
     section 4.11 walkthrough

5. Integration: compile and execute
   - End-to-end test: compile a Smalltalk method from source string,
     install it on a class, send the message, verify the result
   - Test cases: "3 + 4" (arithmetic), "[42] value" (clean block),
     "x > 0 ifTrue: ['yes'] ifFalse: ['no']" (control flow inlining),
     "#(1 2 3) size" (literal array + send), cascades

Create branch: phase1/epic-6-compiler
Implement in order. Run ctest after each. Do not close the epic.
```

---

### Epic 7: Exception Handling

**Depends on:** Epic 6 (compiler needed to compile exception methods)

#### Claude Code Prompt

```
Read CLAUDE.md, PROJECT_STATUS.md, and docs/architecture/bytecode-spec.md
section 7.6 through 7.8 (unwind protection, exception handling),
section 7.11 (phase summary), section 8.8 (block primitives 86-87 for
markForUnwind/unmarkForUnwind).

Phase 1 Epic 7: Exception Handling (Handler Stack).

Phase 1 uses a handler stack approach per section 7.8 — an actor-local
linked list of handler entries, not thisContext chain walking. The
Smalltalk-level API (on:do:, signal, resume:) is identical regardless of
implementation. Phase 3 may replace the handler stack with context-chain
walking.

Phase 1 provides bootstrap-only ensure: for normal completion
(section 7.11). Full abnormal unwinding (ensure: firing during
NLR/exception) is Phase 2.

Create GitHub epic "Phase 1 Epic 7: Exception Handling".

Work items:

1. Handler stack data structure
   - Actor-local stack of handler entries: (exception class, handler block,
     frame reference)
   - Push on on:do: entry, pop on exit
   - signal walks the stack looking for matching handler

2. Exception/Error/MessageNotUnderstood/BlockCannotReturn class methods
   - Compile from Smalltalk source using the compiler (Epic 6)
   - Exception>>signal, Exception>>messageText, on:do: on BlockClosure
   - doesNotUnderstand: default implementation (section 10.9) signals
     MessageNotUnderstood

3. ensure: bootstrap-only normal-completion cleanup (section 7.7, 7.11)
   - markForUnwind: and unmarkForUnwind primitives (86-87) set/clear
     frame unwindHandler slot
   - ensure: implementation: markForUnwind, execute body, unmarkForUnwind,
     execute ensure block. NOT full abnormal unwinding — Phase 2 concern.

4. Tests
   - on:do: catches MessageNotUnderstood
   - signal propagates up handler stack
   - ensure: executes cleanup on normal completion
   - nested on:do: handlers

Create branch: phase1/epic-7-exceptions
Do not close the epic.
```

---

### Epic 8: Kernel Source Loading

**Depends on:** Epics 6 and 7

#### Claude Code Prompt

```
Read CLAUDE.md, PROJECT_STATUS.md, docs/architecture/bytecode-spec.md
section 11.3 (methods loaded from kernel source), section 11.7 step 10.

Phase 1 Epic 8: Kernel Source Loading (File-In).

After bootstrap primitives are wired, the rest of the class library is loaded
from .st source files. The file-in mechanism reads source, compiles each method,
and installs it. This is section 11.7 step 10.

Create GitHub epic "Phase 1 Epic 8: Kernel Source Loading".

Work items:

1. File-in reader
   - Create src/bootstrap/filein.h and src/bootstrap/filein.c
   - Read .st source files in chunk format (class definitions + method source)
   - For each method chunk: parse class context, compile method, install
   - For each class definition chunk: create class via
     subclass:instanceVariableNames:classVariableNames:poolDictionaries:category:

2. Kernel source files
   - Create kernel/ directory with .st files for:
     Object (printOn:, printString, copy, hash, etc.)
     Collection family (do:, collect:, select:, reject:, detect:, inject:into:)
     Number, Magnitude, SmallInteger (full arithmetic fallbacks for overflow)
     String, Symbol, Array, ByteArray (full protocol)
     Stream family (ReadStream, WriteStream)
     Boolean, True, False (full control flow)
     Exception, Error (full exception protocol)
   - Each file must be compilable by the Phase 1 compiler

3. Tier 2 class creation from source
   - Load Tier 2 classes from section 11.2: LargePositiveInteger,
     LargeNegativeInteger, Integer, Fraction, OrderedCollection,
     Dictionary, Set, Stream family, Boolean
   - These are created via subclass:instanceVariableNames:... in
     Smalltalk source

4. Integration: bootstrap + file-in produces a complete kernel image
   - Test: after file-in, OrderedCollection new add: 42; yourself works
   - Test: 'hello' reversed produces 'olleh'
   - Test: exception handling across loaded methods

Create branch: phase1/epic-8-kernel-source
Do not close the epic.
```

---

### Epic 9: Image Save/Load (Production)

**Depends on:** Epic 8 (complete kernel to save)

#### Claude Code Prompt

```
Read CLAUDE.md, PROJECT_STATUS.md, docs/decisions/012-image-format.md,
docs/architecture/bytecode-spec.md section 11.7 step 11, section 11.8,
and section 13 of the architecture doc.

Phase 1 Epic 9: Image Save/Load (Production).

Promote the spike-006 image format to production. The bootstrap creates a
complete kernel image (Epics 3-8), then saves it. Subsequent launches load
this image and skip bootstrap. This is section 11.7 step 11 and section 11.8.

Open decisions #16 (quiescing), #17 (root table), #18 (class identifiers)
must be resolved by Epic 0 before this epic starts.

Create GitHub epic "Phase 1 Epic 9: Image Save/Load (Production)".

Work items:

1. Production image writer
   - Create src/image/image.h and src/image/image.c (production, not spike)
   - Flat binary format per ADR 012: header + immutable section + object
     records + relocation table
   - Root table extension per the resolved decision on open item #17
   - Class identifier encoding per the resolved decision on open item #18
   - Single-actor quiescing per the resolved decision on open item #16

2. Production image loader
   - Load image, reconstruct: special object table, class table, symbol
     table, heap objects, global dictionary
   - Send #startUp to Smalltalk after load (section 11.8 step 6)

3. Bootstrap-to-image pipeline
   - sta_bootstrap_create_image() runs full bootstrap (Epics 3-8) then saves
   - sta_vm_load_image() loads saved image, skips bootstrap
   - Round-trip test: bootstrap, save, load, verify all classes, methods,
     globals survive

4. Image startup protocol
   - SystemDictionary>>startUp method (compiled from source)
   - Phase 1: minimal — just marks the system as running
   - Tests: load image, verify #startUp was sent

Create branch: phase1/epic-9-image
Do not close the epic.
```

---

### Epic 10: Eval Loop & Smoke Test

**Depends on:** Epic 9

#### Claude Code Prompt

```
Read CLAUDE.md, PROJECT_STATUS.md, docs/decisions/013-native-bridge.md
(sta_eval), docs/architecture/bytecode-spec.md section 11.

Phase 1 Epic 10: Eval Loop and Integration Smoke Test.

This is the Phase 1 capstone. The sta_eval() public API function compiles and
evaluates a Smalltalk expression string in the live image, returning a result.
This is the foundation for the Phase 3 workspace and for the embed_basic example.

Create GitHub epic "Phase 1 Epic 10: Eval Loop and Smoke Test".

Work items:

1. sta_eval() implementation
   - Implement sta_eval(STA_VM* vm, const char* expression) returning STA_OOP
   - Compiles expression as a "do-it" (anonymous method on UndefinedObject)
   - Executes the compiled method
   - Returns the result OOP (caller uses sta_inspect to examine)
   - Error handling: compilation errors and runtime exceptions return error OOP

2. Smoke test suite
   - Arithmetic: sta_eval("3 + 4") yields SmallInteger 7
   - String: sta_eval("'hello' size") yields SmallInteger 5
   - Block: sta_eval("[42] value") yields SmallInteger 42
   - Control flow: sta_eval("3 > 2 ifTrue: ['yes'] ifFalse: ['no']")
     yields 'yes'
   - Collection: sta_eval("#(1 2 3) collect: [:x | x * 2]") yields #(2 4 6)
   - Class creation:
     sta_eval("Object subclass: #Foo instanceVariableNames: ''...")
   - doesNotUnderstand: sta_eval("42 frobnicate") yields
     MessageNotUnderstood
   - Image round-trip: save image, load image, re-run smoke tests

3. Update embed_basic example
   - The examples/embed_basic/ smoke test uses sta_eval to demonstrate
     the API
   - Compile, run, print result to stdout

4. Update PROJECT_STATUS.md
   - Mark Phase 1 as complete
   - Update "Current phase" to "Phase 2 — Actor Runtime and Headless"
   - List Phase 2 blockers from the open decisions list

Create branch: phase1/epic-10-eval
Do not close the epic — leave for review.
```

---

## Part 4: Notes on the Plan

### What this plan does NOT cover

- **GC implementation.** Phase 1 uses bump allocation without collection.
  The nursery fills up and does not reclaim. For a bootstrap + smoke test
  workload this is fine — you allocate a large enough heap and the bootstrap
  completes before it fills. Real GC is a Phase 1.5 or early Phase 2 concern,
  after the system is functional enough to stress-test allocation patterns.

- **Multi-threading.** Phase 1 is single-actor, single-threaded. The scheduler
  spike (ADR 009) proved the design; Phase 2 activates it.

- **Closures and non-local return.** Phase 2. Phase 1 has clean blocks only.

### Risk areas

- **Epic 3 (metaclass circularity)** is the highest-risk task. Budget significant
  time. The difficulty is not conceptual but it is tedious and unforgiving.

- **Epic 5/6 feedback loop.** The interpreter needs compiled methods to test, but
  the compiler needs the interpreter to verify output. Resolution: Epic 5 uses
  hand-assembled bytecode arrays for initial testing. Epic 6 then verifies its
  output against the interpreter. Once both exist, integration tests tie them
  together.

- **Epic 8 (kernel source)** is the largest by volume — writing the full kernel
  class library in .st source. It is not technically difficult but is significant
  effort. Consider splitting it into sub-epics per class family if it becomes
  unwieldy.
