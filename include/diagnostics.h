#ifndef KRU_DIAGNOSTICS_H
#define KRU_DIAGNOSTICS_H

/*
    Single source of truth for kru0's diagnostic codes.

    Per the Codex (Ch.79 "Diagnostic Codes" / Ch.77 "Diagnostic
    Structure"), diagnostic codes are permanent, class-scoped
    identifiers:

        K1xxx  Language errors (parser, type, name resolution)
        K2xxx  Warnings
        K3xxx  Memory system errors
        K4xxx  Security errors (capability / `use`-permission violations)
        K5xxx  Compiler/toolchain errors

    Once a code is assigned to a condition, it is never reused or
    reinterpreted for a different condition. This table is updated
    any time a diagnostic is added, renumbered, or removed so the
    mapping cannot drift out of sync with the source again.

    NOTE ON A KNOWN CODEX INTERNAL INCONSISTENCY (flagged, not fixed
    here — out of scope for this pass): Appendix E.2 further
    subdivides K1xxx into K1001-1099 (name resolution), K1100-1199
    (type checking), K1200-1299 (parser), K1300-1399 (trait/generic
    resolution) — but Chapter 79's own worked examples (K1001 Unknown
    identifier, K1002 Type mismatch, K1004 Immutable binding modified,
    K1005 Expected expression, K1006 Impure expression in match guard)
    are numbered as one flat cluster that ignores those subranges.
    This codebase follows Chapter 79's flat numbering (matching what
    was already in place and what the Ch.77 worked example uses),
    since re-deriving every existing K1xxx code against the Appendix
    E.2 subrange scheme would be a numbering-scheme rewrite, not a
    bug fix. Flagging so it isn't silently "fixed" twice, differently,
    in a future pass.

    ------------------------------------------------------------------
    K1xxx — Language errors
    ------------------------------------------------------------------

    K1001   invalid assignment target                  parse.c
    K1002   expected expression                         parse.c
    K1003   expected variable name                      parse.c
    K1004   cannot modify immutable binding              sema.c:444
            (Codex Ch.77's own worked example. Previously misfiled as
            K3001 — a memory-class number — corrected in this pass.)
    K1007   expected '}'                                 parse.c
    K1008   expected ')'                                 parse.c
    K1009   expected '('                                 parse.c
    K1010   expected '{' after match expression           parse.c
    K1011   expected fn                                   parse.c
    K1012   expected function name                        parse.c
    K1013   expected parameter                             parse.c
    K1014   expected '='                                   parse.c
    K1015   expected 'ref'                                 parse.c
    K1016   '=' cannot initialize binding; use ':='          parse.c
    K1017   expected expression in binding initializer       parse.c
    K1018   expected constant name                          parse.c
    K1019   expected ':=' for const initializer              parse.c
    K1020   expected type alias name                         parse.c
    K1021   expected ':=' for type alias                      parse.c
    K1022   expected struct name                                parse.c
    K1023   expected field name                                  parse.c
    K1024   expected enum name                                    parse.c
    K1025   expected enum variant                                  parse.c
    K1026   expected field name after '.'                          parse.c
    K1027   expected match pattern                                  parse.c
    K1028   expected '=>' in match arm                               parse.c
    K1029   expected loop variable name after 'for'                    parse.c
    K1030   expression nested too deeply                             parse.c
    K1031   expected ';' in array type                                parse.c
    K1032   expected array length                                      parse.c
    K1033   expected ']' in array type                                  parse.c
    K1034   array length literal does not fit in a 64-bit integer        parse.c
    K1035   integer literal does not fit in a 64-bit / 128-bit integer    parse.c
    K1036   expected type name                              parse.c (7 call sites:
            125 [comment], 409, 536, 606, 1624, 2424, 2780, 2817)
            Was K1004 — collided with the Codex's own K1004 worked
            example ("cannot modify immutable binding"). Corrected in
            this pass; K1004 now correctly means what the Codex says.
    K1037   name already declared in this scope           sema.c:359,395
            Was K3002 — memory class. Redeclaration is a name-resolution
            error (K1xxx by the Codex's own class definitions), not a
            memory-system error. Also freed the real K3002 ("mutable
            access conflicts with active reference") for future use.
    K1038   used before it is initialized                 sema.c:507
            Was K3003 — memory class. Definite-assignment/read-before-
            init is name-resolution, not memory-system. Also freed the
            real K3003 ("escaping closure captures reference to local
            binding") for future use.
    K1039   division/modulo by constant zero              codegen.c:1959-1960
            Was K4001 — security class. Div-by-zero is a plain
            arithmetic/UB correctness error, not a capability/security
            violation. Also freed the real K4001 ("capability
            violation") for future use.
    K1040   literal does not fit in declared type          codegen.c
            Was K4002 — security class, same misclassification as
            K1039 above.
    K1041   unrecognized integer literal suffix             parse.c
    K1042   for-loop collection is not a known fixed array   codegen.c
    K1043   unknown identifier                               sema.c

    RESERVED — do not reuse (Codex-defined future language errors,
    not yet implemented by this compiler):
    K1006   impure expression in match guard (match-guard purity is
            not implemented yet; Codex Ch.79 and body text ~line 833
            both name this code specifically)

    ------------------------------------------------------------------
    K2xxx — Warnings (none emitted by kru0 yet)
    ------------------------------------------------------------------
    K2001   Unused variable                    (Codex-reserved, unimplemented)
    K2002   Missing documentation               (Codex-reserved, unimplemented)
    K2003   Match arm with always-false guard    (Codex-reserved, unimplemented)
    K2010   Unused mutable binding                (Codex-reserved, unimplemented)
    K2050   Compiler tracking-capacity warning         codegen.c
    K2100   Repeated heap allocation in loop        (Codex-reserved, unimplemented)

    ------------------------------------------------------------------
    K3xxx — Memory system errors (none emitted by kru0 yet — freed by
    this pass; previously misused for K1037/K1038/K1004 above)
    ------------------------------------------------------------------
    K3001   Reference lifetime violation           (Codex-reserved, unimplemented)
    K3002   Mutable access conflicts with active reference (Codex-reserved, unimplemented)
    K3003   Escaping closure captures reference to local binding (Codex-reserved, unimplemented)
    K3010   Cannot return reference to local value  (Codex-reserved, unimplemented)

    ------------------------------------------------------------------
    K4xxx — Security errors (none emitted by kru0 yet — freed by this
    pass; previously misused for K1039/K1040 above)
    ------------------------------------------------------------------
    K4001   Capability violation                    (Codex-reserved, unimplemented —
            real use awaits the `use`-permission manifest system,
            Codex Principle 6, not attempted in this pass)

    ------------------------------------------------------------------
    K5xxx — Compiler/toolchain errors (none emitted by kru0 yet)
    ------------------------------------------------------------------
    K5001   Internal compiler error                 (Codex-reserved, unimplemented)

*/

#endif
