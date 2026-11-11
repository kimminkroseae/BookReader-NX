#!/usr/bin/env bash
# Installs devkitPro (devkitA64 + libnx + Switch portlibs) on Arch Linux.
# Run once:  bash tools/install_devkitpro.sh
set -euo pipefail

KEY=BC26F752D25B92CE272E0F44F7FD5492264BB9D0

if ! grep -q '^\[dkp-libs\]' /etc/pacman.conf; then
  echo ">>> Importing devkitPro signing key"
  sudo pacman-key --recv "$KEY" --keyserver keyserver.ubuntu.com
  sudo pacman-key --lsign "$KEY"

  echo ">>> Installing devkitpro-keyring"
  # Official method from the devkitPro wiki: let pacman fetch the package.
  sudo pacman -U --noconfirm https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.zst

  echo ">>> Adding devkitPro repositories to /etc/pacman.conf"
  sudo tee -a /etc/pacman.conf >/dev/null <<'CONF'

[dkp-libs]
Server = https://pkg.devkitpro.org/packages

[dkp-linux]
Server = https://pkg.devkitpro.org/packages/linux/$arch/
CONF
fi

echo ">>> Syncing and installing switch-dev + portlibs"
sudo pacman -Sy --noconfirm
sudo pacman -S --needed --noconfirm switch-dev \
  switch-sdl2 switch-sdl2_ttf switch-sdl2_image switch-sdl2_gfx \
  switch-libarchive switch-libconfig switch-libjpeg-turbo switch-libwebp \
  switch-libpng switch-freetype switch-harfbuzz switch-zlib switch-bzip2 \
  switch-liblzma switch-lz4 switch-libzstd switch-mesa switch-glad switch-libdrm_nouveau

if ! grep -q DEVKITPRO "$HOME/.bashrc" 2>/dev/null; then
  cat >> "$HOME/.bashrc" <<'RC'
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM
export DEVKITPPC=/opt/devkitpro/devkitPPC
RC
fi
echo ">>> Done. devkitPro is at /opt/devkitpro"
