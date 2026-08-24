
struct annotate {
    char const* nm;
};

struct entry {
    char const *nm;
    void (*fn)();
};

template <std::meta::info R>
inline constinit entry registry_entry = ;

consteval {
    // 1. Get all members in this namespace, which marked with `annotate`
    template for (auto constexpr r : std::meta::members_of(^^::)) {
        // 2. For each member, instaniate a registry_entry.
        template registry_entry<r>;
    }
}

[[=annotate()]]
auto f() - > void;