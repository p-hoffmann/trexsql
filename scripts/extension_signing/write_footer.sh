#!/bin/bash
# write_footer.sh - Write signature to DuckDB extension footer
# Writes a 256-byte RSA signature to the extension's footer
#
# Footer Layout (512 bytes total):
#   Offset 0-255:   Metadata (preserved)
#   Offset 256-511: RSA Signature (256 bytes, written by this script)

set -e

# Only set SCRIPT_DIR if not already set (when sourced from main script)
if [[ -z "${SCRIPT_DIR:-}" ]]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    source "${SCRIPT_DIR}/common.sh"
fi

readonly FOOTER_SIZE=512
readonly SIGNATURE_SIZE=256
readonly METADATA_SIZE=256

# Write signature to extension file
# Args:
#   $1 - Extension file path (modified in-place)
#   $2 - Signature file path (must be exactly 256 bytes)
# Returns: 0 on success, non-zero on error
write_signature() {
    local extfile="$1"
    local sigfile="$2"

    if ! validate_file_exists "$extfile" "Extension file"; then
        return $EXIT_FILE_NOT_FOUND
    fi

    if ! validate_file_exists "$sigfile" "Signature file"; then
        return $EXIT_FILE_NOT_FOUND
    fi

    # Verify signature is exactly 256 bytes
    local sigsize
    sigsize=$(get_file_size "$sigfile")

    if [[ $sigsize -ne $SIGNATURE_SIZE ]]; then
        error "Invalid signature size" \
              "Expected $SIGNATURE_SIZE bytes, got $sigsize bytes" \
              "Ensure the signature was generated correctly with RSA-2048"
        return $EXIT_INVALID_EXTENSION
    fi

    local filesize
    filesize=$(get_file_size "$extfile")

    # Calculate signature offset (last 256 bytes of file)
    local sig_offset=$((filesize - SIGNATURE_SIZE))

    verbose "Writing signature to $extfile"
    verbose "File size: $filesize bytes"
    verbose "Signature offset: $sig_offset"

    # Write signature using dd with conv=notrunc to modify in-place
    dd if="$sigfile" of="$extfile" bs=1 seek=$sig_offset conv=notrunc 2>/dev/null

    verbose "Signature written successfully"
    return 0
}

# Write signature from stdin
# Args:
#   $1 - Extension file path (modified in-place)
# Stdin: 256 bytes of signature data
write_signature_stdin() {
    local extfile="$1"

    if ! validate_file_exists "$extfile" "Extension file"; then
        return $EXIT_FILE_NOT_FOUND
    fi

    # Create temp file for signature
    local tmpfile
    tmpfile=$(mktemp)
    trap "rm -f '$tmpfile'" EXIT

    # Read exactly 256 bytes from stdin
    dd of="$tmpfile" bs=$SIGNATURE_SIZE count=1 2>/dev/null

    local sigsize
    sigsize=$(get_file_size "$tmpfile")

    if [[ $sigsize -ne $SIGNATURE_SIZE ]]; then
        error "Invalid signature size from stdin" \
              "Expected $SIGNATURE_SIZE bytes, got $sigsize bytes"
        return $EXIT_INVALID_EXTENSION
    fi

    write_signature "$extfile" "$tmpfile"
}

# Copy extension and write signature to the copy
# Args:
#   $1 - Source extension file path
#   $2 - Destination extension file path
#   $3 - Signature file path (must be exactly 256 bytes)
# Returns: 0 on success, non-zero on error
write_signature_to_copy() {
    local srcfile="$1"
    local dstfile="$2"
    local sigfile="$3"

    if ! validate_file_exists "$srcfile" "Source extension file"; then
        return $EXIT_FILE_NOT_FOUND
    fi

    verbose "Copying $srcfile to $dstfile"
    cp "$srcfile" "$dstfile"

    write_signature "$dstfile" "$sigfile"
}

# Clear signature (set to zeros)
# Args:
#   $1 - Extension file path (modified in-place)
clear_signature() {
    local extfile="$1"

    if ! validate_file_exists "$extfile" "Extension file"; then
        return $EXIT_FILE_NOT_FOUND
    fi

    local filesize
    filesize=$(get_file_size "$extfile")
    local sig_offset=$((filesize - SIGNATURE_SIZE))

    verbose "Clearing signature in $extfile"

    # Write 256 bytes of zeros
    dd if=/dev/zero of="$extfile" bs=1 seek=$sig_offset count=$SIGNATURE_SIZE conv=notrunc 2>/dev/null

    verbose "Signature cleared"
    return 0
}

# Main function for standalone usage
write_footer_main() {
    local extfile=""
    local sigfile=""
    local output=""
    local clear=0

    while [[ $# -gt 0 ]]; do
        case "$1" in
            -s|--signature)
                sigfile="$2"
                shift 2
                ;;
            -o|--output)
                output="$2"
                shift 2
                ;;
            --clear)
                clear=1
                shift
                ;;
            --stdin)
                sigfile="-"
                shift
                ;;
            -h|--help)
                cat << EOF
Usage: write_footer.sh [OPTIONS] <extension_file>

Write or clear signature in DuckDB extension footer.

Arguments:
  extension_file    Path to .duckdb_extension file

Options:
  -s, --signature FILE    Signature file to write (256 bytes)
  --stdin                 Read signature from stdin
  -o, --output FILE       Write to new file instead of modifying in-place
  --clear                 Clear signature (set to zeros)
  -h, --help              Show this help message

Examples:
  # Write signature in-place
  write_footer.sh -s signature.bin my_extension.duckdb_extension

  # Write signature to new file
  write_footer.sh -s signature.bin -o signed.duckdb_extension unsigned.duckdb_extension

  # Clear signature
  write_footer.sh --clear my_extension.duckdb_extension

  # Read signature from stdin
  openssl ... | write_footer.sh --stdin my_extension.duckdb_extension
EOF
                exit 0
                ;;
            -*)
                error "Unknown option: $1"
                exit $EXIT_INVALID_ARGS
                ;;
            *)
                extfile="$1"
                shift
                ;;
        esac
    done

    if [[ -z "$extfile" ]]; then
        error "Extension file not specified" "" "Usage: write_footer.sh <extension_file>"
        exit $EXIT_INVALID_ARGS
    fi

    if [[ $clear -eq 1 ]]; then
        if [[ -n "$output" ]]; then
            cp "$extfile" "$output"
            clear_signature "$output"
        else
            clear_signature "$extfile"
        fi
    elif [[ -n "$sigfile" ]]; then
        if [[ "$sigfile" == "-" ]]; then
            if [[ -n "$output" ]]; then
                cp "$extfile" "$output"
                write_signature_stdin "$output"
            else
                write_signature_stdin "$extfile"
            fi
        elif [[ -n "$output" ]]; then
            write_signature_to_copy "$extfile" "$output" "$sigfile"
        else
            write_signature "$extfile" "$sigfile"
        fi
    else
        error "No signature specified" "" "Use -s/--signature or --stdin to provide signature, or --clear to remove"
        exit $EXIT_INVALID_ARGS
    fi

    info "Signature written successfully"
}

# Only run main if script is executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    write_footer_main "$@"
fi
