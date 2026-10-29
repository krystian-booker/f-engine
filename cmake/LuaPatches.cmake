# A small patch to the Lua 5.4 standard library, applied at configure time to
# copies of three source files (the downloaded sources stay untouched). It
# makes the library calls that loop in C without running any Lua bounded, so a
# script cannot hang the engine past the script watchdog with one call
# (docs/architecture.md, "Sandbox"):
#
# - lauxlib.c: defines `fe_lua_step_hook`, a function the engine sets
#   (LuaHost) and the patched loops call every 65536 steps; the engine's hook
#   raises the watchdog error once the frame has run too long. Strings built
#   in a luaL_Buffer (string.rep, string.format, string.gsub, table.concat,
#   tostring...) are limited to FE_LUA_MAX_BUFFER bytes (256 MiB), raising
#   "string too large".
# - lstrlib.c: the pattern matcher (string.find, match, gmatch, gsub) counts
#   one step per pattern item it tries, and a plain string.find one per byte
#   compared, so a pattern that backtracks for minutes stops at the watchdog.
# - ltablib.c: table.move counts a step per element and moves at most
#   FE_LUA_MAX_MOVE (2^26) elements per call, raising "too many elements to
#   move".
#
# Each replacement must match the source exactly once, so a Lua upgrade that
# changes these lines fails the configure step instead of silently dropping
# the patch.

set(FE_LUA_PATCH_PREAMBLE [=[
/* f-engine patch (cmake/LuaPatches.cmake): bounded library loops. */
extern void (*fe_lua_step_hook)(lua_State *L);
#define FE_LUA_STEP_INTERVAL 65536u
#define FE_LUA_STEP(L, counter, weight) \
  do { \
    (counter) += (weight); \
    if (l_unlikely((counter) >= FE_LUA_STEP_INTERVAL)) { \
      (counter) = 0; \
      if (fe_lua_step_hook) fe_lua_step_hook(L); \
    } \
  } while (0)
#define FE_LUA_MAX_BUFFER ((size_t)256 << 20)
#define FE_LUA_MAX_MOVE ((lua_Integer)1 << 26)
]=])

function(fe_lua_replace text_var old new file)
  string(FIND "${${text_var}}" "${old}" first)
  if(first EQUAL -1)
    message(FATAL_ERROR "cmake/LuaPatches.cmake: ${file} no longer contains:\n${old}")
  endif()
  string(LENGTH "${old}" old_length)
  math(EXPR rest_start "${first} + ${old_length}")
  string(SUBSTRING "${${text_var}}" ${rest_start} -1 rest)
  string(FIND "${rest}" "${old}" second)
  if(NOT second EQUAL -1)
    message(FATAL_ERROR "cmake/LuaPatches.cmake: ${file} contains this more than once:\n${old}")
  endif()
  string(REPLACE "${old}" "${new}" result "${${text_var}}")
  set(${text_var} "${result}" PARENT_SCOPE)
endfunction()

# Writes the patched copy of src/<file> to <output_dir>/<file> (only when it
# changes, so reconfiguring does not rebuild Lua) and sets <out_var> to it.
function(fe_patch_lua_file source_dir output_dir file out_var)
  file(READ "${source_dir}/src/${file}" text)
  set(include_line "#include \"lauxlib.h\"\n")
  fe_lua_replace(text "${include_line}" "${include_line}${FE_LUA_PATCH_PREAMBLE}" ${file})
  if(file STREQUAL "lauxlib.c")
    fe_lua_replace(text
      "static size_t newbuffsize (luaL_Buffer *B, size_t sz) {
  size_t newsize = (B->size / 2) * 3;  /* buffer size * 1.5 */
  if (l_unlikely(MAX_SIZET - sz < B->n))  /* overflow in (B->n + sz)? */
    return luaL_error(B->L, \"buffer too large\");
  if (newsize < B->n + sz)  /* not big enough? */
    newsize = B->n + sz;
  return newsize;
}"
      "void (*fe_lua_step_hook)(lua_State *L) = NULL;

static size_t newbuffsize (luaL_Buffer *B, size_t sz) {
  size_t newsize = (B->size / 2) * 3;  /* buffer size * 1.5 */
  if (l_unlikely(MAX_SIZET - sz < B->n))  /* overflow in (B->n + sz)? */
    return luaL_error(B->L, \"buffer too large\");
  if (l_unlikely(B->n + sz > FE_LUA_MAX_BUFFER))
    return luaL_error(B->L, \"string too large (the limit is %d MiB)\",
                      (int)(FE_LUA_MAX_BUFFER >> 20));
  if (newsize < B->n + sz)  /* not big enough? */
    newsize = B->n + sz;
  if (newsize > FE_LUA_MAX_BUFFER)
    newsize = FE_LUA_MAX_BUFFER;
  return newsize;
}" ${file})
  elseif(file STREQUAL "lstrlib.c")
    fe_lua_replace(text
      "  int matchdepth;  /* control for recursive depth (to avoid C stack overflow) */"
      "  int matchdepth;  /* control for recursive depth (to avoid C stack overflow) */
  unsigned int fe_steps;  /* f-engine: steps since the last step hook call */" ${file})
    fe_lua_replace(text
      "  init: /* using goto to optimize tail recursion */
  if (p != ms->p_end) {  /* end of pattern? */"
      "  init: /* using goto to optimize tail recursion */
  FE_LUA_STEP(ms->L, ms->fe_steps, 1u);
  if (p != ms->p_end) {  /* end of pattern? */" ${file})
    fe_lua_replace(text
      "  ms->L = L;
  ms->matchdepth = MAXCCALLS;"
      "  ms->L = L;
  ms->matchdepth = MAXCCALLS;
  ms->fe_steps = 0;" ${file})
    fe_lua_replace(text
      "static const char *lmemfind (const char *s1, size_t l1,
                               const char *s2, size_t l2) {"
      "static const char *lmemfind (lua_State *L, const char *s1, size_t l1,
                               const char *s2, size_t l2) {
  unsigned int fe_steps = 0;" ${file})
    fe_lua_replace(text
      "      init++;   /* 1st char is already checked */
      if (memcmp(init, s2+1, l2) == 0)"
      "      init++;   /* 1st char is already checked */
      FE_LUA_STEP(L, fe_steps, 1u + (unsigned int)(l2 >> 6));
      if (memcmp(init, s2+1, l2) == 0)" ${file})
    fe_lua_replace(text
      "lmemfind(s + init, ls - init, p, lp);"
      "lmemfind(L, s + init, ls - init, p, lp);" ${file})
  elseif(file STREQUAL "ltablib.c")
    fe_lua_replace(text
      "    n = e - f + 1;  /* number of elements to move */"
      "    n = e - f + 1;  /* number of elements to move */
    luaL_argcheck(L, n <= FE_LUA_MAX_MOVE, 3,
                  \"too many elements to move (the limit is 67108864)\");" ${file})
    fe_lua_replace(text
      "      for (i = 0; i < n; i++) {
        lua_geti(L, 1, f + i);"
      "      unsigned int fe_steps = 0;
      for (i = 0; i < n; i++) {
        FE_LUA_STEP(L, fe_steps, 1u);
        lua_geti(L, 1, f + i);" ${file})
    fe_lua_replace(text
      "      for (i = n - 1; i >= 0; i--) {
        lua_geti(L, 1, f + i);"
      "      unsigned int fe_steps = 0;
      for (i = n - 1; i >= 0; i--) {
        FE_LUA_STEP(L, fe_steps, 1u);
        lua_geti(L, 1, f + i);" ${file})
  endif()
  set(output "${output_dir}/${file}")
  file(WRITE "${output}.new" "${text}")
  file(COPY_FILE "${output}.new" "${output}" ONLY_IF_DIFFERENT)
  file(REMOVE "${output}.new")
  set(${out_var} "${output}" PARENT_SCOPE)
endfunction()
