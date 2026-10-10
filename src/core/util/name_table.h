// GL object name management (glGen*/glDelete*/glIs* and bind-time creation).
// A name is "generated" once glGen* returns it; the object itself may be
// created later, on first bind. Callers serialise access (the share group
// lock for shared objects; container objects are per context).
#pragma once

#include <unordered_map>

#include "core/gl/types.h"
#include "core/util/ref.h"

namespace wmgl {

template <class T>
class NameTable {
public:
    void generate(GLsizei count, GLuint* names) {
        for (GLsizei i = 0; i < count; ++i) {
            while (entries_.count(next_) || next_ == 0) ++next_;
            entries_.emplace(next_, RefPtr<T>());
            names[i] = next_++;
        }
    }
    // Reserve a name chosen by the application (compatibility profile binds).
    void reserve(GLuint name) { if (name) entries_.emplace(name, RefPtr<T>()); }
    bool isGenerated(GLuint name) const { return name && entries_.count(name); }
    bool exists(GLuint name) const {
        auto it = entries_.find(name);
        return it != entries_.end() && it->second;
    }
    T* get(GLuint name) const {
        auto it = entries_.find(name);
        return it == entries_.end() ? nullptr : it->second.get();
    }
    RefPtr<T> getRef(GLuint name) const {
        auto it = entries_.find(name);
        return it == entries_.end() ? RefPtr<T>() : it->second;
    }
    void set(GLuint name, RefPtr<T> object) { entries_[name] = std::move(object); }
    // Removes the name; returns the object so the caller can unbind it.
    RefPtr<T> remove(GLuint name) {
        auto it = entries_.find(name);
        if (it == entries_.end()) return RefPtr<T>();
        RefPtr<T> object = std::move(it->second);
        entries_.erase(it);
        return object;
    }
    template <class F>
    void forEach(F&& f) const { for (auto& [name, object] : entries_) if (object) f(name, object.get()); }

private:
    std::unordered_map<GLuint, RefPtr<T>> entries_;
    GLuint next_ = 1;
};

}  // namespace wmgl
