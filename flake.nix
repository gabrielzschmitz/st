{
  description = "Custom st terminal build";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.callPackage ./default.nix { };
          st = self.packages.${system}.default;
          stGL = pkgs.callPackage ./default.nix { withGL = true; };
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          mkDevShell =
            withGL:
            pkgs.mkShell {
              inputsFrom = [
                (if withGL then self.packages.${system}.stGL else self.packages.${system}.default)
              ];
              nativeBuildInputs = with pkgs; [
                pkg-config
                ncurses
                gnumake
                gcc
                bear
                clang-tools
                python3
                xorg-server
              ];
            };
        in
        {
          default = mkDevShell false;
          gl = mkDevShell true;
        }
      );
    };
}
