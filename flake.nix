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
              license = licenses.gpl3Plus;
              platforms = platforms.linux;
              mainProgram = "pluto";
            };
          };

          pluto-xwayland = pluto.overrideAttrs (old: {
            pname = "pluto-xwayland";
            nativeBuildInputs = (old.nativeBuildInputs or []) ++ [ pkgs.makeWrapper ];
            buildInputs = (old.buildInputs or []) ++ [ pkgs.libxcb-wm pkgs.xwayland ];
            preBuild = (old.preBuild or "") + ''
              export XWAYLAND=-DXWAYLAND
              export XLIBS="xcb xcb-icccm"
            '';
            postInstall = (old.postInstall or "") + ''
              wrapProgram $out/bin/pluto \
                --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.xwayland ]}
            '';
          });
        in {
          pluto = pluto;
          pluto-xwayland = pluto-xwayland;
          default = pluto;
        });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.pluto ];
          packages = with pkgs; [
            clang-tools
            foot
            gdb
            libxcb-wm
            xwayland
          ];
        };
      });

      checks = forAllSystems (pkgs: {
        inherit (self.packages.${pkgs.stdenv.hostPlatform.system}) pluto;
      });
    };
}
