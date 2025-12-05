#!/bin/bash
# sign.sh - Sign a DuckDB extension with a private key
# Implements the two-level hash signing algorithm compatible with DuckDB verification

set -e

# Only set SCRIPT_DIR if not already set (when sourced from main script)
if [[ -z "${SCRIPT_DIR:-}" ]]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    source "${SCRIPT_DIR}/common.sh"
fi
source "${SCRIPT_DIR}/hash.sh"
source "${SCRIPT_DIR}/parse_footer.sh"
source "${SCRIPT_DIR}/write_footer.sh"

# Show help for sign command
sign_help() {
    cat << EOF
Usage: duckdb-sign.sh sign [OPTIONS] <extension_file>

Sign a DuckDB extension binary with a private key.

Arguments:
  extension_file    Path to .duckdb_extension file

Options:
  -k, --key FILE    Path to private key file (PEM format) [required]
  -o, --output FILE Output path for signed extension (default: in-place)
  -h, --help        Show this help message

Environment Variables:
  DUCKDB_SIGN_KEY   Default private key path (used if --key not specified)

Examples:
  # Sign extension in-place
  duckdb-sign.sh sign -k ./signing_key.pem ./my_extension.duckdb_extension

  # Sign to new file
  duckdb-sign.sh sign -k ./key.pem -o ./signed.duckdb_extension ./unsigned.duckdb_extension

Exit Codes:
  0   Success
  1   Invalid arguments
  2   Extension file not found
  3   Private key file not found
  4   Invalid private key format
  5   Invalid extension format
  6   Signing failed
EOF
}

# Sign an extension file
# Args:
#   $1 - Extension file path
#   $2 - Private key file path
#   $3 - Output file path (optional, defaults to in-place)
sign_extension() {
    local extfile="$1"
    local keyfile="$2"
    local outfile="${3:-$extfile}"

    # Validate inputs
    if ! validate_extension "$extfile"; then
        return $?
    fi

    if ! validate_private_key "$keyfile"; then
        return $?
    fi

    # Create temp directory for intermediate files
    local tmpdir
    tmpdir=$(mktemp -d)
    trap "rm -rf '$tmpdir'" EXIT

    local hash_file="${tmpdir}/hash.bin"
    local sig_file="${tmpdir}/signature.bin"

    # Step 1: Compute two-level hash
    info "Computing extension hash..."
    verbose "Input: $extfile"

    if ! compute_extension_hash "$extfile" "$hash_file"; then
        error "Failed to compute extension hash"
        return $EXIT_SIGNING_FAILED
    fi

    verbose "Hash computed: $(xxd -p -c 32 "$hash_file")"

    # Step 2: Sign hash with private key using OpenSSL
    info "Signing with private key..."
    verbose "Key: $keyfile"

    # Use pkeyutl for signing (works with both PKCS#8 and traditional RSA keys)
    if ! openssl pkeyutl -sign \
        -in "$hash_file" \
        -inkey "$keyfile" \
        -out "$sig_file" \
        -pkeyopt digest:sha256 2>/dev/null; then
        error "Signing failed" \
              "OpenSSL pkeyutl returned an error" \
              "Verify that the private key is a valid RSA-2048 key"
        return $EXIT_SIGNING_FAILED
    fi

    # Verify signature is 256 bytes
    local sigsize
    sigsize=$(get_file_size "$sig_file")
    if [[ $sigsize -ne 256 ]]; then
        error "Invalid signature size" \
              "Expected 256 bytes, got $sigsize bytes" \
              "This may indicate the key is not RSA-2048"
        return $EXIT_SIGNING_FAILED
    fi

    verbose "Signature generated: $(xxd -p -c 64 "$sig_file" | head -1)..."

    # Step 3: Write signature to extension
    info "Writing signature to extension..."

    if [[ "$outfile" != "$extfile" ]]; then
        # Copy to output file first
        cp "$extfile" "$outfile"
    fi

    if ! write_signature "$outfile" "$sig_file"; then
        error "Failed to write signature to extension"
        return $EXIT_SIGNING_FAILED
    fi

    # Success output
    success "Extension signed successfully:"
    info "  Input:     $extfile"
    info "  Output:    $outfile"

    local sig_hash
    sig_hash=$(openssl dgst -sha256 "$sig_file" 2>/dev/null | cut -d' ' -f2 | head -c 16)
    info "  Signature: SHA256:${sig_hash}..."

    # Show extension metadata
    info ""
    info "Extension metadata:"
    info "  Platform:  $(get_platform "$outfile")"
    info "  ABI Type:  $(get_abi_type "$outfile")"
    info "  Version:   $(get_extension_version "$outfile")"

    return 0
}

# Main function for sign command
sign_main() {
    local extfile=""
    local keyfile=""
    local outfile=""

    # Check for DUCKDB_SIGN_KEY environment variable
    if [[ -n "${DUCKDB_SIGN_KEY:-}" ]]; then
        keyfile="$DUCKDB_SIGN_KEY"
        verbose "Using key from DUCKDB_SIGN_KEY environment variable"
    fi

    while [[ $# -gt 0 ]]; do
        case "$1" in
            -k|--key)
                keyfile="$2"
                shift 2
                ;;
            -o|--output)
                outfile="$2"
                shift 2
                ;;
            -h|--help)
                sign_help
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
                error "Unknown option: $1" "" "Run 'duckdb-sign.sh sign --help' for usage"
                exit $EXIT_INVALID_ARGS
                ;;
            *)
                extfile="$1"
                shift
                ;;
        esac
    done

    # Validate required arguments
    if [[ -z "$extfile" ]]; then
        error "Extension file not specified" "" "Usage: duckdb-sign.sh sign -k <key.pem> <extension.duckdb_extension>"
        exit $EXIT_INVALID_ARGS
    fi

    if [[ -z "$keyfile" ]]; then
        error "Private key not specified" \
              "Use -k/--key option or set DUCKDB_SIGN_KEY environment variable" \
              "Generate a key pair with: duckdb-sign.sh keygen"
        exit $EXIT_INVALID_ARGS
    fi

    # Set output file
    if [[ -z "$outfile" ]]; then
        outfile="$extfile"
    fi

    # Perform signing
    sign_extension "$extfile" "$keyfile" "$outfile"
}

# Only run main if script is executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    sign_main "$@"
fi
