# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

{
  description = "QSoC - Quick System on Chip Studio";

  inputs = {
    self.submodules = true;
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

          # GitHub archives omit submodules, even with self.submodules.
          src = if builtins.pathExists ./external/slang/CMakeLists.txt then
            ./.
          else if self ? rev then
            builtins.fetchGit {
              url = "https://github.com/vowstar/qsoc.git";
              rev = self.rev;
              shallow = true;
              submodules = true;
            }
          else
            throw
            "QSoC requires submodules. Run git submodule update --init --recursive and use nix build '.?submodules=1'.";

          # Extract version from config.h and pin slang's FetchContent
          # to bundled external/fmt so the nix sandbox never reaches
          # for github during configure.
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

            # Steer slang's FetchContent(fmt) to the bundled submodule
            # so the configure step does not try to clone from github
            # (the nix sandbox has no network).
            cmakeFlagsArray+=("-DFETCHCONTENT_SOURCE_DIR_FMT=$src/external/fmt")
          '';

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            qt6.wrapQtAppsHook
            qt6.qttools # For Qt Linguist tools
            python3 # external/slang code generation
            jdk # external/antlr4 parser build
          ];

          buildInputs = with pkgs; [
            # Qt dependencies - consolidated packages
            qt6.qtbase # Core, Gui, Widgets, PrintSupport, Sql, Network
            qt6.qtsvg # SVG support
            qt6.qttools # LinguistTools
            qt6.qt5compat # Core5Compat

            # Database
            sqlite

            # Qt6.8.2 in nixpkgs ships qtbase that links libproxy at
            # runtime but does not declare the dep, so wrapQtAppsHook
            # fails to add the path. Pull it in explicitly so the
            # final binary's RUNPATH resolves QNetworkProxyFactory.
            libproxy

            # slang's external/CMakeLists.txt prefers find_package(fmt)
            # before its FetchContent fallback. Without network in the
            # sandbox the fallback fails, so provide fmt from nixpkgs.
            fmt

            # Other dependencies...
            llvmPackages_20.clang-tools
          ];

          # Disable slang's optional subprojects that would otherwise
          # trigger network-dependent FetchContent: mimalloc, cpptrace,
          # Catch2 (tests). Default slang turns these on; nix sandbox
          # has no network.
          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DSLANG_USE_MIMALLOC=OFF"
            "-DSLANG_USE_CPPTRACE=OFF"
            "-DSLANG_INCLUDE_TESTS=OFF"
          ];

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
            echo "Run 'cmake --build build -j16' to build"
            echo "Run 'cmake --build build --target test' to test"
            echo "Run 'cmake --build build --target clang-format' to format code"
          '';
        };
      });
}
