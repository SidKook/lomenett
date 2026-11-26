#!/bin/bash
# Build and run the host tests and UI simulator; makes out/contact_sheet.png
# if ImageMagick is installed. Usage: sim/build.sh
set -e
cd "$(dirname "$0")"
mkdir -p out
g++ -std=c++17 -O1 -I stubs -I .. harness.cpp -o sim_run
echo "== vault edit/import regression tests =="
g++ -std=c++17 -O1 -I stubs -I .. test_vault.cpp -o test_vault
./test_vault || { echo "VAULT TESTS FAILED"; exit 1; }
echo "== 2FA (TOTP) tests =="
g++ -std=c++17 -O1 -I stubs -I .. test_totp.cpp -o test_totp
./test_totp || { echo "TOTP TESTS FAILED"; exit 1; }
echo "== running UI simulator =="
set +e
./sim_run
RC=$?
set -e
# labelled contact sheet: 6x scale, screen name under each tile
if command -v magick >/dev/null; then
  rm -f out/tiles_*.png
  for screen_ppm in out/*.ppm; do
    screen_name=$(basename "$screen_ppm" .ppm)
    magick "$screen_ppm" -scale 600% -bordercolor '#222' -border 2 \
      -background '#222' -fill white -gravity south -splice 0x16 \
      -pointsize 12 -annotate +0+2 "$screen_name" "out/tile_$screen_name.png"
  done
  magick montage out/tile_*.png -tile 4x -geometry +4+4 -background black out/contact_sheet.png
  echo "contact sheet -> sim/out/contact_sheet.png"
fi
exit $RC
