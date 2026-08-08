/* Is assert() actually live in this build?
 *
 * Most of this suite asserts with assert(). Under -DNDEBUG that macro expands
 * to nothing, so every one of those files compiles to a program that computes
 * some values, checks none of them and exits 0. ctest reports the suite green.
 *
 * That is not hypothetical. It was the state of this repo's own build directory
 * on 2026-08-08: CMAKE_BUILD_TYPE=Release (as install.sh leaves it) put
 * -DNDEBUG on every test, and an assert(0) placed on the first line of
 * test_map's main() exited 0. tests/CMakeLists.txt now appends -UNDEBUG to each
 * test target to undo it.
 *
 * But that fix is a claim about compiler command-line ordering — that a target's
 * compile options land after CMAKE_C_FLAGS_<CONFIG>. If that ever stops holding,
 * for a new generator, a new CMake, or a build type nobody tried, the suite goes
 * silently green again. Nothing else in the tree would notice: there is no
 * output to compare and no test to fail.
 *
 * So this file checks the mechanism instead of any library behaviour. It is
 * deliberately the cheapest possible test and deliberately not folded into
 * another one — a test that verifies the assertions work must not itself depend
 * on the assertions working.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int fired = 0;

    /* Not assert(0): the point is to observe whether the macro EVALUATES, and
     * to say so without relying on abort(). A live assert never returns here,
     * so reaching the check below means it expanded to nothing. */
#ifdef NDEBUG
    fprintf(stderr,
            "test_assert: NDEBUG is defined in this translation unit — every\n"
            "assert() in this test suite is a no-op and the green results mean\n"
            "nothing. tests/CMakeLists.txt is supposed to pass -UNDEBUG.\n");
    return 1;
#endif

    /* NDEBUG being undefined is necessary but not sufficient: a toolchain could
     * in principle define assert away regardless. Prove it evaluates its
     * argument by giving it one with a side effect that must be observable. */
    assert((fired = 1) != 0);
    if (!fired) {
        fprintf(stderr,
                "test_assert: assert() did not evaluate its argument — the\n"
                "suite's checks are inert even without NDEBUG.\n");
        return 1;
    }

    return 0;
}
