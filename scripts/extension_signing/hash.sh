#!/bin/bash
# hash.sh - Two-level SHA256 hash computation for DuckDB extensions
# Computes the hash used for signature verification
#
# Algorithm:
# 1. Split file into 1MB chunks (excluding last 256 bytes for signature)
# 2. Compute SHA256 hash for each chunk
# 3. Concatenate all chunk hashes
# 4. Compute SHA256 of concatenation -> final hash

set -e

# Only set SCRIPT_DIR if not already set (when sourced from main script)
if [[ -z "${SCRIPT_DIR:-}" ]]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    source "${SCRIPT_DIR}/common.sh"
fi

readonly CHUNK_SIZE=$((1024 * 1024))  # 1MB
readonly SIGNATURE_SIZE=256

# Compute two-level hash of extension file
# Args:
#   $1 - Extension file path
#   $2 - Output file for binary hash (optional, defaults to stdout as hex)
# Returns: Binary hash (32 bytes) written to stdout or file
compute_extension_hash() {
    local extfile="$1"
    local outfile="${2:-}"

    if ! validate_file_exists "$extfile" "Extension file"; then
        return $EXIT_FILE_NOT_FOUND
    fi

    local filesize
    filesize=$(get_file_size "$extfile")

    # Content to hash is everything except the last 256 bytes (signature)
    local content_size=$((filesize - SIGNATURE_SIZE))

    if [[ $content_size -le 0 ]]; then
        error "Invalid extension file" \
              "File is too small to contain valid content" \
              "Expected at least $((SIGNATURE_SIZE + 1)) bytes"
        return $EXIT_INVALID_EXTENSION
    fi

    verbose "Computing hash for $extfile"
    verbose "File size: $filesize bytes"
    verbose "Content size (excluding signature): $content_size bytes"

    # Create temp directory for chunks
    local tmpdir
    tmpdir=$(mktemp -d)
    trap "rm -rf '$tmpdir'" EXIT

    local hash_concat="${tmpdir}/hash_concat"
    : > "$hash_concat"  # Create empty file

    local offset=0
    local chunk_num=0

    while [[ $offset -lt $content_size ]]; do
        local remaining=$((content_size - offset))
        local chunk_bytes=$CHUNK_SIZE

        if [[ $remaining -lt $CHUNK_SIZE ]]; then
            chunk_bytes=$remaining
        fi

        verbose "Processing chunk $chunk_num: offset=$offset, size=$chunk_bytes"

        # Extract chunk and compute its SHA256 hash
        dd if="$extfile" bs=1 skip=$offset count=$chunk_bytes 2>/dev/null | \
            openssl dgst -binary -sha256 >> "$hash_concat"

        offset=$((offset + chunk_bytes))
        chunk_num=$((chunk_num + 1))
    done

    verbose "Processed $chunk_num chunks"
    verbose "Computing final hash of concatenated chunk hashes"

    # Compute final hash of concatenated hashes
    if [[ -n "$outfile" ]]; then
        openssl dgst -binary -sha256 "$hash_concat" > "$outfile"
    else
        # Output as hex to stdout
        openssl dgst -binary -sha256 "$hash_concat" | xxd -p -c 32
    fi
}

# Faster implementation using dd with larger blocks
compute_extension_hash_fast() {
    local extfile="$1"
    local outfile="${2:-}"

    if ! validate_file_exists "$extfile" "Extension file"; then
        return $EXIT_FILE_NOT_FOUND
    fi

    local filesize
    filesize=$(get_file_size "$extfile")

    # Content to hash is everything except the last 256 bytes (signature)
    local content_size=$((filesize - SIGNATURE_SIZE))

    if [[ $content_size -le 0 ]]; then
        error "Invalid extension file" \
              "File is too small to contain valid content"
        return $EXIT_INVALID_EXTENSION
    fi

    # Create temp directory
    local tmpdir
    tmpdir=$(mktemp -d)
    trap "rm -rf '$tmpdir'" EXIT

    # Extract content (without signature) to temp file
    local content_file="${tmpdir}/content"
    dd if="$extfile" of="$content_file" bs=$CHUNK_SIZE count=$((content_size / CHUNK_SIZE)) 2>/dev/null

    # Handle remaining bytes
    local remaining=$((content_size % CHUNK_SIZE))
    if [[ $remaining -gt 0 ]]; then
        local full_chunks=$((content_size / CHUNK_SIZE))
        dd if="$extfile" bs=1 skip=$((full_chunks * CHUNK_SIZE)) count=$remaining 2>/dev/null >> "$content_file"
    fi

    # Now split and hash
    local hash_concat="${tmpdir}/hash_concat"
    : > "$hash_concat"

    # Split into 1MB chunks
    cd "$tmpdir"
    split -b $CHUNK_SIZE "$content_file" chunk_

    # Hash each chunk
    for chunk in chunk_*; do
        openssl dgst -binary -sha256 "$chunk" >> "$hash_concat"
    done

    # Compute final hash
    if [[ -n "$outfile" ]]; then
        openssl dgst -binary -sha256 "$hash_concat" > "$outfile"
    else
        openssl dgst -binary -sha256 "$hash_concat" | xxd -p -c 32
    fi
}

# Main function for standalone usage
hash_main() {
    local extfile=""
    local outfile=""
    local fast=0

    while [[ $# -gt 0 ]]; do
        case "$1" in
            -o|--output)
                outfile="$2"
                shift 2
                ;;
            --fast)
                fast=1
                shift
                ;;
            -h|--help)
                cat << EOF
Usage: hash.sh [OPTIONS] <extension_file>

Compute two-level SHA256 hash of a DuckDB extension.

Arguments:
  extension_file    Path to .duckdb_extension file

Options:
  -o, --output FILE    Write binary hash to FILE (default: hex to stdout)
  --fast               Use faster implementation (requires more temp space)
  -h, --help           Show this help message

Output:
  32-byte SHA256 hash (hex on stdout, or binary to file)
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
        error "Extension file not specified" "" "Usage: hash.sh <extension_file>"
        exit $EXIT_INVALID_ARGS
    fi

    if [[ $fast -eq 1 ]]; then
        compute_extension_hash_fast "$extfile" "$outfile"
    else
        compute_extension_hash "$extfile" "$outfile"
    fi
}

# Only run main if script is executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    hash_main "$@"
fi
