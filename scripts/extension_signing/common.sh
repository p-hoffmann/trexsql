#!/bin/bash
# common.sh - Shared utility functions for DuckDB extension signing
# Part of DuckDB Extension Signing Tools

set -e

# Exit codes
readonly EXIT_SUCCESS=0
readonly EXIT_INVALID_ARGS=1
readonly EXIT_FILE_NOT_FOUND=2
readonly EXIT_INVALID_KEY=3
readonly EXIT_SIGNING_FAILED=4
readonly EXIT_INVALID_EXTENSION=5
readonly EXIT_KEY_GEN_FAILED=3
readonly EXIT_SIGNATURE_INVALID=10

# Colors for output (if terminal supports it)
if [[ -t 1 ]]; then
    readonly RED='\033[0;31m'
    readonly GREEN='\033[0;32m'
    readonly YELLOW='\033[0;33m'
    readonly NC='\033[0m' # No Color
else
    readonly RED=''
    readonly GREEN=''
    readonly YELLOW=''
    readonly NC=''
fi

# Verbosity levels
VERBOSE=0
QUIET=0

# Print error message to stderr
error() {
    if [[ $QUIET -eq 0 ]]; then
        echo -e "${RED}Error: $1${NC}" >&2
        if [[ -n "$2" ]]; then
            echo "Details: $2" >&2
        fi
        if [[ -n "$3" ]]; then
            echo "Hint: $3" >&2
        fi
    fi
}

# Print warning message to stderr
warn() {
    if [[ $QUIET -eq 0 ]]; then
        echo -e "${YELLOW}Warning: $1${NC}" >&2
    fi
}

# Print success message
success() {
    if [[ $QUIET -eq 0 ]]; then
        echo -e "${GREEN}$1${NC}"
    fi
}

# Print info message (respects quiet mode)
info() {
    if [[ $QUIET -eq 0 ]]; then
        echo "$1"
    fi
}

# Print verbose message (only if verbose mode is on)
verbose() {
    if [[ $VERBOSE -eq 1 && $QUIET -eq 0 ]]; then
        echo "[verbose] $1"
    fi
}

# Check if a command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Check required dependencies
check_dependencies() {
    local missing=()

    if ! command_exists openssl; then
        missing+=("openssl")
    fi

    if ! command_exists xxd; then
        missing+=("xxd")
    fi

    if [[ ${#missing[@]} -gt 0 ]]; then
        error "Missing required dependencies: ${missing[*]}" \
              "These tools are required for extension signing" \
              "Install them using your package manager (e.g., apt install ${missing[*]})"
        return 1
    fi

    return 0
}

# Validate that a file exists
validate_file_exists() {
    local file="$1"
    local description="${2:-File}"

    if [[ ! -f "$file" ]]; then
        error "$description not found" "$file does not exist"
        return 1
    fi
    return 0
}

# Validate PEM public key format
validate_public_key() {
    local keyfile="$1"

    if ! validate_file_exists "$keyfile" "Public key file"; then
        return $EXIT_FILE_NOT_FOUND
    fi

    if ! grep -q "^-----BEGIN PUBLIC KEY-----" "$keyfile"; then
        error "Invalid public key format" \
              "$keyfile does not contain a valid PEM public key" \
              "Expected header: -----BEGIN PUBLIC KEY-----"
        return $EXIT_INVALID_KEY
    fi

    # Verify it's a valid RSA key using OpenSSL
    if ! openssl rsa -pubin -in "$keyfile" -noout 2>/dev/null; then
        error "Invalid RSA public key" \
              "$keyfile could not be parsed as an RSA public key" \
              "Generate a new key pair with: duckdb-sign.sh keygen"
        return $EXIT_INVALID_KEY
    fi

    return 0
}

# Validate PEM private key format
validate_private_key() {
    local keyfile="$1"

    if ! validate_file_exists "$keyfile" "Private key file"; then
        return $EXIT_FILE_NOT_FOUND
    fi

    # Check for either PKCS#8 or traditional RSA format
    if ! grep -qE "^-----BEGIN (RSA )?PRIVATE KEY-----" "$keyfile"; then
        error "Invalid private key format" \
              "$keyfile does not contain a valid PEM private key" \
              "Expected header: -----BEGIN PRIVATE KEY----- or -----BEGIN RSA PRIVATE KEY-----"
        return $EXIT_INVALID_KEY
    fi

    # Verify it's a valid RSA key using OpenSSL
    if ! openssl rsa -in "$keyfile" -noout 2>/dev/null; then
        error "Invalid RSA private key" \
              "$keyfile could not be parsed as an RSA private key" \
              "Generate a new key pair with: duckdb-sign.sh keygen"
        return $EXIT_INVALID_KEY
    fi

    return 0
}

# Validate extension file format
validate_extension() {
    local extfile="$1"

    if ! validate_file_exists "$extfile" "Extension file"; then
        return $EXIT_FILE_NOT_FOUND
    fi

    local filesize
    filesize=$(stat -c%s "$extfile" 2>/dev/null || stat -f%z "$extfile" 2>/dev/null)

    # Extension must be at least 512 bytes (footer size)
    if [[ $filesize -lt 512 ]]; then
        error "Invalid extension format" \
              "$extfile is too small to be a valid DuckDB extension" \
              "Expected at least 512 bytes, got $filesize bytes"
        return $EXIT_INVALID_EXTENSION
    fi

    # Check magic value (should be '4' at offset filesize-512)
    local magic_offset=$((filesize - 512))
    local magic
    magic=$(dd if="$extfile" bs=1 skip=$magic_offset count=1 2>/dev/null | xxd -p)

    if [[ "$magic" != "34" ]]; then  # '4' in hex is 0x34
        error "Invalid extension format" \
              "Magic value mismatch at footer offset" \
              "This file may not be a valid DuckDB extension or may be corrupted"
        return $EXIT_INVALID_EXTENSION
    fi

    return 0
}

# Get file size
get_file_size() {
    local file="$1"
    stat -c%s "$file" 2>/dev/null || stat -f%z "$file" 2>/dev/null
}

# Parse common arguments (--quiet, --verbose, --help)
parse_common_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -q|--quiet)
                QUIET=1
                shift
                ;;
            -v|--verbose)
                VERBOSE=1
                shift
                ;;
            *)
                # Return remaining arguments
                echo "$@"
                return
                ;;
        esac
    done
}

# Get script directory
get_script_dir() {
    local source="${BASH_SOURCE[0]}"
    while [[ -L "$source" ]]; do
        local dir
        dir=$(cd -P "$(dirname "$source")" && pwd)
        source=$(readlink "$source")
        [[ $source != /* ]] && source="$dir/$source"
    done
    cd -P "$(dirname "$source")" && pwd
}
