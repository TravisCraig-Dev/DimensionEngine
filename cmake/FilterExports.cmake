# Writes OUT: the module definition IN (every symbol of the engine's objects, from cmake -E __create_def) without the
# symbols of lambdas, which no other module can refer to. Run with -DIN=<all.def> -DOUT=<engine.def>.
file(READ "${IN}" content)
string(REGEX REPLACE "[^\n]*<lambda[^\n]*\n" "" content "${content}")
# Nor C++/WinRT's projection (header-only, used only inside the engine).
string(REGEX REPLACE "[^\n]*@winrt@@[^\n]*\n" "" content "${content}")
# Nor functions of standard library templates made only for non-engine types (std::vector<int>::push_back): headers
# define them, so every module compiles its own (Windows allows 65535 exports per DLL). Lines naming an engine type, and
# data, stay: they are marked, the rest filtered, then the marks removed.
string(REGEX REPLACE "\n([^\n]*(Dimension|DATA)[^\n]*)" "\n#KEEP#\\1" content "${content}")
string(REGEX REPLACE "\n\t[^\n]*@std@@[^\n]*" "" content "${content}")
string(REPLACE "\n#KEEP#" "\n" content "${content}")
file(WRITE "${OUT}.tmp" "${content}")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
