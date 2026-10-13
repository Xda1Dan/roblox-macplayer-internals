// Illustrative reconstruction of the observed RBX::Reflection descriptor
// pattern: every Instance class/method/property/event is a descriptor row,
// and script access dispatches through them. Original code, MIT.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rbx {

// Dynamically-typed value crossing the native<->script boundary.
// Observed bridge types: LuauBuffer, LuauInteger, LuauCollectionHandle.
struct Variant {
    enum class Type : uint8_t { Nil, Bool, Int, Double, String, Instance, Buffer };
    Type type = Type::Nil;
    union { bool b; int64_t i; double d; const void* inst; };
    std::string str; // heap side for String/Buffer payloads
};

using Tuple = std::vector<Variant>;

struct DescribedBase { // anything reflection can describe
    virtual ~DescribedBase() = default;
};

struct PropertyDescriptor {
    std::string name;
    std::function<Variant(const DescribedBase*)> get;
    std::function<void(DescribedBase*, Variant)> set; // may be empty: read-only
};

struct FunctionDescriptor {
    struct Arguments { Tuple positional; };
    std::string name;
    bool yields = false; // BoundYieldFuncDesc: suspends the calling coroutine
    std::function<Tuple(DescribedBase*, const Arguments&)> invoke;
};

struct EventDescriptor {
    std::string name;
    using Slot = std::function<void(const Tuple&)>;
    std::vector<Slot> connections; // Connect/Disconnect/Fire
    EventDescriptor() = default;
    EventDescriptor(std::string n) : name(std::move(n)) {}
};

struct EnumDescriptor {
    std::string name;
    std::unordered_map<std::string, int> items;
};

struct ClassDescriptor {
    std::string name;
    const ClassDescriptor* super = nullptr;
    std::vector<PropertyDescriptor> properties;
    std::vector<FunctionDescriptor> functions;
    std::vector<EventDescriptor> events;
};

class ReflectionRegistry {
public:
    static ReflectionRegistry& instance() {
        static ReflectionRegistry reg;
        return reg;
    }
    void registerClass(ClassDescriptor cls) { classes_[cls.name] = std::move(cls); }
    const ClassDescriptor* find(const std::string& name) const {
        auto it = classes_.find(name);
        return it == classes_.end() ? nullptr : &it->second;
    }
private:
    std::unordered_map<std::string, ClassDescriptor> classes_;
};

} // namespace rbx
