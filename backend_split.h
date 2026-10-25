#ifndef KRU_BACKEND_SPLIT_H
#define KRU_BACKEND_SPLIT_H

/*
    Mixed tcc/cc compilation.

    kru0's generated C is one translation unit. tcc can't compile any
    file that mentions __int128 (Kru's i128/u128 backing type)
    anywhere, even in an unused declaration -- so a program that uses
    128-bit types anywhere at all currently loses the tcc speedup for
    its *entire* file, even though most of its functions have nothing
    to do with 128-bit integers.

    backend_split_try() re-splits the single generated file into two
    translation units -- one with every function that never touches
    __int128 (compiled with tcc, fast), one with the handful that do
    (compiled with cc -O2, correct) -- and links the two object files
    together. This only happens when it's provably safe to do (see
    the safety precondition in backend_split.c); otherwise the
    caller should fall back to compiling output_path as a single
    file the old way.

    Returns:
        0  - split-compiled and linked successfully; bin_path is
             ready to run.
        1  - split wasn't attempted or wasn't safe/worthwhile for
             this file (e.g. no __int128 usage to split around, or
             __int128 appears somewhere that can't be safely
             isolated) -- caller should fall back to its normal
             single-file compile path. Not an error.
        -1 - split was attempted but a compile or link step failed
             -- caller should fall back to its normal single-file
             compile path as a safety net.
*/
int backend_split_try(
    const char* output_path,
    const char* bin_path,
    const char* tcc_cmd,
    const char* cc_cmd
    );

#endif
