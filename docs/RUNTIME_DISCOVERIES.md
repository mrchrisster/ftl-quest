# FTL: Quest 3 Port — Runtime Bring-Up & ABI Discoveries

**Target Binary:** `FTL` v1.6.21 ARM64 iOS Mach-O (`FTL_MODE_LOADER=ON`)  
**Test Device:** Meta Quest 3 (Android `arm64-v8a`)  
**Last Verified Build & Deploy:** Debug APK built with `gradle assembleDebug` and launched on device (`SILAppDelegate` completes launch and enters run loop without crashing).

---

## 1. Current Bring-Up Status (Observed on Quest 3)

Verified in `adb logcat` after launch (`com.subsetgames.ftl/.MainActivity`):

| Bring-Up Milestone | Status | Verified Telemetry / Notes |
| :--- | :--- | :--- |
| Mach-O In-Memory Map (`0x100000000`) | Passed | `__TEXT` / `__DATA` segments mapped preserving Mach-O header (`macho_loader.cpp`). |
| Dyld Bind Opcode Execution | Passed | **581 dyld binds** applied across bind / weak_bind / lazy_bind (`LC_DYLD_INFO_ONLY`). |
| ObjC Class Registration | Passed | 8 Mach-O classes registered (`DialogAlertViewDelegate`, `AudioNotificationHandler`, `OSKTextFieldDelegate`, `SILViewController`, `SILView`, `ControllerNotificationHandler`, `AuthChangeHandler`, `SILAppDelegate`). |
| C++ Static Initializers (`__mod_init_func`) | Passed | All **47 initializers** (`0x10000D790` .. `0x1001E2508`) execute cleanly. |
| Entry Point & App Launch | Passed | Enters `main()` (`0x10019AA3C`) &rarr; `UIApplicationMain` &rarr; `-[SILAppDelegate application:didFinishLaunchingWithOptions:]`. |
| GL Context Creation & Binding | Passed | `eagl_create_context(api=3)` + `eagl_set_current_context(...) -> 1 (err=0x0)` on GL thread. |
| Resource Archive (`ftl.dat`) Mount | Passed | `darwin_open('/sdcard/Android/data/com.subsetgames.ftl/files/ftl.dat', dflags=0x0) = 86`. |
| CoreAudio RemoteIO Bridge | Passed | `AURenderCallback` registered at `0x1000497BC`; audio worker thread started. |
| Launch Completion | Passed | Logs `"SILAppDelegate launch sequence completed. Entering run loop wait..."`; process remains alive. |
| Frame Swap (`eglSwapBuffers`) | Open | `eagl_present_renderbuffer` not yet observed (test run occurred with `mWakefulness=Asleep` and `OpenGLRenderer: setStopped(1)`). |
| Anomalous Heap Allocation | Open | Engine thread logged `W libc: malloc(502513117680) failed: returning null pointer`. |

---

## 2. Resolved Darwin vs. Android Bionic ABI Incompatibilities

Bringing the unencrypted ARM64 Mach-O binary directly onto Android Bionic exposed several silent ABI mismatches that cannot be solved by naive `dlsym` symbol forwarding:

### 2.1 Apple Alternate `std::__1::basic_string<char>` Layout (`apple_string_shim.cpp`)
- **Symptom:** `SIGSEGV` at bogus address `0x0200000000000000` inside `mod_init[8]` (`0x100083558`) when `libc++_shared.so` dereferenced Apple-compiled `std::string` objects.
- **Root Cause:** Apple ARM64 `libc++` uses `_LIBCPP_ABI_ALTERNATE_STRING_LAYOUT`: in a 24-byte `basic_string<char>`, byte `23` stores the short-string length (0..22) and the long-mode flag in bit 7 (`0x80`). Android NDK `libc++_shared.so` builds with `_LIBCPP_ABI_UNSTABLE`, where bit 0 of byte `0` indicates long mode.
- **Fix:** Intercept all **19 external `std::__1::basic_string<char>` methods** in [apple_string_shim.cpp](../shim/src/apple_string_shim.cpp) matching Apple's byte-23 SSO layout, while continuing to rewrite non-string `St3__1` C++ imports to `St6__ndk1` in `libc++_shared.so`.

### 2.2 `LC_DYLD_INFO_ONLY` Opcode Stream Binding & `___CFConstantStringClassReference` (`macho_loader.cpp`)
- **Finding:** External/internal pointer fixups in the FTL Mach-O rely heavily on `LC_DYLD_INFO_ONLY` opcode streams (`bind`, `weak_bind`, `lazy_bind`) rather than just simple GOT scans, plus internal `__DATA,__cfstring` pointers to `___CFConstantStringClassReference`, and `_OBJC_CLASS_$_*` / `_OBJC_METACLASS_$_*` references.
- **Fix:** Implemented `apply_dyld_bind_stream` with `bound_addrs` deduplication (applying **581 binds**), mapped `g_dummy_cfstring_ptrs` pointer indirection, translated `St3__1` &rarr; `St6__ndk1`, and installed `dyld_stub_binder_trap` diagnostics.

### 2.3 `PTHREAD_ONCE_INIT` & Mutex Attribute Constants (`darwin_abi.cpp`)
- **Finding 1 (`pthread_once`):** Apple defines `PTHREAD_ONCE_INIT` as `{0x30B1BCBA, {0}}`, whereas Bionic treats any non-zero first word (`!= 0`) as already completed. Passing Darwin statically initialized `pthread_once_t` globals directly to Bionic caused SIL / game subsystems to skip one-time initialization completely.
- **Finding 2 (`pthread_mutexattr_settype`):** Darwin swaps mutex type values relative to Bionic (`PTHREAD_MUTEX_ERRORCHECK = 1`, `PTHREAD_MUTEX_RECURSIVE = 2` on Darwin vs `NORMAL = 0`, `RECURSIVE = 1`, `ERRORCHECK = 2` on Bionic).
- **Finding 3 (Static initializers & thread helpers):** Darwin static initializers (`PTHREAD_MUTEX_INITIALIZER = 0x32AAABA7/8/9`, `PTHREAD_COND_INITIALIZER = 0x3CB0B1BB`) are unrecognized by Bionic unless lazily initialized; Darwin `pthread_setname_np` takes 1 argument (`const char*`) instead of 2; and `pthread_key_create` writes a 64-bit key slot.
- **Fix:** Wrapped `darwin_pthread_once`, `darwin_pthread_mutexattr_settype`, lazy mutex/cond initializers, `darwin_pthread_setname_np`, `darwin_pthread_key_create`, and mutex/cond/rwlock entry points in [darwin_abi.cpp](../shim/src/darwin_abi.cpp).

### 2.4 192-Byte `jmp_buf` vs. 256-Byte Bionic `jmp_buf` (`objc_msgSend_arm64.S`)
- **Finding:** Darwin ARM64 `jmp_buf` (used by `setjmp`/`longjmp`, e.g., inside libpng / C error unwinds) is **192 bytes**, while Bionic's `jmp_buf` is **256 bytes** (`_JBLEN=32` `long`s). Calling Bionic `setjmp` directly overwrites 64 bytes past caller-allocated `jmp_buf` storage.
- **Fix:** Implemented custom 192-byte-safe `darwin_setjmp` and `darwin_longjmp` in AArch64 assembly ([objc_msgSend_arm64.S](../shim/src/objc_msgSend_arm64.S)).

### 2.5 Filesystem, `dirent64`, `dlsym`, & `sysconf` Translation (`darwin_abi.cpp`)
- **`open` / `fcntl`:** Translates Darwin file open/descriptor flags (`O_CREAT`, `O_TRUNC`, `O_EXCL`, `O_NONBLOCK`, `O_APPEND`, etc.) and VFS paths before invoking Bionic syscalls.
- **`opendir` / `readdir` / `closedir`:** Translates Bionic `dirent` records into `darwin_dirent64` layout (`d_name` at offset `21` vs Bionic offset `19`).
- **`dlsym` / `sysconf`:** Maps Darwin `RTLD_DEFAULT (-2)` / `RTLD_NEXT (-1)` and Darwin `_SC_PAGESIZE (29)` to Android Bionic equivalents.
- **`CFBundleGetMainBundle` / `CFBundleGetInfoDictionary`:** Replaced raw integer sentinels (`0x4000` / `0x4010`, which previously triggered `BAD ObjC self=0x4010 sel='objectForKey:' caller=0x1000b8e6c`) with real `NSBundle` and populated `NSMutableDictionary` instances (`CFBundleDisplayName`, `CFBundleName`, `CFBundleIdentifier`, `CFBundleShortVersionString`, `CFBundleVersion`).

### 2.6 Per-Thread `EAGLContext` Binding (`eagl_shim.cpp`)
- **Symptom:** Short-lived SIL loader threads spawned during startup (`player-list.txt`, `settings.ini`, `continue.sav`, `prof.sav`) call `[EAGLContext setCurrentContext:nil]` during thread teardown (`0x10019AD30` &rarr; `0x10017EDA8`), which previously cleared a single process-global `g_active_context_ptr`.
- **Fix:** Replaced `g_active_context_ptr` with `__thread EAGLContextRef g_tls_eagl_context = nullptr` in [eagl_shim.cpp](../shim/src/eagl_shim.cpp) so background threads unbind only their own EGL context (`EGL_NO_SURFACE`/`EGL_NO_CONTEXT`).

### 2.7 Objective-C Runtime & Stub Additions (`objc_shim.cpp`, `objc_msgSend_arm64.S`)
- Updated AArch64 `objc_msgSend` to pass `caller_lr` (`mov x2, x30`) to `objc_lookup_imp_for_send`, guarded invalid `self`/`isa` pointers (`< 0x100000`), and deduplicated unhandled-selector logging.
- Added missing `UIKit`/`CAEAGLLayer` setters (`setIdleTimerDisabled:`, `setContentScaleFactor:`, `setMultipleTouchEnabled:`, `setOpaque:`, `setDrawableProperties:`, `setRootViewController:`, `makeKeyAndVisible`).
- Added `NSNotificationCenter` observer stubs (`addObserver:selector:name:object:`, `removeObserver:`, `removeObserver:name:object:`, `postNotificationName:object:`, `postNotificationName:object:userInfo:`) and `NSUserDefaults` (`registerDefaults:`, `integerForKey:`, `stringForKey:`, `setBool:forKey:`, `setInteger:forKey:`) + `NSNumber` boolean/integer helpers.

---

## 3. Known Open Issues & Next Diagnostic Targets

1. **Anomalous `malloc(502513117680)` (`0x75...` Heap Address Passed as Size):**
   - Observed on engine/GL worker thread right before launch completion (`W libc: malloc(502513117680) failed: returning null pointer`). The magnitude falls inside Android's 40-bit heap VA range (`0x7500000000`), strongly suggesting a mismatched return value / pointer-vs-size ABI mismatch from an external call or struct layout.
2. **Frame Presentation (`-[SILView present]` / `eagl_present_renderbuffer`) Not Yet Observed:**
   - Recent `adb` launch ran while the Quest 3 headset was asleep (`mWakefulness=Asleep`) and Android UI reported `OpenGLRenderer: setStopped(1)`.
   - Needs re-testing with the headset awake and focused (`SurfaceView` active), plus verification that `CADisplayLink` callbacks (`-[SILView vsync_callback]`) schedule and reach `-[SILView present]` (`0x1000D3A60`).
3. **Minor Unhandled Selectors & GL Probes:**
   - `-[GKLocalPlayer authenticateWithCompletionHandler:]` (`caller=0x1000B9044`) logs once as unhandled (GameKit sign-in can safely invoke completion or no-op).
   - Unresolved OpenGL 4.5 DSA function probes (`_glCreateTextures`, `_glTextureStorage2D`, `_glBindTextureUnit`, etc.) occur during SIL OpenGL feature detection and are expected to fall back to standard GLES3 entry points.
