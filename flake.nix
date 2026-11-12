{
  description = "iperf3 as a 16 KB page aligned JNI library for OpenMobileNetworkToolkit (OMNT)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      # Hosts the Android NDK ships prebuilt toolchains for.
      systems = [
        "x86_64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f (pkgsFor system));

      pkgsFor =
        system:
        import nixpkgs {
          inherit system;
          # The Android NDK is unfree and requires accepting its license.
          config = {
            allowUnfree = true;
            android_sdk.accept_license = true;
          };
        };

      pin = builtins.fromJSON (builtins.readFile ./iperf-version.json);

      abis = [
        "arm64-v8a"
        "armeabi-v7a"
        "x86"
        "x86_64"
      ];

      # Matches APP_PLATFORM of the previous ndk-build setup.
      apiLevel = 29;
    in
    {
      packages = forAllSystems (
        pkgs:
        let
          inherit (pkgs) lib;

          src = pkgs.fetchFromGitHub {
            owner = "esnet";
            repo = "iperf";
            tag = pin.version;
            inherit (pin) hash;
          };

          ndk = pkgs.androidenv.androidPkgs.ndk-bundle;

          perAbi = lib.genAttrs abis (
            abi:
            pkgs.callPackage ./nix/iperf-android.nix {
              inherit src ndk abi apiLevel;
              inherit (pin) version;
            }
          );

          iperf3 = pkgs.symlinkJoin {
            name = "iperf3-android-${pin.version}";
            paths = builtins.attrValues perAbi;
            passthru = { inherit (pin) version; } // perAbi;
          };

          # jniLibs.zip in the layout OMNT's CI unpacks into app/src/main.
          jniLibsZip = pkgs.runCommand "iperf3-android-${pin.version}-jniLibs.zip" {
            nativeBuildInputs = [ pkgs.zip ];
          } ''
            cp -rL ${iperf3}/jniLibs jniLibs
            chmod -R u+w jniLibs
            find jniLibs -exec touch -h -d '1980-01-02T00:00:00Z' {} +
            mkdir -p $out
            find jniLibs -print | LC_ALL=C sort | zip -X -q -D $out/jniLibs.zip -@
          '';
        in
        {
          inherit iperf3 jniLibsZip;
          default = iperf3;
        }
        // lib.mapAttrs' (abi: drv: lib.nameValuePair "iperf3-${abi}" drv) perAbi
      );

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = [
            pkgs.jq
            pkgs.gh
          ];
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt-rfc-style);
    };
}
