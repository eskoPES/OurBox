// C++ exceptions through title frames: needs the title's .pdata registered.
#include <stdexcept>
#include <cstdio>
static int depth(int n) { if (n == 0) throw std::runtime_error("deep"); return depth(n - 1) + 1; }
int main()
{
    int fails = 0;
    try { depth(50); fails++; } catch (const std::exception &e) { if (std::string(e.what()) != "deep") fails++; }
    std::printf(fails ? "RESULT FAIL (%d)\n" : "RESULT PASS\n", fails);
    return fails;
}
