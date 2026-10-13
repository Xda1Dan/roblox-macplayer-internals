// Illustrative reconstruction: Instance tree + DataModel root + services.
// Original code, MIT.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "reflection.hpp"

namespace rbx {

class DataModel; // root, defined in datamodel.hpp

class Instance : public DescribedBase,
                   public std::enable_shared_from_this<Instance> {
public:
    virtual ~Instance() = default;
    virtual const ClassDescriptor* classDescriptor() const = 0;

    const std::string& name() const { return name_; }
    void setName(std::string n) { name_ = std::move(n); }

    std::shared_ptr<Instance> parent() const { return parent_.lock(); }
    void addChild(std::shared_ptr<Instance> child) {
        child->parent_ = shared_from_this();
        children_.push_back(std::move(child));
    }
    // Only instances under Workspace participate in physics/prediction.
    bool isDescendantOf(const Instance* ancestor) const;

    // Reflection-backed dynamic dispatch (what Lua calls land on).
    Variant getProperty(const std::string& prop) const;
    void setProperty(const std::string& prop, Variant v);
    Tuple callMethod(const std::string& method, Tuple args);

protected:
    std::string name_;
    std::weak_ptr<Instance> parent_;
    std::vector<std::shared_ptr<Instance>> children_;
};

class ServiceProvider : public Instance {
public:
    // Services are children looked up by class name, e.g. "Workspace".
    template <typename T>
    std::shared_ptr<T> getService() {
        for (auto& c : children_)
            if (auto t = std::dynamic_pointer_cast<T>(c)) return t;
        return nullptr;
    }
    template <typename T>
    void addService(std::shared_ptr<T> svc) {
        addChild(std::static_pointer_cast<Instance>(svc));
    }
};

} // namespace rbx
