#!/bin/bash
# verify.sh - Verify a DuckDB extension's signature against a public key

set -e

# Only set SCRIPT_DIR if not already set (when sourced from main script)
if [[ -z "${SCRIPT_DIR:-}" ]]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    source "${SCRIPT_DIR}/common.sh"
fi
source "${SCRIPT_DIR}/hash.sh"
source "${SCRIPT_DIR}/parse_footer.sh"

# Show help for verify command
verify_help() {
    cat << EOF
Usage: duckdb-sign.sh verify [OPTIONS] <extension_file>

Verify a DuckDB extension's signature against a public key.

Arguments:
  extension_file    Path to .duckdb_extension file

Options:
  -k, --key FILE    Path to public key file (PEM format) [required]
  -h, --help        Show this help message

Examples:
  duckdb-sign.sh verify -k ./signing_key.pub ./my_extension.duckdb_extension

Exit Codes:
  0    Signature valid
  1    Invalid arguments
  2    Extension file not found
  3    Public key file not found
  4    Invalid public key format
  5    Invalid extension format
  10   Signature invalid
EOF
}

# Verify extension signature
# Args:
#   $1 - Extension file path
#   $2 - Public key file path
verify_extension() {
    local extfile="$1"
    local keyfile="$2"

    # Validate inputs
    if ! validate_extension "$extfile"; then
        return $?
    fi

    if ! validate_public_key "$keyfile"; then
        return $?
    fi

    # Check if extension has a signature
    if ! has_signature "$extfile"; then
        error "Extension is not signed" \
              "No signature found in extension footer" \
              "Sign the extension with: duckdb-sign.sh sign -k <key.pem> <extension>"
        return $EXIT_SIGNATURE_INVALID
    fi

    # Create temp directory for intermediate files
    local tmpdir
    tmpdir=$(mktemp -d)
    trap "rm -rf '$tmpdir'" EXIT

    local hash_file="${tmpdir}/hash.bin"
    local sig_file="${tmpdir}/signature.bin"

    # Step 1: Compute two-level hash
    verbose "Computing extension hash..."
    if ! compute_extension_hash "$extfile" "$hash_file"; then
        error "Failed to compute extension hash"
        return $EXIT_INVALID_EXTENSION
    fi

    verbose "Hash computed: $(xxd -p -c 32 "$hash_file")"

    # Step 2: Extract signature from extension
    verbose "Extracting signature..."
    get_signature "$extfile" > "$sig_file"

    verbose "Signature extracted: $(xxd -p -c 64 "$sig_file" | head -1)..."

    # Step 3: Verify signature with public key
    verbose "Verifying signature..."

    if openssl pkeyutl -verify \
        -in "$hash_file" \
        -sigfile "$sig_file" \
        -pubin -inkey "$keyfile" \
        -pkeyopt digest:sha256 2>/dev/null; then

        success "Signature verification: PASSED"
        info ""
        info "Extension metadata:"
        info "  Platform:  $(get_platform "$extfile")"
        info "  ABI Type:  $(get_abi_type "$extfile")"
        info "  Version:   $(get_extension_version "$extfile")"
        return 0
    else
        error "Signature verification: FAILED"
        info ""
        info "The extension signature does not match the provided public key."
        info "This could mean:"
        info "  - The extension was signed with a different key"
        info "  - The extension was modified after signing"
        info "  - The extension is unsigned"
        return $EXIT_SIGNATURE_INVALID
    fi
}

# Main function for verify command
verify_main() {
    local extfile=""
    local keyfile=""

    while [[ $# -gt 0 ]]; do
        case "$1" in
            -k|--key)
                keyfile="$2"
                shift 2
                ;;
            -h|--help)
                verify_help
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
                error "Unknown option: $1" "" "Run 'duckdb-sign.sh verify --help' for usage"
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
        error "Extension file not specified" "" "Usage: duckdb-sign.sh verify -k <key.pub> <extension.duckdb_extension>"
        exit $EXIT_INVALID_ARGS
    fi

    if [[ -z "$keyfile" ]]; then
        error "Public key not specified" \
              "Use -k/--key option to specify the public key file"
        exit $EXIT_INVALID_ARGS
    fi

    # Perform verification
    verify_extension "$extfile" "$keyfile"
}

# Only run main if script is executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    verify_main "$@"
fi
