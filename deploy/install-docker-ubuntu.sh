#!/usr/bin/env bash

set -Eeuo pipefail

# Installs Docker Engine from Docker's official apt repository.
# Reference: https://docs.docker.com/engine/install/ubuntu/

supported_codenames=(jammy noble questing resolute)
docker_key_fingerprint=9DC858229FC7DD38854AE2D88D81803C0EBFCD88
docker_key_urls=(
    https://download.docker.com/linux/ubuntu/gpg
    https://mirrors.tuna.tsinghua.edu.cn/docker-ce/linux/ubuntu/gpg
    https://mirrors.aliyun.com/docker-ce/linux/ubuntu/gpg
)
conflicting_packages=(
    docker.io
    docker-compose
    docker-compose-v2
    docker-doc
    docker-buildx
    podman-docker
    containerd
    runc
)

die() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

if [[ ! -r /etc/os-release ]]; then
    die 'Cannot read /etc/os-release.'
fi

# shellcheck disable=SC1091
source /etc/os-release

if [[ ${ID:-} != ubuntu ]]; then
    die "This installer only supports Ubuntu; detected ${PRETTY_NAME:-unknown system}."
fi

ubuntu_codename=${UBUNTU_CODENAME:-${VERSION_CODENAME:-}}
if [[ -z $ubuntu_codename ]]; then
    die 'Cannot determine the Ubuntu codename.'
fi

codename_supported=false
for supported_codename in "${supported_codenames[@]}"; do
    if [[ $ubuntu_codename == "$supported_codename" ]]; then
        codename_supported=true
        break
    fi
done
if [[ $codename_supported != true ]]; then
    die "Ubuntu codename '$ubuntu_codename' is not in the supported list: ${supported_codenames[*]}."
fi

command_exists apt-get || die 'apt-get is required.'
command_exists dpkg || die 'dpkg is required.'
command_exists systemctl || die 'systemd is required to manage the Docker service.'

if ((EUID == 0)) && [[ -n ${SUDO_USER:-} ]]; then
    die 'Do not run this script with sudo. Run it as your normal user; it requests sudo when needed.'
fi

if ((EUID == 0)); then
    privilege_command=()
    target_user=root
else
    command_exists sudo || die 'sudo is required when the installer is not run as root.'
    privilege_command=(sudo)
    target_user=$(id -un)
    sudo -v
fi

proxy_environment=()
for proxy_name in HTTP_PROXY HTTPS_PROXY NO_PROXY http_proxy https_proxy no_proxy; do
    proxy_value=${!proxy_name-}
    if [[ -n $proxy_value ]]; then
        proxy_environment+=("$proxy_name=$proxy_value")
    fi
done

run_apt() {
    "${privilege_command[@]}" env \
        DEBIAN_FRONTEND=noninteractive \
        "${proxy_environment[@]}" \
        apt-get -o Acquire::Retries=5 "$@"
}

download_docker_key() {
    local attempt
    local downloaded_fingerprint
    local key_url

    for key_url in "${docker_key_urls[@]}"; do
        for attempt in 1 2; do
            : >"$docker_key_file"
            if curl -fsSL \
                --retry 2 \
                --retry-all-errors \
                --connect-timeout 15 \
                --max-time 60 \
                "$key_url" \
                -o "$docker_key_file" && [[ -s $docker_key_file ]]; then
                downloaded_fingerprint=$(
                    gpg --batch --show-keys --with-colons "$docker_key_file" 2>/dev/null |
                        awk -F: '$1 == "fpr" && fingerprint == "" { fingerprint = $10 }
                            END { print fingerprint }'
                ) || downloaded_fingerprint=''
                if [[ $downloaded_fingerprint == "$docker_key_fingerprint" ]]; then
                    return
                fi
            fi
            printf 'Docker signing key download or validation failed from %s (attempt %d of 2).\n' \
                "$key_url" "$attempt" >&2
        done
    done

    die 'Could not download and validate Docker signing key; the system key was not replaced.'
}

architecture=$(dpkg --print-architecture)
case $architecture in
    amd64 | armhf | arm64 | s390x | ppc64el) ;;
    *) die "Docker does not list architecture '$architecture' as supported on Ubuntu." ;;
esac

installed_conflicts=()
for package_name in "${conflicting_packages[@]}"; do
    package_status=$(dpkg-query -W -f='${db:Status-Abbrev}' "$package_name" 2>/dev/null || true)
    if [[ $package_status == ii* ]]; then
        installed_conflicts+=("$package_name")
    fi
done

if ((${#installed_conflicts[@]} > 0)); then
    printf 'Conflicting packages are installed: %s\n' "${installed_conflicts[*]}" >&2
    printf 'Review existing container workloads, then remove the conflicts explicitly before rerunning.\n' >&2
    printf 'Docker documents this command: sudo apt-get remove %s\n' "${installed_conflicts[*]}" >&2
    exit 1
fi

printf 'Installing Docker Engine for %s (%s, %s).\n' \
    "${PRETTY_NAME:-Ubuntu}" "$ubuntu_codename" "$architecture"

docker_key_file=$(mktemp)
repository_file=$(mktemp)
trap 'rm -f "$docker_key_file" "$repository_file"' EXIT

# A previous interrupted run may have created the source file with an empty key.
# Repair the key before the first apt update so apt can authenticate that source.
docker_key_installed=false
if [[ -e /etc/apt/sources.list.d/docker.sources ]]; then
    command_exists curl || die 'curl is required to repair the existing Docker apt source.'
    command_exists gpg || die 'gpg is required to repair the existing Docker apt source.'
    download_docker_key
    "${privilege_command[@]}" install -m 0755 -d /etc/apt/keyrings
    "${privilege_command[@]}" install -m 0644 "$docker_key_file" /etc/apt/keyrings/docker.asc
    docker_key_installed=true
fi

run_apt update
run_apt install -y \
    ca-certificates \
    curl \
    gnupg

if [[ $docker_key_installed != true ]]; then
    download_docker_key
    "${privilege_command[@]}" install -m 0755 -d /etc/apt/keyrings
    "${privilege_command[@]}" install -m 0644 "$docker_key_file" /etc/apt/keyrings/docker.asc
fi

printf '%s\n' \
    'Types: deb' \
    'URIs: https://download.docker.com/linux/ubuntu' \
    "Suites: $ubuntu_codename" \
    'Components: stable' \
    "Architectures: $architecture" \
    'Signed-By: /etc/apt/keyrings/docker.asc' \
    >"$repository_file"
"${privilege_command[@]}" install -m 0644 \
    "$repository_file" /etc/apt/sources.list.d/docker.sources

run_apt update
run_apt install -y \
    docker-ce \
    docker-ce-cli \
    containerd.io \
    docker-buildx-plugin \
    docker-compose-plugin

"${privilege_command[@]}" systemctl enable --now docker.service containerd.service

user_added=false
if [[ $target_user != root ]]; then
    if ! getent group docker >/dev/null; then
        "${privilege_command[@]}" groupadd docker
    fi
    if ! id -nG "$target_user" | tr ' ' '\n' | grep -Fxq docker; then
        "${privilege_command[@]}" usermod -aG docker "$target_user"
        user_added=true
    fi
fi

"${privilege_command[@]}" docker version --format \
    'Docker client {{.Client.Version}}, server {{.Server.Version}}'
"${privilege_command[@]}" docker compose version
"${privilege_command[@]}" docker run --rm hello-world

printf '\nDocker Engine installation and daemon verification completed successfully.\n'
if [[ $user_added == true ]]; then
    printf "User '%s' was added to the docker group. Log out and back in before using Docker without sudo.\n" \
        "$target_user"
fi
printf 'Security note: membership in the docker group grants root-level privileges.\n'
printf 'Firewall note: review published container ports because Docker port rules can bypass ufw rules.\n'
