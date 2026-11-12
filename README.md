# iperf3 for OMNT (Android JNI)

This repository builds [iPerf3](https://github.com/esnet/iperf) as a JNI shared
library (`libiperf3.so`) for the
[OpenMobileNetworkToolkit](https://github.com/omnt/OpenMobileNetworkToolkit)
(OMNT) developed by Fraunhofer FOKUS.

It no longer carries a fork of the iperf sources. The build fetches the pristine
upstream release pinned in [`iperf-version.json`](iperf-version.json), applies
the small OMNT patch in [`patches/`](patches), adds the JNI wrapper from
[`jni/`](jni), and compiles everything with the Android NDK from nixpkgs.

* ABIs: `arm64-v8a`, `armeabi-v7a`, `x86`, `x86_64`
* Minimum API level: 29
* All `LOAD` segments are **16 KB page aligned**, as required for Android 15+
  devices. The build fails if they aren't.

## Building locally

You need [Nix](https://nixos.org/download/) with flakes enabled. If they aren't
enabled, add `--extra-experimental-features 'nix-command flakes'` to the
commands below.

```bash
nix build .#iperf3          # -> result/jniLibs/<abi>/libiperf3.so
nix build .#jniLibsZip      # -> result/jniLibs.zip (same as the release asset)
nix build .#iperf3-arm64-v8a  # a single ABI
```

`nix build` (without an attribute) is the same as `nix build .#iperf3`.

Each per-ABI build also checks that:

1. every `PT_LOAD` segment has an alignment of at least `0x4000` (16 KB), and
2. the JNI entry points OMNT calls are exported.

The Android NDK is unfree software. The flake accepts its license on your
behalf (`android_sdk.accept_license = true`).

## Using the library in OMNT

Download `jniLibs` from the
[latest release](https://github.com/omnt/iperf/releases/latest), or build it
yourself, and unpack it into `app/src/main`:

```
app/src/main/jniLibs/
├─ arm64-v8a/libiperf3.so
├─ armeabi-v7a/libiperf3.so
├─ x86/libiperf3.so
└─ x86_64/libiperf3.so
```

The library name no longer contains the iperf version, so the app doesn't need
to change when iperf is updated:

```java
System.loadLibrary("iperf3");
```

The JNI interface ([`jni/iperf3_jni.c`](jni/iperf3_jni.c)):

```java
// de.fraunhofer.fokus.OpenMobileNetworkToolkit.Iperf3.Worker.Iperf3ExecutorWorker
private native int iperf3Run(String[] argv);   // blocks until the test is done
private native void iperf3Stop();              // ends a running test early

// called from native code for every iperf3 --json-stream event
private void onIperf3Event(byte[] utf8Json);
```

`argv` is the iperf3 command line without the program name and must contain
`--json-stream`. `iperf3Run()` drives one test through libiperf's API (what
upstream `main()` + `run()` do) and returns iperf3's exit code. While the test
runs, every JSON stream line (`start`, `interval`, `end`, `error`, ...) is passed
to `onIperf3Event()` through libiperf's JSON callback, so no log file is
involved. The callback runs on iperf3's thread: hand the event off quickly.

Per call, the wrapper resets the state libiperf keeps in globals (getopt's
`optind`/`optreset`, `i_errno`), ignores `SIGPIPE` for the duration of the test
and forwards iperf3's stdout/stderr to logcat (tag `iperf3_stdio`). Only one
test can run per process at a time; OMNT runs each test in a worker process of
its own.

## Automatic updates and releases

* **`update-iperf.yml`** runs daily (and on demand) and looks for a newer
  `3.x` tag on esnet/iperf. When it finds one, it bumps `iperf-version.json`,
  updates `flake.lock` (newest NDK), builds all ABIs and opens a pull request.
  If the build fails, usually because the patch needs rebasing, the PR is opened
  as a draft.
* **`build.yml`** builds on every push and pull request and uploads `jniLibs.zip`
  as a workflow artifact. On `main`, if there is no release `v<version>` yet
  (for example right after merging an update PR), it creates that release with
  the `jniLibs` asset that OMNT's CI downloads. Pushing a tag `v<version>` or
  `v<version>-N` manually also creates a release.

To update by hand:

```bash
nix develop -c scripts/update-iperf.sh          # newest upstream tag
nix develop -c scripts/update-iperf.sh 3.22     # a specific version
nix build .#iperf3
```

## What the OMNT patch changes

OMNT runs iperf3 in-process from a worker, so libiperf must not call `exit()`:
not on its normal paths (`-v`, `-h`, usage errors, the one-off idle timeout)
and not on fatal errors (`iperf_errexit()`). The patch

* adds `src/omnt_exit.h`, which provides `IPERF_EXIT()`, and uses it instead of
  `exit()` in `iperf_api.c`, `iperf_error.c` and `iperf_server_api.c`. On
  Android it calls `iperf_exit_jump()`, which the JNI wrapper implements by
  `longjmp()`ing back to where it started the test; elsewhere it calls `exit()`
  as before.

Upstream's `main.c` is not used.

## Acknowledgments

- android-iperf - [GitHub](https://github.com/KnightWhoSayNi/android-iperf)
- iPerf - [GitHub](https://github.com/esnet/iperf)
- integrating iPerf - [matt9j.net](https://matt9j.net/posts/integrating-iperf/)
