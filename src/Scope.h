#ifndef HALIDE_SCOPE_H
#define HALIDE_SCOPE_H

#include <iostream>
#include <map>
#include <unordered_map>
#include <stack>
#include <string>
#include <utility>

#include "Debug.h"
#include "Error.h"
#include "Util.h"

/** \file
 * Defines the Scope class, which is used for keeping track of names in a scope while traversing IR
 */

namespace Halide {
namespace Internal {

/** A stack which can store one item very efficiently. Using this
 * instead of std::stack speeds up Scope substantially. */
template<typename T>
class SmallStack {
private:
    T _top;
    std::vector<T> _rest;
    bool _empty;

public:
 HALIDE_ALWAYS_INLINE
    SmallStack() : _empty(true) {}

HALIDE_ALWAYS_INLINE
    void pop() {
        if (_rest.empty()) {
            _empty = true;
            _top = T();
        } else {
            _top = _rest.back();
            _rest.pop_back();
        }
    }

HALIDE_ALWAYS_INLINE
    void push(const T &t) {
        if (_empty) {
            _empty = false;
        } else {
            _rest.push_back(_top);
        }
        _top = t;
    }

HALIDE_ALWAYS_INLINE
    T top() const {
        return _top;
    }

HALIDE_ALWAYS_INLINE
    T &top_ref() {
        return _top;
    }

HALIDE_ALWAYS_INLINE
    const T &top_ref() const {
        return _top;
    }

HALIDE_ALWAYS_INLINE
    bool empty() const {
        return _empty;
    }
};

template<>
class SmallStack<void> {
    // A stack of voids. Voids are all the same, so just record how many voids are in the stack
    int counter = 0;
public:
HALIDE_ALWAYS_INLINE
    void pop() {
        counter--;
    }
HALIDE_ALWAYS_INLINE
    void push() {
        counter++;
    }
HALIDE_ALWAYS_INLINE
    bool empty() const {
        return counter == 0;
    }
};

/** A common pattern when traversing Halide IR is that you need to
 * keep track of stuff when you find a Let or a LetStmt, and that it
 * should hide previous values with the same name until you leave the
 * Let or LetStmt nodes This class helps with that. */
template<typename T = void>
class Scope {
private:
    std::map<std::string, SmallStack<T>> m_table;
    std::unordered_map<std::string, SmallStack<T>> o_table;

    // Copying a scope object copies a large table full of strings and
    // stacks. Bad idea.
    Scope(const Scope<T> &);
    Scope<T> &operator=(const Scope<T> &);

    const Scope<T> *containing_scope = nullptr;

public:
HALIDE_ALWAYS_INLINE
    Scope() = default;

HALIDE_ALWAYS_INLINE
    /** Set the parent scope. If lookups fail in this scope, they
     * check the containing scope before returning an error. Caller is
     * responsible for managing the memory of the containing scope. */
    void set_containing_scope(const Scope<T> *s) {
        containing_scope = s;
    }

HALIDE_ALWAYS_INLINE
    /** A const ref to an empty scope. Useful for default function
     * arguments, which would otherwise require a copy constructor
     * (with llvm in c++98 mode) */
    static const Scope<T> &empty_scope() {
        static Scope<T> *_empty_scope = new Scope<T>();
        return *_empty_scope;
    }

    /** Retrieve the value referred to by a name */
    template<typename T2 = T,
             typename = typename std::enable_if<!std::is_same<T2, void>::value>::type>
HALIDE_ALWAYS_INLINE
    T2 get(const std::string &name) const {
        auto m_iter = m_table.find(name);
        auto o_iter = o_table.find(name);
        internal_assert((m_iter == m_table.end()) == (o_iter == o_table.end()));

        if (o_iter == o_table.end() || o_iter->second.empty()) {
            if (containing_scope) {
                return containing_scope->get(name);
            } else {
                internal_error << "Name not in Scope: " << name << "\n" << *this << "\n";
            }
        }
        internal_assert(m_iter->first == o_iter->first);
        // internal_assert(m_iter->second == o_iter->second);
        return o_iter->second.top();
    }

    /** Return a reference to an entry. Does not consider the containing scope. */
    template<typename T2 = T,
             typename = typename std::enable_if<!std::is_same<T2, void>::value>::type>
HALIDE_ALWAYS_INLINE
    void replace(const std::string &name, const T2 &value) {
        auto m_iter = m_table.find(name);
        auto o_iter = o_table.find(name);
        internal_assert((m_iter == m_table.end()) == (o_iter == o_table.end()));

        if (o_iter == o_table.end() || o_iter->second.empty()) {
            internal_error << "Name not in Scope: " << name << "\n" << *this << "\n";
        }

        internal_assert(m_iter->first == o_iter->first);
        m_iter->second.top_ref() = value;
        o_iter->second.top_ref() = value;
    }

    /** Tests if a name is in scope */
HALIDE_ALWAYS_INLINE
    bool contains(const std::string &name) const {
        auto m_iter = m_table.find(name);
        auto o_iter = o_table.find(name);
        internal_assert((m_iter == m_table.end()) == (o_iter == o_table.end()));

        if (o_iter == o_table.end() || o_iter->second.empty()) {
            if (containing_scope) {
                return containing_scope->contains(name);
            } else {
                return false;
            }
        }

        internal_assert(m_iter->first == o_iter->first);
        return true;
    }

    /** Add a new (name, value) pair to the current scope. Hide old
     * values that have this name until we pop this name.
     */
    template<typename T2 = T,
             typename = typename std::enable_if<!std::is_same<T2, void>::value>::type>
HALIDE_ALWAYS_INLINE
    void push(const std::string &name, const T2 &value) {
        m_table[name].push(value);
        o_table[name].push(value);
    }

    template<typename T2 = T,
             typename = typename std::enable_if<std::is_same<T2, void>::value>::type>
HALIDE_ALWAYS_INLINE
    void push(const std::string &name) {
        m_table[name].push();
        o_table[name].push();
    }

    /** A name goes out of scope. Restore whatever its old value
     * was (or remove it entirely if there was nothing else of the
     * same name in an outer scope) */
HALIDE_ALWAYS_INLINE
    void pop(const std::string &name) {
        auto m_iter = m_table.find(name);
        auto o_iter = o_table.find(name);
        internal_assert((m_iter == m_table.end()) == (o_iter == o_table.end()));

        internal_assert(m_iter != m_table.end()) << "Name not in Scope: " << name << "\n" << *this << "\n";
        internal_assert(m_iter->first == o_iter->first);
        m_iter->second.pop();
        o_iter->second.pop();
        if (m_iter->second.empty()) {
            m_table.erase(m_iter);
        }
        if (o_iter->second.empty()) {
            o_table.erase(o_iter);
        }
    }

#if 0
    /** Iterate through the scope. Does not capture any containing scope. */
    class const_iterator {
        typename std::map<std::string, SmallStack<T>>::const_iterator m_iter;
    public:
HALIDE_ALWAYS_INLINE
         explicit const_iterator(const typename std::map<std::string, SmallStack<T>>::const_iterator &i) :
            m_iter(i) {
        }

HALIDE_ALWAYS_INLINE
        const_iterator() {}

HALIDE_ALWAYS_INLINE
        bool operator!=(const const_iterator &other) {
            return m_iter != other.m_iter;
        }

HALIDE_ALWAYS_INLINE
        void operator++() {
            ++m_iter;
        }

HALIDE_ALWAYS_INLINE
        const std::string &name() {
            return m_iter->first;
        }

HALIDE_ALWAYS_INLINE
        const SmallStack<T> &stack() {
            return m_iter->second;
        }

        template<typename T2 = T,
                 typename = typename std::enable_if<!std::is_same<T2, void>::value>::type>
HALIDE_ALWAYS_INLINE
        const T2 &value() {
            return m_iter->second.top_ref();
        }
    };

 HALIDE_ALWAYS_INLINE
    const_iterator cbegin() const {
        assert(m_table.size() == o_table.size());
if (m_table.size()) {
std::ostringstream o;
o<<"Iterating over Scope, ordered is:\n";
for (const auto &m : m_table) {
    o << "  " << m.first << "\n";
}
o<<"unordered is:\n";
for (const auto &m : o_table) {
    o << "  " << m.first << "\n";
}
o << "\n";
std::cerr<<o.str()<<std::flush;
}
        return const_iterator(m_table.begin());
    }

HALIDE_ALWAYS_INLINE
     const_iterator cend() const {
        return const_iterator(m_table.end());
    }
#else
    /** Iterate through the scope. Does not capture any containing scope. */
    class const_iterator {
        typename std::unordered_map<std::string, SmallStack<T>>::const_iterator m_iter;
    public:
HALIDE_ALWAYS_INLINE
         explicit const_iterator(const typename std::unordered_map<std::string, SmallStack<T>>::const_iterator &i) :
            m_iter(i) {
        }

HALIDE_ALWAYS_INLINE
        const_iterator() {}

HALIDE_ALWAYS_INLINE
        bool operator!=(const const_iterator &other) {
            return m_iter != other.m_iter;
        }

HALIDE_ALWAYS_INLINE
        void operator++() {
            ++m_iter;
        }

HALIDE_ALWAYS_INLINE
        const std::string &name() {
            return m_iter->first;
        }

HALIDE_ALWAYS_INLINE
        const SmallStack<T> &stack() {
            return m_iter->second;
        }

        template<typename T2 = T,
                 typename = typename std::enable_if<!std::is_same<T2, void>::value>::type>
HALIDE_ALWAYS_INLINE
        const T2 &value() {
            return m_iter->second.top_ref();
        }
    };

 HALIDE_ALWAYS_INLINE
    const_iterator cbegin() const {
        assert(m_table.size() == o_table.size());
if (m_table.size()) {
std::ostringstream o;
o<<"Iterating over Scope, ordered is:\n";
for (const auto &m : m_table) {
    o << "  " << m.first << "\n";
}
o<<"unordered is:\n";
for (const auto &m : o_table) {
    o << "  " << m.first << "\n";
}
o << "\n";
std::cerr<<o.str()<<std::flush;
}
        return const_iterator(o_table.begin());
    }

HALIDE_ALWAYS_INLINE
     const_iterator cend() const {
        return const_iterator(o_table.end());
    }
#endif

 HALIDE_ALWAYS_INLINE
    void swap(Scope<T> &other) {
        m_table.swap(other.m_table);
        o_table.swap(other.o_table);
        std::swap(containing_scope, other.containing_scope);
    }
};

template<typename T>
HALIDE_ALWAYS_INLINE
std::ostream &operator<<(std::ostream &stream, const Scope<T>& s) {
    stream << "{\n";
    typename Scope<T>::const_iterator m_iter;
    for (m_iter = s.cbegin(); m_iter != s.cend(); ++m_iter) {
        stream << "  " << m_iter.name() << "\n";
    }
    stream << "}";
    return stream;
}

/** Helper class for pushing/popping Scope<> values, to allow
 * for early-exit in Visitor/Mutators that preserves correctness.
 * Note that this name can be a bit confusing, since there are two "scopes"
 * involved here:
 * - the Scope object itself
 * - the lifetime of this helper object
 * The "Scoped" in this class name refers to the latter, as it temporarily binds
 * a name within the scope of this helper's lifetime. */
template<typename T = void>
struct ScopedBinding {
    Scope<T> *scope;
    std::string name;
HALIDE_ALWAYS_INLINE
    ScopedBinding(Scope<T> &s, const std::string &n, const T &value) :
        scope(&s), name(n) {
        scope->push(name, value);
    }
HALIDE_ALWAYS_INLINE
    ScopedBinding(bool condition, Scope<T> &s, const std::string &n, const T &value) :
        scope(condition ? &s : nullptr), name(n) {
        if (condition) {
            scope->push(name, value);
        }
    }
HALIDE_ALWAYS_INLINE
    ~ScopedBinding() {
        if (scope) {
            scope->pop(name);
        }
    }
};

template<>
struct ScopedBinding<void> {
    Scope<> *scope;
    std::string name;
HALIDE_ALWAYS_INLINE
    ScopedBinding(Scope<> &s, const std::string &n) : scope(&s), name(n) {
        scope->push(name);
    }
HALIDE_ALWAYS_INLINE
    ScopedBinding(bool condition, Scope<> &s, const std::string &n) :
        scope(condition ? &s : nullptr), name(n) {
        if (condition) {
            scope->push(name);
        }
    }
HALIDE_ALWAYS_INLINE
    ~ScopedBinding() {
        if (scope) {
            scope->pop(name);
        }
    }
};

}  // namespace Internal
}  // namespace Halide

#endif
