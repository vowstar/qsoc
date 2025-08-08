# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

{
  inputs.nixpkgs.url = "nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgName = "qsoc-manual";
        pkgs = import nixpkgs { inherit system; };
        version = builtins.head
          (builtins.match ''.*QSOC_VERSION "([0-9]+\.[0-9]+\.[0-9]+)".*''
            (builtins.readFile ../src/common/config.h));
      in {
        packages = {
          ${pkgName} = pkgs.stdenv.mkDerivation {
            pname = pkgName;
            inherit version;
            src = pkgs.lib.cleanSource ./en;
            buildInputs = [
              pkgs.typst
              pkgs.pandoc
              pkgs.sarasa-gothic
              pkgs.noto-fonts
              pkgs.fontconfig
            ];

            buildPhase = ''
              font_dirs=(
                "${pkgs.sarasa-gothic}/share/fonts/truetype"
                "${pkgs.noto-fonts}/share/fonts"
                # Add more font directories here...
              )
              # Create custom fontconfig configuration
              mkdir -p fontconfig/conf.d
              cat > fontconfig/conf.d/99-custom-fonts.conf << EOF
              <?xml version="1.0"?>
              <!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
              <fontconfig>
                <!-- Add your font directories and set high priority -->
                <dir>${pkgs.sarasa-gothic}/share/fonts/truetype</dir>
                <dir>${pkgs.noto-fonts}/share/fonts</dir>
                <!-- Set font priorities -->
                <alias binding="strong">
                  <family>sans-serif</family>
                  <prefer>
                    <family>Sarasa Gothic</family>
                    <family>Noto Sans</family>
                  </prefer>
                </alias>
              </fontconfig>
              EOF
              # Use custom configuration
              export FONTCONFIG_FILE=$(pwd)/fontconfig/conf.d/99-custom-fonts.conf
              export FONTCONFIG_PATH=${pkgs.fontconfig.out}/etc/fonts
              # Update version in cover.svg
              if [ -f "image/cover.svg" ]; then
                sed -i 's/Version [0-9]\+\.[0-9]\+\.[0-9]\+/Version '"$version"'/g' image/cover.svg
              fi
              # Convert Markdown files to Typst
              for md in *.md; do
                if [ -f "$md" ]; then
                  typ_file="$md.typ"
                  pandoc "$md" -f markdown -t typst -o "$typ_file"

                  # Handle table width - wrap tables in box to fill page width
                  sed -i '
                    # Handle numeric format: columns: number → columns: (1fr,) * number
                    s/columns: \([0-9]\+\),/columns: (1fr,) * \1,/g
                    # Handle auto format: columns: (auto,auto,...) → columns: (1fr,1fr,...)
                    s/columns: (auto\(,auto\)*)/columns: (1fr\1)/g
                    # Convert percentage to fr units
                    s/\([0-9]\+\.[0-9]\+\)%/\1fr/g
                    s/\([0-9]\+\)%/\1fr/g
                  ' "$typ_file"
                fi
              done
              # Compile Typst files
              export FILE_NAME=$(echo "${pkgName}" | sed 's/-/_/g')_$version.pdf
              typst compile main.typ "$FILE_NAME"
            '';

            installPhase = ''
              mkdir -p "$out"
              cp "$FILE_NAME" "$out"
            '';
          };
        };

        formatter = pkgs.nixfmt-rfc-style;

        devShells = {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.${pkgName} ];
            nativeBuildInputs = with pkgs; [ git ];
            shellHook = ''
              export FONTCONFIG_PATH="${pkgs.fontconfig.out}/etc/fonts"
            '';
          };
        };

        defaultPackage = flake-utils.lib.eachDefaultSystem
          (system: self.packages.${system}.${pkgName});
      });
}
