# vendored fmt

This directory vendors a minimal header-only subset of `{fmt}` from upstream `fmtlib/fmt`.

- Upstream version: `11.1.4`
- Vendored files:
  - `include/fmt/base.h`
  - `include/fmt/format.h`
  - `include/fmt/format-inl.h`
  - `include/fmt/ranges.h`
  - `LICENSE`

Project code should include `raftpp/fmt.h` instead of including `{fmt}` headers directly.

Set `RAFTPP_USE_EXTERNAL_FMT=ON` to use an externally provided `fmt` package via `find_package(fmt CONFIG REQUIRED)`.
