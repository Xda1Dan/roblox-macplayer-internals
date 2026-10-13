// Demo: descriptor registration + property get/set + method call + event,
// in the observed RBX::Reflection style. Build: make reflection_demo
#include <cstdio>
#include "rbx/reflection.hpp"
#include "rbx/instance.hpp"

namespace rbx {
// Minimal concrete Instance for the demo.
class Part : public Instance {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }
    double size = 1.0;
};
const ClassDescriptor Part::kDescriptor = {
    "Part", nullptr,
    { { "Size",
        [](const DescribedBase* b) {
            Variant v; v.type = Variant::Type::Double;
            v.d = static_cast<const Part*>(b)->size; return v;
        },
        [](DescribedBase* b, Variant v) {
            static_cast<Part*>(b)->size = v.d;
        } } },
    { { "Grow", false,
        [](DescribedBase* b, const FunctionDescriptor::Arguments& a) {
            auto* p = static_cast<Part*>(b);
            p->size += a.positional.empty() ? 1.0 : a.positional[0].d;
            return Tuple{};
        } } },
    { { "Touched" } },
};
const ClassDescriptor* PartDescInit = (ReflectionRegistry::instance().registerClass(Part::kDescriptor), &Part::kDescriptor);
} // namespace rbx

// Instance dynamic dispatch implemented for the demo.
namespace rbx {
bool Instance::isDescendantOf(const Instance* a) const {
    for (auto p = parent(); p; p = p->parent()) if (p.get() == a) return true;
    return false;
}
Variant Instance::getProperty(const std::string& prop) const {
    for (auto d = classDescriptor(); d; d = d->super)
        for (auto& pd : d->properties)
            if (pd.name == prop && pd.get) return pd.get(this);
    return {};
}
void Instance::setProperty(const std::string& prop, Variant v) {
    for (auto d = classDescriptor(); d; d = d->super)
        for (auto& pd : d->properties)
            if (pd.name == prop && pd.set) { pd.set(this, std::move(v)); return; }
}
Tuple Instance::callMethod(const std::string& m, Tuple args) {
    for (auto d = classDescriptor(); d; d = d->super)
        for (auto& f : d->functions)
            if (f.name == m) return f.invoke(this, {std::move(args)});
    return {};
}
} // namespace rbx

int main() {
    auto part = std::make_shared<rbx::Part>();
    part->setName("Baseplate");
    rbx::Variant v; v.type = rbx::Variant::Type::Double; v.d = 4.0;
    part->setProperty("Size", v);
    rbx::Tuple args; rbx::Variant one; one.type = rbx::Variant::Type::Double; one.d = 2.0;
    args.push_back(one);
    part->callMethod("Grow", std::move(args));
    // connect a listener, like a Lua :Connect()
    auto desc = rbx::ReflectionRegistry::instance().find("Part");
    const_cast<rbx::EventDescriptor&>(desc->events[0]).connections.push_back(
        [](const rbx::Tuple&) { std::puts("Touched fired"); });
    const_cast<rbx::EventDescriptor&>(desc->events[0]).connections[0]({});
    std::printf("Part.Size = %f\n", part->getProperty("Size").d);
    return 0;
}
