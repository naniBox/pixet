#include "TestHarness.h"

#include <cstdio>
#include <exception>

int main() {
    int failed = 0;
    for (auto &tc : pixet_test::registry()) {
        std::printf("[ RUN  ] %s\n", tc.name.c_str());
        try {
            tc.fn();
            std::printf("[  OK  ] %s\n", tc.name.c_str());
        } catch (const pixet_test::AssertFailure &e) {
            std::printf("[ FAIL ] %s: %s\n", tc.name.c_str(), e.msg.c_str());
            ++failed;
        } catch (const std::exception &e) {
            std::printf("[ FAIL ] %s: unexpected exception: %s\n", tc.name.c_str(), e.what());
            ++failed;
        }
    }

    int total = (int)pixet_test::registry().size();
    std::printf("\n%d/%d tests passed\n", total - failed, total);
    return failed == 0 ? 0 : 1;
}
