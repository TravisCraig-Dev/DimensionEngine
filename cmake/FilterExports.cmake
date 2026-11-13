# Writes OUT: the module definition IN (every symbol of the engine's objects, from cmake -E __create_def) without the
# symbols of lambdas, which no other module can refer to. Run with -DIN=<all.def> -DOUT=<engine.def>.
file(READ "${IN}" content)
string(REGEX REPLACE "[^\n]*<lambda[^\n]*\n" "" content "${content}")
# Nor C++/WinRT's projection (header-only, used only inside the engine).
string(REGEX REPLACE "[^\n]*@winrt@@[^\n]*\n" "" content "${content}")
file(WRITE "${OUT}.tmp" "${content}")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
