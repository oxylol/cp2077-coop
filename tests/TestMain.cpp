#include <cstdio>
#include <cstring>

#include "Test.hpp"
#include "core/Log.hpp"

int main(int aArgc, char** aArgv)
{
    const char* filter = aArgc > 1 ? aArgv[1] : nullptr;
    coop::Log::SetMinLevel(coop::LogLevel::Warn);

    int run = 0;
    for (const auto& testCase : test::Registry())
    {
        if (filter && !std::strstr(testCase.name, filter))
            continue;
        const int before = test::Failures();
        std::printf("[ RUN  ] %s\n", testCase.name);
        testCase.fn();
        std::printf("[ %s ] %s\n", test::Failures() == before ? " OK " : "FAIL", testCase.name);
        ++run;
    }

    std::printf("\n%d test(s), %d failure(s)\n", run, test::Failures());
    return test::Failures() == 0 ? 0 : 1;
}
