#pragma once

#include <functional>
#include <string>
#include <vector>

namespace pixet_test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase> &registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) { registry().push_back({std::move(name), std::move(fn)}); }
};

struct AssertFailure {
    std::string msg;
};

} // namespace pixet_test

#define PIXET_TEST(name)                                                                                             \
    void pixet_test_##name();                                                                                        \
    static pixet_test::Registrar pixet_registrar_##name(#name, pixet_test_##name);                                   \
    void pixet_test_##name()

#define PIXET_CHECK(cond)                                                                                             \
    do {                                                                                                              \
        if (!(cond))                                                                                                  \
            throw pixet_test::AssertFailure{std::string(__FILE__) + ":" + std::to_string(__LINE__) +                  \
                                              ": CHECK failed: " #cond};                                               \
    } while (0)
