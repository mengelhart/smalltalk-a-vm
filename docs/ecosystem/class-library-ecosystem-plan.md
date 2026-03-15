# Smalltalk/A Class Library and Web Ecosystem Plan

*Draft planning document — March 2026*

> **Purpose**
>
> This document captures the plan for Smalltalk/A's class library, web infrastructure, and supporting ecosystem. It defines what ships with the system, what ships as official packages, and what is left to the community — with the explicit goal of making Smalltalk/A a credible platform for building and deploying web applications and services.

> **Guiding principle**
>
> A language runtime without a practical story for building web applications is, in 2026, a hobby. The runtime is necessary but not sufficient. The ecosystem on top is what makes engineers choose it for real work.

---

## 1. Strategic Context

Smalltalk/A's primary opportunity is the web — web services, APIs, web applications, and microservices. This is where the actor architecture and BEAM-class concurrency provide a genuine competitive advantage. The combination of lightweight actors, async I/O, hot code reloading, and live development is unique. But none of that matters if an engineer cannot build and deploy a web service without writing everything from scratch.

The class library and ecosystem must be designed so that an engineer can go from "I've never heard of Smalltalk/A" to "I have a running service deployed on AWS" in an afternoon.

### 1.1 The competitive landscape

Every successful server-side runtime tells the same story: the language alone is not enough.

- **Erlang/Elixir:** the BEAM would be an academic curiosity without OTP, Cowboy, Ecto, and Phoenix.
- **Go:** would be a footnote without its standard library HTTP server, JSON support, and database interfaces.
- **Ruby:** would be nothing without Rails.
- **Python:** owes its server-side adoption to Django, Flask, and the ecosystem around them.

Smalltalk/A must follow this pattern. The runtime proves the architecture. The class library and web ecosystem prove it's practical.

### 1.2 What "batteries included" means

The class library ships with every image. It is the foundation that every Smalltalk/A program builds on. An engineer should never have to search for a third-party package to accomplish basic web service tasks — HTTP, JSON, database access, TLS. If they have to go looking for a JSON parser before building a hello-world web service, they will close the tab and never come back.

---

## 2. The Base Class Library

### 2.1 Core classes — ported from Cuis Smalltalk

The core class hierarchy is ported from Cuis Smalltalk (the lean, cleaned-up Smalltalk kernel). The Blue Book governs the core language and VM architecture. ANSI Smalltalk-98 governs the class library surface and serves as the conformance target.

These classes are proven over four decades. The logic and structure are sound. The work is translation and adaptation to Smalltalk/A's object memory and actor model, not invention.

**Collections:** `Array`, `ByteArray`, `OrderedCollection`, `Dictionary`, `IdentityDictionary`, `Set`, `IdentitySet`, `Bag`, `SortedCollection`, `LinkedList`, `Interval`, `String`, `Symbol`, and the full `Collection` protocol hierarchy.

**Streams:** `ReadStream`, `WriteStream`, `ReadWriteStream`. Adapted for the async I/O substrate where appropriate.

**Numbers:** `SmallInteger`, `LargePositiveInteger`, `LargeNegativeInteger`, `Float`, `Fraction`, `ScaledDecimal`, and the `Number`/`Magnitude` hierarchy.

**Core objects:** `Object`, `UndefinedObject`, `Boolean`, `True`, `False`, `Character`, `Message`, `BlockClosure`, `Association`.

**Classes and reflection:** `Behavior`, `ClassDescription`, `Class`, `Metaclass`, `MethodDictionary`, `CompiledMethod`.

**Exceptions:** `Exception`, `Error`, `MessageNotUnderstood`, `ZeroDivide`, and the exception handling protocol (`on:do:`, `ensure:`, `ifCurtailed:`).

**Text and strings:** standard `String` protocols ported from Cuis, but internal representation and encoding built fresh on Smalltalk/A's compiled Unicode tables from UnicodeData.txt. Full Unicode support from day one.

### 2.2 What is not ported from Cuis

Anything touching Morphic, display, graphics, sound, eToys, or the Squeak-specific IDE classes. These are excluded entirely and permanently.

Anything touching Squeak/Pharo-specific I/O — their `FileStream`, `Socket`, networking classes. These are all built against a synchronous single-threaded model that is fundamentally incompatible with the actor architecture. The protocol *names* and API surface are useful references; the implementations are not.

### 2.3 Collections and the actor model

The standard mutable collection classes work correctly within actors because each actor has an isolated heap. An `OrderedCollection` created inside an actor lives on that actor's heap and is never shared.

When collections cross actor boundaries via messaging, they are deep-copied by default. This enforces isolation and is correct for the common case.

**Future consideration:** persistent (structurally-shared) immutable collection variants — similar to Clojure's persistent vectors and hash maps — could avoid expensive deep copies for large read-only datasets passed between actors. This is an optimization for later phases, not a requirement for the initial class library.

---

## 3. Tiered Ecosystem Plan

The ecosystem is organized into four tiers based on where each component belongs — in the class library, as an official package, or as community-contributed.

### 3.1 Tier 1 — ships with the class library (non-negotiable)

These are so fundamental that not having them makes the system feel broken.

**HTTP/1.1 server.** An actor-based HTTP server. The listener is an actor that accepts connections and spawns per-connection or per-request handler actors. Keep-alive, chunked transfer encoding, and standard HTTP/1.1 semantics. This is where Smalltalk/A's architecture shines — every connection is an actor, isolation is free, concurrency scales with actor count. HTTP/2 support is a fast follow (multiplexed streams map naturally to actors — each stream as an actor within a connection actor — but the HPACK header compression and framing are real implementation work).

**HTTP client.** Connection pooling via a pool actor that manages connection actors. Redirects, timeouts, configurable headers. Essential because every web service talks to other web services.

**JSON parser and generator.** Fast, stream-capable, built-in. The parser takes bytes off the wire and produces Smalltalk objects (`Dictionary`, `Array`, `String`, `Number`, `Boolean`, `nil`). The generator takes Smalltalk objects and produces bytes. Well-specified, small, no reason to make anyone go looking for it.

**TLS.** Wraps a C library (LibreSSL or OpenSSL) via a primitive bridge. The Smalltalk-facing API is clean — a `TLSSocket` that wraps a regular socket transparently. Nobody implements TLS from scratch and nobody should.

**URL parsing and encoding.** Sounds trivial, is foundational. Every HTTP operation depends on it. RFC 3986 compliant parsing, query parameter encoding/decoding, path manipulation.

**File I/O.** Actor-aware file operations through the async I/O substrate. The API surface looks synchronous to the programmer (`FileStream read: '/tmp/config.json'`) but the actor suspends and resumes on I/O completion. Standard Smalltalk file and directory protocols adapted for the async model.

**Structured logging.** A logging actor that accepts log messages and writes them out. Structured (key-value) output suitable for headless deployment. Configurable log levels, output formats, and destinations. Works in both interactive and headless mode.

**Configuration.** Environment variables, config files, command-line arguments. The mechanism for injecting runtime configuration into a headless deployment. Every service needs it.

### 3.2 Tier 2 — ships with the class library (strong expectation)

Most web applications need these. A brief period without them during early releases is survivable but not ideal.

**WebSocket — client and server.** Real-time web applications depend on it. Long-lived WebSocket connections map naturally to long-lived actors — each connection is an actor with its own state and lifecycle, supervised and fault-isolated.

**PostgreSQL client.** PostgreSQL is the default database for serious web applications. The wire protocol is well-documented. The client is an actor-based implementation: a `DatabaseConnectionPool` actor manages `DatabaseConnection` actors, each handling the PostgreSQL wire protocol asynchronously through the I/O substrate. Non-blocking queries, prepared statements, binary and text result formats, LISTEN/NOTIFY (which maps beautifully to actors — a notification from PostgreSQL becomes a message in an actor's mailbox). This is the single largest implementation piece in Tier 2 but it's well-documented protocol work, not research.

**SQLite.** An embedded database — a C library linked and called through the primitive bridge. SQLite calls go through the async I/O substrate's thread pool (like file I/O) so they don't block scheduler threads. The default for development, testing, and lightweight applications.

**OpenTelemetry (OTEL) instrumentation.** Three signal types: traces, metrics, and logs.

*Traces* map naturally to actors — each actor boundary is a natural span boundary. The actor's mailbox receive is the span start, the reply or completion is the span end. Instrumenting at the runtime level (rather than requiring manual span creation) is a compelling differentiator.

*Metrics* are collected by a metrics actor that accumulates counters, gauges, and histograms. Recording a metric from application code is an async message send that never blocks the sender.

*Logs* connect to the structured logging from Tier 1, adding OTEL's standardized format and export path.

*Export:* initially over HTTP/JSON (using the Tier 1 HTTP client and JSON generator). The gRPC/protobuf export path is significantly more work and can come later — most OTEL collectors accept HTTP/JSON.

The instrumentation API (`Span`, `Tracer`, `Meter`, `Counter`, `Histogram`, `Gauge`) should be designed early even if export is stubbed. Application code written against the API today works when export is wired up later.

**OTEL matters strategically** because it signals to evaluating engineers that this is a serious production runtime. An ops engineer who hears "it exports OTEL traces and metrics" immediately thinks "this fits into our existing observability stack."

### 3.3 Tier 3 — official packages (maintained by the project)

Important for a complete ecosystem but don't need to be in the base class library. Shipped separately, maintained to the same quality standard.

**AWS SDK (Smithy-generated).** See Section 4 for the code generation strategy. Priority services: S3 (file storage — simplest API to exercise), DynamoDB (default for serverless-style architectures), SQS/SNS (message queues and pub/sub — natural fits for an actor-based system), CloudWatch (metrics and logging complementing OTEL), ECR/ECS (container deployment), Lambda (function deployment).

**OpenAPI client generator.** Same architecture as the Smithy generator but reading OpenAPI specs. Produces Smalltalk client classes for any API that publishes an OpenAPI definition — Google Cloud, Azure, Stripe, Twilio, and thousands of others.

**XML parser.** Unfortunately still needed — RSS, SVG, configuration files, legacy integrations. Well-specified, not something every service needs on day one.

**OAuth client.** Important for third-party API integration. The OAuth spec is sprawling and provider-specific quirks require ongoing maintenance — better as a dedicated package than embedded in the class library.

**HTML templating.** Needed for web applications (as opposed to pure API services). A simple, fast template engine for generating HTML from Smalltalk objects.

**Markdown parser.** Increasingly important for content-oriented applications.

**YAML / TOML.** Configuration file formats.

**Redis client.** Common in web architectures for caching, sessions, and pub/sub.

### 3.4 Tier 4 — community packages

Not the project's responsibility initially, but important for ecosystem growth. The project provides good documentation, a package format, and a registry so the community can build and share these.

MySQL client, MongoDB client, GraphQL (client and server), gRPC, message queue clients (RabbitMQ, Kafka), email/SMTP, CSS parsing, image processing, PDF generation, CSV parsing, regular expressions (extended beyond what the class library provides), date/time libraries (timezone-aware, beyond the base `Date` and `Time`), cryptography utilities (beyond what TLS provides), rate limiting, circuit breakers, service discovery.

---

## 4. API Client Code Generation Strategy

### 4.1 The problem with pure code generators

Mechanical code generators that read API specs (Smithy, OpenAPI) produce technically correct but ergonomically poor code. Every field, every optional parameter, every edge case error type — faithful to the spec but miserable to use. Nobody wants to write `PutBucketAccelerateConfigurationRequest new` when what they want is `s3 put: data at: key`.

Conversely, hand-written or LLM-generated clients are idiomatic but unreliable — they hallucinate endpoints, miss pagination, and get parameter types wrong.

### 4.2 The three-layer approach

**Layer 1 — mechanical generation (always correct).** A code generator reads the spec and produces low-level faithful classes — every operation, every shape, every error type. These classes are the foundation. They live in the package but most developers rarely touch them directly. Correctness is guaranteed because the code is derived directly from the spec.

**Layer 2 — idiomatic wrapper API (handcrafted with taste).** A clean Smalltalk API wraps the generated classes with the interface developers actually want to use. `S3Client >> put:at:` delegates to the generated `PutObjectRequest` underneath. This layer is where the taste and ergonomics live — concise, discoverable, Smalltalk-feeling method names with sensible defaults.

**Layer 3 — LLM-assisted development of the wrapper layer.** Given the generated low-level classes as context, an LLM proposes the clean high-level API surface — convenience method names, operation groupings, default parameter values. A developer reviews and refines in the live image. The LLM handles pattern recognition over known APIs; the developer ensures it feels right.

### 4.3 Self-hosted in Smalltalk (no external language dependency)

The code generators are Smalltalk programs running headlessly. No Python, no Node, no external toolchain. The generator reads JSON (Smithy models are JSON), walks the tree, applies string transformations, and writes `.st` files.

    sta_vm --headless generator.image \
      --eval "SmithyGenerator generateFrom: '/path/to/s3.json' to: '/output/aws/'"

By the time the generators are needed, everything they require already exists in the class library: JSON parser, collections, string manipulation, file I/O.

**This is deliberate dogfooding.** The generator proves that Smalltalk/A is practical for real tooling tasks, not just the applications it generates code for. The entire project — runtime, class library, code generators, deployment tooling — uses one language.

### 4.4 Reusable architecture

The emitter infrastructure is shared across spec formats. A `SmithyParser` and an `OpenAPIParser` produce a common intermediate representation of services, operations, and shapes. A shared `SmalltalkClientEmitter` generates the Smalltalk classes from that representation. Adding support for a new API spec format means writing a new parser frontend, not a new code generation backend.

**Generator classes (approximate shape):**

- `SmithyModel` / `OpenAPIModel` — parses spec JSON into an in-memory representation
- `ServiceDescription` — intermediate representation of a service (operations, shapes, errors)
- `SmalltalkClassEmitter` — takes a shape and writes a `.st` class definition
- `ServiceClientEmitter` — generates the actor-based client class with a method per operation
- `SmithyGenerator` / `OpenAPIGenerator` — orchestrates the pipeline

Maybe a dozen classes total. Developed interactively in the live image — load a spec, inspect the parsed structure, generate a single class, examine the output, refine.

---

## 5. Async I/O Architecture for the Web Tier

All networking and I/O in the web tier is built on the async I/O substrate (libuv-backed) established in Phase 0 (Spike 005). This section describes how the web-facing classes use that substrate.

### 5.1 The fundamental pattern

Every I/O operation follows the same actor-friendly pattern:

1. An actor initiates an I/O operation (socket read, file write, DNS lookup)
2. The operation is registered with the I/O substrate
3. The actor is suspended (removed from the scheduler's run queue)
4. The scheduler thread is free to run other actors
5. When the I/O operation completes, a completion message is placed in the actor's mailbox
6. The actor is made runnable and resumes on the next scheduling quantum

This pattern is identical across TCP sockets, UDP, file I/O, DNS, timers, and subprocesses. The only thing that changes is which libuv function is called and what the completion data looks like.

### 5.2 Implications for the web tier

**HTTP server:** scheduler threads never block on socket operations. A server handling ten thousand concurrent connections has ten thousand actors, each individually lightweight (~300 bytes), each independently scheduled across all CPU cores. This is the BEAM model applied to HTTP.

**Database clients:** PostgreSQL queries are non-blocking. A database connection actor sends the query over its socket, suspends, and resumes when the response arrives. Other actors continue running on the same scheduler thread. Connection pool depth is bounded by the pool actor, not by thread count.

**External API calls (AWS, etc.):** HTTP client requests to external services are non-blocking. An actor making an S3 API call suspends while waiting for the response. Thousands of concurrent external API calls are limited only by network capacity and actor count, not by threads.

### 5.3 What this means for developers

Code inside an actor reads sequentially even though execution is concurrent:

    handleRequest: request
        | user orders |
        user := db query: 'SELECT * FROM users WHERE id = $1' with: {request userId}.
        orders := db query: 'SELECT * FROM orders WHERE user_id = $1' with: {user id}.
        ^ Response json: (JsonObject new
            at: 'user' put: user;
            at: 'orders' put: orders;
            yourself)

Each `db query:` suspends the actor and resumes when the result arrives. The developer writes sequential code. The runtime executes it concurrently. This is the same model Erlang, Go, and every other serious concurrent runtime uses.

---

## 6. Deployment Model

### 6.1 The deployment artifact

A Smalltalk/A deployment consists of two files:

- **The VM binary** (`sta_vm`) — compiled for the target platform (macOS arm64 or Linux x86_64/arm64)
- **The application image** — contains the class library, application code, compiled methods, and optionally application state

The image is platform-independent. The same image file runs on macOS and Linux. Only the VM binary differs per platform.

### 6.2 Container deployment

    FROM ubuntu:24.04
    COPY sta_vm /usr/local/bin/
    COPY myapp.image /app/
    ENTRYPOINT ["sta_vm", "--headless", "/app/myapp.image"]

No special base image. No display server. No multi-gigabyte runtime. A binary and a file. An ops engineer who has never heard of Smalltalk can understand this Dockerfile in five seconds.

### 6.3 The image as deployment and state artifact

The image is not just a deployment artifact — it is a state artifact. Saving the image captures everything: code, class library, compiled methods, actor state at the moment of snapshot. This enables:

- **Rollback:** deploy a previous image file to restore a previous version
- **Bug reproduction:** hand someone the exact image the service was running
- **State migration:** snapshot a running service, move the image to a different server, resume

### 6.4 Hot code reloading in production

A running headless service can accept code changes without restart. The mechanism is the same as live development: a new compiled method atomically replaces the old entry in the method dictionary. In-flight requests finish on the old code. New requests use the patched code. Zero downtime.

Access to the live system is through a REPL actor listening on a Unix domain socket or TCP port, capability-gated for security. The same eval path the IDE workspace uses. SSH into the server, connect to the REPL, patch the bug, verify, disconnect.

---

## 7. Level of Effort Estimates

These are rough order-of-magnitude estimates assuming the class library core, actor runtime, and async I/O substrate are complete (end of Phase 2).

| Component | Estimated effort | Notes |
|---|---|---|
| **Tier 1** | | |
| HTTP/1.1 server | 3–4 weeks | Protocol parsing is the bulk; actor integration is straightforward |
| HTTP client | 2–3 weeks | Connection pooling, redirects, timeouts |
| JSON parser + generator | 1 week | Well-specified, small |
| TLS (C library wrapper) | 1–2 weeks | Primitive bridge to LibreSSL/OpenSSL |
| URL parsing | 2–3 days | Small but needs to be correct (RFC 3986) |
| File I/O (async) | 1 week | libuv fs wrapper + Smalltalk API |
| Structured logging | 3–5 days | Logging actor + formatters |
| Configuration | 2–3 days | Env vars, config files, CLI args |
| **Tier 1 total** | **~10–12 weeks** | |
| **Tier 2** | | |
| WebSocket (client + server) | 2 weeks | Upgrade from HTTP, framing, ping/pong |
| PostgreSQL client | 3–4 weeks | Wire protocol, auth, prepared statements, LISTEN/NOTIFY |
| SQLite wrapper | 1 week | C library bridge + async thread pool integration |
| OTEL instrumentation API | 1–2 weeks | Span, Tracer, Meter classes; stub export |
| OTEL HTTP/JSON export | 1 week | Uses HTTP client + JSON generator |
| **Tier 2 total** | **~8–10 weeks** | |
| **Tier 3 (selected)** | | |
| Smithy code generator | 2–3 weeks | Parser + emitter framework |
| AWS S3 client (generated + wrapper) | 1–2 weeks | First service; proves the generator |
| AWS DynamoDB client | 1 week | Second service; mostly generated |
| OpenAPI generator | 1–2 weeks | Reuses emitter framework |
| XML parser | 2 weeks | |
| OAuth client | 1–2 weeks | |
| HTML templating | 1–2 weeks | |
| Redis client | 1 week | Simple protocol, actor-friendly |

**Total Tier 1 + Tier 2:** approximately 18–22 weeks of focused implementation work after Phase 2 is complete.

These estimates assume a single developer working full-time. They are rough guides for planning, not commitments. Some components will be faster than expected (JSON parser); others will surface surprises (PostgreSQL auth handshake edge cases, TLS integration quirks).

---

## 8. Sequencing

### 8.1 What comes before the web tier

- **Phase 1:** Object memory, bytecode interpreter, bootstrap kernel, compiler, eval loop, image save/load. The class library core (collections, numbers, streams, strings, exceptions).
- **Phase 2:** Actor runtime, per-actor heaps, work-stealing scheduler, async I/O substrate, supervision, cross-actor messaging.

### 8.2 The web tier as the bridge to the IDE

The web tier is built after Phase 2 and before the native IDE (Phase 3 in the original plan). This resequencing is deliberate:

1. Phase 2 delivers a working actor runtime with async I/O
2. The web tier (Tier 1 + Tier 2) proves the runtime is practical for real-world services
3. By the time the IDE is built, the runtime is battle-tested from months of serving web traffic
4. The IDE builds against a stable `sta/vm.h` surface that has been exercised by the web tier
5. The "wow" demo is a live web service, not a toy UI

### 8.3 Development workflow during the web tier

All web tier development happens headlessly using the Emacs-based workflow:

- `.st` files edited in Emacs with Smalltalk syntax highlighting
- An Emacs minor mode sends code to the running VM over a Unix socket for evaluation
- Manual file-in for loading class definitions
- REPL for interactive testing and exploration
- CTest and Smalltalk-level test frameworks for automated testing
- Git for version control of `.st` source files

The native IDE is not needed. The Emacs workflow is a first-class development experience, not a stopgap.

---

## 9. The Adoption Demo

The demo that sells Smalltalk/A to a skeptical engineer:

1. **Build** a REST API with PostgreSQL storage in the live image (Emacs or IDE)
2. **Deploy** to a Linux container on AWS (ECS/Fargate)
3. **Show** request throughput with thousands of concurrent connections, each handled by its own actor
4. **SSH** into the running server, inspect the actor tree, check mailbox depths
5. **Hot-patch** a bug in a request handler while traffic is flowing — zero dropped requests
6. **Show** OTEL traces flowing into the observability stack, with automatic span boundaries at actor transitions
7. **Open** the same application in the native macOS IDE, edit a method, see it take effect immediately

This is a demo no other Smalltalk can give. It is also a demo that most languages cannot give — the combination of actor-level concurrency visibility, hot code patching, and live development is unique to the Erlang world, and Smalltalk/A brings it to a language with more approachable syntax and a native IDE.

---

## 10. Open Questions

- **HTTP/2 priority:** is HTTP/2 a Tier 1 requirement or a fast follow after HTTP/1.1? HPACK and the framing layer are significant work. Most services behind a reverse proxy (nginx, AWS ALB) can accept HTTP/1.1 from the application tier.
- **Database abstraction layer:** should there be a common `Database` protocol that PostgreSQL and SQLite both implement, or are they different enough to remain separate? Erlang's Ecto provides a useful reference here.
- **Package format and registry:** what does the package distribution mechanism look like? How are Tier 3 and Tier 4 packages discovered, installed, and version-managed? This is a significant design question that affects ecosystem growth.
- **Test framework:** what does the Smalltalk/A test framework look like? SUnit is the standard Smalltalk test framework and should be ported early. It's needed before the web tier can be developed with confidence.
- **Benchmarking and performance baselines:** at what point do we establish performance targets for HTTP throughput, JSON parsing speed, and database query latency? Having concrete numbers early prevents silent performance regression.
