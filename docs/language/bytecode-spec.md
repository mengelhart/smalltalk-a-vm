# Smalltalk/A Bytecode Specification v2

## Table of Contents

- [§1 Scope and Conventions](#1-scope-and-conventions)
- [§2 Instruction Encoding](#2-instruction-encoding)
- [§3 Opcode Table](#3-opcode-table)
- [§4 Compiled Method and Literal Frame](#4-compiled-method-and-literal-frame)
- [§5 Syntax-to-Bytecode Compilation](#5-syntax-to-bytecode-compilation)
- [§6 Block and Closure Model](#6-block-and-closure-model)
- [§7 Non-Local Return and Unwinding](#7-non-local-return-and-unwinding)
- [§8 Primitive Table](#8-primitive-table)
- [§9 Actor Extension Opcodes](#9-actor-extension-opcodes)
- [§10 Message Dispatch and Caching](#10-message-dispatch-and-caching)
- [§11 Special Objects and Bootstrap Requirements](#11-special-objects-and-bootstrap-requirements)
- [§12 Quick Reference Table](#12-quick-reference-table)


## §1 Scope and Conventions

### 1.1 What this document is

This is the authoritative specification for the Smalltalk/A bytecode instruction set, compiled method format, block/closure model, primitive table, and actor extension opcodes. The compiler emits bytecode conforming to this spec. The interpreter executes bytecode conforming to this spec. Where this document and any implementation disagree, this document is correct.

This is not a tutorial, a design rationale document, or an architecture overview. For architecture and rationale, see `docs/architecture/smalltalk-a-vision-architecture-v3.md`. For individual design decisions, see the ADR series in `docs/decisions/`.

### 1.2 Relationship to the Blue Book

The Smalltalk/A language is standard Smalltalk as defined in Goldberg & Robson (1983). The bytecode instruction set implements the same *operations* as the Blue Book bytecode set — the same pushes, stores, sends, returns, and jumps — but uses a clean modern encoding. Blue Book byte values are not preserved. A direct mapping between Blue Book bytecodes and Smalltalk/A opcodes is neither intended nor useful.

Where this specification does not explicitly override Blue Book semantics, Blue Book semantics apply. Where this specification defines behavior that the Blue Book does not address (actor extensions, capability primitives, async I/O), this specification is the sole authority.

ANSI Smalltalk (X3J20) governs the class library surface. The Blue Book governs the core language, VM structure, and any ambiguous behavior. Where they conflict, this project documents the chosen behavior explicitly in the relevant section.

### 1.3 Phase markers

Every opcode, primitive, and semantic feature in this document carries a phase marker:

| Marker | Meaning |
|---|---|
| **Phase 1** | Implemented in Phase 1 — Minimal Live Kernel. The interpreter executes it; the compiler emits it. |
| **Phase 2** | Implemented in Phase 2 — Actor Runtime and Headless. Opcode exists in Phase 1 (reserved byte value, parseable by tooling) but raises `NotYetImplemented` if executed. |
| **Phase 3** | Implemented in Phase 3 — Native IDE, or later. Same reservation rule as Phase 2. |

Phase markers are implementation scheduling, not importance rankings. A Phase 3 feature is not less specified than a Phase 1 feature — it is fully defined here so that Phase 1 decisions never need to be retrofitted.

### 1.4 Notation

**Stack effects** are written in Forth-style notation: `( before -- after )` where the rightmost element is the top of stack.

Examples:
- `( -- value )` — pushes one value
- `( receiver -- result )` — pops receiver, pushes result
- `( receiver arg1 arg2 -- result )` — pops three, pushes one
- `( value -- )` — pops one value, pushes nothing

**Operand** refers to the byte (or two bytes after `OP_WIDE`) immediately following the opcode byte. It is written as `operand` in instruction descriptions.

**Literal frame index** is an operand value that indexes into the compiled method's literal frame (§4). Written as `lit[operand]`.

**Temp index** is an operand value that indexes into the current frame's temporary/argument slots. Written as `temp[operand]`.

**InstVar index** is an operand value that indexes into the receiver's named instance variable slots. Written as `instVar[operand]`.

**OOP** means Object-Oriented Pointer — a tagged machine word as defined in ADR 007. Either a SmallInteger immediate (bit 0 set) or a heap object pointer (bit 0 clear).

### 1.5 Architectural dependencies

This specification depends on decisions recorded in the following ADRs. If a conflict is found between this spec and an ADR, resolve it explicitly — do not silently prefer one over the other.

| ADR | Dependency |
|---|---|
| 007 | OOP tagging, object header layout, SmallInteger range |
| 008 | Deep-copy semantics for cross-actor messages |
| 009 | Reduction counting and `STA_REDUCTION_QUOTA` |
| 010 | Frame layout (`STA_Frame` = 40 bytes), TCO lookahead (`STA_SEND_WIDTH = 2`), stack slab model |
| 011 | Async I/O suspension and resume protocol |
| 012 | Image format and snapshot quiescing |
| 013 | Public API, handle lifecycle, live update semantics |

### 1.6 Typographic conventions

- `OP_PUSH_SELF` — an opcode mnemonic. Always uppercase with `OP_` prefix.
- `0x0A` — a hexadecimal byte value.
- `prim 1` — a primitive number.
- `#at:put:` — a Smalltalk selector.
- `STA_Frame` — a C type or constant from the implementation.
- `( a b -- c )` — a stack effect.

### 1.7 Revision history

| Version | Date | Summary |
|---|---|---|
| v1 | March 2026 | Initial specification. |
| v2 | March 2026 | Revisions from external review. Key changes: owner class literal changed from Association to direct class OOP (§4.4); OP_PUSH_GLOBAL unbound-global error language removed (§3.2); OP_ACTOR_SPAWN operand corrected to spawn descriptor (§3.9); primitive dispatch protocol clarified (§4.8); TCO/NLR compiler rule added (§5.11, §7.10); Phase 1 block restriction made explicit (§6.11); nil/true/false representation noted as open (§11.1). |

---

## §2 Instruction Encoding

### 2.1 Standard instruction format

Every Smalltalk/A bytecode instruction is exactly **2 bytes**: one opcode byte followed by one operand byte.

```
Byte 0      Byte 1
┌──────────┬──────────┐
│  opcode  │ operand  │
└──────────┴──────────┘
```

There are no 1-byte instructions and no variable-length instructions other than the wide prefix described in §2.2. This fixed-width property is load-bearing — the TCO lookahead in the interpreter checks `bytecode[pc + 2]` unconditionally after any send instruction, and `STA_SEND_WIDTH = 2` is a compile-time constant (ADR 010).

The operand byte is an unsigned 8-bit value (0–255). Its interpretation depends on the opcode: it may be a literal frame index, a temp index, an instVar index, a jump offset, an argument count, or unused (set to 0x00 for opcodes that take no operand).

### 2.2 Wide prefix

When an operand value exceeds 255, the instruction is preceded by the `OP_WIDE` prefix byte and a high-byte operand, forming a 4-byte sequence:

```
Byte 0      Byte 1      Byte 2      Byte 3
┌──────────┬──────────┬──────────┬──────────┐
│ OP_WIDE  │ high byte│  opcode  │ low byte │
└──────────┴──────────┴──────────┴──────────┘
```

The effective operand is `(high_byte << 8) | low_byte`, yielding a 16-bit unsigned range of 0–65535.

`OP_WIDE` is not an instruction — it is a prefix that modifies the next instruction. It has no stack effect of its own. The interpreter, upon seeing `OP_WIDE`, reads the high byte, advances to the opcode, reads the low byte, computes the 16-bit operand, and dispatches the opcode normally.

Rules:
- `OP_WIDE` may precede any opcode that uses its operand byte as an index or offset (push, store, send, jump).
- `OP_WIDE` must not precede opcodes where the operand is an argument count that will never exceed 255 (no Smalltalk method takes 256+ arguments).
- `OP_WIDE` must not precede another `OP_WIDE`. The maximum operand is 16 bits.
- For the purposes of TCO lookahead, `OP_WIDE + instruction` occupies 4 bytes. The lookahead checks `bytecode[pc + STA_SEND_WIDTH]` for a standard send and `bytecode[pc + 4]` for a wide send. The compiler must not emit a wide send immediately before a return if it expects TCO — see §2.4.

### 2.3 Instruction length summary

| Form | Length | When used |
|---|---|---|
| `opcode operand` | 2 bytes | Operand fits in 0–255 (common case) |
| `OP_WIDE high opcode low` | 4 bytes | Operand requires 256–65535 |

There are no other instruction lengths. Any tool that needs to scan or disassemble bytecode can advance by 2 bytes per instruction, checking for `OP_WIDE` to advance by 4 instead.

### 2.4 TCO lookahead interaction

The interpreter performs tail-call optimization by checking whether the instruction immediately following a send is `OP_RETURN_TOP`. This check uses a fixed offset:

```
Standard send at pc:
  bytecode[pc]     == send opcode
  bytecode[pc + 1] == operand
  bytecode[pc + 2] == OP_RETURN_TOP?   ← TCO check

Wide send at pc:
  bytecode[pc]     == OP_WIDE
  bytecode[pc + 1] == high byte
  bytecode[pc + 2] == send opcode
  bytecode[pc + 3] == operand
  bytecode[pc + 4] == OP_RETURN_TOP?   ← TCO check
```

The compiler must be aware that TCO only fires when `OP_RETURN_TOP` is the *physically next* instruction after the send. Inserting any instruction between a tail-position send and its return (including `OP_POP`, `OP_DUP`, or another `OP_WIDE`) defeats TCO. This is a compiler responsibility, not an interpreter responsibility — the interpreter's check is purely mechanical.

**Additional restriction:** A send is not in tail position if the enclosing method contains any block literal with a non-local return (`^`). See §5.11 for the full rule.

### 2.5 Operand value 0x00

For opcodes that do not use their operand (e.g., `OP_PUSH_RECEIVER`, `OP_RETURN_TOP`, `OP_POP`, `OP_DUP`), the compiler must emit `0x00` as the operand byte. The interpreter ignores it, but a consistent zero simplifies disassembly, image serialization, and debugging tools.

### 2.6 Byte order

Bytecode streams are a flat array of bytes with no alignment requirements. The wide prefix high byte and low byte are always in the order shown in §2.2 (high byte first, low byte second) regardless of platform endianness. This is a bytecode convention, not a memory-layout dependency — the interpreter reads individual bytes, not multi-byte words.

---

## §3 Opcode Table

### 3.1 Opcode map overview

Opcodes are assigned in contiguous ranges by functional group. Unassigned values within a range are reserved for future use within that group. Unassigned ranges are reserved for future groups. Executing a reserved opcode is a fatal interpreter error.

| Range | Group |
|---|---|
| `0x00` | `OP_NOP` |
| `0x01` | `OP_WIDE` (prefix, not an instruction — see §2.2) |
| `0x02–0x0F` | Push |
| `0x10–0x1F` | Store |
| `0x20–0x2F` | Send |
| `0x30–0x3F` | Return |
| `0x40–0x4F` | Jump |
| `0x50–0x5F` | Stack and miscellaneous |
| `0x60–0x6F` | Block and closure |
| `0x70–0x7F` | Actor extensions |
| `0x80–0xEF` | Reserved for future use |
| `0xF0–0xFF` | Debug and instrumentation (Phase 3) |

### 3.2 Push group (`0x02–0x0F`)

| Byte | Mnemonic | Operand | Stack effect | Semantics | Phase |
|---|---|---|---|---|---|
| `0x02` | `OP_PUSH_RECEIVER` | unused (0x00) | `( -- receiver )` | Push the current receiver (`self`) onto the stack. | Phase 1 |
| `0x03` | `OP_PUSH_NIL` | unused (0x00) | `( -- nil )` | Push the special object `nil`. | Phase 1 |
| `0x04` | `OP_PUSH_TRUE` | unused (0x00) | `( -- true )` | Push the special object `true`. | Phase 1 |
| `0x05` | `OP_PUSH_FALSE` | unused (0x00) | `( -- false )` | Push the special object `false`. | Phase 1 |
| `0x06` | `OP_PUSH_LIT` | lit index | `( -- value )` | Push `lit[operand]` — a literal from the compiled method's literal frame (§4). | Phase 1 |
| `0x07` | `OP_PUSH_TEMP` | temp index | `( -- value )` | Push `temp[operand]` — a temporary or argument from the current frame. Arguments occupy temps 0 through `numArgs - 1`; temporaries follow. | Phase 1 |
| `0x08` | `OP_PUSH_INSTVAR` | instVar index | `( -- value )` | Push `instVar[operand]` — a named instance variable of the receiver. Index 0 is the first instance variable. | Phase 1 |
| `0x09` | `OP_PUSH_GLOBAL` | lit index | `( -- value )` | Push the value of the global variable whose Association is `lit[operand]`. The Association object is stored in the literal frame; the interpreter reads its `value` slot. The compiler only emits this opcode for globals that have been resolved to an Association at compile time. Reading an Association whose `value` slot contains `nil` simply pushes `nil` — this is not an error condition at the interpreter level. | Phase 1 |
| `0x0A` | `OP_PUSH_CONTEXT` | unused (0x00) | `( -- context )` | Push a reified `thisContext` object representing the current activation frame. The frame is promoted to a heap object on demand (see §6). | Phase 3 |
| `0x0B` | `OP_PUSH_SMALLINT` | unsigned byte | `( -- int )` | Push the SmallInteger value `operand` (range 0–255). For values outside this range, the compiler uses `OP_PUSH_LIT` with a SmallInteger literal in the literal frame. With `OP_WIDE`, range extends to 0–65535. | Phase 1 |
| `0x0C` | `OP_PUSH_MINUS_ONE` | unused (0x00) | `( -- -1 )` | Push SmallInteger `-1`. Common enough to warrant its own opcode; avoids a literal frame slot. | Phase 1 |
| `0x0D` | `OP_PUSH_ZERO` | unused (0x00) | `( -- 0 )` | Push SmallInteger `0`. | Phase 1 |
| `0x0E` | `OP_PUSH_ONE` | unused (0x00) | `( -- 1 )` | Push SmallInteger `1`. | Phase 1 |
| `0x0F` | `OP_PUSH_TWO` | unused (0x00) | `( -- 2 )` | Push SmallInteger `2`. | Phase 1 |

**Note on OP_PUSH_SMALLINT range:** the current unsigned-only inline range (0–65535 with WIDE) means negative integers other than -1 require a literal frame slot. A signed inline range (e.g., -128 to 127 in 8 bits) may be revisited if Phase 1 profiling shows significant literal frame pressure from negative constants. This is an optimization question, not a correctness issue.

### 3.3 Store group (`0x10–0x1F`)

| Byte | Mnemonic | Operand | Stack effect | Semantics | Phase |
|---|---|---|---|---|---|
| `0x10` | `OP_STORE_TEMP` | temp index | `( value -- value )` | Store top of stack into `temp[operand]`. The value remains on the stack (store, not pop-store). | Phase 1 |
| `0x11` | `OP_STORE_INSTVAR` | instVar index | `( value -- value )` | Store top of stack into `instVar[operand]` of the receiver. Value remains on the stack. | Phase 1 |
| `0x12` | `OP_STORE_GLOBAL` | lit index | `( value -- value )` | Store top of stack into the `value` slot of the Association at `lit[operand]`. Value remains on the stack. | Phase 1 |
| `0x13` | `OP_POP_STORE_TEMP` | temp index | `( value -- )` | Pop top of stack and store into `temp[operand]`. This is the common case for local assignment — the compiler emits this rather than `OP_STORE_TEMP` + `OP_POP` to save an instruction. | Phase 1 |
| `0x14` | `OP_POP_STORE_INSTVAR` | instVar index | `( value -- )` | Pop top of stack and store into `instVar[operand]` of the receiver. | Phase 1 |
| `0x15` | `OP_POP_STORE_GLOBAL` | lit index | `( value -- )` | Pop top of stack and store into the `value` slot of the Association at `lit[operand]`. | Phase 1 |
| `0x16` | `OP_STORE_OUTER_TEMP` | encoded index | `( value -- value )` | Store top of stack into a temporary in an enclosing scope. Operand encodes both the scope depth and the temp index within that scope (see §6 for encoding). Value remains on the stack. | Phase 2 |
| `0x17` | `OP_POP_STORE_OUTER_TEMP` | encoded index | `( value -- )` | Pop-and-store variant of `OP_STORE_OUTER_TEMP`. | Phase 2 |

Note on store semantics: the non-pop variants (`OP_STORE_*`) leave the value on the stack because Smalltalk assignment is an expression — `x := y := 3` requires the assigned value to remain available. The pop variants (`OP_POP_STORE_*`) are an optimization for statement-level assignment where the value is immediately discarded. The compiler chooses between them based on whether the assignment result is used.

### 3.4 Send group (`0x20–0x2F`)

All send opcodes increment the reduction counter. When the counter exceeds `STA_REDUCTION_QUOTA` (ADR 009), the interpreter yields the current actor to the scheduler after the send completes.

| Byte | Mnemonic | Operand | Stack effect | Semantics | Phase |
|---|---|---|---|---|---|
| `0x20` | `OP_SEND` | lit index | `( receiver arg1 ... argN -- result )` | Send the message whose selector is `lit[operand]` to the receiver. The number of arguments is determined by the selector's arity (unary = 0, binary = 1, keyword = number of colons). The receiver and arguments are popped; the result is pushed. Full dispatch described in §10. | Phase 1 |
| `0x21` | `OP_SEND_SUPER` | lit index | `( receiver arg1 ... argN -- result )` | Super send. Identical to `OP_SEND` except that method lookup begins at the superclass of the class in which the sending method is defined, not at the receiver's class. The literal frame slot at `operand` holds the selector. The class to start the lookup from is determined by the compiled method's owner class (stored in the last literal slot — see §4.4). | Phase 1 |

The compiler emits `OP_SEND` for all general message sends. `OP_SEND_SUPER` is emitted only for explicit `super` sends. There is no special-selector opcode — all selectors, including common ones like `+`, `-`, `<`, `at:`, `at:put:`, and `==`, use `OP_SEND` with the selector in the literal frame. Inline caching (§10) handles the performance side; there is no need for a parallel dispatch path for frequent selectors.

Byte values `0x22–0x2F` are reserved for future use within the send group.

### 3.5 Return group (`0x30–0x3F`)

| Byte | Mnemonic | Operand | Stack effect | Semantics | Phase |
|---|---|---|---|---|---|
| `0x30` | `OP_RETURN_TOP` | unused (0x00) | `( value -- )` | Return the top of stack to the sender. This is the normal method return. The current frame is popped; execution resumes in the calling frame with the returned value pushed onto its stack. This is the opcode the TCO lookahead checks for (§2.4). | Phase 1 |
| `0x31` | `OP_RETURN_SELF` | unused (0x00) | `( -- )` | Return `self` (the receiver) to the sender. Equivalent to `OP_PUSH_RECEIVER` + `OP_RETURN_TOP` but saves an instruction. This is the implicit return for methods that don't end with an explicit `^ expression`. | Phase 1 |
| `0x32` | `OP_RETURN_NIL` | unused (0x00) | `( -- )` | Return `nil` to the sender. Used by methods that explicitly return `^ nil`. | Phase 1 |
| `0x33` | `OP_RETURN_TRUE` | unused (0x00) | `( -- )` | Return `true` to the sender. | Phase 1 |
| `0x34` | `OP_RETURN_FALSE` | unused (0x00) | `( -- )` | Return `false` to the sender. | Phase 1 |
| `0x35` | `OP_NON_LOCAL_RETURN` | unused (0x00) | `( value -- )` | Non-local return from within a block. Unwinds the stack to the home method's frame and returns the top-of-stack value to the home method's sender. If the home method has already returned (dead frame), signals `BlockCannotReturn`. See §7 for full semantics. | Phase 2 |

Byte values `0x36–0x3F` are reserved for future use within the return group.

### 3.6 Jump group (`0x40–0x4F`)

Jump operands are **forward byte offsets** from the instruction following the jump. A jump with operand 0 is a no-op (falls through to the next instruction). Backward jumps use `OP_JUMP_BACK`. The compiler computes offsets after code generation; the operand is an unsigned byte (0–255), or 16-bit unsigned with `OP_WIDE`.

| Byte | Mnemonic | Operand | Stack effect | Semantics | Phase |
|---|---|---|---|---|---|
| `0x40` | `OP_JUMP` | forward offset | `( -- )` | Unconditional forward jump. `pc := pc + 2 + operand`. | Phase 1 |
| `0x41` | `OP_JUMP_TRUE` | forward offset | `( value -- )` | Pop top of stack. If it is `true`, jump forward by `operand` bytes. If `false`, fall through. If neither `true` nor `false`, send `mustBeBoolean` to the value (Blue Book semantics). | Phase 1 |
| `0x42` | `OP_JUMP_FALSE` | forward offset | `( value -- )` | Pop top of stack. If it is `false`, jump forward. If `true`, fall through. If neither, send `mustBeBoolean`. | Phase 1 |
| `0x43` | `OP_JUMP_BACK` | backward offset | `( -- )` | Unconditional backward jump. `pc := pc + 2 - operand`. Used for loops. A backward jump also checks the reduction counter — if the counter exceeds `STA_REDUCTION_QUOTA`, the actor yields before taking the jump. This prevents infinite loops from starving the scheduler. | Phase 1 |

Byte values `0x44–0x4F` are reserved for future use within the jump group. Nil-check jump optimizations may be added in a future phase if profiling justifies them, but no semantics are defined here.

Note on reduction counting: backward jumps (`OP_JUMP_BACK`) are the only jump that checks the reduction counter. Forward jumps and conditional jumps do not, because they cannot form loops by themselves. The combination of sends and backward jumps covers all preemption points: straight-line code preempts on sends, loops preempt on back-edges. A primitive-heavy tight loop always has a back-edge (`OP_JUMP_BACK`) and will therefore yield. Successful primitives do not independently count as reductions in Phase 1; this may be revisited in Phase 2 if profiling reveals scheduling fairness issues with primitive-heavy inner loops.

### 3.7 Stack and miscellaneous group (`0x50–0x5F`)

| Byte | Mnemonic | Operand | Stack effect | Semantics | Phase |
|---|---|---|---|---|---|
| `0x50` | `OP_POP` | unused (0x00) | `( value -- )` | Discard the top of stack. Used after expression statements whose value is unused. | Phase 1 |
| `0x51` | `OP_DUP` | unused (0x00) | `( value -- value value )` | Duplicate the top of stack. Used for cascades and for store-and-use patterns. | Phase 1 |
| `0x52` | `OP_PRIMITIVE` | prim index | *varies* | Execute primitive number `operand` (0–255) or the 16-bit primitive number with `OP_WIDE`. Arguments are already on the stack per the method's argument count. On success, the result replaces the arguments and receiver. On failure, a failure code is written to a designated temp (see §8) and execution falls through to the method's bytecode body. See §8 for the full primitive table. | Phase 1 |

Byte values `0x53–0x5F` are reserved for future use within this group.

### 3.8 Block and closure group (`0x60–0x6F`)

Block and closure creation use the standard 2-byte instruction format. The block body length, argument count, and other metadata are stored in a **block descriptor** object in the literal frame, not encoded inline in the bytecode stream. This preserves the universal 2-byte instruction rule with no exceptions.

| Byte | Mnemonic | Operand | Stack effect | Semantics | Phase |
|---|---|---|---|---|---|
| `0x60` | `OP_BLOCK_COPY` | lit index | `( -- block )` | Create a clean block (no captured variables). `lit[operand]` is a block descriptor in the literal frame containing: the block's argument count, the block body's start PC (absolute offset within the method's bytecode), and the block body's length in bytes. The interpreter pushes a block object referencing the current method and the descriptor, then jumps past the block body (start PC + body length). See §6. | Phase 1 |
| `0x61` | `OP_CLOSURE_COPY` | lit index | `( val1 ... valN -- closure )` | Create a capturing closure. `lit[operand]` is a closure descriptor in the literal frame containing: the number of copied values, the block's argument count, the block body's start PC, and the block body's length. The interpreter pops `num_copied` values from the stack, allocates a closure object with the copied values, start PC, argument count, and a reference to the enclosing context (for outer temp access via indirect cells), then jumps past the body. See §6. | Phase 2 |
| `0x62` | `OP_PUSH_OUTER_TEMP` | encoded index | `( -- value )` | Push a temporary variable from an enclosing scope, accessed via the closure's context chain. Operand encodes scope depth and temp index (see §6 for encoding). | Phase 2 |

Byte values `0x63–0x6F` are reserved for future use within the block group.

### 3.9 Actor extension group (`0x70–0x7F`)

All actor opcodes trigger deep-copy of message arguments per ADR 008. Immutable objects (`STA_OBJ_IMMUTABLE` flag set) are shared by pointer; all other objects are deep-copied into a transfer buffer owned by the target actor.

| Byte | Mnemonic | Operand | Stack effect | Semantics | Phase |
|---|---|---|---|---|---|
| `0x70` | `OP_SEND_ASYNC` | lit index | `( actor arg1 ... argN -- )` | Asynchronous fire-and-forget message send to an actor. Selector is `lit[operand]`. Arguments are deep-copied and enqueued in the target actor's mailbox. No result is returned — the stack is cleared of the actor reference and arguments. If the target mailbox is full, `STA_ERR_MAILBOX_FULL` is signaled to the sender (ADR 008). | Phase 2 |
| `0x71` | `OP_ASK` | lit index | `( actor arg1 ... argN -- future )` | Asynchronous request-response send. Selector is `lit[operand]`. Arguments are deep-copied and enqueued. A future object is pushed onto the sender's stack. The future resolves to the result, a failure, or a timeout (see §9 for future semantics). | Phase 2 |
| `0x72` | `OP_ACTOR_SPAWN` | lit index | `( arg1 ... argN -- actor )` | Spawn a new actor. `lit[operand]` is a **spawn descriptor** in the literal frame containing the actor class OOP and the initialization argument count (see §4.7, §9.4). Arguments are deep-copied and passed to the new actor's `initialize` method. The new actor's opaque address is pushed onto the spawning actor's stack. The new actor is registered with the scheduler and becomes runnable. | Phase 2 |
| `0x73` | `OP_SELF_ADDRESS` | unused (0x00) | `( -- addr )` | Push the current actor's opaque address. Used when an actor needs to pass its own address to another actor so it can receive replies. | Phase 2 |

Phase 1 behavior: these opcodes occupy their assigned byte values. The interpreter recognizes them but signals `NotYetImplemented` if executed. The compiler may emit them in Phase 1 for testing purposes, but no actor runtime exists to service them until Phase 2.

Byte values `0x74–0x7F` are reserved for future use within the actor group.

### 3.10 Reserved: Debug and instrumentation (`0xF0–0xFF`)

These byte values are reserved for Phase 3 debug and instrumentation opcodes. No semantics are defined here. Possible future uses include breakpoint traps, trace hooks, coverage counters, and profiling markers. Executing any opcode in this range before Phase 3 is a fatal interpreter error.

### 3.11 Opcode 0x00 — NOP

| Byte | Mnemonic | Operand | Stack effect | Semantics | Phase |
|---|---|---|---|---|---|
| `0x00` | `OP_NOP` | unused (0x00) | `( -- )` | No operation. Advances PC by 2. May be used for alignment padding by tooling. The compiler never emits it. | Phase 1 |

### 3.12 Summary of defined opcodes

Total defined opcodes: **40** (31 Phase 1, 8 Phase 2, 1 Phase 3)

| Group | Count | Byte range |
|---|---|---|
| NOP | 1 | `0x00` |
| Wide prefix | 1 | `0x01` |
| Push | 14 | `0x02–0x0F` |
| Store | 8 | `0x10–0x17` |
| Send | 2 | `0x20–0x21` |
| Return | 6 | `0x30–0x35` |
| Jump | 4 | `0x40–0x43` |
| Stack/misc | 3 | `0x50–0x52` |
| Block/closure | 3 | `0x60–0x62` |
| Actor | 4 | `0x70–0x73` |

Remaining byte values are reserved. The opcode space accommodates substantial future growth without renumbering.

---

## §4 Compiled Method and Literal Frame

### 4.1 Compiled method object

A compiled method is a heap object like any other — it has an `STA_ObjHeader` (ADR 007) and is an instance of class `CompiledMethod`. It lives in the shared immutable region after compilation (bytecode and literals are write-once; live update replaces the entire method object atomically per ADR 004/013).

A compiled method contains three logical sections laid out in a single contiguous object:

```
┌──────────────────────────────────────────────────────┐
│  STA_ObjHeader  (standard object header — ADR 007)   │
├──────────────────────────────────────────────────────┤
│  Method header word                                  │
├──────────────────────────────────────────────────────┤
│  Literal frame   (array of OOP slots)                │
│    lit[0]                                            │
│    lit[1]                                            │
│    ...                                               │
│    lit[numLiterals - 1]                              │
├──────────────────────────────────────────────────────┤
│  Bytecode array   (raw bytes, not OOP-containing)    │
│    byte[0]                                           │
│    byte[1]                                           │
│    ...                                               │
│    byte[numBytecodes - 1]                            │
└──────────────────────────────────────────────────────┘
```

The GC must know where the literal frame ends and the bytecode array begins, because it scans the literal frame for OOP roots but must not interpret bytecode bytes as pointers. The method header word provides the literal count for this purpose.

### 4.2 Method header word

The method header is a single tagged SmallInteger OOP stored as the first word of the method's payload (before the literal frame). Encoding it as a SmallInteger means the GC treats it as a non-pointer value and does not attempt to trace it.

Bit layout (63 usable bits after the SmallInteger tag in bit 0):

| Bits | Width | Field | Range | Meaning |
|---|---|---|---|---|
| 1–8 | 8 | `numArgs` | 0–255 | Number of arguments the method expects. |
| 9–16 | 8 | `numTemps` | 0–255 | Number of temporaries (including arguments). Arguments occupy temps 0 through `numArgs - 1`; compiler-generated temporaries follow. |
| 17–24 | 8 | `numLiterals` | 0–255 | Number of OOP slots in the literal frame. With `OP_WIDE`, literal indices can reach 65535 — but the header field is 8 bits, limiting a single method to 255 literal slots. This is a deliberate constraint; methods needing more than 255 literals should be decomposed. |
| 25–32 | 8 | `primitiveIndex` | 0–255 | Primitive number for this method, or 0 if no primitive. Values 1–255 address Blue Book primitives directly. For Smalltalk/A extension primitives (256+), this field is 0 and the extended primitive number is encoded in the bytecode preamble — see §4.8 for the complete dispatch protocol. |
| 33 | 1 | `hasPrimitive` | 0–1 | 1 if this method has a primitive (the interpreter tries the primitive before executing bytecode). 0 otherwise. |
| 34 | 1 | `largeFrame` | 0–1 | 1 if this method requires a large stack frame. 0 for the default frame size. The interpreter uses this to decide how much stack slab space to reserve when activating the method. This is a Phase 1 minimum; it may become a multi-bit field or computed value in later phases once the stack slab growth policy (open decision #6 from PROJECT_STATUS) is resolved. |
| 35–62 | 28 | reserved | — | Reserved for future use (invocation counters, JIT metadata, etc.). Must be 0. |

The method header is read by the interpreter on every method activation. It must be decodable with simple shifts and masks — no indirection, no secondary lookups.

### 4.3 Literal frame

The literal frame is an array of `numLiterals` OOP slots immediately following the method header word. Every operand in the bytecode that references the literal frame (push literal, push global, send, super send, block copy, closure copy, actor spawn) indexes into this array.

The literal frame contains the following kinds of values, intermixed in whatever order the compiler assigns:

| Kind | What it is | How it gets there | Who references it |
|---|---|---|---|
| **Literal constant** | A SmallInteger, Float, String, Symbol, Array, or other immutable value. | Compiler places it directly. | `OP_PUSH_LIT` |
| **Selector symbol** | A Symbol used as a message selector. | Compiler places it for each distinct send site (or shares a slot if the same selector is sent multiple times in the method). | `OP_SEND`, `OP_SEND_SUPER`, `OP_SEND_ASYNC`, `OP_ASK` |
| **Global association** | An Association object (key→value pair) from the global dictionary. The key is the global's name; the value is the global's current value. | Compiler resolves the global name at compile time and stores the Association. | `OP_PUSH_GLOBAL`, `OP_STORE_GLOBAL`, `OP_POP_STORE_GLOBAL` |
| **Block descriptor** | A small object describing a block's argument count, start PC, and body length. | Compiler creates it for each block literal in the method. | `OP_BLOCK_COPY` |
| **Closure descriptor** | A small object describing a closure's copied-value count, argument count, start PC, and body length. | Compiler creates it for each capturing block. | `OP_CLOSURE_COPY` |
| **Owner class** | The class OOP in which this method is defined. Used for super-send lookup. | Compiler places it as the last literal slot (see §4.4). | `OP_SEND_SUPER` (implicitly) |
| **Spawn descriptor** | A small object containing an actor class OOP and initialization argument count. | Compiler creates it for `OP_ACTOR_SPAWN` call sites. | `OP_ACTOR_SPAWN` |

The compiler is free to share literal frame slots when the same value appears at multiple sites. For example, if a method sends `#at:` three times, the selector symbol `#at:` occupies one literal slot referenced by all three `OP_SEND` instructions.

### 4.4 Owner class — the last literal convention

The last slot in the literal frame (`lit[numLiterals - 1]`) holds **the class OOP in which this method is defined**. This is the **owner class** and is used for two purposes:

1. **Super sends.** `OP_SEND_SUPER` starts method lookup at the superclass of the owner class, not at the receiver's class. The interpreter reads the owner class from the last literal slot and takes its superclass.
2. **Recompilation and tooling.** When a method is inspected or replaced via live update (ADR 013), the runtime can identify which class the method belongs to by reading the last literal.

The owner class is stored as a **direct class OOP**, not wrapped in an Association. This is a deliberate departure from some historical Smalltalk implementations that stored an Association whose value was the class. The direct reference is simpler (one fewer dereference for super sends), avoids coupling method identity to the global dictionary, and is stable across class renames — the class object's identity does not change when its name binding changes.

A method with no sends, no globals, and no literals still has `numLiterals >= 1` because the owner class slot is always present.

### 4.5 Block descriptor object

A block descriptor is a small immutable object (instance of class `BlockDescriptor`) stored in the literal frame. It contains:

| Field | Type | Meaning |
|---|---|---|
| `startPC` | SmallInteger | Byte offset of the block body's first instruction within the method's bytecode array. |
| `bodyLength` | SmallInteger | Length of the block body in bytes. The interpreter jumps to `startPC + bodyLength` after creating the block object. |
| `numArgs` | SmallInteger | Number of arguments the block expects (0 for `[ ... ]`, 1 for `[:x | ... ]`, etc.). |

The block descriptor is immutable and shared. If the same block literal appears in the literal frame, only one descriptor object exists.

### 4.6 Closure descriptor object

A closure descriptor extends the block descriptor with one additional field:

| Field | Type | Meaning |
|---|---|---|
| `startPC` | SmallInteger | Same as block descriptor. |
| `bodyLength` | SmallInteger | Same as block descriptor. |
| `numArgs` | SmallInteger | Same as block descriptor. |
| `numCopied` | SmallInteger | Number of values copied from the enclosing scope into the closure. These values are on the stack when `OP_CLOSURE_COPY` executes; the interpreter pops this many values. |

The closure descriptor is an instance of class `ClosureDescriptor` (a subclass of `BlockDescriptor` or a separate class — the compiler and interpreter care only about the field layout, not the class hierarchy).

### 4.7 Spawn descriptor object

A spawn descriptor is a small immutable object stored in the literal frame for `OP_ACTOR_SPAWN` call sites. It contains:

| Field | Type | Meaning |
|---|---|---|
| `actorClass` | OOP → Class | The class of the actor to create. Must be a subclass of `Actor`. |
| `numArgs` | SmallInteger | Number of initialization arguments on the stack. |

The spawn descriptor is an instance of class `SpawnDescriptor`. Unlike `OP_SEND` (which derives argument count from selector arity), `OP_ACTOR_SPAWN` needs the argument count explicitly because there is no selector to inspect. The compiler creates the spawn descriptor at compile time.

### 4.8 Bytecode array

The bytecode array is a sequence of raw bytes following the literal frame. It is not an OOP-containing region — the GC does not scan it. The first byte of the bytecode array is at a known offset computed from the method header: `header_size + (numLiterals * word_size)`.

The bytecode array contains:
- The method's executable instructions, starting at byte offset 0.
- Block bodies, embedded inline. A block body's start PC (stored in its descriptor) is an offset within this array. The enclosing method's bytecode jumps past block bodies using the body length from the descriptor.
- A primitive preamble, if the method has a primitive. See §4.9 for the complete primitive dispatch protocol.

The bytecode array length is not stored explicitly in the method header. It is determined by the overall object size (from `STA_ObjHeader.size`) minus the header word and literal frame.

### 4.9 Primitive dispatch protocol

This section defines the single authoritative protocol for primitive dispatch. The method header and the bytecode preamble work together in a defined two-stage sequence — there is no ambiguity about which is authoritative.

**Stage 1 — Header check.** On method activation, the interpreter checks `hasPrimitive` in the method header. If clear (0), this method has no primitive — skip to bytecode execution.

**Stage 2 — Primitive number resolution.** If `hasPrimitive` is set (1):

- If `primitiveIndex != 0` (range 1–255): the primitive number is `primitiveIndex`. Call the C function registered for that number directly. **No bytecode is read.** This is the fast path for Blue Book kernel primitives.
- If `primitiveIndex == 0`: this is an extension primitive (number 256+). The first instruction in the bytecode array is the primitive preamble:

```
For extension primitives:
  OP_WIDE  highByte  OP_PRIMITIVE  lowByte     ← 4 bytes
  Effective primitive number: (highByte << 8) | lowByte
```

The interpreter reads these 4 bytes, computes the extended primitive number, and calls the C function registered for that number.

**On primitive success:** the C function writes the result. The interpreter pushes the result onto the caller's stack in place of the receiver and arguments. No frame is created. No bytecode executes. The method returns immediately.

**On primitive failure:** the C function returns a failure code. The interpreter creates a frame normally, stores the failure code in the first compiler-generated temporary (`temp[numArgs]`), and begins executing the bytecode body at the instruction after the primitive preamble (offset 2 for a standard primitive, offset 4 for a wide primitive). The bytecode body is the fallback — typically error handling or a Smalltalk-level reimplementation.

This means primitive methods have two entry points: the fast path (primitive succeeds, no frame needed) and the slow path (primitive fails, frame is created, bytecode executes). The compiler is responsible for generating meaningful fallback bytecode.

### 4.10 Argument count and selector arity

The number of arguments a send pops from the stack is determined by the selector's arity, not by the opcode or an explicit argument-count operand. The interpreter knows the arity because:

- **Unary selectors** (e.g., `#size`, `#yourself`) — 0 arguments. The selector symbol contains no colons.
- **Binary selectors** (e.g., `#+`, `#=`, `#@`) — 1 argument. The selector symbol starts with a special character.
- **Keyword selectors** (e.g., `#at:`, `#at:put:`, `#inject:into:`) — N arguments, where N is the number of colons in the selector.

The selector's arity is computable from the selector symbol at send time. In practice, the inline cache (§10) stores the arity alongside the cached class and method, so the arity lookup happens once per cache miss, not on every send.

### 4.11 Example: method layout walkthrough

Consider the following method:

```smalltalk
Rectangle >> area
    ^ width * height
```

Compiled method contents:

**Method header:**
- `numArgs`: 0 (unary message, no explicit arguments; the receiver is always present but not counted)
- `numTemps`: 0 (no temporaries)
- `numLiterals`: 4
- `primitiveIndex`: 0 (no primitive)
- `hasPrimitive`: 0
- `largeFrame`: 0

**Literal frame:**
- `lit[0]`: `#width` (selector symbol)
- `lit[1]`: `#height` (selector symbol)
- `lit[2]`: `#*` (selector symbol)
- `lit[3]`: `Rectangle` (owner class — direct class OOP, last literal convention, §4.4)

**Bytecode array:**

| Offset | Bytes | Instruction | Stack after |
|---|---|---|---|
| 0 | `0x02 0x00` | `OP_PUSH_RECEIVER` | `self` |
| 2 | `0x20 0x00` | `OP_SEND lit[0]` (#width) | `widthValue` |
| 4 | `0x02 0x00` | `OP_PUSH_RECEIVER` | `widthValue self` |
| 6 | `0x20 0x01` | `OP_SEND lit[1]` (#height) | `widthValue heightValue` |
| 8 | `0x20 0x02` | `OP_SEND lit[2]` (#*) | `result` |
| 10 | `0x30 0x00` | `OP_RETURN_TOP` | *(returned)* |

Total: 12 bytes of bytecode, 4 literal slots, method header word. The TCO lookahead at offset 8 checks byte 10 — finds `OP_RETURN_TOP` (0x30) — so the `#*` send is a tail call and the interpreter reuses the current frame. Note: this TCO is safe because the method contains no block literals with non-local returns (§5.11).

---

## §5 Syntax-to-Bytecode Compilation

This section defines the exact bytecode sequence the compiler emits for every Smalltalk syntactic construct. Each subsection shows a source fragment and the annotated bytecode listing. The compiler is not required to emit these exact sequences — it may optimize — but any optimization must preserve the observable semantics described here. These sequences are the *reference* compilation, and CC should implement them first before attempting any optimization.

### 5.1 Literals

**SmallInteger literals (0–255):**

```smalltalk
42
```

```
OP_PUSH_SMALLINT  42       ( -- 42 )
```

**SmallInteger literals (256–65535):**

```smalltalk
1000
```

```
OP_WIDE  0x03  OP_PUSH_SMALLINT  0xE8    ( -- 1000 )
  ← operand = (0x03 << 8) | 0xE8 = 1000
```

**SmallInteger literals outside 0–65535, and negative integers (except -1):**

```smalltalk
100000
```

```
OP_PUSH_LIT  N             ( -- 100000 )
  ← lit[N] contains SmallInteger 100000
```

**Common small integers:**

```smalltalk
-1        →  OP_PUSH_MINUS_ONE  0x00    ( -- -1 )
0         →  OP_PUSH_ZERO       0x00    ( -- 0 )
1         →  OP_PUSH_ONE        0x00    ( -- 1 )
2         →  OP_PUSH_TWO        0x00    ( -- 2 )
```

**String literals:**

```smalltalk
'hello'
```

```
OP_PUSH_LIT  N             ( -- 'hello' )
  ← lit[N] contains String object 'hello'
```

**Symbol literals:**

```smalltalk
#foo
```

```
OP_PUSH_LIT  N             ( -- #foo )
  ← lit[N] contains Symbol #foo (interned)
```

**Character literals:**

```smalltalk
$A
```

```
OP_PUSH_LIT  N             ( -- $A )
  ← lit[N] contains Character object for $A
```

**Float literals:**

```smalltalk
3.14
```

```
OP_PUSH_LIT  N             ( -- 3.14 )
  ← lit[N] contains Float object 3.14
```

**Array literals:**

```smalltalk
#(1 2 3)
```

```
OP_PUSH_LIT  N             ( -- #(1 2 3) )
  ← lit[N] contains an immutable Array object
```

**nil, true, false:**

```smalltalk
nil       →  OP_PUSH_NIL    0x00     ( -- nil )
true      →  OP_PUSH_TRUE   0x00     ( -- true )
false     →  OP_PUSH_FALSE  0x00     ( -- false )
```

### 5.2 Variable access

**Temporary and argument access:**

Arguments occupy temps 0 through `numArgs - 1`. Compiler-generated temporaries follow starting at index `numArgs`.

```smalltalk
"In a method:  foo: anArg | temp1 temp2 |"
anArg     →  OP_PUSH_TEMP  0        ( -- anArg )      "temp[0] = first argument"
temp1     →  OP_PUSH_TEMP  1        ( -- temp1 )      "temp[1] = first temporary"
temp2     →  OP_PUSH_TEMP  2        ( -- temp2 )      "temp[2] = second temporary"
```

**Instance variable access:**

```smalltalk
"In class Point with instVars: 'x y'"
x         →  OP_PUSH_INSTVAR  0     ( -- xValue )
y         →  OP_PUSH_INSTVAR  1     ( -- yValue )
```

**Global variable access:**

```smalltalk
Transcript
```

```
OP_PUSH_GLOBAL  N           ( -- transcriptValue )
  ← lit[N] contains the Association #Transcript → <the Transcript object>
```

**self and super:**

```smalltalk
self      →  OP_PUSH_RECEIVER  0x00   ( -- self )
```

`super` is not a variable access — it modifies the send that follows it. See §5.4.

### 5.3 Assignment

**Temporary assignment (result unused — statement level):**

```smalltalk
x := 42.
```

```
OP_PUSH_SMALLINT  42       ( -- 42 )
OP_POP_STORE_TEMP  1       ( -- )         "assuming x is temp[1]"
```

**Temporary assignment (result used — expression level):**

```smalltalk
y := x := 42.
```

```
OP_PUSH_SMALLINT  42       ( -- 42 )
OP_STORE_TEMP  1           ( -- 42 )      "x := 42; value stays on stack"
OP_POP_STORE_TEMP  2       ( -- )         "y := 42; value consumed"
```

```smalltalk
"As an expression (value needed, e.g. as argument to a send):"
self foo: (y := x := 42)
```

```
OP_PUSH_RECEIVER           ( -- self )
OP_PUSH_SMALLINT  42       ( -- self 42 )
OP_STORE_TEMP  1           ( -- self 42 )      "x := 42"
OP_STORE_TEMP  2           ( -- self 42 )      "y := 42; value stays for send"
OP_SEND  lit[N]            ( -- result )        "send #foo: with arg 42"
```

The compiler decides between `OP_STORE_*` (value stays) and `OP_POP_STORE_*` (value consumed) based on whether the assignment is in expression context or statement context.

**Instance variable assignment:**

```smalltalk
x := 10.
```

```
OP_PUSH_SMALLINT  10       ( -- 10 )
OP_POP_STORE_INSTVAR  0    ( -- )         "instVar[0] = x in class Point"
```

**Global assignment:**

```smalltalk
Globals := nil.
```

```
OP_PUSH_NIL                ( -- nil )
OP_POP_STORE_GLOBAL  N     ( -- )         "lit[N] = Association for Globals"
```

### 5.4 Message sends

**Unary send:**

```smalltalk
x size
```

```
OP_PUSH_TEMP  1            ( -- x )
OP_SEND  lit[N]            ( -- result )     "lit[N] = #size"
```

**Binary send:**

```smalltalk
a + b
```

```
OP_PUSH_TEMP  0            ( -- a )
OP_PUSH_TEMP  1            ( -- a b )
OP_SEND  lit[N]            ( -- result )     "lit[N] = #+"
```

**Keyword send:**

```smalltalk
dict at: key put: value
```

```
OP_PUSH_TEMP  0            ( -- dict )
OP_PUSH_TEMP  1            ( -- dict key )
OP_PUSH_TEMP  2            ( -- dict key value )
OP_SEND  lit[N]            ( -- result )     "lit[N] = #at:put:"
```

The receiver is pushed first, then arguments left to right, then `OP_SEND`. The number of values popped is 1 (receiver) + selector arity. One result is always pushed.

**Super send:**

```smalltalk
super initialize
```

```
OP_PUSH_RECEIVER           ( -- self )
OP_SEND_SUPER  lit[N]      ( -- result )     "lit[N] = #initialize"
```

The receiver on the stack is `self` (same object), but `OP_SEND_SUPER` starts lookup at the superclass of the owner class (§4.4), not at `self`'s class.

**Chained sends (left to right evaluation):**

```smalltalk
(a + b) * c
```

```
OP_PUSH_TEMP  0            ( -- a )
OP_PUSH_TEMP  1            ( -- a b )
OP_SEND  lit[N]            ( -- sumResult )       "lit[N] = #+"
OP_PUSH_TEMP  2            ( -- sumResult c )
OP_SEND  lit[M]            ( -- mulResult )       "lit[M] = #*"
```

### 5.5 Cascades

A cascade sends multiple messages to the same receiver. The receiver is evaluated once and duplicated for each additional send. The result of each send except the last is discarded; the result of the last send is the cascade's value.

```smalltalk
stream nextPut: $a; nextPut: $b; yourself
```

```
OP_PUSH_TEMP  0            ( -- stream )
OP_DUP                     ( -- stream stream )
OP_PUSH_LIT  lit[A]        ( -- stream stream $a )
OP_SEND  lit[B]            ( -- stream result1 )     "lit[B] = #nextPut:"
OP_POP                     ( -- stream )              "discard first result"
OP_DUP                     ( -- stream stream )
OP_PUSH_LIT  lit[C]        ( -- stream stream $b )
OP_SEND  lit[B]            ( -- stream result2 )     "reuse lit[B] = #nextPut:"
OP_POP                     ( -- stream )              "discard second result"
OP_SEND  lit[D]            ( -- result3 )             "lit[D] = #yourself"
```

Pattern: `DUP` before each non-final cascade message, `POP` after each non-final result. The final message is a normal send with no `DUP` or `POP`.

### 5.6 Control structures

Smalltalk has no special syntax for control flow. `ifTrue:`, `ifFalse:`, `ifTrue:ifFalse:`, `whileTrue:`, `whileFalse:`, `timesRepeat:`, and `to:do:` are ordinary message sends whose block arguments happen to be inlined by the compiler for performance. The compiler recognizes these patterns and emits jump-based bytecode rather than actual message sends with block objects.

**if/then:**

```smalltalk
x > 0 ifTrue: [ self doSomething ]
```

```
OP_PUSH_TEMP  0            ( -- x )
OP_PUSH_ZERO               ( -- x 0 )
OP_SEND  lit[N]            ( -- bool )        "lit[N] = #>"
OP_JUMP_FALSE  offset      ( -- )             "jump past the body if false"
OP_PUSH_RECEIVER           ( -- self )
OP_SEND  lit[M]            ( -- result )      "lit[M] = #doSomething"
  ← JUMP_FALSE lands here if false; result is nil (see below)
```

When `ifTrue:` is used as a statement (result discarded), the compiler emits `OP_POP` after the body. When the condition is false and the body is skipped, the result of the entire expression is `nil` — the compiler pushes `nil` at the jump target if the expression result is needed:

```smalltalk
y := x > 0 ifTrue: [ 42 ]
```

```
OP_PUSH_TEMP  0            ( -- x )
OP_PUSH_ZERO               ( -- x 0 )
OP_SEND  lit[N]            ( -- bool )        "lit[N] = #>"
OP_JUMP_FALSE  +6          ( -- )             "jump to nil push"
OP_PUSH_SMALLINT  42       ( -- 42 )
OP_JUMP  +2                ( -- 42 )          "jump past nil push"
OP_PUSH_NIL                ( -- nil )         "← JUMP_FALSE lands here"
OP_POP_STORE_TEMP  1       ( -- )             "y := result"
```

**if/then/else:**

```smalltalk
x > 0 ifTrue: [ 'positive' ] ifFalse: [ 'non-positive' ]
```

```
OP_PUSH_TEMP  0            ( -- x )
OP_PUSH_ZERO               ( -- x 0 )
OP_SEND  lit[N]            ( -- bool )
OP_JUMP_FALSE  +6          ( -- )             "jump to else branch"
OP_PUSH_LIT  lit[A]        ( -- 'positive' )
OP_JUMP  +4                ( -- 'positive' )  "jump past else"
OP_PUSH_LIT  lit[B]        ( -- 'non-positive' )   "← JUMP_FALSE lands here"
```

Both branches leave exactly one value on the stack. The compiler must guarantee equal stack depth at the join point after the if/then/else.

**while loop:**

```smalltalk
[ x < 10 ] whileTrue: [ x := x + 1 ]
```

```
  ← loop top (JUMP_BACK target)
OP_PUSH_TEMP  0            ( -- x )
OP_PUSH_SMALLINT  10       ( -- x 10 )
OP_SEND  lit[N]            ( -- bool )        "lit[N] = #<"
OP_JUMP_FALSE  +8          ( -- )             "exit loop"
OP_PUSH_TEMP  0            ( -- x )
OP_PUSH_ONE                ( -- x 1 )
OP_SEND  lit[M]            ( -- sum )         "lit[M] = #+"
OP_POP_STORE_TEMP  0       ( -- )             "x := x + 1"
OP_JUMP_BACK  offset       ( -- )             "back to loop top"
  ← JUMP_FALSE lands here (loop exit)
```

`OP_JUMP_BACK` checks the reduction counter on every iteration. A tight loop that does no sends (unusual but possible) is still preemptible at the back-edge.

**to:do: loop:**

```smalltalk
1 to: 10 do: [:i | self process: i ]
```

The compiler inlines this as an incrementing loop:

```
OP_PUSH_ONE                ( -- 1 )
OP_POP_STORE_TEMP  1       ( -- )             "i := 1 (temp[1])"
  ← loop top
OP_PUSH_TEMP  1            ( -- i )
OP_PUSH_SMALLINT  10       ( -- i 10 )
OP_SEND  lit[N]            ( -- bool )        "lit[N] = #<="
OP_JUMP_FALSE  +12         ( -- )             "exit loop"
OP_PUSH_RECEIVER           ( -- self )
OP_PUSH_TEMP  1            ( -- self i )
OP_SEND  lit[M]            ( -- result )      "lit[M] = #process:"
OP_POP                     ( -- )             "discard result"
OP_PUSH_TEMP  1            ( -- i )
OP_PUSH_ONE                ( -- i 1 )
OP_SEND  lit[P]            ( -- i+1 )         "lit[P] = #+"
OP_POP_STORE_TEMP  1       ( -- )             "i := i + 1"
OP_JUMP_BACK  offset       ( -- )             "back to loop top"
  ← loop exit
```

### 5.7 Return

**Explicit return:**

```smalltalk
^ 42
```

```
OP_PUSH_SMALLINT  42       ( -- 42 )
OP_RETURN_TOP              "returns 42 to sender"
```

**Implicit return (method with no explicit `^`):**

The compiler appends `OP_RETURN_SELF` at the end of any method body that does not end with an explicit return.

```smalltalk
Object >> yourself
    "no explicit return"
```

```
OP_RETURN_SELF             "returns self to sender"
```

**Return of common constants:**

```smalltalk
^ nil       →  OP_RETURN_NIL
^ true      →  OP_RETURN_TRUE
^ false     →  OP_RETURN_FALSE
^ self      →  OP_RETURN_SELF
```

These are compiler optimizations — the compiler recognizes that the return value is a known constant and emits the specialized return opcode instead of `OP_PUSH_* + OP_RETURN_TOP`.

### 5.8 Temporaries

Temporary variables are initialized to `nil` at method entry. The interpreter does this by zero-filling the temp slots in the frame (since `nil` is a well-known OOP, and the frame layout from ADR 010 has the temp/stack area following the fixed header).

The compiler assigns temp indices sequentially:
- `temp[0]` through `temp[numArgs - 1]` — arguments (filled by the caller)
- `temp[numArgs]` through `temp[numTemps - 1]` — declared temporaries (initialized to nil)

For primitive methods, the first temp after the arguments (`temp[numArgs]`) is reserved for the primitive failure code. The compiler must account for this when assigning indices.

```smalltalk
foo: a bar: b
    | x y |
    ...
```

```
temp[0] = a       (argument)
temp[1] = b       (argument)
temp[2] = x       (temporary, initialized to nil)
temp[3] = y       (temporary, initialized to nil)
numArgs = 2
numTemps = 4
```

### 5.9 Block literals

**Clean block (no captured variables) — Phase 1:**

```smalltalk
[ 42 ]
```

```
OP_BLOCK_COPY  lit[N]      ( -- block )
  ← block body start (interpreter jumps past this)
  OP_PUSH_SMALLINT  42     ( -- 42 )
  OP_RETURN_TOP            "return from block"
  ← block body end (interpreter resumes here)
```

The descriptor at `lit[N]` tells the interpreter where the body starts and how long it is. After creating the block object, the interpreter sets `pc` to `startPC + bodyLength`, which is the first instruction after the block body.

**Block with arguments:**

```smalltalk
[:x :y | x + y]
```

```
OP_BLOCK_COPY  lit[N]      ( -- block )
  ← block body start
  OP_PUSH_TEMP  0           ( -- x )          "block arg 0"
  OP_PUSH_TEMP  1           ( -- x y )        "block arg 1"
  OP_SEND  lit[M]           ( -- result )     "lit[M] = #+"
  OP_RETURN_TOP             "return from block"
  ← block body end
```

Block arguments are mapped to temp indices local to the block activation frame. When the block is invoked via `value:value:`, the caller pushes arguments, and the block's frame receives them as `temp[0]`, `temp[1]`, etc.

**Block as argument to an inlined control structure:**

When the compiler inlines `ifTrue:`, `whileTrue:`, etc., the block is not created as an object at all — its body is compiled inline with jumps. This is shown in §5.6. `OP_BLOCK_COPY` is only emitted when the block is used as a first-class value (passed as an argument, stored in a variable, or returned).

### 5.10 Non-local return in blocks

```smalltalk
foo
    | result |
    result := collection detect: [:each | each isValid ifTrue: [^ each]].
    ^ result
```

The `^ each` inside the block is a non-local return — it returns from `foo`, not from the block. The compiler emits `OP_NON_LOCAL_RETURN` instead of `OP_RETURN_TOP`:

```
  ← inside the block body
  OP_PUSH_TEMP  0           ( -- each )
  OP_NON_LOCAL_RETURN        "unwind to foo's sender and return each"
```

Full semantics of non-local return are in §7. For compilation purposes: the compiler emits `OP_RETURN_TOP` for `^` at method level and `OP_NON_LOCAL_RETURN` for `^` inside any block.

### 5.11 Tail position

The compiler tracks whether a send is in tail position — meaning the instruction immediately following it would be `OP_RETURN_TOP`. In tail position, the compiler emits the send followed immediately by `OP_RETURN_TOP` with no intervening instructions, enabling the interpreter's TCO lookahead (§2.4).

```smalltalk
foo
    ^ self bar
```

```
OP_PUSH_RECEIVER           ( -- self )
OP_SEND  lit[N]            ( -- result )     "lit[N] = #bar"
OP_RETURN_TOP              "← immediately after send; TCO fires"
```

The compiler must not insert `OP_POP`, `OP_DUP`, or any other instruction between a tail-position send and its return. If optimizations or desugaring insert instructions, the send is no longer in tail position and TCO does not fire. This is acceptable — TCO is an optimization, not a semantic guarantee. The compiler should prefer preserving tail position where natural but must not contort code to achieve it.

**Critical restriction — NLR-bearing blocks disable TCO:** A send is **not in tail position** if the enclosing method contains any block literal with a non-local return (`^` inside a block body). The compiler must not emit `OP_RETURN_TOP` immediately after such a send as a TCO candidate. This rule prevents TCO from invalidating the home frame marker of NLR-bearing blocks, which would change observable program semantics — a `BlockCannotReturn` that would not occur without TCO. See §7.10 for the full interaction analysis. The runtime's frame marker check (§7.4) remains as a safety net for pathological cases (e.g., hand-assembled bytecode), but the compiler rule eliminates the hazard in practice.

Methods that do not contain any NLR-bearing blocks are unaffected by this restriction and may use TCO freely.

### 5.12 Compilation of `self` sends

A send to `self` is a normal send — there is no special opcode. The compiler pushes the receiver and emits `OP_SEND`:

```smalltalk
self doSomething
```

```
OP_PUSH_RECEIVER           ( -- self )
OP_SEND  lit[N]            ( -- result )     "lit[N] = #doSomething"
```

This is identical to any other unary send where the receiver happens to be `self`. The dispatch mechanism (§10) handles it through normal lookup on `self`'s class.

---

## §6 Block and Closure Model

This section defines the runtime representation and semantics of blocks and closures. It is the most complex part of the bytecode specification because blocks interact with the frame layout (ADR 010), the GC (stack scanning and heap allocation), non-local return (§7), and eventually `thisContext` reification (Phase 3).

### 6.1 Two kinds of blocks

Smalltalk/A distinguishes two kinds of blocks at compile time:

| Kind | Captures variables? | Runtime representation | Phase |
|---|---|---|---|
| **Clean block** | No. References only arguments, literals, and `self`. | Lightweight object: method reference + start PC + arg count. No heap allocation of captured state. | Phase 1 |
| **Capturing closure** | Yes. References one or more temporaries from an enclosing scope. | Heap-allocated closure object: copied values + indirect cells + method reference + start PC + arg count + outer context pointer. | Phase 2 |

The compiler determines which kind a block is by analyzing its free variable references. If the block references only its own arguments, literals in the enclosing method's literal frame, and `self`, it is a clean block. If it references any temporary variable from an enclosing scope (method or outer block), it is a capturing closure.

This distinction is invisible to the programmer. Both kinds respond to `value`, `value:`, `value:value:`, etc. with identical behavior. The distinction exists solely to avoid heap allocation in the common case where no variables are captured.

### 6.2 Clean block representation

A clean block is an instance of class `BlockClosure` (or a subclass — the interpreter cares about the field layout, not the class name). It contains:

| Field | Type | Meaning |
|---|---|---|
| `homeMethod` | OOP → CompiledMethod | The compiled method containing this block's bytecode. |
| `startPC` | SmallInteger | Byte offset of the block body's first instruction within `homeMethod`'s bytecode array. |
| `numArgs` | SmallInteger | Number of arguments the block expects. |
| `receiver` | OOP | The receiver (`self`) of the method in which the block was created. Blocks close over `self`. |

Total: 4 OOP-sized fields + object header. This is a small, fixed-size object. For Phase 1, clean blocks may be allocated on the actor's heap. A future optimization could allocate them on the stack or in a special region, but this is not required.

The `receiver` field is necessary because a block literal like `[self foo]` must send `#foo` to the receiver of the enclosing method, even if the block is passed to another method or another object entirely. The block closes over `self` at creation time.

**Creation:** when the interpreter executes `OP_BLOCK_COPY`, it reads the block descriptor from the literal frame (§4.5), allocates a `BlockClosure` object, fills in `homeMethod` (the currently executing method), `startPC`, `numArgs`, and `receiver` (the current frame's receiver), and pushes the block onto the stack. The interpreter then advances `pc` past the block body.

**Invocation:** when the block receives `value`, `value:`, etc., the interpreter creates a new activation frame. The frame's `method` field points to `homeMethod`, the `pc` starts at `startPC`, arguments are placed in `temp[0]` through `temp[numArgs - 1]`, and the receiver is the block's `receiver` field. Execution proceeds normally through the block body until `OP_RETURN_TOP` pops the block's frame and returns the result to the caller.

### 6.3 Capturing closure representation

A capturing closure is an instance of class `BlockClosure` with additional fields for captured state. It contains everything a clean block has, plus:

| Field | Type | Meaning |
|---|---|---|
| `homeMethod` | OOP → CompiledMethod | Same as clean block. |
| `startPC` | SmallInteger | Same as clean block. |
| `numArgs` | SmallInteger | Same as clean block. |
| `receiver` | OOP | Same as clean block. |
| `outerContext` | OOP or nil | Pointer to the enclosing activation's context object (for accessing outer temps via indirect cells). Nil if all captures are by-copy. |
| `copiedValues` | OOP → Array | Array of values copied from the enclosing scope at closure creation time. Used for by-copy captures. |

**Creation:** when the interpreter executes `OP_CLOSURE_COPY`, it reads the closure descriptor from the literal frame (§4.6), pops `numCopied` values from the stack (these are the values to copy), allocates a `BlockClosure` object, fills in all fields, and pushes the closure onto the stack.

### 6.4 Captured variable semantics

This is where the complexity lives. A block that captures a variable from an enclosing scope must decide: does it get a snapshot of the value at block creation time (copy), or does it share the variable with the enclosing scope so that mutations in either direction are visible (shared)?

**The rule:** Smalltalk semantics require shared access. If a block captures a variable and the enclosing scope mutates it after the block is created, the block must see the new value. If the block mutates the variable, the enclosing scope must see the change.

```smalltalk
| x |
x := 1.
block := [x := x + 1].
block value.    "x is now 2"
x              "→ 2, not 1"
```

This means by-copy capture is only correct for variables that are read but never written after the block is created. The compiler performs a simple analysis:

- If a captured variable is never written (by either the block or the enclosing scope after the block's creation point), it is captured **by copy**. The value is snapshotted into `copiedValues` at block creation. This is the common case and avoids heap allocation of an indirect cell.
- If a captured variable is written by the block, or written by the enclosing scope after the point where the block is created, it is captured **by shared indirect cell**.

### 6.5 Indirect cells for shared captures

An indirect cell is a tiny heap object (instance of class `IndirectVariable` or similar) with a single `value` field:

| Field | Type | Meaning |
|---|---|---|
| `value` | OOP | The current value of the shared variable. |

When the compiler determines that a variable must be shared:

1. At method entry (or at the point the variable enters scope), the compiler allocates an indirect cell and stores the variable's initial value in it. The cell itself is stored in the temp slot — the temp now holds the cell, not the value directly.
2. Every read of the variable (in the enclosing scope or in any block that captures it) goes through the cell: read `temp[N]` to get the cell, then read the cell's `value` field.
3. Every write of the variable goes through the cell: read `temp[N]` to get the cell, then write the cell's `value` field.
4. The block's `copiedValues` or `outerContext` chain provides access to the same cell object. Because both the enclosing scope and the block reference the same cell, mutations are visible in both directions.

**Bytecode for reading a shared variable inside a block:**

```
OP_PUSH_OUTER_TEMP  encoded    ( -- cell )
OP_SEND  lit[N]                ( -- value )     "lit[N] = #value"
```

Or, if the compiler inlines the cell access (an optimization for Phase 2+), a dedicated opcode could read the cell directly. For Phase 2, the `OP_SEND #value` approach is correct and avoids new opcodes.

**Bytecode for writing a shared variable inside a block:**

```
<push new value>               ( -- newVal )
OP_PUSH_OUTER_TEMP  encoded    ( -- newVal cell )
OP_SEND  lit[N]                ( -- result )     "lit[N] = #value:"
OP_POP                         ( -- )            "discard #value: result"
```

### 6.6 Outer temp encoding

`OP_PUSH_OUTER_TEMP`, `OP_STORE_OUTER_TEMP`, and `OP_POP_STORE_OUTER_TEMP` use an operand that encodes both the scope depth and the temp index:

```
operand = (scopeDepth << 4) | tempIndex
```

- `scopeDepth` (high 4 bits): number of context levels to traverse. 0 = immediate enclosing scope, 1 = two levels out, etc. Maximum depth: 15.
- `tempIndex` (low 4 bits): index of the temp within that scope. Maximum index: 15.

With `OP_WIDE`, the encoding extends to:

```
16-bit operand = (scopeDepth << 8) | tempIndex
```

- `scopeDepth` (high 8 bits): maximum depth 255.
- `tempIndex` (low 8 bits): maximum index 255.

The standard 8-bit encoding handles blocks nested up to 15 levels deep with up to 15 temps per scope — sufficient for virtually all real code. The wide form exists for pathological cases.

### 6.7 Block invocation and frame layout

When a block is invoked (`value`, `value:`, etc.), the interpreter creates a new activation frame on the actor's stack slab:

| Frame field | Value | Notes |
|---|---|---|
| `method` | block's `homeMethod` | The compiled method containing the block's bytecode. |
| `pc` | block's `startPC` | Execution begins at the first instruction of the block body. |
| `receiver` | block's `receiver` | `self` inside the block is the enclosing method's receiver. |
| `sender` | the invoking frame | Normal frame linkage — the frame that called `value` / `value:`. |
| `homeFrame` | pointer to the method frame that created this block | Used for non-local return (§7). For clean blocks, this is the frame in which `OP_BLOCK_COPY` executed. For closures, this is the home method's frame. |
| temps | arguments in `temp[0..numArgs-1]`, remaining temps initialized to nil | Block arguments are passed by the caller. |

The `homeFrame` field is critical for non-local return. It is a direct pointer into the stack slab — if the home frame has already been popped (the method returned), this pointer is stale, and non-local return must detect this condition (§7).

### 6.8 Block return semantics

**`OP_RETURN_TOP` inside a block:** returns from the block activation to the block's caller (the frame that sent `value` / `value:`). This is a local return — it pops the block's frame and pushes the result onto the caller's stack. It does **not** return from the enclosing method.

**`OP_NON_LOCAL_RETURN` inside a block:** returns from the enclosing method. See §7 for full mechanics.

**Implicit block return:** if the block body ends without an explicit return, the last expression's value is the block's return value. The compiler ensures every block body ends with `OP_RETURN_TOP`. If the block body is a sequence of statements, the last statement's value is left on the stack and `OP_RETURN_TOP` sends it back. If the block is empty (`[]`), the compiler emits `OP_PUSH_NIL` + `OP_RETURN_TOP`.

### 6.9 Nested blocks

Blocks can be nested arbitrarily. Each level of nesting increases the scope depth by one for outer temp access.

```smalltalk
| x |
x := 0.
outer := [ | y |
    y := 1.
    inner := [ x + y ].
    inner value
].
outer value
```

The inner block `[ x + y ]` captures `x` from the method scope (depth 2 from inner) and `y` from the outer block scope (depth 1 from inner). The compiler must track scope depth correctly and emit the right encoded operands for `OP_PUSH_OUTER_TEMP`.

If both `x` and `y` are read-only after their assignment, both are captured by copy. If either is written after the inner block's creation point, that variable uses an indirect cell.

### 6.10 `thisContext` — Phase 3

`thisContext` is a pseudo-variable that, when referenced, reifies the current activation frame as a first-class heap object. This is used for exception handling, debugging, and reflective access to the call stack.

**Phase 1 and Phase 2:** `OP_PUSH_CONTEXT` (opcode `0x0A`) is reserved but signals `NotYetImplemented` if executed. The frame layout (ADR 010) is designed to be compatible with eventual reification, but no reification machinery exists.

**Phase 3 semantics:**

When the interpreter executes `OP_PUSH_CONTEXT`:

1. The current frame (a C struct in the stack slab) is copied to a heap-allocated `MethodContext` object.
2. The heap context object has the same fields as the stack frame plus a `sender` field that points to the caller's context (lazily reified — the caller is not promoted until someone reads the `sender` field).
3. The original stack frame is marked as **forwarded** — it now contains a pointer to the heap context. All future frame accesses (including non-local return target checks) go through the heap object.
4. The heap context is pushed onto the stack.

This is expensive and should be rare. The compiler emits `OP_PUSH_CONTEXT` only when the source code explicitly references `thisContext`. Normal exception handling and debugging use the C-level frame walking (ADR 010) without reification.

**Interaction with blocks:** a block activation's `thisContext` reifies the block's frame, not the enclosing method's frame. The block's `sender` field points to the invoking frame's context. The block's `homeFrame` field (used for non-local return) is a separate concept from the context chain.

**Interaction with GC:** once a frame is promoted to the heap, it is a normal heap object subject to GC. The stack slab's forwarding pointer keeps the stack walker and the heap object in sync. The GC must trace all reified context objects as roots (they may reference temps that contain the only pointer to live objects).

### 6.11 Phase implementation summary

| Feature | Phase | Notes |
|---|---|---|
| Clean blocks (`OP_BLOCK_COPY`) | Phase 1 | No captured variables. Block invocation via `value` / `value:` / etc. |
| `OP_RETURN_TOP` in blocks | Phase 1 | Local return from block to block caller. |
| Implicit `^ self` at method end | Phase 1 | Compiler emits `OP_RETURN_SELF`. |
| Capturing closures (`OP_CLOSURE_COPY`) | Phase 2 | By-copy and shared indirect cell captures. |
| Outer temp access (`OP_PUSH_OUTER_TEMP`, `OP_STORE_OUTER_TEMP`, `OP_POP_STORE_OUTER_TEMP`) | Phase 2 | Scope depth + temp index encoding. |
| Non-local return (`OP_NON_LOCAL_RETURN`) | Phase 2 | Requires `homeFrame` tracking. See §7. |
| `thisContext` reification (`OP_PUSH_CONTEXT`) | Phase 3 | Frame promotion to heap. Lazy sender chain. |

**Phase 1 blocks are a deliberately restricted subset.** In Phase 1, the compiler must reject (as a compile-time error) any block that would require variable capture or non-local return. Only clean blocks — those referencing only their own arguments, literals, and `self` — are legal in Phase 1. Control structure inlining (`ifTrue:`, `whileTrue:`, `to:do:`, etc.) does not create block objects and is unaffected. This restriction is lifted in Phase 2 when `OP_CLOSURE_COPY` and `OP_NON_LOCAL_RETURN` become available.

---

## §7 Non-Local Return and Unwinding

Non-local return is the most mechanically complex feature in the Smalltalk runtime. It interacts with the frame layout, the block model, the actor stack slab, exception handling, and the actor isolation boundary. This section defines the complete semantics — what happens, in what order, and what the error cases are.

### 7.1 What non-local return is

In Smalltalk, the `^` operator inside a block returns not from the block, but from the method in which the block was syntactically defined — the **home method**. This is non-local return: control transfers across one or more intermediate activation frames to reach the home method's sender.

```smalltalk
Collection >> detect: aBlock ifNone: exceptionBlock
    self do: [:each |
        (aBlock value: each) ifTrue: [^ each]   "← returns from #detect:ifNone:"
    ].
    ^ exceptionBlock value
```

When `^ each` executes inside the inner block, it does not return from the `[:each | ...]` block, and it does not return from `#do:`. It returns from `#detect:ifNone:` — the method in which the block literal `[^ each]` was written. All intervening frames (`#do:`, the `[:each | ...]` block activation, and the `ifTrue:` inlined branch) are unwound.

### 7.2 The home frame

Every block activation frame has a `homeFrame` field (§6.7) that points to the activation frame of the method in which the block was textually defined. For a block defined directly in a method, `homeFrame` points to that method's frame. For a nested block, `homeFrame` still points to the outermost enclosing method's frame — non-local return always targets the method level, never an intermediate block.

The `homeFrame` is set at block creation time:
- For `OP_BLOCK_COPY`: the `homeFrame` is the currently executing frame (which must be a method activation, since clean blocks are Phase 1 and only exist in method scope).
- For `OP_CLOSURE_COPY`: the `homeFrame` is copied from the enclosing block's `homeFrame` (for nested blocks) or is the current frame (if the closure is defined directly in a method). The chain always terminates at a method activation frame.

### 7.3 Non-local return mechanics

When the interpreter executes `OP_NON_LOCAL_RETURN`:

1. **Read the return value** from the top of the stack.

2. **Read the home frame pointer** from the current block activation frame's `homeFrame` field.

3. **Validate the home frame.** The home frame must still be live — it must exist on the current actor's stack slab and must not have already returned. Validation is described in §7.4.

4. **Unwind intervening frames.** Starting from the current frame, walk the sender chain toward the home frame. For each intervening frame:
   - If the frame has a registered unwind-protect handler (`ensure:` / `ifCurtailed:`), execute the handler (§7.6).
   - Pop the frame from the stack.

5. **Pop the home frame itself.** The home method is now returning.

6. **Push the return value** onto the home frame's sender's stack — the frame that originally called the home method.

7. **Resume execution** in the home frame's sender.

Steps 4–7 are identical to what happens when the home method executes `OP_RETURN_TOP` — the only difference is that non-local return reaches the home method by unwinding intervening frames rather than by being the current frame.

### 7.4 Dead frame detection

The home frame may no longer be live. This happens when the block outlives the method that created it — the method has already returned, its frame has been popped from the stack slab, and the `homeFrame` pointer is stale.

```smalltalk
makeBlock
    ^ [^ 42]     "returns the block; method frame is popped"

test
    | block |
    block := self makeBlock.
    block value   "← non-local return targets a dead frame"
```

When `block value` triggers `OP_NON_LOCAL_RETURN`, the home frame (the `#makeBlock` activation) no longer exists on the stack.

**Detection mechanism:** each activation frame carries a **frame marker** — a small integer value written into the frame at creation and cleared (set to zero) when the frame is popped. The non-local return checks whether the home frame's marker is still valid:

```
if homeFrame->marker == 0:
    signal BlockCannotReturn
else:
    proceed with unwind
```

The marker approach is cheap (one word per frame, one comparison on non-local return) and reliable. It does not depend on memory protection, stack pointer comparison, or any platform-specific mechanism. The cost is one additional word in `STA_Frame` — this must be accounted for in the frame layout but does not affect the per-actor density target (the frame is on the stack slab, not in the actor struct).

**Alternative considered:** comparing the home frame pointer against the current stack slab bounds (if `homeFrame` is below the stack base pointer, it is dead). This is fragile — stack slab growth or relocation could invalidate the comparison. The marker approach is more robust.

### 7.5 `BlockCannotReturn` error

When a non-local return targets a dead frame:

1. The interpreter does **not** unwind any frames.
2. The interpreter signals `BlockCannotReturn` as an exception in the current actor. This is a normal Smalltalk exception that can be caught by an exception handler in the block's caller chain.
3. If `BlockCannotReturn` is not handled, the actor's default exception handler terminates the actor and notifies its supervisor (normal actor failure semantics).

The `BlockCannotReturn` exception object carries the attempted return value and a reference to the block that attempted the return. This gives exception handlers enough information to log or recover.

### 7.6 Unwind protection: `ensure:` and `ifCurtailed:`

Smalltalk provides two forms of unwind protection:

- `aBlock ensure: ensureBlock` — `ensureBlock` is executed when `aBlock` completes, whether normally or via non-local return or exception.
- `aBlock ifCurtailed: curtailedBlock` — `curtailedBlock` is executed only if `aBlock` completes abnormally (non-local return or exception), not on normal completion.

These are not bytecode-level features — they are implemented as Smalltalk methods on `BlockClosure`. However, their implementation depends on a runtime mechanism for registering unwind handlers on activation frames so that non-local return can find and execute them during unwinding.

**Runtime mechanism:**

Each activation frame has an optional **unwind handler** slot. When `ensure:` or `ifCurtailed:` is activated, the implementation registers the handler block on the current frame:

```smalltalk
BlockClosure >> ensure: aBlock
    | result |
    self markForUnwind: aBlock.   "← primitive: register aBlock as unwind handler on this frame"
    result := self value.
    self unmarkForUnwind.         "← primitive: remove unwind handler"
    aBlock value.                 "execute ensure block on normal completion"
    ^ result
```

The `markForUnwind:` primitive writes the handler block reference into the current frame's unwind handler slot. The `unmarkForUnwind` primitive clears it.

During non-local return unwinding (§7.3 step 4), for each frame being unwound:
1. Check whether the frame has a non-nil unwind handler.
2. If yes, invoke the handler block (a normal `value` send).
3. The handler executes in its own frame on the stack. If the handler itself performs a non-local return, that non-local return takes precedence — the original non-local return is abandoned.
4. After the handler completes (or if there was no handler), pop the frame and continue unwinding.

**`ifCurtailed:` difference:** `ifCurtailed:` only registers the handler and does not execute it on normal completion. The runtime mechanism is identical — the difference is in the Smalltalk-level implementation of `ifCurtailed:`, which does not call `aBlock value` after `self value` returns normally.

### 7.7 Unwind handler slot in the frame

The unwind handler is stored as an OOP in the activation frame. This means `STA_Frame` needs an `unwindHandler` field:

| Field | Type | Meaning |
|---|---|---|
| `unwindHandler` | OOP | The block to execute on abnormal unwinding, or `nil` if no handler is registered. |

This field is initialized to `nil` on frame creation. It is set by the `markForUnwind:` primitive and cleared by `unmarkForUnwind`. The GC must trace this field when scanning frames for roots.

**Frame layout impact:** one additional OOP-sized field (8 bytes on 64-bit). This increases `STA_Frame` from 40 bytes (ADR 010) to 48 bytes. The increase affects stack slab consumption (deeper call chains use more slab space) but does not affect per-actor creation cost — the stack slab is pre-allocated at a fixed size and grows on demand.

**Phase 1 stance:** the `unwindHandler` field exists in the frame layout from Phase 1. It is always initialized to `nil`. The `markForUnwind:` and `unmarkForUnwind` primitives exist as stubs that set and clear the field. `ensure:` is implementable in Phase 1 as a Smalltalk method using these primitives, but only for normal completion — `ensureBlock` is called at the Smalltalk level after `self value` returns. Abnormal unwinding (non-local return or exception triggering unwind handler execution) is a Phase 2 concern. Phase 1's `ensure:` is a deliberately incomplete subset that works correctly for the normal-completion path only.

### 7.8 Exception handling

Smalltalk exception handling (`on:do:`, `signal`, `resume:`, `retry`, `pass`) is built on the same frame-walking and unwind mechanism as non-local return. However, exception handling is implemented almost entirely in Smalltalk — it does not require dedicated bytecodes.

The runtime provides:

1. **`thisContext` access** (Phase 3) — exception handler lookup walks the context chain to find handlers registered via `on:do:`.
2. **Frame marker checking** — the same mechanism used for dead-frame detection (§7.4) is used to validate handler frames.
3. **Unwind handler execution** — `ensure:` handlers fire during exception-triggered unwinding exactly as they do during non-local return unwinding.

**Phase 1 implementation path:** basic exception handling can work without `thisContext` by maintaining a handler stack as an explicit data structure (an actor-local variable holding a linked list of handler entries). This is less elegant than the Blue Book's context-chain approach but is fully functional and avoids the Phase 3 dependency. The handler stack is pushed when `on:do:` is entered and popped when it exits. `signal` walks the handler stack, not the context chain.

The handler stack approach is an implementation detail, not a semantic difference — the Smalltalk-level API (`on:do:`, `signal`, `resume:`, `retry`, `pass`) is identical regardless of whether the implementation walks a handler stack or the context chain. Phase 3 may replace the handler stack with context-chain walking if `thisContext` reification warrants it.

### 7.9 Cross-actor non-local return — prohibited

A non-local return must never cross an actor boundary. An actor's stack slab is private — no other actor can reference frames in it, and no block should be able to unwind into another actor's execution.

This prohibition is enforced structurally:

1. **Blocks cannot be sent cross-actor.** Cross-actor messages use deep copy (ADR 008). A block closure contains a `homeFrame` pointer into the sending actor's stack slab. Deep copy cannot meaningfully copy a frame pointer — it would be a dangling pointer in the receiving actor. Therefore, block closures are **not copyable** across actor boundaries. Attempting to include a block in a cross-actor message is an error, detected during deep copy and signaled as `CannotCopyBlock` (or similar).

2. **Actor message handlers return normally.** An actor processes a message by executing a method. The method may use blocks with non-local returns internally — all of that is local to the actor's stack. The method's return value is sent back to the requesting actor (for `ask:`) or discarded (for `send:`). There is no mechanism for a non-local return to escape the message handler.

3. **No shared stack slabs.** Each actor has its own stack slab (ADR 010). There is no physical path from one actor's frame to another's.

This is not a limitation that needs to be worked around. It is a direct consequence of actor isolation and is the correct semantic behavior. Blocks are local computational tools; actors are the concurrency boundary. The two do not mix.

### 7.10 Interaction with tail-call optimization

TCO (§2.4) reuses the current frame for a tail-position send. This interacts with non-local return:

**Scenario:** method `foo` creates a block containing `^`, then tail-calls `bar`. The block holds a `homeFrame` pointing to `foo`'s frame. TCO reuses `foo`'s frame for `bar`. If the block now attempts a non-local return targeting `foo`'s frame, it lands in `bar`'s frame instead — wrong target.

**Primary prevention — compiler rule (§5.11):** The compiler does not emit TCO for any method that contains an NLR-bearing block (a block with `^`). This eliminates the hazard for all normally compiled code. Methods without NLR-bearing blocks use TCO freely and safely.

**Secondary safety net — runtime marker check:** If TCO does occur in the presence of NLR-bearing blocks (e.g., hand-assembled bytecode), the runtime handles it correctly via the frame marker: TCO invalidates the old frame's marker before reusing it. When a frame is reused for TCO:

1. The old frame's marker is cleared (set to zero).
2. A new marker is written for the new activation.

If the block subsequently attempts `OP_NON_LOCAL_RETURN` and checks the `homeFrame`'s marker, it finds zero — dead frame — and signals `BlockCannotReturn`. This is correct behavior: the method that created the block has effectively returned (via tail call), so non-local return to it is invalid.

**ADR 010 policy TC-A compatibility:** ADR 010 specifies that TCO is disabled per-actor when a debug capability is attached. When TCO is disabled, frames are never reused, and this interaction does not arise.

### 7.11 Phase implementation summary

| Feature | Phase | Notes |
|---|---|---|
| Frame marker (live/dead detection) | Phase 1 | Written on frame creation, cleared on frame pop. Always present even though non-local return is Phase 2 — the field costs one word and the write/clear cost is negligible. |
| Unwind handler slot in frame | Phase 1 | Always initialized to nil. `markForUnwind:` / `unmarkForUnwind` primitives exist as stubs. |
| `ensure:` normal completion | Phase 1 | Works via Smalltalk-level implementation; handler block called after `self value` returns. This is a deliberately incomplete subset — abnormal unwinding is Phase 2. |
| Exception handling via handler stack | Phase 1 | `on:do:`, `signal`, `resume:`, basic unwind. No `thisContext` dependency. |
| `OP_NON_LOCAL_RETURN` | Phase 2 | Full unwind with home frame validation, `BlockCannotReturn`, and unwind handler execution. |
| `ensure:` / `ifCurtailed:` abnormal unwinding | Phase 2 | Unwind handlers fire during non-local return and exception propagation. |
| Cross-actor block prohibition | Phase 2 | Deep copy rejects block closures. |
| TCO + non-local return interaction | Phase 2 | Compiler rule prevents TCO in NLR-bearing methods; runtime marker check as safety net. |
| `thisContext`-based exception handling | Phase 3 | Optional replacement for handler stack approach. Context chain walking. |

---

## §8 Primitive Table

Primitives are operations implemented in C that cannot be expressed in Smalltalk bytecode — either because they require direct hardware access (arithmetic, memory operations), interact with the runtime itself (object allocation, GC, class mutation), or bridge to external resources (I/O, OS services).

### 8.1 Calling convention

A primitive is associated with a compiled method via the `hasPrimitive` flag and `primitiveIndex` field in the method header (§4.2), and the `OP_PRIMITIVE` instruction in the method's bytecode preamble (§4.9).

**Activation sequence:** see §4.9 for the complete two-stage dispatch protocol.

**Why no frame on primitive success:** this is a critical performance property. Primitives are the hot path for arithmetic, comparison, and array access. Creating and tearing down a frame for `3 + 4` would dominate the cost of the addition itself. The frame is only created when the primitive fails and the fallback bytecode needs to execute.

**Failure codes:** failure codes are small non-negative integers. 0 means success. Non-zero values identify the failure reason. The fallback bytecode can inspect the failure code to decide how to respond. Standard failure codes:

| Code | Meaning |
|---|---|
| 0 | Success (never stored — primitive returned normally) |
| 1 | Receiver is wrong type |
| 2 | Argument is wrong type |
| 3 | Argument out of range |
| 4 | Insufficient memory (allocation failed) |
| 5 | Primitive not available (stub) |

Additional failure codes may be defined by specific primitives. The failure code occupies `temp[numArgs]` in the fallback frame — the compiler reserves this slot in every primitive method.

### 8.2 Primitive index ranges

| Range | Allocation | Phase |
|---|---|---|
| 0 | No primitive | — |
| 1–127 | Blue Book kernel primitives | Phase 1 |
| 128–255 | Blue Book extended primitives (I/O, system, platform) | Phase 1–2 |
| 256–511 | Smalltalk/A actor and concurrency primitives | Phase 2 |
| 512–767 | Smalltalk/A capability primitives | Phase 4 |
| 768–1023 | Smalltalk/A I/O and OS primitives (libuv-backed) | Phase 2 |
| 1024–65535 | Reserved for future use | — |

Primitives 1–255 are addressed directly by the `primitiveIndex` field in the method header. Primitives 256+ use the `OP_WIDE` + `OP_PRIMITIVE` encoding in the bytecode preamble (§4.9), with `hasPrimitive` set and `primitiveIndex` set to 0 in the header.

### 8.3 Blue Book arithmetic primitives (1–22)

These operate on SmallInteger and Float receivers. On type mismatch (receiver or argument is not the expected numeric type), the primitive fails with code 1 or 2, and the fallback bytecode handles coercion or error signaling.

| Prim | Selector | Receiver | Args | Result | Failure | Phase |
|---|---|---|---|---|---|---|
| 1 | `#+` | SmallInteger | 1 SmallInteger | SmallInteger | Overflow or type mismatch | Phase 1 |
| 2 | `#-` | SmallInteger | 1 SmallInteger | SmallInteger | Overflow or type mismatch | Phase 1 |
| 3 | `#<` | SmallInteger | 1 SmallInteger | Boolean | Type mismatch | Phase 1 |
| 4 | `#>` | SmallInteger | 1 SmallInteger | Boolean | Type mismatch | Phase 1 |
| 5 | `#<=` | SmallInteger | 1 SmallInteger | Boolean | Type mismatch | Phase 1 |
| 6 | `#>=` | SmallInteger | 1 SmallInteger | Boolean | Type mismatch | Phase 1 |
| 7 | `#=` | SmallInteger | 1 SmallInteger | Boolean | Type mismatch | Phase 1 |
| 8 | `#~=` | SmallInteger | 1 SmallInteger | Boolean | Type mismatch | Phase 1 |
| 9 | `#*` | SmallInteger | 1 SmallInteger | SmallInteger | Overflow or type mismatch | Phase 1 |
| 10 | `#/` | SmallInteger | 1 SmallInteger | SmallInteger or Fraction | Zero divide or type mismatch | Phase 1 |
| 11 | `#\\` | SmallInteger | 1 SmallInteger | SmallInteger | Zero divide or type mismatch | Phase 1 |
| 12 | `#//` | SmallInteger | 1 SmallInteger | SmallInteger | Zero divide or type mismatch | Phase 1 |
| 13 | `#quo:` | SmallInteger | 1 SmallInteger | SmallInteger | Zero divide or type mismatch | Phase 1 |
| 14 | `#bitAnd:` | SmallInteger | 1 SmallInteger | SmallInteger | Type mismatch | Phase 1 |
| 15 | `#bitOr:` | SmallInteger | 1 SmallInteger | SmallInteger | Type mismatch | Phase 1 |
| 16 | `#bitXor:` | SmallInteger | 1 SmallInteger | SmallInteger | Type mismatch | Phase 1 |
| 17 | `#bitShift:` | SmallInteger | 1 SmallInteger | SmallInteger | Overflow or type mismatch | Phase 1 |

Float arithmetic primitives:

| Prim | Selector | Receiver | Args | Result | Failure | Phase |
|---|---|---|---|---|---|---|
| 18 | `#+` | Float | 1 Float | Float | Type mismatch | Phase 1 |
| 19 | `#-` | Float | 1 Float | Float | Type mismatch | Phase 1 |
| 20 | `#<` | Float | 1 Float | Boolean | Type mismatch | Phase 1 |
| 21 | `#>` | Float | 1 Float | Boolean | Type mismatch | Phase 1 |
| 22 | `#*` | Float | 1 Float | Float | Type mismatch | Phase 1 |

SmallInteger overflow: when a SmallInteger arithmetic operation overflows the 63-bit signed range (ADR 007), the primitive fails. The fallback bytecode is responsible for promoting to LargePositiveInteger or LargeNegativeInteger. LargeInteger support is a Phase 1 class library concern, not a primitive concern — the primitive simply reports overflow and the Smalltalk code handles it.

### 8.4 Comparison and identity primitives (23–30)

| Prim | Selector | Receiver | Args | Result | Failure | Phase |
|---|---|---|---|---|---|---|
| 23 | `#=` | Float | 1 Float | Boolean | Type mismatch | Phase 1 |
| 24 | `#~=` | Float | 1 Float | Boolean | Type mismatch | Phase 1 |
| 25 | `#<=` | Float | 1 Float | Boolean | Type mismatch | Phase 1 |
| 26 | `#>=` | Float | 1 Float | Boolean | Type mismatch | Phase 1 |
| 27 | `#/` | Float | 1 Float | Float | Zero divide or type mismatch | Phase 1 |
| 28 | `#truncated` | Float | 0 | SmallInteger | Overflow (result too large for SmallInt) | Phase 1 |
| 29 | `#==` | Object | 1 Object | Boolean | Never fails | Phase 1 |
| 30 | `#class` | Object | 0 | Class | Never fails | Phase 1 |

Primitive 29 (`#==`) is identity comparison — it compares OOP values directly. It never fails because any two objects can be compared for identity. Primitive 30 (`#class`) returns the receiver's class by reading the `class_index` from the object header (ADR 007) and looking up the class table.

### 8.5 Object and memory primitives (31–50)

| Prim | Selector | Receiver | Args | Result | Failure | Phase |
|---|---|---|---|---|---|---|
| 31 | `#basicNew` | Class | 0 | new instance | Allocation failure | Phase 1 |
| 32 | `#basicNew:` | Class | 1 SmallInteger | new indexable instance | Size negative or allocation failure | Phase 1 |
| 33 | `#basicAt:` | Object | 1 SmallInteger | element | Index out of bounds or not indexable | Phase 1 |
| 34 | `#basicAt:put:` | Object | 2 (index, value) | value | Index out of bounds or not indexable | Phase 1 |
| 35 | `#basicSize` | Object | 0 | SmallInteger | Never fails | Phase 1 |
| 36 | `#hash` | Object | 0 | SmallInteger | Never fails | Phase 1 |
| 37 | `#become:` | Object | 1 Object | receiver | Objects in different actors or immutable | Phase 1 |
| 38 | `#instVarAt:` | Object | 1 SmallInteger | value | Index out of bounds | Phase 1 |
| 39 | `#instVarAt:put:` | Object | 2 (index, value) | value | Index out of bounds or immutable | Phase 1 |
| 40 | `#identityHash` | Object | 0 | SmallInteger | Never fails | Phase 1 |
| 41 | `#shallowCopy` | Object | 0 | new object | Allocation failure | Phase 1 |
| 42 | `#yourself` | Object | 0 | receiver | Never fails | Phase 1 |

**`become:` (prim 37) scope restriction:** in Smalltalk/A, `become:` swaps the identity of two objects *within the current actor's heap only*. It does not and cannot affect objects in other actors' heaps or in the shared immutable region. Attempting `become:` on an immutable object or on an object not owned by the current actor fails with code 1. This is a stronger restriction than classical Smalltalk, where `become:` was global and notoriously expensive. The per-actor restriction makes `become:` a local operation with bounded cost.

### 8.6 Array and stream primitives (51–67)

| Prim | Selector | Receiver | Args | Result | Failure | Phase |
|---|---|---|---|---|---|---|
| 51 | `#at:` | Array/String | 1 SmallInteger | element | Index out of bounds | Phase 1 |
| 52 | `#at:put:` | Array/String | 2 (index, value) | value | Index out of bounds or immutable | Phase 1 |
| 53 | `#size` | Array/String | 0 | SmallInteger | Never fails | Phase 1 |
| 54 | `#replaceFrom:to:with:startingAt:` | Array/String | 4 | receiver | Bounds error | Phase 1 |
| 55 | `#species` | Collection | 0 | Class | Never fails | Phase 1 |
| 60 | `#basicAt:` | ByteArray | 1 SmallInteger | SmallInteger (0–255) | Index out of bounds | Phase 1 |
| 61 | `#basicAt:put:` | ByteArray | 2 (index, byte) | byte value | Index out of bounds or value out of 0–255 | Phase 1 |
| 62 | `#basicSize` | ByteArray | 0 | SmallInteger | Never fails | Phase 1 |
| 63 | `#stringAt:` | String | 1 SmallInteger | Character | Index out of bounds | Phase 1 |
| 64 | `#stringAt:put:` | String | 2 (index, Character) | Character | Index out of bounds | Phase 1 |

### 8.7 Class and method primitives (68–80)

| Prim | Selector | Receiver | Args | Result | Failure | Phase |
|---|---|---|---|---|---|---|
| 68 | `#compiledMethodAt:` | Class | 1 Symbol | CompiledMethod | Selector not found | Phase 1 |
| 69 | `#addSelector:withMethod:` | Class | 2 (selector, method) | receiver | Not a valid method | Phase 1 |
| 70 | `#subclass:instanceVariableNames:classVariableNames:poolDictionaries:category:` | Class | 5 | new Class | Name conflict or invalid | Phase 1 |
| 71 | `#superclass` | Class | 0 | Class or nil | Never fails | Phase 1 |
| 72 | `#methodDictionary` | Class | 0 | MethodDictionary | Never fails | Phase 1 |
| 73 | `#instanceCount` | Class | 0 | SmallInteger | Never fails | Phase 1 |
| 74 | `#name` | Class | 0 | String | Never fails | Phase 1 |
| 75 | `#allInstances` | Class | 0 | Array | Allocation failure | Phase 1 |

Primitive 69 (`#addSelector:withMethod:`) is the live update path. It atomically updates the class's method dictionary and invalidates inline caches (§10, ADR 013). This primitive is the only way to install a method — there is no back-channel. The IDE, the compiler, the file-sync actor, and any future agent all use this primitive through the same Smalltalk message send.

### 8.8 Block and closure primitives (81–90)

| Prim | Selector | Receiver | Args | Result | Failure | Phase |
|---|---|---|---|---|---|---|
| 81 | `#value` | BlockClosure | 0 | block result | Wrong number of args | Phase 1 |
| 82 | `#value:` | BlockClosure | 1 | block result | Wrong number of args | Phase 1 |
| 83 | `#value:value:` | BlockClosure | 2 | block result | Wrong number of args | Phase 1 |
| 84 | `#value:value:value:` | BlockClosure | 3 | block result | Wrong number of args | Phase 1 |
| 85 | `#valueWithArguments:` | BlockClosure | 1 Array | block result | Wrong number of args | Phase 1 |
| 86 | `#markForUnwind:` | BlockClosure | 1 BlockClosure | receiver | Not in valid context | Phase 1 |
| 87 | `#unmarkForUnwind` | BlockClosure | 0 | receiver | Not in valid context | Phase 1 |

Primitives 81–85 invoke a block. The primitive checks that the argument count matches the block's `numArgs`. On match, it creates a block activation frame (§6.7) and begins executing the block body. On mismatch, it fails with code 2.

Primitives 86–87 manage unwind handlers (§7.6–7.7). In Phase 1 they set and clear the frame's `unwindHandler` slot. Full unwind execution during non-local return is Phase 2.

### 8.9 Symbol and character primitives (91–100)

| Prim | Selector | Receiver | Args | Result | Failure | Phase |
|---|---|---|---|---|---|---|
| 91 | `#asSymbol` | String | 0 | Symbol | Allocation failure | Phase 1 |
| 92 | `#asString` | Symbol | 0 | String | Allocation failure | Phase 1 |
| 93 | `#intern:` | Symbol class | 1 String | Symbol | Allocation failure | Phase 1 |
| 94 | `#value` | Character | 0 | SmallInteger | Never fails | Phase 1 |
| 95 | `#characterFor:` | Character class | 1 SmallInteger | Character | Value out of Unicode range | Phase 1 |

Primitive 93 (`#intern:`) is the symbol table interning operation. It acquires the symbol table lock (lock-free read path, locked write path per the concurrency architecture) and returns the canonical Symbol for the given string. If the symbol already exists, no allocation occurs.

### 8.10 System and image primitives (100–127)

| Prim | Selector | Receiver | Args | Result | Failure | Phase |
|---|---|---|---|---|---|---|
| 100 | `#perform:` | Object | 1 Symbol | result | doesNotUnderstand | Phase 1 |
| 101 | `#perform:with:` | Object | 2 (Symbol, arg) | result | doesNotUnderstand | Phase 1 |
| 102 | `#perform:with:with:` | Object | 3 | result | doesNotUnderstand | Phase 1 |
| 103 | `#perform:withArguments:` | Object | 2 (Symbol, Array) | result | doesNotUnderstand | Phase 1 |
| 110 | `#snapshotTo:` | SystemDictionary | 1 String (path) | Boolean | I/O error | Phase 1 |
| 111 | `#quit` | SystemDictionary | 0 | — | Never fails (terminates) | Phase 1 |
| 112 | `#garbageCollect` | SystemDictionary | 0 | SmallInteger (bytes freed) | Never fails | Phase 1 |
| 113 | `#printToTranscript:` | SystemDictionary | 1 String | nil | Never fails | Phase 1 |
| 114 | `#errorMessage:` | SystemDictionary | 1 String | nil | Never fails | Phase 1 |
| 120 | `#respondsTo:` | Object | 1 Symbol | Boolean | Never fails | Phase 1 |
| 121 | `#doesNotUnderstand:` | Object | 1 Message | — | — (see note) | Phase 1 |
| 122 | `#cannotReturn:` | BlockClosure | 1 Object | — | — | Phase 2 |

Primitive 121 is special: `doesNotUnderstand:` is not normally called as a primitive. It is the fallback the interpreter invokes when message lookup fails (§10). The primitive number exists so that `doesNotUnderstand:` can be implemented as a primitive method if desired (e.g., for proxy objects that intercept all messages).

Primitive 122 (`#cannotReturn:`) is invoked by the interpreter when `OP_NON_LOCAL_RETURN` detects a dead home frame (§7.5). The argument is the value that was being returned. The default implementation signals `BlockCannotReturn`.

### 8.11 Smalltalk/A actor primitives (256–299)

These require `OP_WIDE` + `OP_PRIMITIVE` encoding. All are Phase 2 unless noted.

| Prim | Selector | Receiver | Args | Result | Failure | Phase |
|---|---|---|---|---|---|---|
| 256 | `#spawn:` | ActorClass | 1+ (init args) | actor address | Allocation failure | Phase 2 |
| 257 | `#send:to:` | Object | 2 (selector, actor) | nil | Mailbox full | Phase 2 |
| 258 | `#ask:of:` | Object | 2 (selector, actor) | Future | Mailbox full | Phase 2 |
| 259 | `#selfAddress` | Actor | 0 | actor address | Not in actor context | Phase 2 |
| 260 | `#supervisorAddress` | Actor | 0 | actor address | Not in actor context | Phase 2 |
| 261 | `#link:` | Actor | 1 actor address | nil | Invalid address | Phase 2 |
| 262 | `#unlink:` | Actor | 1 actor address | nil | Invalid address | Phase 2 |
| 263 | `#isAlive:` | Actor | 1 actor address | Boolean | Invalid address | Phase 2 |
| 264 | `#mailboxSize` | Actor | 0 | SmallInteger | Not in actor context | Phase 2 |
| 265 | `#yield` | Actor | 0 | nil | Never fails | Phase 2 |

Note on actor opcodes vs. actor primitives: the actor extension opcodes (§9 — `OP_SEND_ASYNC`, `OP_ASK`, `OP_ACTOR_SPAWN`) are the primary interface for actor operations. The primitives listed here are the lower-level entry points that those opcodes call internally, and they are also available for direct use from Smalltalk code via `perform:`-style dynamic dispatch or from system-level code that needs finer control than the opcodes provide.

### 8.12 Smalltalk/A capability primitives (512–549)

| Prim | Selector | Receiver | Args | Result | Failure | Phase |
|---|---|---|---|---|---|---|
| 512 | `#grantCapability:to:` | CapabilityRegistry | 2 (cap, actor) | token | Not authorized | Phase 4 |
| 513 | `#revokeCapability:` | CapabilityRegistry | 1 token | nil | Invalid token | Phase 4 |
| 514 | `#checkCapability:` | CapabilityRegistry | 1 token | Boolean | Invalid token | Phase 4 |
| 515 | `#attenuate:to:` | CapabilityRegistry | 2 (token, perms) | new token | Not authorized | Phase 4 |
| 516 | `#registerService:as:` | CapabilityRegistry | 2 (actor, manifest) | nil | Already registered | Phase 4 |
| 517 | `#lookupService:` | CapabilityRegistry | 1 manifest | actor address or nil | Never fails | Phase 4 |

These are stubs through Phase 3. The primitive numbers are reserved so that class library code can be written against the capability API before the full implementation exists. The stubs fail with code 5 (primitive not available).

### 8.13 Smalltalk/A I/O primitives (768–830)

These wrap libuv operations (ADR 011). All are Phase 2. Each I/O primitive is non-blocking — it registers the operation with the I/O thread and suspends the calling actor. The actor resumes when the I/O completes, with the result available as the primitive's return value.

| Prim | Selector | Receiver | Args | Result | Failure | Phase |
|---|---|---|---|---|---|---|
| 768 | `#tcpConnect:port:` | Socket class | 2 (host String, port SmallInt) | Socket | Connection refused, DNS failure, timeout | Phase 2 |
| 769 | `#tcpListen:port:backlog:` | Socket class | 3 | ListenerSocket | Bind failure | Phase 2 |
| 770 | `#accept` | ListenerSocket | 0 | Socket | Not listening | Phase 2 |
| 771 | `#read:` | Socket | 1 SmallInteger (max bytes) | ByteArray | Connection closed, error | Phase 2 |
| 772 | `#write:` | Socket | 1 ByteArray | SmallInteger (bytes written) | Connection closed, error | Phase 2 |
| 773 | `#close` | Socket | 0 | nil | Already closed | Phase 2 |
| 780 | `#fileOpen:mode:` | File class | 2 (path String, mode Symbol) | FileHandle | Permission denied, not found | Phase 2 |
| 781 | `#fileRead:count:` | FileHandle | 1 SmallInteger | ByteArray | I/O error | Phase 2 |
| 782 | `#fileWrite:data:` | FileHandle | 1 ByteArray | SmallInteger | I/O error | Phase 2 |
| 783 | `#fileClose` | FileHandle | 0 | nil | Already closed | Phase 2 |
| 784 | `#fileStat:` | File class | 1 String (path) | StatResult | Not found | Phase 2 |
| 790 | `#timerAfter:` | Timer class | 1 SmallInteger (ms) | nil | Invalid duration | Phase 2 |
| 791 | `#dnsResolve:` | DNS class | 1 String (hostname) | Array of Strings | Resolution failure | Phase 2 |
| 800 | `#subprocessRun:args:` | Subprocess class | 2 (cmd, Array) | SubprocessHandle | Exec failure | Phase 2 |
| 801 | `#subprocessWait` | SubprocessHandle | 0 | SmallInteger (exit code) | Not started | Phase 2 |

**I/O suspension semantics:** when an I/O primitive is called, it does not block the scheduler thread. Instead:

1. The primitive registers the operation with the I/O thread (ADR 011).
2. The primitive sets the actor's `io_state` to `STA_IO_PENDING` and `sched_flags` to `STA_SCHED_SUSPENDED`.
3. The interpreter yields the actor. The scheduler thread picks up another runnable actor.
4. When the I/O completes, the I/O thread sets the result in the actor's `io_result`, clears `STA_SCHED_SUSPENDED`, and pushes the actor onto a scheduler deque via `sta_io_sched_push` (ADR 011).
5. The actor resumes. The interpreter reads `io_result` and returns it as the primitive's result.

From the Smalltalk programmer's perspective, the I/O call looks synchronous — `socket read: 1024` returns a ByteArray. The actor was suspended and resumed transparently. No callbacks, no futures, no explicit async API. This is the BEAM model applied to I/O.

### 8.14 Primitive registration

The interpreter maintains a **primitive table** — a C array mapping primitive indices to C function pointers:

```c
typedef STA_PrimResult (*STA_PrimFunc)(STA_VM* vm, OOP receiver, OOP* args, int numArgs);

STA_PrimFunc primitive_table[65536];  // sparse; most entries NULL
```

A NULL entry means the primitive is not implemented. Invoking a NULL primitive fails with code 5 (primitive not available).

Primitives are registered at VM startup during bootstrap. The registration is a simple assignment:

```c
primitive_table[1]  = prim_smallint_add;
primitive_table[2]  = prim_smallint_sub;
// ... etc.
```

This table is read-only after bootstrap — primitives are not dynamically installed or removed. The table is shared across all scheduler threads (read-only shared data, no synchronization needed).

### 8.15 Phase implementation summary

| Range | Content | Phase |
|---|---|---|
| 1–100 | Blue Book kernel primitives (arithmetic, comparison, object, array, block, symbol, system) | Phase 1 |
| 100–127 | System primitives (perform, snapshot, GC, transcript) | Phase 1 |
| 128–255 | Extended Blue Book range (reserved, filled as needed) | Phase 1–2 |
| 256–299 | Actor primitives (spawn, send, ask, link, yield) | Phase 2 |
| 512–549 | Capability primitives (grant, revoke, check, attenuate, lookup) | Phase 4 |
| 768–830 | I/O primitives (TCP, file, DNS, timer, subprocess) | Phase 2 |

All primitive numbers are reserved from Phase 1 onward. Unimplemented primitives return failure code 5. The compiler may emit `OP_PRIMITIVE` or `OP_WIDE` + `OP_PRIMITIVE` for any primitive number in any phase — the interpreter handles unimplemented primitives gracefully via the failure path.

---

## §9 Actor Extension Opcodes

This section defines the complete semantics of the four actor extension opcodes introduced in §3.9. These opcodes are the bytecode-level interface between Smalltalk code and the actor runtime. They are the only instructions that cross actor boundaries — every other opcode operates within a single actor's heap, stack slab, and execution context.

### 9.1 Relationship to actor primitives

The actor opcodes (`OP_SEND_ASYNC`, `OP_ASK`, `OP_ACTOR_SPAWN`) and the actor primitives (§8.11, prims 256–265) serve different roles:

| | Opcodes (§9) | Primitives (§8.11) |
|---|---|---|
| **Emitted by** | Compiler, for actor-specific syntax | Compiler, for general method bodies |
| **Trigger deep copy** | Yes — the opcode itself initiates copy | Yes — the primitive implementation initiates copy |
| **Argument count** | Determined by selector arity (same as `OP_SEND`) | Fixed per primitive signature |
| **Primary use** | Normal actor message sending and spawning in application code | System-level code, dynamic dispatch (`perform:`-style), and fine-grained control |

In practice, the compiler emits the opcodes for the common cases. The primitives exist for the same reasons all Smalltalk primitives exist — to provide a Smalltalk-callable entry point that reflective and system-level code can use without special syntax.

### 9.2 `OP_SEND_ASYNC` — asynchronous fire-and-forget send

**Opcode:** `0x70`
**Operand:** literal frame index (selector symbol)
**Stack effect:** `( actorAddr arg1 ... argN -- )`
**Phase:** Phase 2 (signals `NotYetImplemented` in Phase 1)

**Semantics:**

1. **Read the selector** from `lit[operand]`. Determine argument count N from the selector's arity.

2. **Pop N arguments and the actor address** from the stack. The actor address is below the arguments (pushed first, per normal Smalltalk evaluation order). Total values popped: N + 1.

3. **Validate the actor address.** The address must be a valid opaque actor reference. If invalid, signal `InvalidActorAddress` as an exception in the sending actor. Do not enqueue anything.

4. **Deep-copy the arguments** per ADR 008. Each argument is copied into a transfer buffer:
   - Immutable objects (`STA_OBJ_IMMUTABLE` flag set) are shared by pointer — no copy.
   - Mutable objects are deep-copied recursively.
   - Block closures cannot be copied — if any argument is or contains a `BlockClosure`, signal `CannotCopyBlock` in the sending actor (§7.9).
   - Cycles and shared structure in the argument graph are handled by the deep-copy visited set (open decision #9 from PROJECT_STATUS.md).

5. **Construct a message envelope.** The envelope contains: the selector, the copied arguments, and the sender's address (so the receiver can reply if it chooses, though fire-and-forget sends do not expect a reply).

6. **Enqueue the envelope** in the target actor's mailbox (lock-free MPSC queue, ADR 008).
   - If the mailbox is full (bounded, default limit 256 per ADR 008), the enqueue fails. The overflow policy is drop-newest: the message is discarded and `MailboxFull` is signaled as an exception in the sending actor.
   - If the target actor is suspended (idle, waiting for I/O, etc.), it is marked runnable and pushed onto a scheduler deque.

7. **Push nothing.** The sending actor's stack has N + 1 fewer values than before. No result is pushed. Execution continues with the next instruction. The sending actor does not wait.

**Reduction counting:** `OP_SEND_ASYNC` increments the reduction counter by 1 (same as any send). The deep-copy cost is not counted as reductions — it is bounded by the argument size, which is typically small. If profiling shows that large argument copies cause scheduling unfairness, a copy-proportional reduction charge can be added, but this is not the initial design. Enqueue failure paths also consume the reduction.

### 9.3 `OP_ASK` — asynchronous request-response send

**Opcode:** `0x71`
**Operand:** literal frame index (selector symbol)
**Stack effect:** `( actorAddr arg1 ... argN -- future )`
**Phase:** Phase 2 (signals `NotYetImplemented` in Phase 1)

**Semantics:**

Steps 1–5 are identical to `OP_SEND_ASYNC` (§9.2), with one addition:

6. **Allocate a future object** on the sending actor's heap. The future is an instance of class `Future` with three possible terminal states:

   | State | Meaning | How it arrives |
   |---|---|---|
   | **Pending** | No result yet. | Initial state at creation. |
   | **Resolved** | The target actor returned a value. | Target method returns normally; result is deep-copied back to the sender and written into the future. |
   | **Failed** | The target actor crashed or signaled an error. | Target actor's message handler raised an unhandled exception; the supervisor notifies the sender via the future. |
   | **Timed out** | No response within a deadline. | Optional timeout set by the sender; the runtime transitions the future to timed-out after the deadline. |

7. **Attach the future's identity to the message envelope.** The envelope includes a reply token that the target actor's runtime uses to route the response back to the correct future in the sending actor.

8. **Enqueue the envelope** in the target actor's mailbox. Same overflow behavior as `OP_SEND_ASYNC`. **Mailbox-full on `OP_ASK`:** the initial stance is to signal `MailboxFull` as an exception in the sender, consistent with `OP_SEND_ASYNC`. The future is not created if the enqueue fails. This behavior is subject to Phase 2 design review (open decision #10 from PROJECT_STATUS.md) — alternative resolutions include transitioning the future to a failed state rather than signaling an exception.

9. **Push the future** onto the sending actor's stack. Execution continues immediately — the sending actor does not block waiting for the reply.

**Future resolution mechanics:**

When the target actor finishes processing the message (its method returns via `OP_RETURN_TOP` or equivalent):

1. The target actor's runtime reads the reply token from the message envelope.
2. The return value is deep-copied into a transfer buffer (same copy rules as outbound messages).
3. The transfer buffer and reply token are delivered to the sending actor — either via its mailbox or via a dedicated reply channel (implementation choice, not observable from Smalltalk).
4. The sending actor's runtime finds the future by reply token and transitions it to **resolved** with the copied value.

**Using a future:**

Futures respond to messages for accessing their result:

```smalltalk
future := someActor ask: #computeResult.

"Option 1: callback style"
future onValue: [:result | self handleResult: result].
future onFailure: [:error | self handleError: error].

"Option 2: blocking wait (suspends the actor, not the scheduler thread)"
result := future wait.

"Option 3: blocking wait with timeout"
result := future waitFor: 5000.  "milliseconds"
```

`future wait` is implemented as a primitive that suspends the calling actor (sets `STA_SCHED_SUSPENDED`) until the future resolves, fails, or times out. The scheduler thread is freed to run other actors. This is identical to the I/O suspension model (§8.13) — from the scheduler's perspective, waiting on a future is the same as waiting on a socket read.

`future onValue:` and `future onFailure:` register callback blocks that are invoked when the future transitions. The callbacks execute in the sending actor's context (as a new message processed by the sender), not in the target actor's context.

**Timeout semantics:**

An `ask:` future may have an optional timeout. If set:
1. The runtime registers a timer (via the I/O thread's timer facility, ADR 011).
2. If the timer fires before the future resolves, the future transitions to **timed out**.
3. A timed-out future delivers a `FutureTimeout` exception to any `wait` call or `onFailure:` callback.
4. The message in the target actor's mailbox is not retracted — it may still be processed, but the result is discarded because the future has already transitioned.

Default timeout: none. If the programmer does not set a timeout, the future remains pending indefinitely until the target responds or crashes. The programmer is responsible for setting timeouts on any `ask:` where unbounded waiting is unacceptable. This is a deliberate design choice — implicit timeouts would create surprising behavior in correct code.

### 9.4 `OP_ACTOR_SPAWN` — create a new actor

**Opcode:** `0x72`
**Operand:** literal frame index (spawn descriptor — see §4.7)
**Stack effect:** `( arg1 ... argN -- actorAddr )`
**Phase:** Phase 2 (signals `NotYetImplemented` in Phase 1)

**Semantics:**

1. **Read the spawn descriptor** from `lit[operand]`. The descriptor contains the actor class OOP and the initialization argument count N (§4.7).

2. **Validate the actor class.** The class must be a subclass of `Actor` (§10.1 in the architecture doc). If it is not, signal `InvalidActorClass` in the spawning actor.

3. **Pop N arguments** from the stack.

4. **Deep-copy the arguments** into a transfer buffer. Same copy rules as `OP_SEND_ASYNC` — the new actor gets its own copies of all mutable arguments.

5. **Allocate the new actor.** The runtime:
   - Allocates an `STA_Actor` struct (~152 bytes, ADR 012).
   - Allocates an initial nursery heap (128 bytes).
   - Assigns the actor an opaque address.
   - Sets the actor's class to the specified class.
   - Links the actor to the spawning actor's supervisor (or to the spawning actor itself, if the spawning actor is a supervisor — supervision linkage policy is a Phase 2 design decision).

6. **Enqueue an initialization message.** The copied arguments and the selector `#initialize` (or `#initialize:`, `#initialize:with:`, etc., matching the argument count) are enqueued as the first message in the new actor's mailbox. The new actor will process this message when it is first scheduled.

7. **Register the actor** with the scheduler. The new actor is added to a scheduler deque as runnable.

8. **Push the new actor's opaque address** onto the spawning actor's stack.

The spawning actor continues immediately. The new actor's `initialize` message is processed asynchronously — the spawner does not wait for initialization to complete.

### 9.5 `OP_SELF_ADDRESS` — get current actor's address

**Opcode:** `0x73`
**Operand:** unused (0x00)
**Stack effect:** `( -- actorAddr )`
**Phase:** Phase 2 (signals `NotYetImplemented` in Phase 1)

**Semantics:**

1. **Read the current actor's opaque address** from the runtime context (the scheduler knows which actor is currently executing on this thread).

2. **Push the address** onto the stack.

The address is an opaque, unforgeable reference. It cannot be decomposed, cannot be forged by arithmetic, and cannot be used to access the actor's internal state. It can only be used as the target of `OP_SEND_ASYNC`, `OP_ASK`, or passed as an argument so other actors can message this actor.

**Typical use:**

```smalltalk
"Inside an actor method — tell another actor how to reach me"
registry send: #register:as: with: self actorAddress with: #myService.
```

The actor calls `self actorAddress`, which compiles to `OP_SELF_ADDRESS`, to obtain its own address for passing to the registry.

### 9.6 Deep-copy rules at the opcode level

All three message-passing opcodes (`OP_SEND_ASYNC`, `OP_ASK`, `OP_ACTOR_SPAWN`) invoke the same deep-copy logic. The rules are defined in ADR 008 and summarized here for completeness:

| Object type | Copy behavior |
|---|---|
| SmallInteger (tagged immediate) | No copy needed — immediates are values, not references. |
| Object with `STA_OBJ_IMMUTABLE` flag | Shared by pointer. No copy. The flag is set at allocation time for symbols, compiled methods, frozen literals, and explicitly frozen user objects. |
| Mutable heap object | Deep-copied recursively. Each reachable mutable object is copied into the transfer buffer. |
| `BlockClosure` | **Not copyable.** Signals `CannotCopyBlock` in the sending actor. Blocks contain frame pointers into the sending actor's stack slab; these are meaningless in the receiving actor. |
| Actor address (opaque token) | Copied as an opaque value. The token itself is immutable — it is a capability reference, not a mutable object. |
| Object containing a cycle | Handled by the deep-copy visited set. Each object is copied once; subsequent references to the same source object are redirected to the single copy. |

**Transfer buffer ownership:** the deep-copy produces a set of objects in a transfer buffer. When the message is enqueued, ownership of the transfer buffer passes to the target actor. The target actor's runtime integrates the transferred objects into its private heap. The sending actor retains no references to the transferred objects.

### 9.7 Error conditions summary

| Opcode | Error condition | Result |
|---|---|---|
| `OP_SEND_ASYNC` | Invalid actor address | `InvalidActorAddress` exception in sender |
| `OP_SEND_ASYNC` | Mailbox full | `MailboxFull` exception in sender |
| `OP_SEND_ASYNC` | Argument contains BlockClosure | `CannotCopyBlock` exception in sender |
| `OP_ASK` | Invalid actor address | `InvalidActorAddress` exception in sender |
| `OP_ASK` | Mailbox full | `MailboxFull` exception in sender (initial stance — see §9.3 for open decision) |
| `OP_ASK` | Argument contains BlockClosure | `CannotCopyBlock` exception in sender |
| `OP_ASK` | Target crashes during processing | Future transitions to **failed** |
| `OP_ASK` | Timeout expires | Future transitions to **timed out** |
| `OP_ACTOR_SPAWN` | Class is not a subclass of `Actor` | `InvalidActorClass` exception in spawner |
| `OP_ACTOR_SPAWN` | Argument contains BlockClosure | `CannotCopyBlock` exception in spawner |
| `OP_ACTOR_SPAWN` | Allocation failure | `AllocationFailure` exception in spawner |
| `OP_SELF_ADDRESS` | Executed outside actor context | `NotInActorContext` exception |

All exceptions are signaled in the sending/spawning actor using the normal exception mechanism (§7.8). They are catchable via `on:do:`. No exception propagates to the target actor — the sender is responsible for handling send failures.

### 9.8 Phase 1 behavior

In Phase 1, all four actor opcodes are recognized by the interpreter (they have assigned byte values and appear in the opcode dispatch table). If any of them is executed:

1. The interpreter signals `NotYetImplemented` as an exception in the current execution context.
2. The exception is catchable via `on:do:` — it is not a fatal interpreter error.
3. The compiler may emit these opcodes freely. Test code can verify that the opcodes parse, that the stack effects are correct (values are on the stack before the opcode), and that the `NotYetImplemented` exception is properly raised and catchable.

This allows Phase 1 to include compiler tests that emit actor opcodes and verify the bytecode structure without requiring the actor runtime to exist.

### 9.9 Descriptor types in the literal frame — summary

| Descriptor type | Class | Fields | Used by |
|---|---|---|---|
| Block descriptor | `BlockDescriptor` | startPC, bodyLength, numArgs | `OP_BLOCK_COPY` |
| Closure descriptor | `ClosureDescriptor` | startPC, bodyLength, numArgs, numCopied | `OP_CLOSURE_COPY` |
| Spawn descriptor | `SpawnDescriptor` | actorClass, numArgs | `OP_ACTOR_SPAWN` |

All three are small immutable objects stored in the literal frame. The compiler creates them at compile time. They are part of the compiled method and live in the shared immutable region.

---

## §10 Message Dispatch and Caching

Message dispatch is the most performance-critical path in the runtime. Every `OP_SEND` and `OP_SEND_SUPER` instruction passes through this mechanism. The design must be correct first and fast second — but "fast" is a hard requirement, not a nice-to-have, because Smalltalk programs send millions of messages per second.

### 10.1 Dispatch algorithm

When the interpreter executes `OP_SEND` with selector S and receiver R:

1. **Determine the receiver's class.** Read R's class index from its object header (`STA_ObjHeader.class_index`, ADR 007). Look up the class object in the class table. Call this class C.

2. **Check the inline cache** at this send site. If the cached class matches C, jump directly to the cached method. See §10.3.

3. **On cache miss, perform a full lookup.** Starting at class C:
   a. Search C's method dictionary for selector S.
   b. If found, the lookup succeeds. The method is the result.
   c. If not found, set C to C's superclass and repeat from (a).
   d. If C is nil (top of hierarchy reached with no match), the lookup has failed. Go to step 4.

4. **Lookup failure — `doesNotUnderstand:`.** Create a `Message` object containing the selector S and the original arguments. Send `#doesNotUnderstand:` to the receiver R with the `Message` as the argument. This is a real message send — it goes through the same dispatch mechanism starting at step 1, with selector `#doesNotUnderstand:` and receiver R. If `#doesNotUnderstand:` itself is not found (catastrophic — `Object` should always implement it), the interpreter signals a fatal error.

5. **Update the inline cache** with the (class, method) pair from the successful lookup. See §10.3.

6. **Activate the method.** Create a new frame (or reuse the current frame if TCO applies), set up the receiver and arguments, and begin executing the method's bytecode (or try its primitive first, if `hasPrimitive` is set).

### 10.2 Super send dispatch

When the interpreter executes `OP_SEND_SUPER` with selector S:

Steps are identical to §10.1 except for step 1:

1. **Determine the lookup start class.** Instead of using the receiver's class, read the owner class from the last literal slot of the *currently executing method* (§4.4). The owner class is stored as a direct class OOP. Take the superclass of the owner class. Call this class C. Begin the method dictionary search at C.

The receiver on the stack is still `self` — the same object as for a normal send. Only the lookup starting point changes. This means `super foo` sends `#foo` to `self`, but lookup starts one level above where the currently executing method is defined. This is standard Blue Book semantics.

**Super sends and inline caching:** super send sites use a separate cache strategy because the lookup start class is determined by the sending method, not by the receiver's runtime class. A super send site is effectively monomorphic by construction — the start class never varies for a given send site. The cache is still useful (it avoids the dictionary search) but does not need polymorphic handling.

### 10.3 Inline cache structure

Each send site in the bytecode has an associated inline cache entry. The cache is stored outside the bytecode itself — either in a side table indexed by send-site PC, or in a per-method cache array. The choice is an implementation detail not observable from Smalltalk; what matters is the lookup contract.

**Monomorphic inline cache (Phase 1):**

Each send site caches a single (class, method) pair:

| Field | Type | Meaning |
|---|---|---|
| `cachedClass` | class index (uint32) | The class of the receiver the last time this send site was executed. |
| `cachedMethod` | OOP → CompiledMethod | The method that was found by lookup for that class. |

**Cache check (fast path):**

```
if receiver.class_index == cache.cachedClass:
    activate cache.cachedMethod    ← cache hit, no lookup
else:
    full lookup → update cache     ← cache miss
```

This is a single integer comparison on the fast path. For monomorphic send sites (the vast majority in practice — a given `x size` almost always sends to the same class), the cache hit rate is very high.

**Megamorphic / polymorphic sends (Phase 2+ optimization):**

Some send sites are polymorphic — the receiver's class varies across invocations (e.g., sending `#printOn:` to elements of a heterogeneous collection). A monomorphic cache thrashes on these sites. Phase 2 or later may introduce:

- **Polymorphic inline cache (PIC):** a small fixed-size array of (class, method) pairs (typically 4–8 entries). Checked linearly on each send. If the receiver's class matches any entry, it is a hit.
- **Megamorphic fallback:** if a PIC overflows, the send site is marked megamorphic and always performs a full lookup (possibly accelerated by a global method lookup cache — see §10.5).

Phase 1 uses monomorphic caches only. This is correct and sufficient — polymorphic optimization is a performance concern, not a correctness concern. The interpreter always falls back to full lookup on a cache miss.

### 10.4 Cache invalidation

The inline cache must be invalidated when the method dictionaries it depends on change. This happens during live update — when a method is installed, removed, or a class hierarchy is modified (ADR 004, ADR 013).

**Invalidation strategy: global flush.**

When `addSelector:withMethod:` (prim 69) installs a new method:

1. The method is atomically written into the class's method dictionary.
2. All inline caches across all compiled methods are invalidated — every cache entry is cleared.
3. Subsequent sends miss the cache and perform full lookups, which re-populate the caches with current method bindings.

Global flush is the simplest correct strategy. It is expensive (every send site pays one cache miss after an invalidation) but method installation is rare compared to method execution. In a development session, a programmer might install a method once every few seconds; the runtime executes millions of sends per second. The cost amortizes instantly.

**Why not selective invalidation:** invalidating only the caches that reference the changed selector or class is more precise but substantially more complex. It requires maintaining reverse mappings from selectors and classes to cache sites. For Phase 1, global flush is the right tradeoff. Selective invalidation can be explored in Phase 2 or later if profiling shows that global flush is a bottleneck during heavy interactive development.

**Implementation of global flush:**

Option A — **generation counter.** A single global `uint64_t cache_generation` counter is incremented on every method installation. Each cache entry stores the generation at which it was populated. The cache check becomes:

```
if receiver.class_index == cache.cachedClass
   && cache.generation == global_cache_generation:
    activate cache.cachedMethod    ← hit
else:
    full lookup → update cache with current generation
```

This is one additional integer comparison on the fast path but avoids walking all cache entries on invalidation. The invalidation cost is O(1) — increment the counter. The repopulation cost is distributed across subsequent sends.

Option B — **bulk zero.** On invalidation, memset all cache entries to zero. The cache check is the same as §10.3 (zero class index never matches a real class). Invalidation is O(N) where N is the total number of cache entries, but N is bounded by the number of send sites in loaded methods and the memset is cache-friendly.

**Recommended:** Option A (generation counter). It makes invalidation O(1) and the per-send cost is one extra comparison, which is negligible compared to the dictionary lookup it avoids on a hit.

### 10.5 Global method lookup cache

Independent of inline caches, the runtime may maintain a **global method lookup cache** — a hash table mapping (class, selector) pairs to methods. This cache is consulted on inline cache misses before performing a full dictionary walk. It is shared across all scheduler threads (read-mostly, updated under a lock or lock-free).

| Field | Type | Meaning |
|---|---|---|
| `classIndex` | uint32 | Class index of the receiver. |
| `selector` | OOP → Symbol | The selector being looked up. |
| `method` | OOP → CompiledMethod | The method found by lookup. |

**Cache size:** a fixed power-of-two hash table (e.g., 4096 entries). Hash collisions are resolved by replacement — a new entry overwrites the old one at the same hash bucket. No chaining.

**Lookup sequence with both caches:**

```
1. Check inline cache at send site     ← fastest (one comparison)
2. On miss: check global lookup cache  ← fast (hash + compare)
3. On miss: full dictionary walk       ← slow (linear search per class level)
4. Update global cache and inline cache with result
```

The global cache is invalidated on method installation by the same generation counter mechanism — the global cache entries store the generation at which they were populated.

**Phase 1 stance:** the global lookup cache is optional in Phase 1. Inline caches alone provide substantial speedup over bare dictionary walks. The global cache can be added when profiling shows that inline cache miss rates on polymorphic sends justify the additional complexity.

### 10.6 Method dictionary structure

Each class has a method dictionary — a hash table mapping selector symbols to compiled method objects. The dictionary is a normal Smalltalk object (instance of `MethodDictionary`) stored as an instance variable of the class.

**Lookup within a single dictionary:** hash the selector symbol (symbols have a precomputed hash), index into the dictionary's internal array, and compare selectors. On collision, linear probe or follow the dictionary's collision resolution strategy.

**Concurrency:** method dictionaries are shared coordinated mutable metadata (§8.5 of the architecture doc). They are read by all scheduler threads during dispatch and written by privileged operations (method installation). The concurrency strategy is:

- **Reads are lock-free.** The dictionary's internal array is accessed by pointer. On method installation, a new array is allocated with the new entry, and the dictionary's array pointer is atomically swapped. Readers that are mid-lookup on the old array continue to completion — they see a consistent (possibly stale) snapshot. Stale results are not a correctness problem because the inline cache will be invalidated and the next lookup will see the new array.
- **Writes are serialized.** Method installation acquires the `install_lock` (ADR 013). Only one method installation can be in progress at a time. This is not a GIL — the lock is held only during the dictionary update and cache invalidation, not during normal execution.

### 10.7 Special selectors

Certain selectors are so frequently used that the runtime benefits from knowing them at startup. The **special selector table** is a fixed-size array of well-known selectors maintained by the runtime:

| Index | Selector | Arity | Notes |
|---|---|---|---|
| 0 | `#+` | 1 | Arithmetic |
| 1 | `#-` | 1 | Arithmetic |
| 2 | `#<` | 1 | Comparison |
| 3 | `#>` | 1 | Comparison |
| 4 | `#<=` | 1 | Comparison |
| 5 | `#>=` | 1 | Comparison |
| 6 | `#=` | 1 | Equality |
| 7 | `#~=` | 1 | Inequality |
| 8 | `#*` | 1 | Arithmetic |
| 9 | `#/` | 1 | Arithmetic |
| 10 | `#\\` | 1 | Modulo |
| 11 | `#@` | 1 | Point creation |
| 12 | `#bitShift:` | 1 | Bitwise |
| 13 | `#//` | 1 | Integer divide |
| 14 | `#bitAnd:` | 1 | Bitwise |
| 15 | `#bitOr:` | 1 | Bitwise |
| 16 | `#at:` | 1 | Indexed access |
| 17 | `#at:put:` | 2 | Indexed store |
| 18 | `#size` | 0 | Collection size |
| 19 | `#next` | 0 | Stream |
| 20 | `#nextPut:` | 1 | Stream |
| 21 | `#atEnd` | 0 | Stream |
| 22 | `#==` | 1 | Identity |
| 23 | `#class` | 0 | Reflection |
| 24 | `#value` | 0 | Block evaluation |
| 25 | `#value:` | 1 | Block evaluation |
| 26 | `#do:` | 1 | Iteration |
| 27 | `#new` | 0 | Instantiation |
| 28 | `#new:` | 1 | Instantiation |
| 29 | `#yourself` | 0 | Identity |
| 30 | `#doesNotUnderstand:` | 1 | Error handling |
| 31 | `#mustBeBoolean` | 0 | Type error (§3.6) |

The special selector table is not an opcode mechanism — there is no `OP_SEND_SPECIAL`. The table serves two purposes:

1. **Bootstrap contract.** These selectors must exist in the symbol table before the first bytecode executes. The bootstrap (§11) creates them as part of initial wiring.

2. **Compiler knowledge.** The compiler knows the arity of special selectors without inspecting the symbol. This is a minor convenience — the compiler could compute arity from any selector by counting colons — but for the special selectors, the arity is hardcoded in the compiler's selector table, which eliminates any ambiguity for binary selectors whose arity is not obvious from the character sequence.

The table does not affect dispatch. Special selectors go through `OP_SEND` like every other selector. They hit inline caches, walk method dictionaries, and trigger `doesNotUnderstand:` exactly like any other message. The table is informational and contractual, not operational.

### 10.8 Reduction counting

Every `OP_SEND` and `OP_SEND_SUPER` increments the current actor's reduction counter by 1. This is the primary preemption mechanism for straight-line code (§3.4). The check is performed after the send completes (method returns):

```
reductions += 1
if reductions >= STA_REDUCTION_QUOTA:
    yield actor to scheduler
    reductions = 0
```

`STA_REDUCTION_QUOTA` is 1000 (ADR 009). An actor executes at most 1000 sends before yielding. Combined with `OP_JUMP_BACK` reduction checks (§3.6), this ensures that no actor can starve the scheduler regardless of its workload pattern.

**Where the counter lives:** the reduction counter is a field in the `STA_Actor` struct (or in a per-scheduler-thread context that maps to the currently executing actor). It is not in the frame — it persists across method calls within a single scheduling quantum.

**What counts as a reduction:** send instructions (`OP_SEND`, `OP_SEND_SUPER`) and backward jumps (`OP_JUMP_BACK`). Primitive executions do not independently count as reductions — they are reached via a send, and that send already counted. Actor opcodes (`OP_SEND_ASYNC`, `OP_ASK`, `OP_ACTOR_SPAWN`) each count as 1 reduction.

### 10.9 `doesNotUnderstand:` protocol

When method lookup fails (§10.1 step 4):

1. **Create a `Message` object.** The object contains:
   - `selector` — the symbol that was not understood.
   - `args` — an Array of the arguments that were on the stack.
   - `lookupClass` — the class where lookup began (the receiver's class for normal sends, the superclass of the owner class for super sends).

2. **Send `#doesNotUnderstand:` to the receiver** with the `Message` as the argument. This is a normal send — it goes through `OP_SEND` dispatch starting at the receiver's class.

3. **If `#doesNotUnderstand:` returns normally,** its return value becomes the result of the original send. This enables proxy patterns — an object can intercept unknown messages and handle them dynamically.

4. **If `#doesNotUnderstand:` is itself not found,** the interpreter signals a fatal error. This should never happen in a correctly bootstrapped system because `Object >> doesNotUnderstand:` is a kernel method installed during bootstrap.

**Default implementation of `Object >> doesNotUnderstand:`:**

```smalltalk
Object >> doesNotUnderstand: aMessage
    MessageNotUnderstood new
        message: aMessage;
        receiver: self;
        signal
```

This signals a `MessageNotUnderstood` exception, which is catchable via `on:do:`. If not caught, it propagates to the actor's top-level handler and terminates the actor (normal actor failure, supervisor notified).

### 10.10 `mustBeBoolean` protocol

When `OP_JUMP_TRUE` or `OP_JUMP_FALSE` encounters a value that is neither `true` nor `false` (§3.6):

1. **Send `#mustBeBoolean` to the value.** This is a normal send.
2. **If `#mustBeBoolean` returns `true` or `false`,** the jump is evaluated using that result.
3. **If `#mustBeBoolean` returns a non-boolean or is not understood,** the interpreter signals `NonBooleanReceiver` as an exception.

The default implementation of `#mustBeBoolean` (on `Object`) signals an error. A class could override it to provide coercion behavior, but this is unusual in practice.

### 10.11 Phase implementation summary

| Feature | Phase | Notes |
|---|---|---|
| Full dictionary-walk lookup | Phase 1 | Correct, complete, required from day one. |
| Super send lookup | Phase 1 | Owner class from last literal slot (direct class OOP). |
| `doesNotUnderstand:` fallback | Phase 1 | Message object creation + recursive send. |
| `mustBeBoolean` fallback | Phase 1 | Send on non-boolean in conditional jump. |
| Monomorphic inline cache | Phase 1 | Single (class, method) pair per send site. |
| Generation counter invalidation | Phase 1 | O(1) invalidation on method install. |
| Reduction counting on sends | Phase 1 | Increment + check after every send. |
| Special selector table | Phase 1 | Bootstrap contract; no dispatch impact. |
| Method dictionary lock-free reads | Phase 1 | Atomic array pointer swap on install. |
| Global method lookup cache | Phase 2 | Optional; added when profiling justifies it. |
| Polymorphic inline cache | Phase 2+ | Optional; for polymorphic send sites. |
| Selective cache invalidation | Phase 2+ | Optional; replaces global flush if needed. |

---

## §11 Special Objects and Bootstrap Requirements

This section defines the objects, classes, and structures that must exist before the interpreter can execute its first bytecode. The bootstrap (§11 of the architecture doc) is responsible for creating these by hand in C — bypassing the object system to build the object system. This section is what CC reads when implementing the bootstrap.

### 11.1 The special object table

The interpreter and runtime reference a fixed set of well-known objects by index into a **special object table** — a C array of OOPs maintained by the runtime. These objects are not looked up by name at runtime; they are accessed by constant index. The table is populated during bootstrap and never resized.

The special object table must be fully populated before the first bytecode executes.

| Index | Name | Object | Notes |
|---|---|---|---|
| 0 | `SPC_NIL` | `nil` | The unique instance of `UndefinedObject`. |
| 1 | `SPC_TRUE` | `true` | The unique instance of `True`. |
| 2 | `SPC_FALSE` | `false` | The unique instance of `False`. |
| 3 | `SPC_SMALLTALK` | `Smalltalk` | The global `SystemDictionary`. All global variables are associations in this dictionary. |
| 4 | `SPC_SPECIAL_SELECTORS` | special selector array | An Array of the selectors listed in §10.7. |
| 5 | `SPC_CHARACTER_TABLE` | character table | An Array of 256 Character objects for ASCII values 0–255. Characters in this range are looked up by index rather than allocated. |
| 6 | `SPC_DOES_NOT_UNDERSTAND` | `#doesNotUnderstand:` | The selector symbol, pre-interned. The interpreter uses this directly when dispatching DNU (§10.9). |
| 7 | `SPC_CANNOT_RETURN` | `#cannotReturn:` | The selector symbol for `BlockCannotReturn` signaling (§7.5). |
| 8 | `SPC_MUST_BE_BOOLEAN` | `#mustBeBoolean` | The selector symbol for non-boolean conditional handling (§10.10). |
| 9 | `SPC_STARTUP` | `#startUp` | The selector sent to the system on image load to resume execution. |
| 10 | `SPC_SHUTDOWN` | `#shutDown` | The selector sent to the system before image save or quit. |
| 11 | `SPC_RUN` | `#run` | The selector sent to the initial actor to begin execution in a freshly bootstrapped image. |
| 12–31 | — | reserved | Reserved for future well-known objects. Initialized to `nil`. |

The table is a C array:

```c
OOP special_objects[32];
```

Accessed by the interpreter as `special_objects[SPC_NIL]`, `special_objects[SPC_TRUE]`, etc. The constants (`SPC_NIL`, `SPC_TRUE`, ...) are `#define`s in the interpreter's private headers.

**Note on nil/true/false representation:** the opcodes `OP_PUSH_NIL`, `OP_PUSH_TRUE`, and `OP_PUSH_FALSE` push the values from this table. The opcode semantics are agnostic to whether these values are heap objects in a shared immutable region (current stance per ADR 007) or tagged immediates. The representation choice (open decision #1 from PROJECT_STATUS) must be resolved before the first dispatch loop is built, but does not affect the bytecode specification. Special-object access, bootstrap table layout, image format, and potential JIT fast paths all depend on the chosen representation.

### 11.2 Kernel classes

The following classes must be created during bootstrap, in dependency order. Each class requires: a class object, a metaclass object, a method dictionary (initially empty or with kernel methods), and correct superclass linkage. The metaclass circularity (Class/Metaclass mutual dependence) must be wired by hand in C before any class creation can use the normal class-building mechanism.

**Tier 0 — metaclass circularity (wired by hand):**

These classes are mutually dependent. They cannot be created using the object system because they *are* the object system. The bootstrap allocates their raw memory, writes their headers, and wires their class/metaclass/superclass pointers manually.

| Class | Superclass | Metaclass | Notes |
|---|---|---|---|
| `Object` | nil | `Object class` | Root of the class hierarchy. |
| `Behavior` | `Object` | `Behavior class` | Defines method dictionary and superclass. |
| `ClassDescription` | `Behavior` | `ClassDescription class` | Adds instance variable names, category. |
| `Class` | `ClassDescription` | `Class class` | Concrete class of all non-metaclasses. |
| `Metaclass` | `ClassDescription` | `Metaclass class` | Concrete class of all metaclasses. |

The circularity: `Metaclass` is an instance of `Metaclass class`, which is an instance of `Metaclass`. `Class` is an instance of `Class class`, which is also an instance of `Metaclass`. The bootstrap resolves this by creating the objects first and wiring the pointers afterward — there is no order of creation that avoids forward references, so all five classes (and their five metaclasses) are allocated in one pass and linked in a second pass.

**Tier 1 — kernel classes (built using Tier 0 machinery):**

Once the metaclass web is wired, new classes can be created using `Class >> subclass:instanceVariableNames:...` — either as a primitive or as a simplified bootstrap version. These classes are needed before the first bytecode can execute meaningfully:

| Class | Superclass | Why it must exist at bootstrap |
|---|---|---|
| `UndefinedObject` | `Object` | Class of `nil`. The interpreter checks `nil`'s class for method lookup. |
| `True` | `Object` | Class of `true`. Required for `OP_JUMP_TRUE` / `OP_JUMP_FALSE`. |
| `False` | `Object` | Class of `false`. Required for `OP_JUMP_TRUE` / `OP_JUMP_FALSE`. |
| `SmallInteger` | `Number` | Class of tagged immediates. Every arithmetic primitive checks this class. |
| `Number` | `Magnitude` | Superclass of `SmallInteger`. Required for inheritance chain. |
| `Magnitude` | `Object` | Superclass of `Number`. |
| `Float` | `Number` | Float arithmetic primitives. |
| `Character` | `Object` | Character table entries. |
| `Symbol` | `String` | Selector symbols in method dictionaries and the special selector table. |
| `String` | `ArrayedCollection` | Superclass of `Symbol`. |
| `Array` | `ArrayedCollection` | Used everywhere — literal frames, arguments arrays, method dictionaries internally. |
| `ByteArray` | `ArrayedCollection` | Bytecode storage in compiled methods. |
| `ArrayedCollection` | `SequenceableCollection` | Superclass of `Array`, `String`, `ByteArray`. |
| `SequenceableCollection` | `Collection` | Superclass chain. |
| `Collection` | `Object` | Superclass chain. |
| `Association` | `Object` | Global variable bindings. `OP_PUSH_GLOBAL` reads Association objects. |
| `MethodDictionary` | `Object` | Every class has one. Required for message dispatch. |
| `CompiledMethod` | `Object` | Bytecode + literal frame container. The interpreter reads these directly. |
| `BlockClosure` | `Object` | Runtime representation of blocks (§6). |
| `BlockDescriptor` | `Object` | Literal frame entries for `OP_BLOCK_COPY` (§4.5). |
| `Message` | `Object` | Created by `doesNotUnderstand:` dispatch (§10.9). |
| `MessageNotUnderstood` | `Error` | Default exception from `doesNotUnderstand:`. |
| `BlockCannotReturn` | `Error` | Exception from dead-frame non-local return (§7.5). |
| `Error` | `Exception` | Base error class. |
| `Exception` | `Object` | Base exception class. |
| `SystemDictionary` | `Object` | Class of the `Smalltalk` globals dictionary. |

**Tier 2 — classes needed soon after bootstrap but not for first bytecode:**

These can be loaded from the kernel source file (step 4 of the bootstrap, §11 of the architecture doc) rather than hand-wired:

| Class | Superclass | Why it is needed |
|---|---|---|
| `LargePositiveInteger` | `Integer` | SmallInteger overflow fallback. |
| `LargeNegativeInteger` | `Integer` | SmallInteger overflow fallback. |
| `Integer` | `Number` | Superclass chain for large integers. |
| `Fraction` | `Number` | Division result type. |
| `OrderedCollection` | `SequenceableCollection` | Dynamic arrays. Common in application code. |
| `Dictionary` | `Collection` | General-purpose hash maps. |
| `Set` | `Collection` | Hash sets. |
| `Stream` | `Object` | Base stream class. |
| `ReadStream` | `Stream` | Reading from collections. |
| `WriteStream` | `Stream` | Writing to collections. |
| `ReadWriteStream` | `Stream` | Bidirectional streams. |
| `Boolean` | `Object` | Abstract superclass of `True` and `False`. |
| `Process` | `Object` | Placeholder — maps to actor in this system. |
| `Semaphore` | `Object` | Intra-actor synchronization if needed. |

**Tier 2 classes for Phase 2 (actor and I/O):**

| Class | Superclass | Why it is needed |
|---|---|---|
| `Actor` | `Object` | Base class for all actors (§10.1 of architecture doc). |
| `Future` | `Object` | Result of `OP_ASK` (§9.3). |
| `SpawnDescriptor` | `Object` | Literal frame entry for `OP_ACTOR_SPAWN` (§4.7, §9.9). |
| `ClosureDescriptor` | `BlockDescriptor` | Literal frame entry for `OP_CLOSURE_COPY` (§4.6). |
| `Socket` | `Object` | TCP socket wrapper (prim 768–773). |
| `FileHandle` | `Object` | File I/O wrapper (prim 780–783). |

### 11.3 Kernel methods

The following methods must be installed in class method dictionaries during bootstrap, either by hand (C-level primitive wiring) or by loading the kernel source file. These are the minimum set required for the interpreter to function — if any of these is missing, basic execution fails.

**Methods that must exist before any bytecode runs:**

| Class | Method | Why |
|---|---|---|
| `Object` | `#doesNotUnderstand:` | Fallback for failed message lookup (§10.9). Without this, any lookup failure is a fatal error. |
| `Object` | `#yourself` | Used by cascades. Trivial (prim 42, returns receiver). |
| `Object` | `#class` | Used by reflection. Prim 30. |
| `Object` | `#==` | Identity comparison. Prim 29. |
| `Object` | `#respondsTo:` | Used by conditional dispatch patterns. Prim 120. |
| `UndefinedObject` | `#subclass:instanceVariableNames:classVariableNames:poolDictionaries:category:` | Required if bootstrap loads Tier 2 classes via Smalltalk source. Can be deferred if all Tier 2 classes are created in C. |
| `True` | `#ifTrue:` | Control flow. Compiler-inlined but must exist for non-inlined cases. |
| `True` | `#ifFalse:` | Control flow. |
| `True` | `#ifTrue:ifFalse:` | Control flow. |
| `False` | `#ifTrue:` | Control flow. |
| `False` | `#ifFalse:` | Control flow. |
| `False` | `#ifTrue:ifFalse:` | Control flow. |
| `SmallInteger` | `#+` | Prim 1. Arithmetic. |
| `SmallInteger` | `#-` | Prim 2. Arithmetic. |
| `SmallInteger` | `#<` | Prim 3. Comparison. |
| `SmallInteger` | `#>` | Prim 4. Comparison. |
| `SmallInteger` | `#=` | Prim 7. Equality. |
| `SmallInteger` | `#*` | Prim 9. Arithmetic. |
| `Array` | `#at:` | Prim 51. Indexed access. |
| `Array` | `#at:put:` | Prim 52. Indexed store. |
| `Array` | `#size` | Prim 53. Collection size. |
| `BlockClosure` | `#value` | Prim 81. Block invocation. |
| `BlockClosure` | `#value:` | Prim 82. Block invocation. |

These methods are installed as primitive methods — the method object has `hasPrimitive` set and contains the appropriate `OP_PRIMITIVE` bytecode. Their fallback bytecode (for primitive failure) can be minimal in bootstrap — signaling an error is acceptable. Full fallback implementations are loaded from the kernel source.

**Methods loaded from kernel source (after bootstrap primitives are wired):**

Everything else in the class library — `#collect:`, `#do:`, `#printOn:`, `#hash`, `#copy`, `#printString`, exception handling methods, stream operations, etc. — is loaded from `.st` source files as the final step of bootstrap. The bootstrap must complete Tier 0 and Tier 1 class creation and install the minimum primitive methods listed above before it can begin loading source, because the source loader itself needs arithmetic, array access, and block evaluation to function.

### 11.4 Symbol table requirements

The following symbols must be interned before the first bytecode executes. They are referenced by the interpreter directly (not via method lookup) or are required by kernel method installation:

- All selectors in the special selector table (§10.7): `#+`, `#-`, `#<`, `#>`, `#<=`, `#>=`, `#=`, `#~=`, `#*`, `#/`, `#\\`, `#@`, `#bitShift:`, `#//`, `#bitAnd:`, `#bitOr:`, `#at:`, `#at:put:`, `#size`, `#next`, `#nextPut:`, `#atEnd`, `#==`, `#class`, `#value`, `#value:`, `#do:`, `#new`, `#new:`, `#yourself`, `#doesNotUnderstand:`, `#mustBeBoolean`
- `#cannotReturn:` — for `BlockCannotReturn` signaling
- `#startUp` — sent on image load
- `#shutDown` — sent before image save
- `#run` — sent to the initial actor
- `#initialize` — sent to newly spawned actors (Phase 2)
- `#ifTrue:`, `#ifFalse:`, `#ifTrue:ifFalse:` — required for kernel method installation on `True`/`False`
- `#signal` — required for exception signaling
- `#on:do:` — required for exception handling
- `#ensure:`, `#ifCurtailed:` — required for unwind protection
- `#value:value:`, `#value:value:value:`, `#valueWithArguments:` — block evaluation variants
- `#printOn:`, `#printString` — required for error messages and transcript output

The symbol table is a lock-free concurrent data structure (read path) with locked writes (§10.6). All symbols listed above are interned during bootstrap in a single-threaded context before any scheduler threads start. After bootstrap, new symbols are interned by the compiler when compiling new methods.

### 11.5 Class table

The class table is a runtime array mapping class indices (`STA_ObjHeader.class_index`) to class objects. It is populated during bootstrap:

1. Each Tier 0 and Tier 1 class is assigned a permanent class index.
2. The class table is sized to accommodate all bootstrap classes plus growth space.
3. New classes added after bootstrap (via `subclass:instanceVariableNames:...`) are assigned the next available index.

**Reserved class indices:**

| Index | Class | Notes |
|---|---|---|
| 0 | — | Reserved (invalid class index). |
| 1 | `SmallInteger` | Tagged immediates have this class index conceptually; the interpreter special-cases SmallInteger dispatch without reading a header. |
| 2 | `Object` | Root class. |
| 3 | `UndefinedObject` | Class of `nil`. |
| 4 | `True` | Class of `true`. |
| 5 | `False` | Class of `false`. |
| 6 | `Character` | Class of character objects. |
| 7 | `Symbol` | Class of interned symbols. |
| 8 | `String` | Class of strings. |
| 9 | `Array` | Class of arrays. |
| 10 | `ByteArray` | Class of byte arrays. |
| 11 | `Float` | Class of floats. |
| 12 | `CompiledMethod` | Class of compiled methods. |
| 13 | `BlockClosure` | Class of blocks and closures. |
| 14 | `Association` | Class of key-value associations. |
| 15 | `MethodDictionary` | Class of method dictionaries. |
| 16 | `Class` | Class of classes. |
| 17 | `Metaclass` | Class of metaclasses. |
| 18 | `Behavior` | Abstract superclass of ClassDescription. |
| 19 | `ClassDescription` | Abstract superclass of Class and Metaclass. |
| 20 | `BlockDescriptor` | Literal frame block descriptors. |
| 21 | `Message` | Message objects for doesNotUnderstand:. |
| 22 | `Number` | Abstract number superclass. |
| 23 | `Magnitude` | Abstract magnitude superclass. |
| 24 | `Collection` | Abstract collection superclass. |
| 25 | `SequenceableCollection` | Abstract sequenceable collection. |
| 26 | `ArrayedCollection` | Abstract arrayed collection. |
| 27 | `Exception` | Base exception class. |
| 28 | `Error` | Base error class. |
| 29 | `MessageNotUnderstood` | DNU exception class. |
| 30 | `BlockCannotReturn` | NLR dead-frame exception class. |
| 31 | `SystemDictionary` | Class of Smalltalk globals. |
| 32+ | — | Assigned dynamically as new classes are created. |

Index 1 (`SmallInteger`) is special: SmallIntegers are tagged immediates with no object header. The interpreter detects SmallIntegers by the tag bit (ADR 007) and dispatches to the `SmallInteger` class without reading a `class_index` field. The class table entry at index 1 still holds the `SmallInteger` class object — it is used by `#class` (prim 30) and reflection, not by the dispatch fast path.

### 11.6 The Smalltalk global dictionary

The `Smalltalk` global dictionary (`SPC_SMALLTALK`, special object index 3) is an instance of `SystemDictionary`. It contains Associations mapping global names to their values. At bootstrap completion, it must contain at minimum:

| Key | Value | Notes |
|---|---|---|
| `#Object` | `Object` class | And every other Tier 0/Tier 1 class. |
| `#Smalltalk` | the dictionary itself | Self-referential. `Smalltalk at: #Smalltalk` returns the dictionary. |
| `#Transcript` | a transcript object or nil | Output target for `printToTranscript:` (prim 113). May be nil until the IDE connects. |
| `#Processor` | the scheduler interface object | Placeholder for the scheduling subsystem. |

Every class created during bootstrap is registered in the global dictionary with its name as the key. The compiler resolves class names by looking up Associations in this dictionary and embedding the Association in the literal frame (`OP_PUSH_GLOBAL`).

### 11.7 Bootstrap execution sequence

The bootstrap runs once — when creating a fresh image from scratch. Subsequent launches load the saved image (ADR 012) and skip the bootstrap entirely.

**Step-by-step sequence:**

1. **Allocate the special object table.** Fill all 32 entries with a temporary nil placeholder (a known bit pattern, not yet the real `nil` object).

2. **Create `nil`, `true`, `false`.** Allocate these as heap objects in the shared immutable region. `nil` is an instance of `UndefinedObject` (class index 3), `true` is an instance of `True` (class index 4), `false` is an instance of `False` (class index 5). The classes don't exist yet — write the class indices directly into the object headers. The class objects will be created in step 4 and the indices will match.

3. **Populate special object table entries 0–2** with the real `nil`, `true`, `false` OOPs.

4. **Create Tier 0 classes.** Allocate raw memory for `Object`, `Behavior`, `ClassDescription`, `Class`, `Metaclass`, and their five metaclasses. Write headers with class indices. Wire superclass and metaclass pointers. Create an empty `MethodDictionary` for each. At the end of this step, the metaclass circularity is resolved and the class table has entries for indices 2, 16, 17, 18, 19.

5. **Create Tier 1 classes.** Using the now-functional (but still primitive) class creation machinery, create all Tier 1 classes listed in §11.2. Each gets a class table entry, a method dictionary, and a global dictionary entry.

6. **Intern kernel symbols.** Create all symbols listed in §11.4. Populate the special selector table (special object index 4).

7. **Create the character table.** Allocate 256 Character instances for ASCII 0–255. Store in special object index 5.

8. **Create the global dictionary.** Instantiate `SystemDictionary`, populate it with Associations for all classes created so far. Store in special object index 3.

9. **Install kernel primitive methods.** For each method listed in §11.3, create a `CompiledMethod` object with the appropriate header (primitive index, arg count), a minimal literal frame (at minimum the owner class OOP in the last slot), and a bytecode array containing `OP_PRIMITIVE` + the prim index (and fallback bytecode if applicable). Install in the appropriate class's method dictionary.

10. **Load kernel source.** Read the kernel `.st` source files and compile them using the now-functional system. This installs the rest of the class library — full method bodies, Tier 2 classes, exception handling, streams, collections, etc.

11. **Save the bootstrap image.** Serialize the complete object graph to disk (ADR 012). This is the initial image that all future launches load.

After step 11, the bootstrap code is never run again. The image is self-sustaining — it contains everything needed to continue development inside the live system.

### 11.8 Post-bootstrap image startup

When an existing image is loaded (not a fresh bootstrap):

1. The runtime loads the image file (ADR 012).
2. The special object table is reconstructed from the image.
3. The class table is reconstructed.
4. The symbol table is reconstructed.
5. Actor heaps are restored. Quiesced actors are in a suspended state.
6. The runtime sends `#startUp` (special object index 9) to the `Smalltalk` global dictionary. The `SystemDictionary >> startUp` method performs any necessary reinitialization — reattaching external resources, reinitializing timers, notifying actors that the image has been restored.
7. Actors resume normal execution.

### 11.9 Phase implementation summary

| Step | Content | Phase |
|---|---|---|
| Special object table | All 32 entries, with entries 0–11 active | Phase 1 |
| Tier 0 classes (metaclass circularity) | Object, Behavior, ClassDescription, Class, Metaclass + metaclasses | Phase 1 |
| Tier 1 classes | All kernel classes needed for bytecode execution | Phase 1 |
| Kernel symbol interning | Special selectors + interpreter-referenced selectors | Phase 1 |
| Character table | 256 ASCII characters | Phase 1 |
| Global dictionary | SystemDictionary with class Associations | Phase 1 |
| Kernel primitive methods | Minimum set from §11.3 | Phase 1 |
| Kernel source loading | Rest of class library from `.st` files | Phase 1 |
| Bootstrap image save | Initial image snapshot | Phase 1 |
| Tier 2 actor/I/O classes | Actor, Future, SpawnDescriptor, Socket, FileHandle | Phase 2 |
| Image startup protocol (`#startUp`) | Reinitialization on image load | Phase 1 |

---

## §12 Quick Reference Table

This section is a flat lookup table — every defined opcode on one row, sorted by byte value. No prose. Exists so CC can look up any opcode without navigating grouped subsections.

### 12.1 Complete opcode table

| Byte | Mnemonic | Operand | Stack effect | Description | Phase |
|---|---|---|---|---|---|
| `0x00` | `OP_NOP` | unused (0x00) | `( -- )` | No operation. Advances PC by 2. | Phase 1 |
| `0x01` | `OP_WIDE` | high byte | *prefix* | Wide prefix. Next instruction uses 16-bit operand: `(high << 8) \| low`. Not a standalone instruction. | Phase 1 |
| `0x02` | `OP_PUSH_RECEIVER` | unused (0x00) | `( -- self )` | Push receiver. | Phase 1 |
| `0x03` | `OP_PUSH_NIL` | unused (0x00) | `( -- nil )` | Push nil. | Phase 1 |
| `0x04` | `OP_PUSH_TRUE` | unused (0x00) | `( -- true )` | Push true. | Phase 1 |
| `0x05` | `OP_PUSH_FALSE` | unused (0x00) | `( -- false )` | Push false. | Phase 1 |
| `0x06` | `OP_PUSH_LIT` | lit index | `( -- value )` | Push literal from literal frame. | Phase 1 |
| `0x07` | `OP_PUSH_TEMP` | temp index | `( -- value )` | Push temporary or argument. | Phase 1 |
| `0x08` | `OP_PUSH_INSTVAR` | instVar index | `( -- value )` | Push instance variable. | Phase 1 |
| `0x09` | `OP_PUSH_GLOBAL` | lit index | `( -- value )` | Push global via Association in literal frame. Reads the Association's value slot. | Phase 1 |
| `0x0A` | `OP_PUSH_CONTEXT` | unused (0x00) | `( -- context )` | Reify thisContext as heap object. | Phase 3 |
| `0x0B` | `OP_PUSH_SMALLINT` | unsigned value | `( -- int )` | Push SmallInteger 0–255 (0–65535 with OP_WIDE). | Phase 1 |
| `0x0C` | `OP_PUSH_MINUS_ONE` | unused (0x00) | `( -- -1 )` | Push SmallInteger -1. | Phase 1 |
| `0x0D` | `OP_PUSH_ZERO` | unused (0x00) | `( -- 0 )` | Push SmallInteger 0. | Phase 1 |
| `0x0E` | `OP_PUSH_ONE` | unused (0x00) | `( -- 1 )` | Push SmallInteger 1. | Phase 1 |
| `0x0F` | `OP_PUSH_TWO` | unused (0x00) | `( -- 2 )` | Push SmallInteger 2. | Phase 1 |
| `0x10` | `OP_STORE_TEMP` | temp index | `( value -- value )` | Store into temp; value stays. | Phase 1 |
| `0x11` | `OP_STORE_INSTVAR` | instVar index | `( value -- value )` | Store into instVar; value stays. | Phase 1 |
| `0x12` | `OP_STORE_GLOBAL` | lit index | `( value -- value )` | Store into global Association; value stays. | Phase 1 |
| `0x13` | `OP_POP_STORE_TEMP` | temp index | `( value -- )` | Pop and store into temp. | Phase 1 |
| `0x14` | `OP_POP_STORE_INSTVAR` | instVar index | `( value -- )` | Pop and store into instVar. | Phase 1 |
| `0x15` | `OP_POP_STORE_GLOBAL` | lit index | `( value -- )` | Pop and store into global Association. | Phase 1 |
| `0x16` | `OP_STORE_OUTER_TEMP` | encoded index | `( value -- value )` | Store into outer scope temp via closure chain; value stays. | Phase 2 |
| `0x17` | `OP_POP_STORE_OUTER_TEMP` | encoded index | `( value -- )` | Pop and store into outer scope temp. | Phase 2 |
| `0x20` | `OP_SEND` | lit index | `( rcvr args -- result )` | Send message; selector from literal frame. Increments reductions. | Phase 1 |
| `0x21` | `OP_SEND_SUPER` | lit index | `( rcvr args -- result )` | Super send; lookup starts at owner class superclass. Increments reductions. | Phase 1 |
| `0x30` | `OP_RETURN_TOP` | unused (0x00) | `( value -- )` | Return top of stack to sender. TCO target. | Phase 1 |
| `0x31` | `OP_RETURN_SELF` | unused (0x00) | `( -- )` | Return self to sender. Implicit return. | Phase 1 |
| `0x32` | `OP_RETURN_NIL` | unused (0x00) | `( -- )` | Return nil to sender. | Phase 1 |
| `0x33` | `OP_RETURN_TRUE` | unused (0x00) | `( -- )` | Return true to sender. | Phase 1 |
| `0x34` | `OP_RETURN_FALSE` | unused (0x00) | `( -- )` | Return false to sender. | Phase 1 |
| `0x35` | `OP_NON_LOCAL_RETURN` | unused (0x00) | `( value -- )` | Non-local return to home method's sender. BlockCannotReturn if dead. | Phase 2 |
| `0x40` | `OP_JUMP` | forward offset | `( -- )` | Unconditional forward jump. | Phase 1 |
| `0x41` | `OP_JUMP_TRUE` | forward offset | `( value -- )` | Pop; jump if true; mustBeBoolean if non-boolean. | Phase 1 |
| `0x42` | `OP_JUMP_FALSE` | forward offset | `( value -- )` | Pop; jump if false; mustBeBoolean if non-boolean. | Phase 1 |
| `0x43` | `OP_JUMP_BACK` | backward offset | `( -- )` | Backward jump. Checks reduction counter. | Phase 1 |
| `0x50` | `OP_POP` | unused (0x00) | `( value -- )` | Discard top of stack. | Phase 1 |
| `0x51` | `OP_DUP` | unused (0x00) | `( value -- value value )` | Duplicate top of stack. | Phase 1 |
| `0x52` | `OP_PRIMITIVE` | prim index | *varies* | Execute primitive. On fail, store error code and fall through. | Phase 1 |
| `0x60` | `OP_BLOCK_COPY` | lit index | `( -- block )` | Create clean block from literal frame descriptor. | Phase 1 |
| `0x61` | `OP_CLOSURE_COPY` | lit index | `( vals -- closure )` | Create capturing closure from literal frame descriptor. | Phase 2 |
| `0x62` | `OP_PUSH_OUTER_TEMP` | encoded index | `( -- value )` | Push temp from enclosing scope via closure chain. | Phase 2 |
| `0x70` | `OP_SEND_ASYNC` | lit index | `( actor args -- )` | Async fire-and-forget send. Deep-copies args. | Phase 2 |
| `0x71` | `OP_ASK` | lit index | `( actor args -- future )` | Async request-response send. Returns future. | Phase 2 |
| `0x72` | `OP_ACTOR_SPAWN` | lit index | `( args -- actor )` | Spawn new actor from spawn descriptor in literal frame. | Phase 2 |
| `0x73` | `OP_SELF_ADDRESS` | unused (0x00) | `( -- addr )` | Push current actor's opaque address. | Phase 2 |

### 12.2 Opcode count by phase

| Phase | Opcodes | Description |
|---|---|---|
| Phase 1 | 31 | Full single-actor Smalltalk: push, store, send, return, jump, stack, primitive, clean blocks. |
| Phase 2 | 8 | Actor runtime: async send, ask, spawn, self-address, non-local return, closures, outer temps. |
| Phase 3 | 1 | thisContext reification. |
| **Total defined** | **40** | |
| Reserved | 216 | Byte values 0x02–0xFF minus 40 defined. Available for future use. |

### 12.3 Opcode range map

```
0x00      NOP
0x01      WIDE prefix
0x02-0x0F Push group          (14 defined, 0 reserved in range)
0x10-0x1F Store group         (8 defined, 8 reserved)
0x20-0x2F Send group          (2 defined, 14 reserved)
0x30-0x3F Return group        (6 defined, 10 reserved)
0x40-0x4F Jump group          (4 defined, 12 reserved)
0x50-0x5F Stack/misc group    (3 defined, 13 reserved)
0x60-0x6F Block/closure group (3 defined, 13 reserved)
0x70-0x7F Actor group         (4 defined, 12 reserved)
0x80-0xEF Unassigned          (112 bytes, future groups)
0xF0-0xFF Debug/instrument    (reserved, Phase 3)
```

### 12.4 Instruction length rules

| Condition | Length |
|---|---|
| Opcode is `OP_WIDE` (`0x01`) | 4 bytes: `OP_WIDE high opcode low` |
| Any other opcode | 2 bytes: `opcode operand` |

No exceptions. Every instruction is 2 bytes. `OP_WIDE` extends the following instruction to 4 bytes total.

### 12.5 Reduction counting summary

| Opcode | Counts reduction? |
|---|---|
| `OP_SEND` | Yes (1 reduction) |
| `OP_SEND_SUPER` | Yes (1 reduction) |
| `OP_SEND_ASYNC` | Yes (1 reduction) |
| `OP_ASK` | Yes (1 reduction) |
| `OP_ACTOR_SPAWN` | Yes (1 reduction) |
| `OP_JUMP_BACK` | Yes (1 reduction) |
| All other opcodes | No |

Preemption check fires when counter >= `STA_REDUCTION_QUOTA` (1000, ADR 009).

### 12.6 Primitive index ranges

| Range | Allocation | Phase |
|---|---|---|
| 0 | No primitive | — |
| 1–127 | Blue Book kernel (arithmetic, comparison, object, array, block, symbol) | Phase 1 |
| 128–255 | Blue Book extended (system, image, platform) | Phase 1–2 |
| 256–511 | Smalltalk/A actor primitives | Phase 2 |
| 512–767 | Smalltalk/A capability primitives | Phase 4 |
| 768–1023 | Smalltalk/A I/O primitives (libuv-backed) | Phase 2 |
| 1024–65535 | Reserved | — |

Primitives 1–255 use `OP_PRIMITIVE` directly. Primitives 256+ use `OP_WIDE` + `OP_PRIMITIVE`.

---
