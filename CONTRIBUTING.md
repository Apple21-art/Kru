# Contributing to Kru

This is mainly a personal project, and I'm still working on a lot of it. Issues are welcome, especially if something actually breaks. I'd rather have a small program that reproduces a bug than ten paragraphs telling me it should work.

## Bugs

Include the `.kru` file or a small reproduction, what command you used, what you expected, and the exact output. Tell me whether it happened through `--emit-c` or `--emit-native`. They are different compiler paths.

## Changes

If you want to send a fix, include a test for it. Run `make test`, and ideally `make check` if your machine supports the sanitizer tests. Please don't rewrite large unrelated areas just to change formatting.

The language design comes from the Kru Codex. An unimplemented feature is not a reason to quietly change the language rules. If a behavior isn't supported, say so.

This version uses host C for bootstrapping. Self-hosting and the full native architecture are still works in progress.
