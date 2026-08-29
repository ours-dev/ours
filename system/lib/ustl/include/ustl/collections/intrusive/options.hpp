/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_OPTIONS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_OPTIONS_HPP 1

#include <ustl/collections/intrusive/intrusive_fwd.hpp>
#include <ustl/collections/intrusive/link_mode.hpp>
#include <ustl/collections/intrusive/pack_options.hpp>

namespace ustl::collections::intrusive {
struct Empty {};

template <typename Functor>
struct Fhtraits;

template <typename T, typename Hook, Hook T::*PtrMut>
struct Mhtraits;

struct DftTag;
struct MemberTag;

template <typename SupposedValueTraits>
struct IsDefaultHookTag;

template <bool Enabled>
struct ConstantTimeSize final {
    template <typename Base>
    struct Pack: Base {
        static constexpr bool CONSTANT_TIME_SIZE = Enabled;
    };
};

template <typename T>
struct HeaderHolderType final {
    template <typename Base>
    struct Pack: Base {
        using HeaderHolderType = T;
    };
};

template <typename T>
struct Size final {
    template <typename Base>
    struct Pack: Base {
        using Size = T;
    };
};

template <typename T>
struct Compare final {
    template <typename Base>
    struct Pack: Base {
        using Compare = T;
    };
};

template <typename T>
struct KeyOfValue final {
    template <typename Base>
    struct Pack: Base {
        using KeyOfValue = T;
    };
};

template <typename T>
struct PriorityOfValue final {
    template <typename Base>
    struct Pack: Base {
        using PriorityOfValue = T;
    };
};

template <bool Enabled>
struct FloatingPoint final {
    template <typename Base>
    struct Pack: Base {
        static constexpr bool FLOATING_POINT = Enabled;
    };
};

template <typename T>
struct Equal final {
    template <typename Base>
    struct Pack: Base {
        using Equal = T;
    };
};

template <typename T>
struct Priority final {
    template <typename Base>
    struct Pack: Base {
        using Priority = T;
    };
};

template <typename T>
struct Hash final {
    template <typename Base>
    struct Pack: Base {
        using Hash = T;
    };
};

template <typename T>
struct ValueTraits final {
    template <typename Base>
    struct Pack: Base {
        using ProtoValueTraits = T;
    };
};

template <typename Parent, typename TMemberHook, TMemberHook Parent::*PtrMut>
struct MemberHook final {
    using MemberValueTraits = Mhtraits<Parent, TMemberHook, PtrMut>;

    template <typename Base>
    struct Pack: Base {
        using ProtoValueTraits = MemberValueTraits;
    };
};

template <typename T>
struct FunctionHook final {
    template <typename Base>
    struct Pack: Base {
        using ProtoValueTraits = Fhtraits<T>;
    };
};

template <typename T>
struct BaseHook final {
    template <typename Base>
    struct Pack: Base {
        using ProtoValueTraits = T;
    };
};

template <typename T>
struct VoidPointer final {
    template <typename Base>
    struct Pack: Base {
        using VoidPointer = T;
    };
};

template <typename T>
struct Tag final {
    template <typename Base>
    struct Pack: Base {
        using Tag = T;
    };
};

template <bool Enabled>
struct OptimizeSize final {
    template <typename Base>
    struct Pack: Base {
        static constexpr bool OPTIMIZE_SIZE = Enabled;
    };
};

template <bool Enabled>
struct Linear final {
    template <typename Base>
    struct Pack: Base {
        static constexpr bool LINEAR = Enabled;
    };
};

template <bool Enabled>
struct CacheLast final {
    template <typename Base>
    struct Pack: Base {
        static constexpr bool CACHE_LAST = Enabled;
    };
};

template <typename T>
struct BucketTraits final {
    template <typename Base>
    struct Pack: Base {
        using BucketTraits = T;
    };
};

template <bool Enabled>
struct StoreHash final {
    template <typename Base>
    struct Pack: Base {
        static constexpr bool STORE_HASH = Enabled;
    };
};

template <bool Enabled>
struct OptimizeMultikey final {
    template <typename Base>
    struct Pack: Base {
        static constexpr bool OPTIMIZE_MULTIKEY = Enabled;
    };
};

template <bool Enabled>
struct Power2Buckets final {
    template <typename Base>
    struct Pack: Base {
        static constexpr bool POWER_2_BUCKETS = Enabled;
    };
};

template <bool Enabled>
struct FastmodBuckets final {
    template <typename Base>
    struct Pack: Base {
        static constexpr bool FASTMOD_BUCKETS = Enabled;
    };
};

template <bool Enabled>
struct CacheBegin final {
    template <typename Base>
    struct Pack: Base {
        static constexpr bool CACHE_BEGIN = Enabled;
    };
};

template <bool Enabled>
struct CompareHash final {
    template <typename Base>
    struct Pack: Base {
        static constexpr bool COMPARE_HASH = Enabled;
    };
};

template <bool Enabled>
struct Incremental final {
    template <typename Base>
    struct Pack: Base {
        static constexpr bool INCREMENTAL = Enabled;
    };
};

template <bool Enabled>
struct LinearBuckets final {
    template <typename Base>
    struct Pack: Base {
        static constexpr bool LINEAR_BUCKETS = Enabled;
    };
};

struct HookDefaults {
    using VoidPointer = void *;
    static constexpr LinkModeType LINK_MODE = LinkModeType::SafeLink;
    using Tag = DftTag;
    static constexpr bool OPTIMIZE_SIZE = false;
    static constexpr bool STORE_HASH = false;
    static constexpr bool LINEAR = false;
    static constexpr bool OPTIMIZE_MULTIKEY = false;
};
} // namespace ustl::collections::intrusive

#ifndef USTL_DECLARE_HOOK_OPTION
#define USTL_DECLARE_HOOK_OPTION(OWNER, MEMBER, ALIAS)                                             \
    using ALIAS =                                                                                  \
        ::ustl::collections::intrusive::MemberHook<OWNER, decltype(OWNER::MEMBER), &OWNER::MEMBER>
#endif

#endif // USTL_COLLECTIONS_INTRUSIVE_OPTIONS_HPP
