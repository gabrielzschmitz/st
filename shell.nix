{
  pkgs ? import <nixpkgs> { },
}:

pkgs.mkShell {
  name = "st-dev";

  nativeBuildInputs = with pkgs; [
    pkg-config
    ncurses
    gnumake
    gcc
    bear
    clang-tools
    python3
    xorg.xorgserver
  ];

  buildInputs = with pkgs; [
    fontconfig
    freetype
    harfbuzz
    imlib2
    libX11
    libXcursor
    libXext
    libXrender
    libXft
    ncurses
  ];
}
