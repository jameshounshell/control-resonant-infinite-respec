{
  description = "CONTROL Resonant Infinite Respec: reproducible MinGW-w64 build";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      pkgs = nixpkgs.legacyPackages.x86_64-linux;
      win = pkgs.pkgsCross.mingwW64;
    in {
      packages.x86_64-linux.default = win.stdenv.mkDerivation {
        pname = "control-resonant-infinite-respec";
        version = "1.3.0";
        src = ./.;
        # No C runtime is linked, so the hardening flags' runtime support (libssp) is absent.
        hardeningDisable = [ "all" ];
        dontStrip = true;
        makeFlags = [ "CC=${win.stdenv.cc.targetPrefix}gcc" ];
        installPhase = ''
          mkdir -p $out
          cp ControlInfiniteRespec.dll $out/
        '';
      };
    };
}
