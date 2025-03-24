# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

{
  description = "QSoC - Quick System on Chip Studio";

  inputs = {
    nixpkgs.url = "nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgName = "qsoc";
        pkgs = import nixpkgs { inherit system; };
      in {
        packages.${pkgName} = pkgs.stdenv.mkDerivation {
          pname = pkgName;
          # This is a placeholder that will be overridden
          version = "0.0.0";

          # Using local path
          src = ./.;

          # Extract version from config.h
          preConfigure = ''
            if [ -f "$src/src/common/config.h" ]; then
              VERSION=$(grep 'QSOC_VERSION' "$src/src/common/config.h" | grep -o '"[0-9]*\.[0-9]*\.[0-9]*"' | tr -d '"')
              if [ ! -z "$VERSION" ]; then
                echo "Extracted version from config.h: $VERSION"
                # Override the package version with the extracted one
                export configureFlags="''${configureFlags} -DPROJECT_VERSION=$VERSION"
              else
                echo "Could not extract version from config.h, using default"
              fi
            else
              echo "config.h not found, using default version"
            fi
          '';

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            qt6.wrapQtAppsHook
            qt6.qttools # For Qt Linguist tools
          ];

          buildInputs = with pkgs; [
            # Qt dependencies - consolidated packages
            qt6.qtbase # Core, Gui, Widgets, PrintSupport, Sql, Network
            qt6.qtsvg # SVG support
            qt6.qttools # LinguistTools
            qt6.qt5compat # Core5Compat

            # Database
            sqlite

            # Other dependencies...
            llvmPackages_20.clang-tools
          ];

          cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" ];

          meta = with pkgs.lib; {
            description =
              "Quick System on Chip Studio based on the Qt framework";
            homepage = "https://github.com/vowstar/qsoc";
            license = licenses.asl20;
            platforms = platforms.unix;
            mainProgram = pkgName;
          };
        };

        defaultPackage = self.packages.${system}.${pkgName};

        apps.${pkgName} = flake-utils.lib.mkApp {
          drv = self.packages.${system}.${pkgName};
          name = pkgName;
        };

        defaultApp = self.apps.${system}.${pkgName};

        formatter = pkgs.nixfmt-rfc-style;

        devShells.default = pkgs.mkShell {
          inputsFrom = [ self.packages.${system}.${pkgName} ];
          buildInputs = with pkgs; [
            # Development tools
            git
            gdb
            cmake-format
            llvmPackages_20.clang-tools
            verible
            gperftools
          ];

          shellHook = ''
            echo "QSoC development environment"

            # Ensure submodules are cloned
            if [ -d .git ] && [ -f .gitmodules ]; then
              echo "Initializing git submodules..."
              git submodule update --init --recursive
            fi

            echo "Run 'cmake -B build -G Ninja' to configure"
            echo "Run 'cmake --build build -j' to build"
            echo "Run 'cmake --build build --target test' to test"
            echo "Run 'cmake --build build --target clang-format' to format code"
          '';
        };
      });
}
