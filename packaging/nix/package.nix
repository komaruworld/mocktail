{
  lib,
  stdenv,
  fetchFromGitHub,

  cmake,
  git,
  lld,
  ninja,
  pkg-config,
  vulkan-headers,
  makeWrapper,

  capstone,
  curl,
  fontconfig,
  glib,
  glib-networking,
  gtk4,
  hicolor-icon-theme,
  libadwaita,
  elfutils,
  libglvnd,
  libplacebo,
  libsoup_3,
  libutf8proc,
  libyaml,
  minizip,
  nlohmann_json,
  openssl,
  sdl3,
  sdl3-ttf,
  vulkan-loader,
  webkitgtk_6_0,
  zlib,
}:
let
  # submodules are not part of the flake source tree
  # so these two vendored dependencies are fetched from their pinned upstream revisions
  libjnivmSrc = fetchFromGitHub {
    owner = "ChristopherHX";
    repo = "libjnivm";
    rev = "f24b98c198fcc5c59a68d1efadd3f5791eb01c2e";
    hash = "sha256-K4+1Zo7AJIouIqnHn2ZE4GyrNrYOczmWYblLAnGYA5w=";
  };
  vulkanHeadersSrc = fetchFromGitHub {
    owner = "KhronosGroup";
    repo = "Vulkan-Headers";
    rev = "8d6039a455a7ecc7d2a592ff97f62db4e59b70bf";
    hash = "sha256-+llJlEdzFcmqGBAdKHjYhkwBuH19ZngavmF1TQgejsI=";
  };

  src = ../..;

  lines = lib.splitString "\n" (builtins.readFile (lib.path.append src "CMakeLists.txt"));

  matches = map (builtins.match "[[:space:]]*VERSION[[:space:]]+([0-9][0-9.]*).*") lines;

  match = lib.findFirst (m: m != null) null matches;

  version =
    if match == null then throw "could not find VERSION in CMakeLists.txt" else builtins.elemAt match 0;
in
stdenv.mkDerivation (finalAttrs: {
  pname = "mocktail";
  inherit version;
  inherit src;

  nativeBuildInputs = [
    cmake
    git
    lld
    ninja
    pkg-config
    vulkan-headers
    makeWrapper
  ];

  buildInputs = [
    capstone
    curl
    fontconfig
    glib
    glib-networking
    gtk4
    hicolor-icon-theme
    libadwaita
    elfutils
    libglvnd
    libplacebo
    libsoup_3
    libutf8proc
    libyaml
    minizip
    nlohmann_json
    openssl
    sdl3
    sdl3-ttf
    vulkan-loader
    webkitgtk_6_0
    zlib
  ];

  preConfigure = ''
    rm -rf third_party/libjnivm third_party/Vulkan-Headers
    cp -r --no-preserve=mode ${libjnivmSrc} third_party/libjnivm
    cp -r --no-preserve=mode ${vulkanHeadersSrc} third_party/Vulkan-Headers
  '';

  # nixpkgs installs the header we need under include/libutf8proc
  NIX_CFLAGS_COMPILE = "-I${libutf8proc}/include/libutf8proc";

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DCMAKE_INSTALL_LIBDIR=lib"
    "-DMOCKTAIL_DEFAULT_COMPATIBILITY_MANIFEST=${placeholder "out"}/share/mocktail/metadata/roblox_compatibility.json"
    "-DMOCKTAIL_DEFAULT_SIGNING_TRUST_MANIFEST=${placeholder "out"}/share/mocktail/metadata/roblox_signing_certificates.json"
    "-DBUILD_TESTING=OFF"
  ];

  postInstall = ''
    for binary in $out/bin/*; do
    	wrapProgram "$binary" --prefix LD_LIBRARY_PATH : ${lib.makeLibraryPath finalAttrs.buildInputs} --prefix GIO_EXTRA_MODULES : "${glib-networking}/lib/gio/modules"
    done
  '';

  fixupPhase = ''
    install -Dm644 ${finalAttrs.src}/LICENSE "$out/share/licenses/${finalAttrs.pname}/LICENSE"
  '';

  meta = with lib; {
    description = "Android x86-64 Roblox compatibility runtime for Linux";
    homepage = "https://github.com/komaruworld/mocktail";
    license = licenses.asl20;
    platforms = platforms.unix;
    mainProgram = "mocktail";
  };
})
