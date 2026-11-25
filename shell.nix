{
  pkgs ? import <nixpkgs> { },
  withGL ? false,
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
    xorg-server
  ];

  buildInputs =
    with pkgs;
    [
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
    ]
    ++ lib.optionals withGL [
      libglvnd
      libGL
    ];
}
