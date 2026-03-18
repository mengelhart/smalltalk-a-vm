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

## 7. Phasing

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

## 8. Design Constraints

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

## 9. Open Questions

- **Manifest file format:** the examples above use a simple key-value
  format. Should this be Smalltalk instead (consistent with the
  configuration convention from the design conventions doc)?
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
