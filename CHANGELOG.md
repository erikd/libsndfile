# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

* This `CHANGELOG.md`. All notable changes to this project will be documented in
  this file. The old `NEWS` file has been renamed to `NEWS.OLD` and is no longer
  updated.

### Changed

* `SFC_SET_DITHER_ON_READ` and `SFC_SET_DITHER_ON_WRITE` enums comments in
  public header, thanks @SmiVan (issue #677).
* `ENABLE_SNDFILE_WINDOWS_PROTOTYPES` define is deprecated and not needed
  anymore.

  Previously, in order for the [`sf_wchar_open`()](http://libsndfile.github.io/libsndfile/api.html#open)
  function to become available on   the Windows platform, it was required to
  perform certain actions:

  ```c
  #include <windows.h>
  #define ENABLE_SNDFILE_WINDOWS_PROTOTYPES 1
  #including <sndfile.h>
  ```

  These steps are no longer required and the `sf_wchar_open`() function is
  always available on the Windows platform.
* Use UTF-8 as internal path encoding on Windows platform.

  This is an internal change to unify and simplify the handling of file paths.

  On the Windows platform, the file path is always converted to UTF-8 and
  converted to UTF-16 only for calls to WinAPI functions.

  The behavior of the functions for opening files on other platforms does not
  change.

### Fixed

* Typo in `docs/index.md`.
* Memory leak in `caf_read_header`(), credit to OSS-Fuzz ([issue 30375](https://bugs.chromium.org/p/oss-fuzz/issues/detail?id=30375)).
* Stack overflow in `guess_file_type`(), thanks @bobsayshilol, credit to
  OSS-Fuzz ([issue 29339](https://bugs.chromium.org/p/oss-fuzz/issues/detail?id=29339)).
* Abort in fuzzer, thanks @bobsayshilol, credit to OSS-Fuzz
  ([issue 26257](https://bugs.chromium.org/p/oss-fuzz/issues/detail?id=26257)).
* Infinite loop in `svx_read_header`(), thanks @bobsayshilol, credit to OSS-Fuzz
  ([issue 25442](https://bugs.chromium.org/p/oss-fuzz/issues/detail?id=25442)).
* GCC and Clang pedantic warnings, thanks @bobsayshilol.
* Normalisation issue when scaling floating point data to `int` in
  `replace_read_f2i`(), thanks @bobsayshilol, (issue #702).
* Missing samples when doing a partial read of Ogg file from index till the end
  of file, thanks @arthurt (issue #643).

### Security

* Heap buffer overflow in `wavlike_ima_decode_block`(), thanks @bobsayshilol,
  credit to OSS-Fuzz ([issue 25530](https://bugs.chromium.org/p/oss-fuzz/issues/detail?id=25530)).

[Unreleased]: https://github.com/libsndfile/libsndfile/compare/1.0.31...HEAD
