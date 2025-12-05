#!/bin/bash
# parse_footer.sh - Parse DuckDB extension footer metadata
# Extracts metadata from the 512-byte footer structure
#
# Footer Layout (512 bytes total):
#   Offset 0-255:   Metadata (8 fields x 32 bytes each)
#   Offset 256-511: RSA Signature (256 bytes)
#
# Metadata Fields (32 bytes each, stored in reverse order from end):
#   Field 7 (offset 0):   Magic value ("4\0...")
#   Field 6 (offset 32):  Platform (e.g., "linux_amd64")
#   Field 5 (offset 64):  DuckDB version (e.g., "v1.1.0")
#   Field 4 (offset 96):  Extension version
#   Field 3 (offset 128): ABI type ("CPP", "C_STRUCT", etc.)
#   Field 2 (offset 160): Reserved
#   Field 1 (offset 192): Reserved
#   Field 0 (offset 224): Reserved

set -e

# Only set SCRIPT_DIR if not already set (when sourced from main script)
if [[ -z "${SCRIPT_DIR:-}" ]]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    source "${SCRIPT_DIR}/common.sh"
fi

readonly FOOTER_SIZE=512
readonly SIGNATURE_SIZE=256
readonly METADATA_SIZE=256
readonly FIELD_SIZE=32

# Read a null-terminated string from a specific offset in the footer
# Args:
#   $1 - Extension file path
#   $2 - Offset from start of footer (0-255 for metadata)
# Returns: String value (null bytes stripped)
read_footer_field() {
    local extfile="$1"
    local field_offset="$2"

    local filesize
    filesize=$(get_file_size "$extfile")
    local footer_start=$((filesize - FOOTER_SIZE))
    local absolute_offset=$((footer_start + field_offset))

    # Read 32 bytes and strip null bytes
    dd if="$extfile" bs=1 skip=$absolute_offset count=$FIELD_SIZE 2>/dev/null | tr -d '\0'
}

# Read raw bytes from footer
# Args:
#   $1 - Extension file path
#   $2 - Offset from start of footer
#   $3 - Number of bytes to read
# Returns: Raw bytes to stdout
read_footer_bytes() {
    local extfile="$1"
    local field_offset="$2"
    local count="$3"

    local filesize
    filesize=$(get_file_size "$extfile")
    local footer_start=$((filesize - FOOTER_SIZE))
    local absolute_offset=$((footer_start + field_offset))

    dd if="$extfile" bs=1 skip=$absolute_offset count=$count 2>/dev/null
}

# Get extension magic value
get_magic() {
    local extfile="$1"
    read_footer_field "$extfile" 0
}

# Get extension platform
get_platform() {
    local extfile="$1"
    read_footer_field "$extfile" 32
}

# Get DuckDB version
get_duckdb_version() {
    local extfile="$1"
    read_footer_field "$extfile" 64
}

# Get extension version
get_extension_version() {
    local extfile="$1"
    read_footer_field "$extfile" 96
}

# Get ABI type
get_abi_type() {
    local extfile="$1"
    read_footer_field "$extfile" 128
}

# Get signature bytes (256 bytes)
get_signature() {
    local extfile="$1"
    read_footer_bytes "$extfile" $METADATA_SIZE $SIGNATURE_SIZE
}

# Check if signature is present (not all zeros)
has_signature() {
    local extfile="$1"
    local sig_hex
    sig_hex=$(get_signature "$extfile" | xxd -p | tr -d '\n')

    # Check if signature is all zeros
    local zeros
    zeros=$(printf '%0512d' 0)  # 256 bytes = 512 hex chars

    if [[ "$sig_hex" == "$zeros" ]]; then
        return 1  # No signature (all zeros)
    else
        return 0  # Has signature
    fi
}

# Get signature as hex string
get_signature_hex() {
    local extfile="$1"
    get_signature "$extfile" | xxd -p -c 256
}

# Parse all metadata from extension
# Args:
#   $1 - Extension file path
#   $2 - Output format: "json" or "text" (default: text)
# Returns: Formatted metadata
parse_metadata() {
    local extfile="$1"
    local format="${2:-text}"

    if ! validate_extension "$extfile"; then
        return $?
    fi

    local magic platform duckdb_version ext_version abi_type
    magic=$(get_magic "$extfile")
    platform=$(get_platform "$extfile")
    duckdb_version=$(get_duckdb_version "$extfile")
    ext_version=$(get_extension_version "$extfile")
    abi_type=$(get_abi_type "$extfile")

    local has_sig="No"
    if has_signature "$extfile"; then
        has_sig="Yes"
    fi

    local filesize
    filesize=$(get_file_size "$extfile")

    if [[ "$format" == "json" ]]; then
        cat << EOF
{
  "file": "$extfile",
  "size": $filesize,
  "magic": "$magic",
  "platform": "$platform",
  "duckdb_version": "$duckdb_version",
  "extension_version": "$ext_version",
  "abi_type": "$abi_type",
  "has_signature": $( [[ "$has_sig" == "Yes" ]] && echo "true" || echo "false" )
}
EOF
    else
        cat << EOF
Extension: $extfile
Size:      $filesize bytes

Metadata:
  Magic:     $magic $([ "$magic" == "4" ] && echo "(valid)" || echo "(INVALID)")
  Platform:  $platform
  DuckDB:    $duckdb_version
  ABI Type:  $abi_type
  Version:   $ext_version

Signature:
  Present:   $has_sig
  Size:      $SIGNATURE_SIZE bytes
EOF
        if [[ "$has_sig" == "Yes" ]]; then
            local sig_hash
            sig_hash=$(get_signature "$extfile" | openssl dgst -sha256 | cut -d' ' -f2 | head -c 16)
            echo "  Hash:      SHA256:${sig_hash}..."
        fi
    fi
}

# Main function for standalone usage
parse_footer_main() {
    local extfile=""
    local format="text"
    local field=""

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --json)
                format="json"
                shift
                ;;
            --field)
                field="$2"
                shift 2
                ;;
            -h|--help)
                cat << EOF
Usage: parse_footer.sh [OPTIONS] <extension_file>

Parse and display DuckDB extension footer metadata.

Arguments:
  extension_file    Path to .duckdb_extension file

Options:
  --json            Output in JSON format
  --field FIELD     Output only specified field (magic, platform, duckdb_version,
                    extension_version, abi_type, signature)
  -h, --help        Show this help message

Examples:
  parse_footer.sh my_extension.duckdb_extension
  parse_footer.sh --json my_extension.duckdb_extension
  parse_footer.sh --field platform my_extension.duckdb_extension
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
        error "Extension file not specified" "" "Usage: parse_footer.sh <extension_file>"
        exit $EXIT_INVALID_ARGS
    fi

    if ! validate_file_exists "$extfile" "Extension file"; then
        exit $EXIT_FILE_NOT_FOUND
    fi

    if [[ -n "$field" ]]; then
        case "$field" in
            magic)
                get_magic "$extfile"
                ;;
            platform)
                get_platform "$extfile"
                ;;
            duckdb_version)
                get_duckdb_version "$extfile"
                ;;
            extension_version)
                get_extension_version "$extfile"
                ;;
            abi_type)
                get_abi_type "$extfile"
                ;;
            signature)
                get_signature_hex "$extfile"
                ;;
            *)
                error "Unknown field: $field" \
                      "Valid fields: magic, platform, duckdb_version, extension_version, abi_type, signature"
                exit $EXIT_INVALID_ARGS
                ;;
        esac
    else
        parse_metadata "$extfile" "$format"
    fi
}

# Only run main if script is executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    parse_footer_main "$@"
fi
