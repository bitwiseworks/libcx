# LIBCx - kLIBC Extension Library

The kLIBC Extension Library extends the functionality of the [kLIBC library](http://trac.netlabs.org/libc) by adding a number of high demand features required by modern applications. The kLIBC library is not actively maintained any more and extending it with the needed functionality is not always a trivial task. LIBCx is a good place to deploy such extensions as it does not require touching complex kLIBC internals and therefore cannot introduce new bugs and regressions in a sensitive piece of software the base C runtime library naturally is.

It is no doubt that all the functionality provided by LIBCx logically belongs to the C runtime and should eventually migrate to kLIBC (or to its probable successor) and this is the final goal of this project. Until then, applications should be manually linked with LIBCx (in addition to the implicit linking with kLIBC performed by the GCC compiler under the hood) in order to use all the implemented extensions.

Currently, LIBCx provides the following extensions:

 - Improved advisory file locking using the `fcntl()` API. The implementation provided by kLIBC uses `DosSetFileLocks` and is broken as it does not guarantee atomicity of lock/unlock operations in many cases (like overlapping lock regions etc.) and does not have deadlock detection.
 - Improved positional read/write operations provided by `pread()` and `pwrite()` APIs that guarantee atomic behavior. kLIBC emulates these functions using a pair of `lseek` and `read()/write()` calls in non-atomic manner which leads to data corruption when accessing the same file from multiple threads or processes.
 - Improved `select()` that now supports regular file descriptors instead of returning EINVAL (22) on them as kLIBC does. Regular files are always reported ready for writing/reading/exceptions (as per POSIX requirements).
 - Implementation of `poll()` using `select()`. kLIBC does not provide the `poll()` call at all.
 - Implementation of POSIX memory mapped files via the `mmap()` API (declared in `sys/mman.h`).
 - Automatic installation of the EXCEPTQ exception handler on the main thread of the executable (prior to calling `main()`) as well as on any additional thread created with `_beginthread()` (prior to calling the thread function). EXCEPTQ is a nice piece of software that creates .TRP files containing a lot of useful technical information whenever an application crashes which helps developers to effectively find and fix various kinds of bugs related to unexpected program termination.
 - Improved `read()`, `__read()`, `_stream_read()`, `fread()` and `DosRead()` calls with workarounds for the OS/2 `DosRead` bug that can cause it to return a weird error code resulting in EINVAL (22) in applications (see https://github.com/bitwiseworks/libcx/issues/21 for more information) and for another `DosRead` bug that can lead to system freezes when reading big files on the JFS file system (see https://github.com/bitwiseworks/libcx/issues/36 for more information).
 - Improved flushing of open streams at program termination. In particular, buffered streams bound to TCP/IP sockets are now properly flushed so that no data loss occurs on the receiving end. TCP/IP sockets are used instead of pipes in many OS/2 ports of Unix software for piping child process output to the parent (due to the limitation of kLIBC select() that doesn't support OS/2 native pipes).
 - New `exeinfo` API that allows to examine an executable or a DLL without actually loading it for execution by the OS/2 kernel.
 - EXPERIMENTAL. Automatic setting of the Unix user ID at process startup to an ID of a user specified with the `LOGNAME` or `USER` environment variable if a match in the passwd database is found for it. This has a numbef of side effects, e.g. all files and directories created by kLIBC functions will have an UID and GID of the specified user rathar than root. Also Unix programs will see the correct user via getuid() and other APIs which in particular will make some tools (e.g. yum) complain about the lack of root priveleges. For this reason this functionality is disabled by default and can be enabled by setting `LIBCX_SETUID=1` in the environment.

## Notes on `mmap()` usage

The `mmap()` implementation needs a special exception handler on every thread of the executable (including thread 1).  There is no legal way to install such a handler in kLIBC other than do it manually from `main()` and from each thread's main function which is beyond `mmap()` specification (and may require quite a lot of additional code in the sources). LIBCx solves this problem by overriding some internal LIBC functions (in particular, `__init_app`) as well as `_beginthread`. If you use LIBCx `mmap()` in your executable, everything is fine since you will build it against LIBCx and the right functions will be picked up from the LIBCx DLL. But if you use LIBCx `mmap()` in a separate DLL (e.g. some 3rd party library), it will not work properly unless you also rebuild all the executables using this DLL against LIBCx to pick up the necessary overrides (see also [Notes on EXCEPTQ usage](#notes-on-exceptq-usage)).

Please note that it is not recommended to install any additional OS/2 system exception handlers in the application code if it uses `mmap()`. Extra care should be taken if such a handler is installed. In particular, all exceptions not explicitly handled by this handler (especially XCPT_ACCESS_VIOLATION) must be passed down to other exception handlers by returning XCPT_CONTINUE_SEARCH for them.

Both anonymous and file-bound memory mappings are supported by LIBCx. For shared mappings bound to files LIBCx implements automatic asynchronous updates of the underlying file when memory within such a mapping is modified by an application. These updates happen with a one second delay to avoid triggering expensive file write operations after each single byte change (imagine a `memcpy()` cycle) and still have up-to-date file contents (which is important in case of a power failure or another abnormal situation leading to an unexpected reboot or system hang). Note though that changed memory is always flushed to the underlying file when the mappig is unmapped with `munmap()` or when the process is terminated (even abnormally with a crash). If an immediate update of the file with current memory contents is needed prior to unmapping the respective region (for instance, to read the file in another application), use the `msync()` function with the MS_SYNC flag.

Shared mappings in two different and possibly unrelated processes that are bound to the same file will always access the same shared memory region (as if it were a mapping inherited by a forked child). This allows two unrelated processes instantly see each other's changes. This behavior is not documented by POSIX but many Linux and BSD systems implement it  too and it is used in real life by some applications (like Samba).

Note that the MAP_FIXED flag is not currently supported by `mmap()` becaues there is no easy way to ask OS/2 to allocate memory at a given address. There is a subset of MAP_FIXED functionality that can be technically implemented on OS/2 and this implementation may be added later once there is an application that really needs it. See https://github.com/bitwiseworks/libcx/issues/19 for more information.

The `msync()` function supports two flags: MS_SYNC and MS_ASYNC. MS_SYNC causes the changes to be immediately written to the file on the current thread and MS_ASYNC simply forces the asynchronous flush operation to happen now instead of waiting for the current update delay inteval to end. The MS_INVALIDATE flag is not supported (silently ignored).

The `mprotect()` function overrides the kLIBC function of the same name (the only function from `sys/mman.h` ever implemented by kLIBC) in order to make sure that changing memory protection doesn not break functionality of mappings created with `mmap()`. Currently it has the following limitations:

 - It supports anonymous mappings only and will fail with EACCES if a mapping bound to a file is encountered within a requested region. Note that if the requested region does not intesect with any mapped regions at all, then the call is simply passed down to the kLIBC implementation of `mprotect()`.
 - It cannot change protection to PROT_NONE on shared mappings (and any shared memory regions, in fact) and will fail with EINVAL if such a region is encountered. This is a limitation of OS/2 itself that does not allow to decommit a shared page and doesn't provide any other way to mark that the page has no read and no write access. This limitation is also present in kLIBC `mprotect()`.

The `madvise()` function is mostly a no-op as the OS/2 kernel doesn't accept any advices about use of memory by an application. The only implemented advice is MADV_DONTNEED which changes memory access semantics. This is advice is supported for private mappings in which case it causes the requested pages to be decommitted (and automatically committed again upon next access and initialized to zeroes for anonymous mappings or to up-to-date file contents for file mappings). Shared mappings are not supported due to limitations of the shared memory management on OS/2 (in particular, it's impossible to decommit a shared memory page or somehow unmap it from a given process or even remove the PAG_READ attribute from it which is necessary to cause a system exception upon next access).

The `posix_madvise()` function is also implemented but it simply returns 0 in all cases (including the POSIX_MADV_DONTNEED advice as it has different semantics, not compatible with MADV_DONTNEED).

## Notes on EXCEPTQ usage

The EXEPTQ support is enabled automatically whenever an application is statically linked against the LIBCx DLL or its import library. There is no need to call any of the LIBCx functions to turn it on and there is currently no way to turn it off (other than call the `SetExceptqOptions` function from `exceptq.dll` directly). Any manual EXCEPTQ installation should be removed from the application code if it is linked against LIBCx to avoid double processing and other curious side effects.

Note that LIBCx doesn't statically link to any of the EXCEPTQ DLLs. It loads them dynamically at program startup. If `exceptq.dll` (or any of its dependencies) cannot be found at startup, the EXCEPTQ support provided by LIBCx will be completely disabled.

## Notes on `exeinfo` API usage.

The key difference of the `exeinfo` API from the "traditional" OS/2 `DosGetResource` API is that it doesn't require to load the executable file with `DosLoadModule` first in order to read its resources — instead, file contents is read and parsed directly by LIBCx. Besides saving some resources that would otherwise be alocated by OS/2 for loading the executable file into system memory for execution, such a direct approach also eliminates a call to `_DLL_InitTerm` in the loaded DLL that could execute arbitrary and potentially dangerous code, as well as it eliminates searching for and loading all dependend DLLs. This is both faster and much more secure.

The `exeinfo` API is defined in the `libcx/exeinfo.h` header and currently allows to query the executable file format and read OS/2 resource objects embedded in such a file. More functionality is planned for future versions.

## Build instructions

### Using LIBCx in your applications

The easiest and the only officially supported way to use LIBCx in your application is to use a binary build provided by bitwise. In the RPM/YUM environment (see the next section) this may be easily achieved by running `yum install libcx-devel` from the command line and then adding `-lcx` to your linker options. If you take a ZIP version from [bitwise ZIP archives](http://rpm.netlabs.org/release/00/zip), you will have to resolve all possible dependencies yourself, set proper include paths and link to `libcx.a` by full name.

### Building LIBCx

The development of LIBCx, as well as all other projects maintained by bitwise, relies on the [RPM/YUM environment for OS/2](http://trac.netlabs.org/rpm/wiki) (also maintained by us). This environment, in particular, builds up a UNIXROOT environment on the OS/2 machine and provides recent versions of the Unix tool chain (including the GCC 4.x.x compiler) ported to OS/2, as well as a great number of open-source libraries and a set of OS/2-specific tools needed for the development process. So, in order to build LIBCx, you will need to do the following:

 1. Install [RPM/YUM](http://trac.netlabs.org/rpm/wiki/RpmInstall) on your existing OS/2 system or upgrade it to [ArcaOS](https://www.arcanoae.com/arcaos/) which comes with RPM/YUM pre-installed.
 2. Install the basic tool chain with `yum install gcc gcc-wlink gcc-wrc libc-devel kbuild kbuild-make rpm-build`.
 3. Inspect [libcx.spec](http://trac.netlabs.org/rpm/browser/spec/trunk/SPECS/libcx.spec) for specific build requirements and build steps which you can either complete automatically with `rpmbuild libcx.spec -bb` or manually from the command line.
