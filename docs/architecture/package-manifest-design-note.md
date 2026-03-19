# Design Note: Package Manifests and Namespace Strategy

*March 2026 — design direction, not yet implemented*

> **Status**
>
> This document captures the design direction for Smalltalk/A's package
> system (STP) manifest format and namespace strategy. Nothing here is
> implemented yet. It records decisions made during architectural
> brainstorming so they are available when package system work begins
> (estimated Phase 4+).

---

## 1. The Problem

Smalltalk has a flat global namespace. Every class lives in one global
dictionary. If two packages define a class with the same name, the second
overwrites the first. Classical Smalltalk dealt with this through naming
conventions — prefix class names with a short tag (Zn, NS, UI). This
works but is ugly, unenforced, and produces names like
`NSMutableURLRequest`.

As the Smalltalk/A ecosystem grows, name collisions between community
packages become inevitable. The design must accommodate this without
changing the Smalltalk language.

## 2. The Principle

**Namespaces are a package concern, not a language concern.**

No new keywords are added to Smalltalk. No `import` statement. No
`namespace` declaration in source code. Inside a `.st` file, the
developer writes standard Smalltalk — identical to today.

The namespace is declared in the package manifest — a metadata file
that lives alongside the `.st` source files. The package loader reads
the manifest at load time and resolves names accordingly.

This follows the same philosophy as Python (directory structure is the
namespace), Erlang (module names are atoms resolved at compile time),
and Go (package path determines namespace). The language syntax is
unaffected. The organizational machinery lives outside the language.

## 3. The Package Manifest

Each distributable package includes a manifest file (`package.sta`)
alongside its `.st` source files.

### 3.1 Repository layout

    smalltalk-a-http/
        package.sta
        HttpServer.st
        HttpRequest.st
        HttpResponse.st
        HttpRouter.st
        tests/
            HttpServerTest.st
            HttpRequestTest.st

### 3.2 Manifest format

    name: 'smalltalk-a-http'
    namespace: 'Http'
    version: '0.1.0'
    provides:
        HttpServer
        HttpRequest
        HttpResponse
        HttpRouter
    requires:
        smalltalk-a-core >= 0.1.0
        smalltalk-a-io >= 0.1.0

The manifest declares:

- **name** — the package identity for dependency resolution
- **namespace** — the logical grouping for collision avoidance
- **version** — semantic version for dependency management
- **provides** — which classes this package defines
- **requires** — which packages must be loaded first, with version constraints

### 3.3 What the manifest is not

The manifest is not stored as an object in the image. It is consumed
at load time by the package loader. Its information becomes part of the
class table and package registry. The manifest is a build/load artifact,
not a runtime artifact.

## 4. Name Resolution

### 4.1 Inside a package

Code within a package refers to its own classes by their short names.
`HttpServer.st` just says `HttpRequest new` — standard Smalltalk. The
loader knows that `HttpRequest` resolves to the class provided by the
same package.

### 4.2 Across packages (no collision)

When there is no name collision, classes from other packages are
available by their short names. If your application uses
`smalltalk-a-http` and no other package defines `HttpServer`, you just
write `HttpServer new`.

### 4.3 Across packages (collision)

When two packages define a class with the same short name, the
application's manifest resolves the ambiguity:

    name: 'myapp'
    version: '1.0.0'
    requires:
        smalltalk-a-logging >= 0.1.0
        smalltalk-a-audit >= 0.1.0
    aliases:
        Logger: smalltalk-a-logging
        AuditLogger: smalltalk-a-audit.Logger

The `aliases` section tells the loader which `Logger` to use by default
and provides an alternative name for the other. Application code uses
`Logger` and `AuditLogger` — standard Smalltalk class names, no special
syntax.

### 4.4 The global dictionary still exists

For development, experimentation, and backward compatibility, classes
can still be filed into the global namespace with no manifest. A loose
`.st` file with no package association goes directly into the global
`Smalltalk` dictionary, exactly as it does today. Manifests are required
only for distributable packages, not for application development.

## 5. Build Process

The package manifest drives deterministic image construction:

    sta_build --from myapp/package.sta --output myapp.image

The build tool:

1. Reads the application's manifest
2. Transitively resolves all dependency packages and their versions
3. Verifies no unresolved name collisions
4. Bootstraps a clean image from the VM
5. Loads packages in dependency order (each package's `.st` files filed in)
6. Saves the resulting image

This produces a reproducible image from declared inputs — no hidden
state, no dependence on a hand-maintained development image.

For container deployment:

    FROM ubuntu:24.04
    COPY sta_vm /usr/local/bin/
    COPY packages/ /build/packages/
    RUN sta_build --from /build/packages/myapp/package.sta \
                  --output /app/myapp.image
    ENTRYPOINT ["sta_vm", "--headless", "/app/myapp.image"]

## 6. IDE Integration

The system browser's primary navigation remains class → protocol →
method. This does not change. Packages add an optional higher-level
navigation:

    [Packages]              [Classes]           [Methods]
    smalltalk-a-core        HttpServer          startWithConfig:
    smalltalk-a-http    →   HttpRequest     →   headerAt:
    smalltalk-a-postgresql  HttpResponse        statusCode
    myapp                   HttpRouter          route:to:

Clicking a package filters the class list to that package's provided
classes. The flat all-classes view remains available for developers
who prefer it. Package manifests are viewable and editable from the
IDE as text files.

## 7. Class Extensions and Package Ownership

### 7.1 The problem

Smalltalk's open class model lets any code add methods to any class.
A developer can open String in the browser and add a `toCamelCase`
method. This is a core Smalltalk strength — it enables expressive
APIs, domain-specific convenience methods, and library extensions
without subclassing.

But when source files are the system of record, a question arises:
which file does that new method belong to? It can't go into the base
class library's `String.st` — the developer doesn't own that file.
It needs to go into the developer's project, recorded as an
**extension** to a class owned by another package.

### 7.2 Package ownership model

Every class and every method in the image has an **owner** — the
package that defined it. This ownership is recorded as metadata in
the image when packages are loaded.

- `String` is owned by `smalltalk-a-core`
- `HttpServer` is owned by `smalltalk-a-http`
- `CartService` is owned by `myapp`

Individual methods can have different owners than their class. The
class `String` is owned by `smalltalk-a-core`, and most of its
methods are too. But if `myapp` adds a `toCamelCase` method to
String, that method is owned by `myapp` — not by `smalltalk-a-core`.

This per-method ownership is what makes extensions work. The class
has a home package. Methods can come from any package. Each method
knows where it came from.

This model is proven — Pharo's Monticello uses identical per-method
package tagging and it works at scale.

### 7.3 Extension file layout

When a project adds methods to a class it doesn't own, those methods
are stored in an `extensions/` directory within the project:

    myapp/
        package.stp
        CartService.st          ← class owned by myapp
        OrderProcessor.st       ← class owned by myapp
        extensions/
            String.st           ← only myapp's added methods
            RedisClient.st      ← only myapp's added methods

The extension file contains only the methods the project added — not
the entire class. When the project is loaded, the loader files in
the extension methods onto the existing classes.

### 7.4 How extensions compose at build time

When building a deployment image, packages are loaded in dependency
order. Each package's extensions are applied after the target class
is loaded:

1. Load `smalltalk-a-core` — String gets its base methods
2. Load `smalltalk-a-redis` — RedisClient is created
3. Load `myapp` — CartService is created, then extensions are
   applied: `String >> toCamelCase` and `RedisClient >> fetchJson:`
   are installed on those classes

The result is a String class that has all its base methods plus the
application's extensions. The deployment image contains everything
needed. The extension mechanism is purely a source organization
concern — at runtime, there is no difference between a base method
and an extension method.

### 7.5 Conflict detection

If two packages both add a method with the same selector to the same
class, the package loader detects the conflict at load time and
raises an error. The developer resolves it by removing one of the
conflicting extensions or by choosing which one wins in the
application manifest.

Silent overwriting of methods from other packages is never the
default behavior. Conflicts must be explicit.

---

## 8. FileSyncActor and Package-Aware Routing

### 8.1 How the FileSyncActor routes changes

The FileSyncActor is responsible for bidirectional sync between the
live image and source files on disk. When a method changes in the
live image (via the browser or REPL), the FileSyncActor must decide
which file to write the change to.

The routing logic uses the package ownership metadata:

1. A method is added or modified on a class in the live image
2. The FileSyncActor queries the package registry:
   - Who owns this class?
   - What is the current working package (the developer's project)?
3. If the class owner matches the working package: write the method
   to the class's `.st` file in the project directory
4. If the class owner is a different package: write the method to
   `extensions/<ClassName>.st` in the project directory

This is entirely transparent to the developer. They add a method to
String in the browser. The FileSyncActor knows String is owned by
`smalltalk-a-core` and the working package is `myapp`, so it writes
the method to `myapp/extensions/String.st`. The developer doesn't
make this decision — the system handles it automatically.

### 8.2 The working package concept

The IDE maintains a "current working package" — the package the
developer is actively working in. This is typically the application
being developed. It's visible in the IDE as a status indicator or
package selector.

Any new class created in the browser belongs to the working package.
Any method added to a class owned by another package becomes an
extension in the working package. This is how the FileSyncActor
knows where to route changes without the developer thinking about it.

In the Emacs workflow, the working package is implicit — the
developer is editing files in their project directory. If they
create `extensions/String.st` in their project, that's an extension.
The directory structure makes ownership explicit.

### 8.3 Reverse sync (file to image)

When the FileSyncActor detects a file change on disk (the developer
edited a file in Emacs), it files in the changes and records the
ownership:

- A change to `myapp/CartService.st` installs methods on CartService
  owned by `myapp`
- A change to `myapp/extensions/String.st` installs methods on String
  owned by `myapp`

The ownership tag is determined by which project directory the file
lives in, not by which class the methods are installed on.

### 8.4 What the FileSyncActor needs from the runtime

The FileSyncActor requires:

- **Package registry access** — the mapping of classes and methods to
  their owning packages. This is populated by the package loader when
  packages are loaded into the image.
- **Method change notifications** — the runtime notifies the
  FileSyncActor when a method is installed, modified, or removed.
  This is a natural extension of the `addSelector:withMethod:`
  primitive (prim 69) — after installing the method, notify
  registered observers.
- **Working package context** — the IDE or environment tells the
  FileSyncActor which package the developer is working in.

None of these require changes to the Smalltalk language or the
bytecode interpreter. They are system-level services built on top
of existing primitives and the actor model.

---

## 9. Standard Library Packaging Model

### 9.1 Three tiers of code

Smalltalk/A organizes code into three tiers with different
distribution characteristics:

**Base class library** — collections, numbers, strings, streams,
exceptions, the compiler. Loaded during bootstrap. Always present
in every image. Lives in `kernel/` in the VM repo. No manifest
needed — these are the language, not packages.

**Standard library** — HTTP server, HTTP client, JSON, TLS, URL
parsing, file I/O, structured logging, configuration. Ships with
the VM and is always available. Organized internally as STP packages
with manifests for structural validation, but loaded automatically
as part of building a standard image. The developer never installs
these — they are just there. Lives in `src/stp/` in the VM repo.

**STP packages** — PostgreSQL, WebSocket, OTEL, AWS SDK, Redis,
OAuth, XML, HTML templating. Distributed separately and installed
explicitly. Not every application needs them. Lives in separate
repos or a package registry.

### 9.2 Standard library layout in the VM repo

    src/stp/
        core/
            package.stp
            Object.st
            Collection.st
            OrderedCollection.st
            ...
        http/
            package.stp
            HttpServer.st
            HttpRequest.st
            HttpResponse.st
            HttpRouter.st
        json/
            package.stp
            JsonParser.st
            JsonGenerator.st
        tls/
            package.stp
            TlsSocket.st
            TlsCertificate.st
        ...

Each directory is a self-contained package with its own manifest.
The build process loads them in dependency order after bootstrap.

### 9.3 Why structure the standard library as packages

Even though the standard library ships built-in and the developer
never installs it explicitly, structuring it as proper STP packages
serves several purposes:

- **Dogfooding.** The manifest format and package loader are
  validated against real packages before any third-party author
  uses them.
- **Organization.** Clear boundaries between HTTP, JSON, TLS,
  and other subsystems. Each has explicit dependencies.
- **Extension points.** A developer can extend standard library
  classes and the FileSyncActor routes extensions correctly
  because the ownership metadata exists.
- **Future flexibility.** If a standard library package is ever
  extracted to a separate repo (e.g., to allow independent
  release cadence), it's already correctly structured.

### 9.4 The developer experience

From the developer's perspective, the three tiers are invisible
during normal work. They start a new image. Collections, HTTP,
JSON, and everything else are already there. They install a
PostgreSQL package because their app needs it. They write their
application code. The tier boundaries matter only for distribution
and build tooling — not for writing code.

This follows the Ruby model: `Net::HTTP` and `JSON` are just there.
`pg` and `redis` are gems you install. The developer doesn't think
about the boundary until they need something that isn't built in.

---

## 10. Phasing

- **Phase 1–2:** No manifests. Kernel `.st` files load directly into
  the global namespace. Development uses the Emacs workflow with manual
  file-in.

- **Phase 3 (IDE):** The browser gains a package-level navigation view.
  No manifest enforcement yet — the view is populated from whatever
  organizational metadata exists.

- **Phase 4 (Capability substrate / STP):** The manifest format is
  formalized. The package loader reads manifests and resolves
  dependencies. Namespace collision detection is implemented.
  The `sta_build` tool is created.

- **Phase 5+:** Package registry, signed packages, version resolution,
  and the full STP distribution system.

## 11. Design Constraints

- **No language changes.** The Smalltalk syntax is untouched. No new
  keywords, no import statements, no namespace declarations in code.
- **Additive, not required.** A developer can write Smalltalk with
  no manifest and everything works. Manifests are required only for
  distributable packages.
- **Load-time, not runtime.** Namespace resolution happens when
  packages are loaded. It does not affect message dispatch at runtime.
  Once classes are in the image, they behave identically regardless
  of how they were loaded.
- **One manifest per package, not per file.** Individual `.st` files
  do not have manifests. A package is a collection of related classes
  with one manifest.

## 12. Open Questions

- **Manifest file format:** the examples above use a simple key-value
  format. Should this be Smalltalk instead (consistent with the
  configuration convention from the design conventions doc)?
- **Manifest file extension:** `.stp` is the working proposal for the
  manifest filename (`package.stp`). This aligns with the STP package
  naming but hasn't been finalized.
- **Namespace depth:** is one level of namespace sufficient
  (`Http.Request`) or should deeper nesting be supported
  (`AWS.S3.Client`)? Deeper nesting adds complexity; one level may
  be enough.
- **Class renaming on load:** if a collision alias changes a class
  name, does this affect `class name` and `printString`? Probably
  yes — the alias becomes the canonical name in the image.
- **Cuis compatibility:** do Cuis-derived kernel classes need a
  namespace? Probably not — the base class library is always loaded
  and its names are canonical.
- **Extension conflict resolution:** beyond detecting conflicts, should
  the manifest support declaring a winner? E.g.,
  `prefer: smalltalk-a-logging.Logger >> format:` to explicitly
  choose which package's extension wins for a specific method.
- **Extension visibility in the browser:** how should the IDE visually
  distinguish extension methods from base methods? A subtle icon or
  color tag is the likely approach — enough to inform, not enough to
  distract.
- **Package ownership storage:** is per-method ownership stored in the
  CompiledMethod header (a package index field) or in a separate
  side table? The header approach is compact but adds a field to
  every method. The side table approach is zero-overhead for methods
  that belong to their class's home package.
- **Base class library vs standard library boundary:** which classes
  belong in bootstrap (`kernel/`) vs standard library (`src/stp/`)?
  The rule of thumb is: if the compiler or interpreter needs it to
  function, it's base. If it's needed for practical development but
  not for the VM itself, it's standard library.
