# Kru — release history

I'm keeping the old versions because I want to be able to look back and see what changed. Some are bad. That's fine. I'd rather have honest old snapshots than rename everything "stable" when it isn't.

| Original public release date | New tag | What it really was | Checked result |
| --- | --- | --- | --- |
| Aug 3, 2026 | `v0.1.0-alpha.0` | Early handwritten C transpiler | GCC build; 3/5 fixtures by manual compile/run |
| Aug 3, 2026 | `v0.1.0-alpha.1` | Wider syntax and more test programs | GCC build; 10/22 bundled tests |
| Aug 4, 2026 | `v0.1.0-alpha.2` | Integer/diagnostic patch, still C-based | GCC build; 10/22 bundled tests |
| Sep 30, 2026 | `v0.2.0-alpha.0` | Experimental direct x86-64 ELF backend | `make test` passed; native Stage 4 ran |

## Items that are not new official releases

- `Kru-ALPHA(1).zip` contains only an early README, describing a Stage 0 compiler. **No compiler files were actually included**, so it cannot be released as runnable Stage 0 source.
- `Kru-ALPHA-1.zip` and `Kru-ALPHA-1(1).zip` have the **same SHA-256**, so they are duplicates. The ZIP also has two almost-identical source trees. Only `Kru-ALPHA-1` was kept in the cleaned Alpha 1 asset.
- `Kru-main.zip` is a later GitHub branch snapshot after two Copilot pull requests were merged on Sep 30, 2026. It is **not a clean successor** in the tested set: `make test` failed at the initial regression suite with 9 passes, 9 failures, and 6 expected failures. Stage 4 fails with `K1043 unknown identifier 'n'`. Kept in `archive-only/`, not promoted as a numbered working release.

Dates above are the original public GitHub release dates. They are not necessarily the dates every line of source was written. Verification was done October 25, 2026, inside a Linux build environment.

**Important:** these are the historical snapshots from the seven uploaded archives. They are *not* the later, more advanced local Kru builds. Do not treat the last one as an up-to-date measurement of the language today.

See each tag's `RELEASE_NOTES.md` for its own limitations and instructions.
