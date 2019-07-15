# History of changes for LIBCx

#### Version 0.6.6 (2019-07-15)

* Handle ERROR_INTERRUPT from Dos calls by retrying [#39].

#### Version 0.6.5 (2019-03-29)

* mmap: Implement thread-safe concurrent access to file-based mappings [#68].

#### Version 0.6.4 (2018-12-31)

* mmap: Fix returning EOVERFLOW on files with sizes greater than 4G.
* mmap: Return EBADF instead of EINVAL when fd is -1 and no MAP_ANON.
* Fix installing EXCEPTQ on LIBC threads (started with _beginthread) in release builds.
* Add automatic FPU exception handler installation to recover from crashes due to unexpected changes of FPU CW by bogus Win/Gpi APIs.
* spawn2: Return PID of wrapped child instead of wrapper in P_2_THREADSAFE mode.
* spawn2: Implement P_SESSION support (including P_PM and other related flags from EMX).
* Add BLDLEVEL info to LIBCx DLL and tools.

#### Version 0.6.3 (2018-09-11)

* Implement `getaddrinfo` and `getnameinfo` family APIs.
* Implement `if_nameindex` and `if_nametointex` family APIs.
* Implement `getifaddrs` family API.

#### Version 0.6.2 (2018-04-17)

* spawn2: Fix crash in P_2_THREADSAFE mode when envp is not NULL.
* spawn2: Add support for P_2_APPENDENV.
* spawn2: Support special pseudo-environment variables (BEGINLIBPATH, ENDLIBPATH, LIBPATHSTRICT).
* spawn2: Accept 1 as stdfds[1] and 2 as stdfds[2] values.
* spawn2: Fix unexpected override of passed envp vars with current ones.

#### Version 0.6.1 (2017-12-27)
* Implement the `spawn2` API that provides standard I/O redirection and thread safety.
* fcntl: Allow to place write locks on files open in r/o mode to conform with POSIX.
* Improve tracking of open files between processes to fix fcntl lock failures in Samba4 tests and similar cases.
* mmap: Fix assertion in msync() on PAG_WRITE-only mmap.
* Force high memory for default C heap to make sure high memory is used even if some DLL (mistakenly) votes for low memory.

#### Version 0.6.0 (2017-08-29)

* Implement the `exeinfo` API that allows to examine an executable or a DLL without actually loading it for execution by the OS/2 kernel.
* Write LIBCx assert messages to a per-process log file in `/@unixroot/var/log/libcx` to simplify tracking of critical bugs at runtime.
* mmap: Increase the file handle limit to avoid premature ENOMEM errors when mapping many files at the same time  (like `git` is used to do).
* Implement tracking LIBC file descriptors in LIBCx to free LIBCx resources when respective files are closed by LIBC. This drops memory usage dramatically (200 times and more) in heavy usage scenraios like reading tens thousand files over an OS/2 Samba share.
* libcx-stats: Print internal LIBCx structure usage information.

#### Version 0.5.3 (2017-06-02)

* Add workaround for another `DosRead` bug that leads to hard system freezes on JFS when reading big files at once.
* Add 'fread()` override to incorporate all 'DosRead` workarounds applied to `read()` and other calls.

#### Version 0.5.2 (2017-03-27)

* EXPERIMENTAL. Make setting Unix user ID at startup disabled by default.

#### Version 0.5.1 (2017-03-24)

* Make streams bound to TCP sockets properly flushed at program exit.
* EXPERIMENTAL. Set Unix user ID at startup to what is set in LOGNAME/USER envvar.

#### Version 0.5.0 (2017-03-10)

* mmap: Support overlapped mappings (needed for e.g. git and BerkleyDB).
* mmap: Significantly reduce memory footprint when mapping files in chunks.
* libcx-stats: Print LIBCx version and DLL path information.

#### Version 0.4.1 (2017-01-18)

* select: Don't reset fd sets on failure (this fixes odd behavior of cmake).
* Improve the workaround for the `DosRead` bug (see version 0.4 changelog) to only touch uncommitted pages and to not modify their contents (this fixes crashes in cmake).

#### Version 0.4 (2016-11-24)

* mmap: Add support for mapping beyond EOF for non-anonymous mappings.
* mmap: Implement instant syncronization between two mappings of the same file.
* mmap: Support partial/multiple regions in `munmap()`, `msync()`, `madvise()` and `mprotect()` as requred by POSIX.
* mmap: Fix race when another process sets needed permissions on shared memory.
* mmap: Optimize LIBCx global memory pool usage.
* poll: Return EFAULT if first argument is NULL (not required by POSIX but many platforms do that).
* Add `read()`, `__read()`, `_stream_read()` and `DosRead()` overrides that fix a known `DosRead` bug when it fails if interrupted by an exception handler and returns garbage.
* Make `pread()` use the fixed `DosRead` call to avoid returning EINVAL on mapped memory.

#### Version 0.3.1 (2016-09-26)

* mmap: Fix crashes when reading from PROT_READ mappings bound to files.

#### Version 0.3 (2016-09-22)

* Automatically install EXCEPTQ trap report generator (if available) on all application threads.
* Support trace groups in debug builds (controlled with LIBCX_TRACE environment var).
* Add complete POSIX `mmap()` implementation (also includes `msync()`, `munmap()`, `mprotect()`, `madvise()`, `posix_madvise()`).
* Provide `libcx.a` import library in addition to `libcxN.a`.

#### Version 0.2.1 (2016-08-19)

* Don't prevent real close from be called after the close hook (also caused failed return values).
* Fix committed memory overgrowth under heavy load (caused ENOMEM in fcntl locks).

#### Version 0.2 (2016-07-18)

* fcntl: Implement joining adjacent lock regions which greatly reduces memory footprint.
* Implement dynamic memory allocation for internal data (currently limited to 2MB maximum).
* Add select() implementation that supports regular files.
* Add poll() emulation using select() including support for POLLRDNORM and friends.
* Add libcx-stats.exe to print live memory usage statistics.
* fcntl: Implement releasing all file locks of a process on file close.

#### Version 0.1 (2016-06-10)

* Initial release.
