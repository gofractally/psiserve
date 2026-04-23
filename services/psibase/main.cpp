// main.cpp — psibase service entry point.
//
// Command-style wasm: wasi-libc's crt1-command synthesizes `_start`,
// which runs global constructors via __wasm_call_ctors, calls main,
// runs destructors, and returns main's value to the host through
// __wasi_proc_exit.
//
// Empty for now — the first brick of the rewrite.

int main()
{
   return 0;
}
