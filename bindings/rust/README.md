# accudisc — Rust bindings

Planned layout:

- `accudisc-sys/` — raw FFI, `bindgen` over `include/accudisc/accudisc.h`,
  linking via the installed `accudisc.pc`
- `accudisc/` — safe idiomatic wrapper crate

Will be scaffolded once the C API surface stabilizes past the initial
`c2read` import.
