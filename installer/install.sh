#!/bin/sh
set -eu

REPO="manuel-alvarez-alvarez/overlAIer"
INSTALL_DIR="$HOME/.overlAIer"
BIN_DIR="$INSTALL_DIR/bin"
PROCESSORS_DIR="$INSTALL_DIR/processors"
WEB_DIR="$INSTALL_DIR/web"
USER_SYSTEMD_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
SYSTEM_SYSTEMD_DIR="/etc/systemd/system"
SUDO=""

# ---------------------------------------------------------------------------
# Colors
# ---------------------------------------------------------------------------
setup_colors() {
    if [ -t 1 ]; then
        RED='\033[0;31m'
        GREEN='\033[0;32m'
        YELLOW='\033[0;33m'
        BLUE='\033[0;34m'
        BOLD='\033[1m'
        RESET='\033[0m'
    else
        RED='' GREEN='' YELLOW='' BLUE='' BOLD='' RESET=''
    fi
}

info()    { printf "${BLUE}::${RESET} %s\n" "$*"; }
success() { printf "${GREEN}::${RESET} %s\n" "$*"; }
warn()    { printf "${YELLOW}:: %s${RESET}\n" "$*"; }
error()   { printf "${RED}:: %s${RESET}\n" "$*" >&2; }
die()     { error "$*"; exit 1; }

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
has_cmd() { command -v "$1" >/dev/null 2>&1; }

fetch() {
    url="$1"; dest="$2"
    if has_cmd curl; then
        curl -fsSL -o "$dest" "$url"
    elif has_cmd wget; then
        wget -qO "$dest" "$url"
    else
        die "Neither curl nor wget found. Install one and retry."
    fi
}

fetch_stdout() {
    url="$1"
    if has_cmd curl; then
        curl -fsSL "$url"
    elif has_cmd wget; then
        wget -qO- "$url"
    else
        die "Neither curl nor wget found. Install one and retry."
    fi
}

check_platform() {
    os="$(uname -s)"
    arch="$(uname -m)"
    [ "$os" = "Linux" ] || die "Unsupported OS: $os (only Linux is supported)"
    [ "$arch" = "aarch64" ] || die "Unsupported architecture: $arch (only aarch64 is supported)"
}

# overlAIer's pipeline binary needs root (configfs USB gadget, /dev/hidgN,
# /dev/input/event*), so it runs as a system service. The installer itself
# must be invoked as the real user — we use sudo internally for the
# system-level operations and keep everything else under the user's home.
ensure_user_with_sudo() {
    if [ "$(id -u)" -eq 0 ]; then
        die "Run this installer as your normal user, not root. It will invoke sudo only for the system service."
    fi
    if has_cmd sudo; then
        SUDO="sudo"
    else
        die "sudo is required to install the system service. Install sudo or run the system-level steps manually."
    fi
}

get_latest_version() {
    api_url="https://api.github.com/repos/$REPO/releases/latest"
    tag=$(fetch_stdout "$api_url" | grep '"tag_name"' | head -1 | sed 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')
    [ -n "$tag" ] || die "Could not determine latest release. Check https://github.com/$REPO/releases"
    printf '%s' "$tag"
}

# ---------------------------------------------------------------------------
# PATH
# ---------------------------------------------------------------------------
PATH_LINE="export PATH=\"\$HOME/.overlAIer/bin:\$PATH\" # overlAIer"

add_to_path() {
    case ":$PATH:" in
        *":$BIN_DIR:"*) return ;;
    esac

    for rc in "$HOME/.bashrc" "$HOME/.zshrc" "$HOME/.profile"; do
        [ -f "$rc" ] || continue
        if ! grep -qF '# overlAIer' "$rc" 2>/dev/null; then
            printf '\n%s\n' "$PATH_LINE" >> "$rc"
            info "Updated $rc"
        fi
    done
}

remove_from_path() {
    for rc in "$HOME/.bashrc" "$HOME/.zshrc" "$HOME/.profile"; do
        [ -f "$rc" ] || continue
        if grep -qF '# overlAIer' "$rc" 2>/dev/null; then
            sed -i '/# overlAIer/d' "$rc"
            info "Cleaned $rc"
        fi
    done
}

# ---------------------------------------------------------------------------
# Systemd units (embedded so the script is self-contained for curl|sh)
# ---------------------------------------------------------------------------
install_service_units() {
    # ---- System units (root): the pipeline + its config watcher --------
    # The pipeline binary needs CAP_SYS_ADMIN for configfs and direct access
    # to /dev/hidg*, /dev/input/event*, so it runs as root. The path watcher
    # tracks the user's config file (absolute path baked in here, since
    # systemd's %h would resolve to /root for a system unit).
    cat << UNIT | $SUDO tee "$SYSTEM_SYSTEMD_DIR/overlAIer.service" > /dev/null
[Unit]
Description=OverlAIer - Real-time HDMI overlay pipeline
Documentation=https://github.com/manuel-alvarez-alvarez/overlAIer

[Service]
Type=simple
ExecStart=$BIN_DIR/overlAIer --config $INSTALL_DIR/overlAIer.toml
EnvironmentFile=-$INSTALL_DIR/overlaier.env
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
UNIT

    cat << UNIT | $SUDO tee "$SYSTEM_SYSTEMD_DIR/overlAIer-config.path" > /dev/null
[Unit]
Description=Watch overlAIer config for changes

[Path]
PathChanged=$INSTALL_DIR/overlAIer.toml
Unit=overlAIer-config-reload.service

[Install]
WantedBy=multi-user.target
UNIT

    cat << 'UNIT' | $SUDO tee "$SYSTEM_SYSTEMD_DIR/overlAIer-config-reload.service" > /dev/null
[Unit]
Description=Restart overlAIer on config change

[Service]
Type=oneshot
ExecStart=/usr/bin/systemctl restart overlAIer.service
UNIT

    $SUDO systemctl daemon-reload
    $SUDO systemctl enable overlAIer.service 2>/dev/null || true
    $SUDO systemctl enable overlAIer-config.path 2>/dev/null || true
    $SUDO systemctl start overlAIer-config.path 2>/dev/null || true

    # ---- User unit: the web UI (no root needed) ------------------------
    mkdir -p "$USER_SYSTEMD_DIR"

    cat > "$USER_SYSTEMD_DIR/overlAIer-web.service" << 'UNIT'
[Unit]
Description=OverlAIer Web Interface
Documentation=https://github.com/manuel-alvarez-alvarez/overlAIer
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=%h/.overlAIer/web
ExecStart=%h/.overlAIer/web/venv/bin/python -m overlaier_web
EnvironmentFile=-%h/.overlAIer/overlaier.env
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
UNIT

    systemctl --user daemon-reload
    systemctl --user enable overlAIer-web.service 2>/dev/null || true
}

remove_service_units() {
    $SUDO systemctl stop overlAIer.service overlAIer-config.path 2>/dev/null || true
    $SUDO systemctl disable overlAIer.service overlAIer-config.path 2>/dev/null || true
    $SUDO rm -f "$SYSTEM_SYSTEMD_DIR/overlAIer.service" \
                "$SYSTEM_SYSTEMD_DIR/overlAIer-config.path" \
                "$SYSTEM_SYSTEMD_DIR/overlAIer-config-reload.service"
    $SUDO systemctl daemon-reload 2>/dev/null || true

    systemctl --user stop overlAIer-web.service 2>/dev/null || true
    systemctl --user disable overlAIer-web.service 2>/dev/null || true
    rm -f "$USER_SYSTEMD_DIR/overlAIer-web.service"
    systemctl --user daemon-reload 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# Python web app
# ---------------------------------------------------------------------------
setup_web_venv() {
    if [ -f "$WEB_DIR/requirements.txt" ]; then
        info "Setting up Python virtual environment..."
        if has_cmd python3; then
            python3 -m venv "$WEB_DIR/venv"
            "$WEB_DIR/venv/bin/pip" install --quiet -r "$WEB_DIR/requirements.txt"
            success "Web application dependencies installed"
        else
            warn "python3 not found — skipping web app setup. Install Python 3 and re-run."
        fi
    else
        info "No web application in this release (skipping)"
    fi
}

# ---------------------------------------------------------------------------
# Install / Uninstall
# ---------------------------------------------------------------------------
do_install() {
    check_platform

    version="${OPT_VERSION:-$(get_latest_version)}"
    tarball="overlaier-${version}-linux-aarch64.tar.gz"
    url="https://github.com/$REPO/releases/download/${version}/${tarball}"

    printf "\n${BOLD}  overlAIer installer${RESET}\n"
    printf "  Version:  %s\n" "$version"
    printf "  Target:   %s\n\n" "$INSTALL_DIR"

    tmpdir="$(mktemp -d)"
    trap 'rm -rf "$tmpdir"' EXIT

    # Stop running service before updating binaries
    if $SUDO systemctl is-active overlAIer.service >/dev/null 2>&1; then
        info "Stopping running service..."
        $SUDO systemctl stop overlAIer.service 2>/dev/null || true
    fi

    info "Downloading $tarball..."
    fetch "$url" "$tmpdir/$tarball"

    info "Extracting..."
    mkdir -p "$BIN_DIR" "$PROCESSORS_DIR" "$INSTALL_DIR/lib" "$WEB_DIR"
    tar xzf "$tmpdir/$tarball" -C "$INSTALL_DIR" --strip-components=1

    chmod +x "$BIN_DIR/overlAIer" "$INSTALL_DIR/overlAIer.bin"

    # Install example config if none exists
    if [ ! -f "$INSTALL_DIR/overlAIer.toml" ]; then
        cat > "$INSTALL_DIR/overlAIer.toml" << 'TOML'
# overlAIer configuration
# CLI arguments always take precedence over values here.
# Uncomment and edit the settings you want to change.

# [device]
# video_in  = "/dev/video0"
# video_out = "/dev/dri/card0:HDMI-A-2"
# audio_in  = "hw:0,0"
# audio_out = "hw:1,0"

# [format]
# fmt_in  = "NV24"
# fmt_out = "BG24"
# res_in  = "1920x1080"
# res_out = "2560x1440"
# fps_in  = 60
# fps_out = 120

# [general]
# log_level  = "info"
# async_flip = false

# Processor plugins to load.
#
# [[processor]]
# path = "/path/to/fps_counter.so"

# USB HID device proxy. Selected USB HID devices are forwarded to the
# target PC via USB OTG; reports pass through the processor chain.
# Run `overlAIer query` to list connected HID devices and their VID:PID.
#
# [usb]
# udc = "fe800000.usb"    # UDC controller (auto-detected if omitted)
#
# [[usb.device]]
# vid_pid = "046d:c52b"   # Logitech Unifying receiver
TOML
        info "Created default config at $INSTALL_DIR/overlAIer.toml"
    fi

    info "Installing systemd services..."
    install_service_units

    info "Configuring PATH..."
    add_to_path

    setup_web_venv

    # Restart service if it was enabled
    if $SUDO systemctl is-enabled overlAIer.service >/dev/null 2>&1; then
        info "Starting service..."
        $SUDO systemctl start overlAIer.service 2>/dev/null || true
    fi

    printf "\n"
    success "overlAIer ${version} installed successfully!"
    printf "\n"
    info "Binary:     $BIN_DIR/overlAIer"
    info "Processors: $PROCESSORS_DIR/"
    info "Config:     $INSTALL_DIR/overlAIer.toml"
    info "Pipeline:   sudo systemctl status overlAIer.service   (logs: journalctl -u overlAIer)"
    info "Web UI:     systemctl --user status overlAIer-web    (logs: journalctl --user -u overlAIer-web)"
    printf "\n"
    warn "Restart your shell or run:  source ~/.bashrc"
    printf "\n"
}

do_uninstall() {
    printf "\n${BOLD}  overlAIer uninstaller${RESET}\n\n"

    info "Stopping services..."
    remove_service_units

    info "Removing PATH entries..."
    remove_from_path

    if [ -d "$INSTALL_DIR" ]; then
        info "Removing $INSTALL_DIR..."
        rm -rf "$INSTALL_DIR"
    fi

    printf "\n"
    success "overlAIer has been uninstalled."
    printf "\n"
}

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
usage() {
    cat <<EOF
Usage: install.sh [OPTIONS]

Install or uninstall overlAIer.

Options:
  --version VERSION   Install a specific version (default: latest)
  --uninstall         Remove overlAIer from this machine
  -h, --help          Show this help message
EOF
}

main() {
    setup_colors

    OPT_VERSION=""
    OPT_UNINSTALL=0

    while [ $# -gt 0 ]; do
        case "$1" in
            --version)
                [ -n "${2:-}" ] || die "--version requires a value"
                OPT_VERSION="$2"; shift 2 ;;
            --uninstall)
                OPT_UNINSTALL=1; shift ;;
            -h|--help)
                usage; exit 0 ;;
            *)
                die "Unknown option: $1" ;;
        esac
    done

    ensure_user_with_sudo

    if [ "$OPT_UNINSTALL" -eq 1 ]; then
        do_uninstall
    else
        do_install
    fi
}

main "$@"
