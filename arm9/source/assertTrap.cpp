// Catches failed asserts in the libraries (DSWiFi, lwIP): libnds's handler
// prints to the console and then waits forever, which on our screens just
// looks like a freeze. Remember what failed so the stuck page can show it,
// then let libnds do its usual thing.
#include <nds/ndstypes.h>
#include "assertTrap.h"

volatile const char* gAssertFile;
volatile const char* gAssertExpr;
volatile const char* gAssertFunc;
volatile int gAssertLine;

extern "C" void __real___assert_func(const char* file, int line, const char* func, const char* expr);

extern "C" void __wrap___assert_func(const char* file, int line, const char* func, const char* expr)
{
    gAssertFile = file;
    gAssertLine = line;
    gAssertFunc = func;
    gAssertExpr = expr;
    __real___assert_func(file, line, func, expr);
    while (true)
        ;
}
