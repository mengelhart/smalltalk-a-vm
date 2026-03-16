# Smalltalk/A Library Design Conventions and Authoring Guidelines

*Draft — March 2026*

> **Purpose**
>
> This document defines the design conventions, patterns, and authoring guidelines for Smalltalk/A's class library and ecosystem packages. Its goal is to ensure that the entire ecosystem feels like a coherent system designed by one mind, not a collection of independent modules written by different teams with different conventions.
>
> The Tier 1 and Tier 2 libraries are the reference implementations of these conventions. Every design decision in the HTTP server, the JSON parser, the PostgreSQL client, and the rest of the class library establishes patterns that community package authors will follow.

> **Why this matters**
>
> Every language ecosystem that launched without strong design opinions about library conventions eventually fragmented. Ruby's HTTP story spawned a dozen competing wrappers because the standard library got the API wrong. The npm ecosystem became a dependency nightmare because there were no conventions and anyone could publish anything. Rust's crates ecosystem is heading the same direction — competing HTTP clients, competing async runtimes, thousands of micro-packages. Smalltalk/A avoids this by being opinionated from day one.

---

## 1. The Guiding Principle

The class library and ecosystem should feel like Smalltalk — not like a collection of utilities that happen to be written in Smalltalk. Every class, every method, every protocol should feel like it belongs to the same system. A developer browsing from `OrderedCollection` to `HttpRequest` to `DatabaseConnection` should experience consistent naming, consistent structure, consistent error handling, and consistent design taste.

The standard for "does this feel right" is: would this class look natural sitting next to the Blue Book kernel classes in a system browser? If it feels like it was bolted on from a different language's conventions, it needs to be redesigned.

---

## 2. Naming Conventions

### 2.1 Class names

Class names are nouns or noun phrases in UpperCamelCase. They describe what the object *is*, not what it does.

Good:

    HttpRequest
    JsonParser
    DatabaseConnection
    ActorSupervisor
    CapabilityToken

Bad:

    HTTPRequest          "don't capitalize acronyms fully — it's Http, not HTTP"
    ParseJSON            "verb phrase — this is a method name, not a class name"
    DBConn               "abbreviations make the system less discoverable"
    RequestHandler       "too vague — handler of what kind of request?"

**Acronym rule:** treat acronyms as words. `Http`, not `HTTP`. `Json`, not `JSON`. `Sql`, not `SQL`. `Tls`, not `TLS`. `Url`, not `URL`. This follows the Smalltalk tradition of readable class names and avoids the visual noise of `HTTPSURLRequest`.

**Actor class names:** actor classes should end in `Actor` only when the actor nature is the primary identity. `DatabaseConnectionPool` is fine — it happens to be an actor but its identity is "connection pool." `WorkspaceActor` is fine — its identity is defined by being an actor that manages a workspace. Use judgment, not a rigid suffix rule.

### 2.2 Method names

Method names follow standard Smalltalk conventions — keyword messages that read as natural language phrases. The receiver is the subject; the method name and arguments form a sentence.

Good:

    request headerAt: 'Content-Type'
    response statusCode
    connection executeQuery: 'SELECT ...' with: params
    pool acquireConnection
    logger info: 'Request handled' with: metadata

Bad:

    request getHeader: 'Content-Type'     "no get/set prefixes — this is not Java"
    response get_status_code              "no snake_case — this is not Python"
    connection exec: 'SELECT ...'          "too abbreviated — spell it out"
    pool getConn                           "abbreviated and uses get prefix"

**The `get`/`set` rule:** Smalltalk does not use `get` and `set` prefixes. Accessors are the bare name: `response statusCode` to read, `response statusCode: 200` to write. This is a fundamental Smalltalk convention. Libraries that use Java-style `getStatusCode` / `setStatusCode` are not idiomatic and should be rejected in review.

**Consistency with the class library:** if the class library uses `at:` and `at:put:` for dictionary access, every library that has dictionary-like access should use `at:` and `at:put:`. If the class library uses `do:` for iteration, every collection-like object should use `do:`. Do not invent new names for existing protocols.

### 2.3 Protocol (category) names

Methods are organized into protocols (categories) that describe their purpose. Use the same protocol names across all libraries for the same kinds of methods.

Standard protocol names:

    'accessing'          — getters and setters for object state
    'testing'            — boolean queries (isEmpty, isValid, hasHeader:)
    'enumerating'        — iteration methods (do:, collect:, select:, reject:)
    'converting'         — type conversion (asString, asDictionary, asJson)
    'comparing'          — equality and ordering (=, hash, <)
    'printing'           — printOn:, printString, displayString
    'initialization'     — initialize, constructor-like setup
    'private'            — internal implementation (not part of the public API)
    'error handling'     — exception handling and error recovery

For domain-specific protocols, use descriptive names that follow the same pattern:

    'sending'            — for actors: methods that send messages
    'querying'           — for database clients: query execution
    'parsing'            — for parsers: parsing entry points
    'serializing'        — for format libraries: serialization methods

---

## 3. Configuration Convention

**This is one of the most important conventions in the entire ecosystem.** If this is left as "up to you," the ecosystem will immediately fragment into libraries configured by JSON, YAML, XML, TOML, key-value files, environment variables, and a dozen other formats. Every application will need a different configuration parser for each dependency. This is the npm/crates failure mode applied to configuration.

### 3.1 The rule: configuration is Smalltalk

Smalltalk/A libraries are configured using Smalltalk — either direct message sends or a standard configuration DSL that is itself Smalltalk. No external configuration format is required for library-level configuration.

The rationale is simple: Smalltalk is already a readable, expressive language. A configuration DSL that reads naturally is just Smalltalk messages sent to a builder object. There is no reason to introduce a second language for configuration when the primary language is already excellent at expressing structured data.

### 3.2 The configuration pattern

Every configurable component accepts a configuration block that receives a builder object:

    HttpServer startWithConfig: [:config |
        config port: 8080.
        config host: '0.0.0.0'.
        config maxConnections: 10000.
        config readTimeout: (Duration seconds: 30).
        config tls: [:tls |
            tls certificate: '/path/to/cert.pem'.
            tls privateKey: '/path/to/key.pem']].

    DatabaseConnectionPool startWithConfig: [:config |
        config host: 'localhost'.
        config port: 5432.
        config database: 'myapp'.
        config poolSize: 20.
        config idleTimeout: (Duration minutes: 5)].

    Logger startWithConfig: [:config |
        config level: #info.
        config format: #structured.
        config output: Stdout].

This pattern is used consistently across every Tier 1 and Tier 2 library. A developer who has configured the HTTP server knows how to configure the database pool, the logger, the OTEL exporter, and every other component — because the pattern is identical.

### 3.3 Configuration builders

Each configurable component defines a configuration class (e.g., `HttpServerConfig`, `DatabasePoolConfig`) that serves as the builder. The builder validates settings eagerly — invalid configuration is caught at startup, not at first use.

    HttpServerConfig >> port: anInteger
        (anInteger between: 1 and: 65535)
            ifFalse: [ConfigurationError signal: 'Port must be between 1 and 65535'].
        port := anInteger.

The builder also provides sensible defaults for every setting. A minimal configuration should work out of the box:

    HttpServer startWithConfig: [:config |
        config port: 8080]

Everything else (host, timeouts, connection limits, TLS) has a reasonable default.

### 3.4 External configuration injection for deployment

While library configuration uses Smalltalk, headless deployment needs a way to inject values from the external environment — environment variables, command-line arguments, or a deployment configuration file.

The standard pattern bridges external sources into the Smalltalk configuration block:

    HttpServer startWithConfig: [:config |
        config port: (Environment at: 'PORT' ifAbsent: [8080]) asInteger.
        config host: (Environment at: 'HOST' ifAbsent: ['0.0.0.0']).
        config tls: [:tls |
            tls certificate: (Environment at: 'TLS_CERT').
            tls privateKey: (Environment at: 'TLS_KEY')]].

`Environment` is a capability-gated system class that reads environment variables. The configuration is still Smalltalk — the values just come from outside. This gives operators control over deployment-specific settings without requiring a separate configuration file format.

For more complex deployment configuration, a single application-level configuration file in a simple format (key-value or TOML, parsed once at startup) provides values that the Smalltalk configuration blocks consume. But the library-level configuration is always Smalltalk message sends — never "parse this YAML file and hope the keys match."

### 3.5 What this prevents

By standardizing on this pattern, the ecosystem avoids:

- Libraries that require YAML configuration files
- Libraries that require JSON configuration files
- Libraries that invent their own DSL
- Libraries where configuration keys are strings looked up at runtime with no validation
- Applications that need four different configuration parsers for four different dependencies
- The Spring Boot phenomenon where configuration is spread across five different file formats and three different annotation systems

---

## 4. Error Handling Conventions

### 4.1 The rule: use exceptions, not return codes

Smalltalk has a well-designed exception system. Use it. Do not return nil to indicate failure. Do not return error code integers. Do not return result-or-error wrapper objects. Signal an exception.

Good:

    connection executeQuery: sql
        "Signals DatabaseError if the query fails"
        ...

Bad:

    connection executeQuery: sql
        "Returns nil if the query fails"
        ...

    connection tryExecuteQuery: sql
        "Returns a Result object with #isSuccess / #error"
        ...

The caller handles errors using the standard exception protocol:

    [connection executeQuery: sql]
        on: DatabaseError
        do: [:err | self handleDatabaseFailure: err].

### 4.2 Exception class hierarchy

Every library defines its own exception hierarchy rooted at a library-specific error class:

    Error
        HttpError
            HttpConnectionError
            HttpTimeoutError
            HttpProtocolError
        DatabaseError
            DatabaseConnectionError
            DatabaseQueryError
            DatabaseConstraintError
        JsonError
            JsonParseError
            JsonGenerationError

This lets callers catch at whatever granularity they need — `on: HttpError do:` catches everything HTTP-related; `on: HttpTimeoutError do:` catches only timeouts.

### 4.3 Error messages

Exception error messages should be specific, actionable, and include relevant context:

Good:

    DatabaseConnectionError new
        messageText: 'Connection to PostgreSQL at localhost:5432 refused — is the server running?'

Bad:

    DatabaseError new
        messageText: 'connection error'

### 4.4 Actor failure and error handling

Inside an actor, an unhandled exception terminates the actor and notifies its supervisor. This is by design — the supervision tree handles recovery. Library code should not catch-all exceptions to prevent actor termination. Let the supervision tree do its job.

Libraries should catch and handle *expected* errors (network timeout, malformed input, constraint violation) and let *unexpected* errors (nil pointer, type mismatch, corrupted state) propagate to the supervisor for restart.

---

## 5. Actor Design Conventions

### 5.1 When to use an actor in a library

A library should use actors for:

- Long-lived stateful resources (connection pools, caches, registries)
- Concurrent I/O operations (each connection, each request)
- Supervised components that need fault isolation and restart

A library should NOT use actors for:

- Stateless transformations (JSON parsing, URL encoding, string manipulation)
- Pure computation (sorting, filtering, arithmetic)
- Short-lived utilities that don't need isolation or lifecycle management

**The test:** does this thing need its own heap, its own lifecycle, or its own fault boundary? If not, it's a plain object.

### 5.2 Actor API surface

Library actors should expose a clean message-based API. The messages should read as domain operations, not as actor plumbing:

Good:

    pool acquireConnection.
    connection executeQuery: sql with: params.
    cache at: key ifAbsent: [self computeExpensiveThing].

Bad:

    pool send: #acquireConnection.
    connection send: (Message selector: #executeQuery:with: arguments: {sql. params}).

The actor boundary should be invisible to the caller for synchronous-feeling operations. The caller sends a message, the actor processes it, the result comes back. The fact that it's an actor message rather than a direct method call is an implementation detail, not something the caller's code reflects.

### 5.3 Supervision conventions

Libraries that create actors should document their supervision expectations:

- What supervisor should own this actor?
- What happens on failure — restart, stop, or escalate?
- What state is lost on restart?
- How does the caller detect and recover from a supervised restart?

Connection pool example: if a `DatabaseConnection` actor crashes mid-query, the pool supervisor restarts it with a fresh connection. The caller's pending query receives a `DatabaseConnectionError`. The caller retries or propagates. The pool remains healthy.

---

## 6. Documentation Conventions

### 6.1 Class comments

Every class has a class comment that explains:

- What the class represents (one sentence)
- When to use it (typical use cases)
- How to create instances (the primary constructor pattern)
- Key collaborators (what other classes it works with)
- Example usage (a short, runnable code snippet)

Example:

    "An HttpRequest represents an incoming HTTP request received by the server.

    Instances are created by the HTTP server infrastructure and passed to
    handler actors. You do not typically create HttpRequest instances directly.

    Key collaborators:
        HttpResponse — the response object returned by handlers
        HttpServer — the server that received this request
        HttpRouter — routes requests to handler actors

    Example:
        handler := [:request |
            | name |
            name := request paramAt: 'name' ifAbsent: ['World'].
            HttpResponse ok: 'Hello, ', name]."

### 6.2 Method comments

Methods that are part of the public API should have a brief comment explaining what they do, what they return, and what exceptions they may signal:

    headerAt: aString
        "Answer the value of the header named aString.
         Signals HttpHeaderNotFound if the header is not present."

Private methods (protocol 'private') do not require comments unless their implementation is non-obvious.

### 6.3 Design rationale

For Tier 1 and Tier 2 libraries, each library should have a companion design document (in `docs/`) explaining:

- Why the library is structured this way
- What alternatives were considered and rejected
- How the library uses actors and why
- How error handling works and why
- How configuration works
- Performance characteristics and known limitations

This is not API documentation — it's the reasoning behind the design. When a community author builds a MySQL client, they should be able to read the PostgreSQL client's design document and follow the same patterns without guessing.

---

## 7. API Design Patterns

### 7.1 The cascade pattern for building objects

When an object requires multiple configuration steps, use cascades:

    HttpResponse new
        statusCode: 200;
        headerAt: 'Content-Type' put: 'application/json';
        body: jsonString;
        yourself.

This is idiomatic Smalltalk. Do not introduce builder classes for simple cases where cascades suffice.

### 7.2 The block parameter pattern for scoped resources

Resources that need cleanup (connections, file handles, transactions) should use block parameters that guarantee cleanup:

    pool withConnection: [:conn |
        conn executeQuery: 'SELECT * FROM users'].
    "connection is returned to the pool here, even if an exception occurred"

    File open: '/tmp/data.txt' do: [:stream |
        stream nextPutAll: 'hello'].
    "file is closed here, even if an exception occurred"

    connection transaction: [:tx |
        tx executeQuery: 'INSERT INTO users ...'.
        tx executeQuery: 'INSERT INTO audit_log ...'].
    "transaction is committed here, or rolled back if an exception occurred"

This is the Smalltalk equivalent of try-with-resources, RAII, or context managers. It is the standard pattern for resource management in Smalltalk/A. Libraries that require manual open/close without providing a block-based alternative are not following conventions.

### 7.3 The protocol pattern for polymorphism

When multiple implementations share a common interface (e.g., `DatabaseConnection` for PostgreSQL and SQLite), define the shared protocol as a set of messages that all implementations must support:

    "The database connection protocol:"
    executeQuery: sqlString                          — simple query
    executeQuery: sqlString with: paramArray          — parameterized query
    prepare: sqlString                                — prepared statement
    transaction: aBlock                               — scoped transaction
    close                                             — release the connection

Document this protocol explicitly. Test it with shared protocol tests that run against every implementation.

### 7.4 The factory pattern for hiding implementation choice

When the caller should not need to know which implementation they're using:

    Database connect: [:config |
        config adapter: #postgresql.
        config host: 'localhost'.
        config database: 'myapp']

The `Database` class selects the correct implementation based on the adapter setting. The caller's code works with any supported database. This is appropriate when the implementations share a common protocol (Section 7.3).

---

## 8. Dependency and Packaging Conventions

### 8.1 Minimize dependencies

A well-designed library depends on the class library and nothing else whenever possible. Each additional dependency is a coupling point, a version compatibility risk, and an installation step.

If a library needs functionality from another package, consider whether that functionality is small enough to inline rather than taking a dependency. A JSON parser does not need a "string utilities" package — it can include the three string methods it needs.

### 8.2 No micro-packages

The npm "left-pad" problem happens when the ecosystem normalizes tiny single-function packages. Smalltalk/A does not normalize this. A package should represent a coherent capability, not an individual function or class.

Bad package boundaries:

    smalltalk-a-string-utils       "too small — this belongs in the class library"
    smalltalk-a-url-encode         "too small — this belongs in the URL library"
    smalltalk-a-is-even            "this is a method on Integer, not a package"

Good package boundaries:

    smalltalk-a-postgresql         "coherent capability: PostgreSQL database access"
    smalltalk-a-aws-s3             "coherent capability: S3 storage operations"
    smalltalk-a-oauth              "coherent capability: OAuth authentication"

### 8.3 Package naming

Packages use lowercase with hyphens: `smalltalk-a-postgresql`, `smalltalk-a-redis`, `smalltalk-a-aws-s3`. The `smalltalk-a-` prefix is reserved for official packages maintained by the project. Community packages use their own prefix or no prefix.

---

## 9. Testing Conventions

### 9.1 Use SUnit

SUnit is the standard Smalltalk test framework and the ancestor of every xUnit framework in existence. It is ported as part of the base class library. All libraries use SUnit for testing. Do not introduce alternative test frameworks.

### 9.2 Test class naming

Test classes are named `[ClassName]Test` and live in the same package as the class they test:

    HttpRequestTest
    JsonParserTest
    DatabaseConnectionPoolTest

### 9.3 Test method naming

Test methods are named `test[Scenario]`:

    testParseValidJson
    testParseInvalidJsonSignalsError
    testConnectionPoolReturnsIdleConnection
    testConnectionPoolCreatesNewConnectionWhenEmpty
    testTimeoutSignalsHttpTimeoutError

Method names should describe the scenario and expected outcome, not just the method being tested.

### 9.4 Protocol tests for shared interfaces

When multiple implementations share a protocol (Section 7.3), define a shared test suite that runs against every implementation:

    DatabaseConnectionProtocolTest subclass: #PostgresqlConnectionTest
        "inherits all protocol tests, runs them against PostgreSQL"

    DatabaseConnectionProtocolTest subclass: #SqliteConnectionTest
        "inherits all protocol tests, runs them against SQLite"

This ensures that all implementations actually conform to the documented protocol.

---

## 10. Performance Conventions

### 10.1 Do not block scheduler threads

This is the single most important performance rule in Smalltalk/A. A library that blocks a scheduler thread — by doing synchronous I/O, by spinning in a loop waiting for a condition, by calling a C function that takes a long time — degrades the entire system.

All I/O must go through the async substrate. Long-running C primitives must go through the thread pool. If a library needs to wait for something, the actor suspends and resumes when the condition is met.

### 10.2 Do not allocate unnecessarily in hot paths

Object allocation is cheap but not free. In hot paths (HTTP request parsing, JSON parsing, database result processing), avoid creating temporary objects that are immediately discarded. Reuse buffers where possible. Use `ByteArray` for binary data rather than converting to `String` and back.

### 10.3 Benchmark before optimizing

Establish performance baselines for key operations (requests per second, parse throughput, query latency) and track them over time. Optimize based on measured data, not intuition. A well-structured idiomatic implementation is usually fast enough; micro-optimization that sacrifices readability requires measured justification.

---

## 11. What Not To Do — Anti-Patterns

### 11.1 Do not import conventions from other languages

- No `get`/`set` prefixes (Java)
- No snake_case method names (Python, Ruby)
- No method chaining that returns `self` for everything (JavaScript fluent APIs) — use Smalltalk cascades instead
- No static factory methods named `of()` or `from()` (Java) — use class-side constructors with descriptive names
- No checked vs unchecked exception distinctions (Java) — Smalltalk exceptions are all the same mechanism
- No null object pattern where nil would be clearer — but also do not return nil where an exception would be clearer

### 11.2 Do not reinvent standard protocols

If the class library defines `do:` for iteration, your collection-like class supports `do:`. If the class library defines `at:` and `at:put:` for keyed access, your dictionary-like class supports `at:` and `at:put:`. If the class library defines `printOn:` for string representation, your class implements `printOn:`.

Do not invent `forEach:` when `do:` exists. Do not invent `get:` when `at:` exists. Do not invent `toString` when `printString` exists.

### 11.3 Do not hide actor boundaries

If a library operation involves an actor message send (and therefore potential suspension, failure, or timeout), the API should make this clear through naming or documentation. A method named `data` that looks like a simple accessor but actually sends a cross-actor message and might timeout is a bug in the API design, not a feature.

### 11.4 Do not catch all exceptions

    [self doSomething]
        on: Error
        do: [:e | "swallow and continue"]

This hides bugs, defeats supervision, and makes debugging impossible. Catch specific exceptions that you know how to handle. Let everything else propagate.

### 11.5 Do not require configuration files

Libraries are configured with Smalltalk (Section 3). A library that requires the user to create a YAML file, a JSON file, or any other external configuration file to function is not following conventions. External files are an application-level concern for deployment, not a library-level requirement.

---

## 12. Review Checklist for Library Authors

Before publishing a Smalltalk/A package, verify:

- [ ] Class names are UpperCamelCase nouns with acronyms treated as words
- [ ] Method names follow Smalltalk keyword message conventions, no `get`/`set` prefixes
- [ ] Standard protocols are used where applicable (`do:`, `at:`, `printOn:`, etc.)
- [ ] Configuration uses the standard builder block pattern (Section 3.2)
- [ ] Errors are signaled as exceptions with specific, actionable messages
- [ ] Exception hierarchy is rooted at a library-specific error class
- [ ] Resources use block-scoped cleanup patterns (Section 7.2)
- [ ] No scheduler thread blocking — all I/O goes through the async substrate
- [ ] Actors are used only where isolation, lifecycle, or concurrency are needed
- [ ] Actor supervision expectations are documented
- [ ] Every public class has a class comment with usage examples
- [ ] SUnit tests cover the public API
- [ ] Protocol tests exist for any shared interface
- [ ] Dependencies are minimized — depend on the class library, not on utility micro-packages
- [ ] The package represents a coherent capability, not a single function
- [ ] No conventions imported from other languages (Java getters, Python snake_case, etc.)
- [ ] Design rationale is documented for non-obvious decisions
