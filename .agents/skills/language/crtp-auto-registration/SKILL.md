---
name: crtp-auto-registration
description: Auto-register types into a singleton registry at static-init time using CRTP + inline static members — no manual wiring needed
---

# CRTP + Inline Static Auto-Registration

## Overview

Use **CRTP with `inline static` members** to automatically register derived
types into a singleton registry during static initialisation.  Each derived
class only needs to inherit from the CRTP base — no `register_command<T>()`
calls, no central list to maintain, no dispatch chains.

Adding a new type is one `.cpp` file.  Nothing else changes.

## The pattern

Three ingredients:

1. **A singleton registry** (Meyer's singleton — thread-safe, initialised on first use).
2. **An RAII registrar** whose constructor inserts an entry into the registry.
3. **A CRTP base** with an `inline static` registrar.  The compiler instantiates
   the static member when the derived class is defined, which runs the
   constructor and registers the type.

### 1. Registry singleton

```cpp
class Registry final {
  public:
    static auto instance() -> Registry & {
        static Registry registry;   // Meyer's singleton
        return registry;
    }

    auto add(std::unique_ptr<Interface> entry) -> void {
        auto const key = entry->name();
        entries_.emplace(key, std::move(entry));
    }

    [[nodiscard]] auto find(std::string_view const key) const -> Interface const * {
        auto const it = entries_.find(key);
        return (it != entries_.end()) ? it->second.get() : nullptr;
    }

  private:
    Registry() = default;
    std::map<std::string, std::unique_ptr<Interface>, std::less<>> entries_;
};
```

### 2. RAII registrar

```cpp
struct Registrar {
    explicit Registrar(std::unique_ptr<Interface> entry) {
        Registry::instance().add(std::move(entry));
    }
};
```

### 3. CRTP base

```cpp
template <typename Derived>
class AutoRegistered : public Interface {
  protected:
    // Pass-through to Interface constructor.
    using Interface::Interface;

  private:
    // This is the key: inline static — one instance per template instantiation,
    // initialised during static-init.  The lambda creates a Derived and hands
    // it to the Registrar, which inserts it into the Registry.
    inline static Registrar registrar_{std::make_unique<Derived>()};
};
```

## Usage

Each derived type lives in its own `.cpp` file.  No other file changes:

```cpp
// alpha.cpp
class Alpha final : public AutoRegistered<Alpha> {
  public:
    Alpha() : AutoRegistered("alpha") {}
    auto execute() const -> int override { return 0; }
};
```

```cpp
// beta.cpp
class Beta final : public AutoRegistered<Beta> {
  public:
    Beta() : AutoRegistered("beta") {}
    auto execute() const -> int override { return 0; }
};
```

That's it.  `Alpha` and `Beta` are registered and discoverable via
`Registry::instance().find("alpha")` and `Registry::instance().find("beta")`.

## How it works

1. The compiler sees `class Alpha final : public AutoRegistered<Alpha>`.
2. It instantiates `AutoRegistered<Alpha>`, including the `inline static`
   member `registrar_`.
3. At static-initialisation time, `registrar_`'s initialiser runs:
   `std::make_unique<Alpha>()` → `Registrar{...}` → `Registry::instance().add(...)`.
4. `Registry::instance()` is a function-local static — guaranteed to be
   constructed before first use, even across translation units.
5. By the time `main()` runs, all `AutoRegistered<T>` instantiations have
   registered themselves.

## Why `inline static` specifically

- `inline` — allows the definition in a header without ODR violations.
- `static` — one per template instantiation (one per derived class), not
  shared across all `AutoRegistered<T>` instantiations.
- Together they produce exactly one registrar per derived type, constructed
  during static init, regardless of how many translation units include the
  header.

Without `inline`, C++17 would require an out-of-line definition in a `.cpp`
for each template instantiation — defeating the zero-boilerplate goal.

## When to use

- Plugin / command / handler registration systems.
- Any pattern where you have a fixed interface, multiple implementations in
  separate `.cpp` files, and you want them discoverable at runtime without
  a manual registry call.
- Replaces: manual `registry.add(...)` chains, `if-else` dispatch, or
  hardcoded lists of subtypes.

## When NOT to use

- When derived types are created dynamically based on runtime data (use a
  factory function map instead).
- When the set of subtypes is small and fixed — a simple `std::variant` or
  handwritten dispatch may be clearer.
- When static-init ordering across shared libraries is a concern — prefer
  explicit initialisation in that case.

## References

- [CRTP (cppreference)](https://en.cppreference.com/w/cpp/language/crtp)
- [inline specifier (cppreference)](https://en.cppreference.com/w/cpp/language/inline)
- [Static local variables — Meyer's Singleton](https://en.cppreference.com/w/cpp/language/storage_duration#Static_local_variables)
