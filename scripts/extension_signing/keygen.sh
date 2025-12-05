#!/bin/bash
# keygen.sh - Generate RSA-2048 key pair for DuckDB extension signing

set -e

# Only set SCRIPT_DIR if not already set (when sourced from main script)
if [[ -z "${SCRIPT_DIR:-}" ]]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    source "${SCRIPT_DIR}/common.sh"
fi

# Show help for keygen command
keygen_help() {
    cat << EOF
Usage: duckdb-sign.sh keygen [OPTIONS]

Generate an RSA-2048 key pair for signing DuckDB extensions.

Options:
  -o, --output PATH   Base path for output files (default: ./signing_key)
                      Creates <path>.pem (private) and <path>.pub (public)
  -f, --force         Overwrite existing files without prompting
  -h, --help          Show this help message

Output Files:
  <output>.pem    Private key (PKCS#8 PEM format) - KEEP THIS SECRET!
  <output>.pub    Public key (X.509 SubjectPublicKeyInfo PEM format)

Examples:
  # Generate key pair with default names
  duckdb-sign.sh keygen

  # Generate with custom output path
  duckdb-sign.sh keygen -o ~/.duckdb/my_signing_key

  # Overwrite existing keys
  duckdb-sign.sh keygen -o ./key --force

Exit Codes:
  0   Success
  1   Invalid arguments
  2   Output file exists (use --force)
  3   Key generation failed
EOF
}

# Generate RSA-2048 key pair
# Args:
#   $1 - Base output path (will create .pem and .pub files)
#   $2 - Force overwrite (1 or 0)
generate_keypair() {
    local output_base="$1"
    local force="${2:-0}"

    local private_key="${output_base}.pem"
    local public_key="${output_base}.pub"

    # Check if files exist
    if [[ -f "$private_key" && $force -eq 0 ]]; then
        error "Private key file already exists: $private_key" \
              "Use --force to overwrite existing files"
        return 2
    fi

    if [[ -f "$public_key" && $force -eq 0 ]]; then
        error "Public key file already exists: $public_key" \
              "Use --force to overwrite existing files"
        return 2
    fi

    # Create output directory if it doesn't exist
    local output_dir
    output_dir=$(dirname "$output_base")
    if [[ ! -d "$output_dir" ]]; then
        verbose "Creating output directory: $output_dir"
        mkdir -p "$output_dir"
    fi

    info "Generating RSA-2048 key pair..."

    # Generate private key
    verbose "Generating private key: $private_key"
    if ! openssl genrsa -out "$private_key" 2048 2>/dev/null; then
        error "Failed to generate private key" \
              "OpenSSL genrsa command failed"
        return $EXIT_KEY_GEN_FAILED
    fi

    # Set restrictive permissions on private key
    chmod 600 "$private_key"
    verbose "Set permissions 600 on private key"

    # Extract public key
    verbose "Extracting public key: $public_key"
    if ! openssl rsa -in "$private_key" -pubout -out "$public_key" 2>/dev/null; then
        error "Failed to extract public key" \
              "OpenSSL rsa -pubout command failed"
        rm -f "$private_key"  # Clean up
        return $EXIT_KEY_GEN_FAILED
    fi

    # Validate generated keys
    verbose "Validating generated keys..."

    # Check key size - extract the bit count from "Private-Key: (2048 bit, ...)"
    local key_info
    key_info=$(openssl rsa -in "$private_key" -text -noout 2>/dev/null | grep "Private-Key:")
    if [[ ! "$key_info" =~ "2048 bit" ]]; then
        error "Generated key has unexpected size (expected 2048 bit)"
        return $EXIT_KEY_GEN_FAILED
    fi

    verbose "Key validation passed: RSA-2048"

    # Success output
    success "Generated RSA-2048 key pair:"
    info "  Private key: $private_key"
    info "  Public key:  $public_key"
    info ""
    warn "Keep your private key secure and never share it!"
    info ""
    info "To embed the public key in DuckDB, rebuild with:"
    info "  cmake -DDUCKDB_CUSTOM_SIGNING_KEYS=$public_key .."

    return 0
}

# Main function for keygen command
keygen_main() {
    local output_base="./signing_key"
    local force=0

    while [[ $# -gt 0 ]]; do
        case "$1" in
            -o|--output)
                output_base="$2"
                shift 2
                ;;
            -f|--force)
                force=1
                shift
                ;;
            -h|--help)
                keygen_help
                exit 0
                ;;
            -q|--quiet)
                QUIET=1
                shift
                ;;
            -v|--verbose)
                VERBOSE=1
                shift
                ;;
            -*)
                error "Unknown option: $1" "" "Run 'duckdb-sign.sh keygen --help' for usage"
                exit $EXIT_INVALID_ARGS
                ;;
            *)
                # If positional argument provided, treat as output path
                output_base="$1"
                shift
                ;;
        esac
    done

    # Perform key generation
    generate_keypair "$output_base" "$force"
}

# Only run main if script is executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    keygen_main "$@"
fi
