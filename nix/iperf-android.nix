# Builds libiperf3.so for a single Android ABI with the NDK's clang.
#
# Result: $out/jniLibs/<abi>/libiperf3.so
{
  lib,
  stdenvNoCC,
  src,
  version,
  ndk,
  abi,
  apiLevel,
  libName ? "iperf3",
  pageSize ? 16384,
}:

let
  # ABI name (as used in jniLibs/) -> clang target prefix / autoconf host triple.
  targets = {
    "arm64-v8a" = "aarch64-linux-android";
    "armeabi-v7a" = "armv7a-linux-androideabi";
    "x86" = "i686-linux-android";
    "x86_64" = "x86_64-linux-android";
  };
  target = targets.${abi} or (throw "unsupported Android ABI: ${abi}");
  soname = "lib${libName}.so";
in
stdenvNoCC.mkDerivation {
  pname = "iperf3-android-${abi}";
  inherit version src;

  patches = [ ../patches/0001-omnt-run-iperf3-in-process.patch ];

  # The NDK ships prebuilt binaries; nothing in here should be patched for
  # the build host or wrapped by Nix's cc-wrapper.
  dontFixup = true;
  dontDisableStatic = true;

  configurePhase = ''
    runHook preConfigure

    ndkRoot="$(echo ${ndk}/libexec/android-sdk/ndk-bundle)"
    if [ ! -d "$ndkRoot" ]; then
      ndkRoot="$(echo ${ndk}/libexec/android-sdk/ndk/*)"
    fi
    toolchain="$(echo "$ndkRoot"/toolchains/llvm/prebuilt/*)"
    if [ ! -x "$toolchain/bin/clang" ]; then
      echo "could not locate the NDK LLVM toolchain under $ndkRoot" >&2
      exit 1
    fi
    echo "Using NDK toolchain: $toolchain"

    export CC="$toolchain/bin/${target}${toString apiLevel}-clang"
    export AR="$toolchain/bin/llvm-ar"
    export RANLIB="$toolchain/bin/llvm-ranlib"
    export NM="$toolchain/bin/llvm-nm"
    export STRIP="$toolchain/bin/llvm-strip"
    export READELF="$toolchain/bin/llvm-readelf"
    export CFLAGS="-O2 -g -fPIC -funwind-tables -D_GNU_SOURCE"

    ./configure \
      --host=${target} \
      --without-openssl \
      --without-sctp \
      --enable-static \
      --disable-shared \
      --with-pic

    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild

    # Reuse upstream's own source list for the library ...
    make -C src -j"$NIX_BUILD_CORES" libiperf.la

    # ... and add the OMNT JNI wrapper on top. It drives a test through
    # libiperf's API itself; upstream's main.c is not used.
    $CC $CFLAGS -DHAVE_CONFIG_H -Isrc -c ${../jni/iperf3_jni.c} -o iperf3_jni.o

    $CC -shared -o ${soname} \
      iperf3_jni.o src/.libs/libiperf.a \
      -llog -lm \
      -Wl,-soname,${soname} \
      -Wl,--no-undefined \
      -Wl,--build-id=sha1 \
      -Wl,-z,noexecstack \
      -Wl,-z,max-page-size=${toString pageSize}

    $STRIP --strip-unneeded ${soname}

    runHook postBuild
  '';

  doCheck = true;
  checkPhase = ''
    runHook preCheck

    # 1. Every PT_LOAD segment must be aligned to at least the page size
    #    (Android 15+ 16 KB page size requirement).
    $READELF -lW ${soname} | awk -v min=${toString pageSize} '
      $1 == "LOAD" {
        n++
        align = strtonum($NF)
        if (align < min) { printf "LOAD segment aligned to %s (< %d)\n", $NF, min; bad = 1 }
      }
      END {
        if (n == 0) { print "no LOAD segments found"; exit 1 }
        if (bad) exit 1
        printf "all %d LOAD segments aligned to >= %d bytes\n", n, min
      }'

    # 2. The JNI entry points OMNT calls must be exported.
    #    Capture nm's output first: piping it into `grep -q` makes nm die of
    #    SIGPIPE on an early match, which fails the check under pipefail.
    exported="$($NM -D --defined-only ${soname})"
    for sym in \
      Java_de_fraunhofer_fokus_OpenMobileNetworkToolkit_Iperf3_Worker_Iperf3ExecutorWorker_iperf3Run \
      Java_de_fraunhofer_fokus_OpenMobileNetworkToolkit_Iperf3_Worker_Iperf3ExecutorWorker_iperf3Stop
    do
      grep -qw "$sym" <<< "$exported" || { echo "missing JNI symbol $sym" >&2; exit 1; }
    done
    echo "JNI symbols present"

    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 ${soname} "$out/jniLibs/${abi}/${soname}"
    runHook postInstall
  '';

  meta = {
    description = "iperf3 ${version} as a JNI shared library for Android (${abi})";
    homepage = "https://github.com/omnt/iperf";
    license = lib.licenses.bsd3;
    platforms = [
      "x86_64-linux"
      "x86_64-darwin"
      "aarch64-darwin"
    ];
  };
}
