#pragma once

namespace minigraph {
    struct Counter {
        struct value_type {
            template<typename T>
            value_type(const T &) {}
        };
        void push_back(const value_type &) { ++count; }
        unsigned long count {0};
    };
}
