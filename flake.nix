{
  description = "Pluto: a small, modular tiling Wayland compositor";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in {
      packages = forAllSystems (pkgs:
        let
          pluto = pkgs.stdenv.mkDerivation {
            pname = "pluto";
            version = "0.1.0";
            src = self;

            nativeBuildInputs = with pkgs; [
              pkg-config
              wayland-scanner
            ];

            buildInputs = with pkgs; [
              wlroots
              wayland
              wayland-protocols
              libinput
              libxkbcommon
              pixman
              libdrm
              udev
              libxcb-wm
            ];

            makeFlags = [
              "PREFIX=$(out)"
              "MANDIR=$(out)/share/man"
              "DATADIR=$(out)/share"
            ];

            doCheck = true;
            checkPhase = ''
              runHook preCheck
              make test
              runHook postCheck
            '';

            meta = with pkgs.lib; {
              description = "Small, modular dynamic tiling Wayland compositor";
              homepage = "https://github.com/plutowm/pluto";
              license = licenses.gpl3Plus;
              platforms = platforms.linux;
              mainProgram = "pluto";
            };
          };
        in {
          inherit pluto;
          default = pluto;
        });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.pluto ];
          packages = with pkgs; [
            clang-tools
            gdb
          ];
        };
      });

      checks = forAllSystems (pkgs: {
        inherit (self.packages.${pkgs.stdenv.hostPlatform.system}) pluto;
      });
    };
}
