#pragma once
// Set when a library assert fails (see assertTrap.cpp).
extern volatile const char* gAssertFile;
extern volatile const char* gAssertExpr;
extern volatile const char* gAssertFunc;
extern volatile int gAssertLine;
