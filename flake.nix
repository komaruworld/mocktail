{
  description = "Experimental Roblox compatibility runtime for Linux and FreeBSD";

  inputs = {
    flake-parts.url = "github:hercules-ci/flake-parts";
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    inputs@{ flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      imports = [

      ];
      systems = [
        "x86_64-linux"
        # "aarch64-linux"
        # "aarch64-darwin"
        # "x86_64-darwin"
      ];
      perSystem =
        {
          pkgs,
          ...
        }:
        let
          package = pkgs.callPackage ./packaging/nix/package.nix { };
        in
        {
          packages.default = package;
          devShells.default = pkgs.mkShell {
            inputsFrom = [ package ];

            # nixpkgs installs the header we need under include/libutf8proc
            NIX_CFLAGS_COMPILE = "-I${pkgs.libutf8proc}/include/libutf8proc";

            nativeBuildInputs = with pkgs; [
              cmake
              git
              lld
              ninja
              pkg-config
              vulkan-headers

              llvmPackages_22.clang-tools
              llvmPackages_22.libclang

              # debugging
              gdb
            ];

            shellHook = ''
              export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:${pkgs.lib.makeLibraryPath package.buildInputs}"

              # nix packages graphics drivers differently, and mocktail will still work if it cannot guarantee symbols
              export MOCKTAIL_REQUIRE_REAL_GRAPHICS=0
            '';
          };
        };
      flake = { };
    };
}
