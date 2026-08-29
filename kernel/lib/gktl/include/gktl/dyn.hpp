#ifndef GKTL_DYN_HPP
#define GKTL_DYN_HPP

#include <memory>
#include <meta>
#include <utility>
#include <vector>

namespace gktl {

template <typename T>
class Dyn {
  private:
    struct Vtable;

    // 1. 编译期动态定义 Vtable 结构体 (同前)
    consteval {
        std::vector<std::meta::data_member_spec> vtable_members;
        auto members = std::meta::members_of(^^T, std::meta::access_context::current());

        for (auto member: members) {
            if (std::meta::is_function(member) && !std::meta::is_constructor(member) &&
                !std::meta::is_destructor(member) && !std::meta::is_static(member)) {

                auto ret_type = std::meta::return_type_of(member);
                auto params = std::meta::parameters_of(member);
                std::vector<std::meta::info> erased_params;

                if (std::meta::is_const_qualified(member)) {
                    erased_params.push_back(std::meta::type_of(^^void const *));
                } else {
                    erased_params.push_back(std::meta::type_of(^^void *));
                }

                for (auto param: params) {
                    erased_params.push_back(std::meta::type_of(param));
                }

                auto fn_ptr_type = std::meta::make_pointer_type(
                    std::meta::make_function_type(ret_type, erased_params));
                vtable_members.push_back(
                    std::meta::data_member_spec(fn_ptr_type, std::meta::name_of(member)));
            }
        }
        vtable_members.push_back(std::meta::data_member_spec(
            std::meta::make_pointer_type(std::meta::make_function_type(^^void,
                                                                       {
                                                                           ^^void *})),
            "__destroy"));

        std::meta::define_aggregate(^^Vtable, vtable_members);
    }

    // 2. 核心：动态绑定工厂，为具体的类型 U 自动生成一个 Vtable 实例
    template <typename U>
    consteval static auto make_vtable() -> Vtable * {
        static Vtable VTABLE;

        // 利用 C++26 的元编程循环，遍历 Interface 里的所有函数成员，完成运行时 Lambda 绑定
        // 提示：GCC 16 扩展允许通过 [:member_info:] 动态访问聚合体字段
        template inline for (auto member: std::meta::members_of(^^T)) {
            if (std::meta::is_function(member) && !std::meta::is_constructor(member) &&
                !std::meta::is_destructor(member) && !std::meta::is_static(member)) {

                // 将 U::member_name 包装成强类型的 void* 擦除回调并绑定到 vt 对应的字段上
                // 基于 P2996 提案的字段动态展开赋值语法：
                VTABLE.[:member:] = [](void *self, auto &&...args) {
                    // 内部自动还原类型并调用对应的业务函数
                    return (static_cast<U *>(self)->*[:member:])(
                        std::forward<decltype(args)>(args)...);
                };
            }
        }

        // 自动绑定析构函数
        VTABLE.__destroy = [](void *self) { delete static_cast<U *>(self); };

        return &VTABLE;
    }

    Vtable const *m_vtable;
    void *m_instance;

  public:
    // 4. 构造函数：当你传入任意对象时，全自动触发多态绑定
    template <typename U>
    Dyn(U &obj) : m_instance(std::addressof(obj)), m_vtable(make_vtable<U>()) {}

    ~Dyn() {
        if (m_instance && m_vtable) {
            m_vtable->__destroy(m_instance);
        }
    }

    auto operator->() const -> Vtable const * {
        return m_vtable;
    }
};
} // namespace gktl

#endif // #ifndef GKTL_DYN_HPP