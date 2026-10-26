#!/usr/bin/env bash
# Starts Jet Studio from a release folder; in the repo, tools/manage.py does the same job.
set -eu
here="$(dirname -- "$(readlink -f -- "$0")")"

# Terminals inside snap apps (VS Code, for one) leak GTK paths that make libdecor abort under Wayland.
for saved in $(compgen -e | grep '_VSCODE_SNAP_ORIG$' || true); do
    name="${saved%_VSCODE_SNAP_ORIG}"
    if [ -n "${!saved}" ]; then export "$name=${!saved}"; else unset "$name"; fi
    unset "$saved"
done
for name in GSETTINGS_SCHEMA_DIR GTK_PATH GTK_EXE_PREFIX GIO_MODULE_DIR GDK_PIXBUF_MODULE_FILE GDK_PIXBUF_MODULEDIR \
            GTK_IM_MODULE_FILE LOCPATH; do
    if [[ "${!name:-}" == */snap/* ]]; then unset "$name"; fi
done
for name in XDG_DATA_DIRS XDG_CONFIG_DIRS; do
    if [ -n "${!name:-}" ]; then export "$name=$(tr ':' '\n' <<< "${!name}" | grep -v /snap/ | paste -sd: -)"; fi
done

# Native Wayland keeps text sharp on HiDPI screens.
display="${JET_STUDIO_DISPLAY:-auto}"
if [ "$display" = auto ]; then
    if [ -n "${WAYLAND_DISPLAY:-}" ]; then display=wayland; else display=x11; fi
fi

exec "$here/bin/jet-studio.x86_64" --display-driver "$display" "$@"
