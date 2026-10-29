/* Forced into every Lua source file by cmake/Dependencies.cmake. Lua 5.4
   seeds string hashing from the clock and memory addresses, which makes
   `pairs` order differ between runs; a constant seed keeps seeded runs,
   tests and replays deterministic. */
#define luai_makeseed(L) ((unsigned int)0x2545f491u)
