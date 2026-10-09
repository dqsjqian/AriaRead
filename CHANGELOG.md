# Changelog

## 0.3.1

- Pin the shared AriaDeps build package and use the same Python interpreter for
  dependency preparation and CMake verification on all platforms.
- Restore valid workflow cache expressions and verify platform recipe and
  third-party attribution contracts through the shared package.
- Make RSS schema migration transactional. Preserve source status and latency,
  rebuild indexes after replacing legacy tables, and recover missing fields
  independently even when a stored source JSON document is malformed.
- Report the actual number of newly inserted sources when imports contain
  duplicate URLs.
- Execute standalone JavaScript selectors through their borrowed runtime,
  including rule context, value conversion, errors and interruption handling.
- Implement injected native JavaScript functions with stateful callbacks,
  preserved aliases, safe replacement, exception reporting and NUL-preserving
  string conversion.
