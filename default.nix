{
  lib,
  stdenv,
  pkg-config,
  ncurses,
  fontconfig,
  freetype,
  harfbuzz,
  imlib2,
  libX11,
  libXext,
  libXrender,
  libXft,
}:

stdenv.mkDerivation {
  pname = "st";
  version = "0.9.3";

  src = ./src;

  nativeBuildInputs = [
    pkg-config
    ncurses
  ];

  buildInputs = [
    fontconfig
    freetype
    harfbuzz
    imlib2
    libX11
    libXext
    libXrender
    libXft
  ];

  installPhase = ''
    runHook preInstall

    mkdir -p $out/bin
    cp st $out/bin/
    chmod 755 $out/bin/st

    mkdir -p $out/share/man/man1
    sed "s/VERSION/$version/g" < st.1 > $out/share/man/man1/st.1
    chmod 644 $out/share/man/man1/st.1

    mkdir -p $out/share/terminfo
    TERMINFO=$out/share/terminfo tic -sx st.info

    mkdir -p $out/share/applications
    cp st.desktop $out/share/applications/st.desktop

    mkdir -p $out/share/icons/hicolor/scalable/apps
    cp st.svg $out/share/icons/hicolor/scalable/apps/st.svg

    runHook postInstall
  '';

  meta = {
    mainProgram = "st";
    description = "Custom build of the suckless simple terminal by gabrielzschmitz";
    longDescription = ''
      st (simple terminal) is a lightweight and minimalist terminal emulator
      developed by the suckless community. This package provides a custom build
      maintained by gabrielzschmitz, with modifications and configuration
      tailored to personal preferences.
    '';
    homepage = "https://github.com/gabrielzschmitz/st";
    license = lib.licenses.mit;
    maintainers = with lib.maintainers; [ gabrielzschmitz ];
    platforms = lib.platforms.linux;
  };
}
