#!/bin/bash
# info.sh - Display DuckDB extension metadata and signature status

set -e

# Only set SCRIPT_DIR if not already set (when sourced from main script)
if [[ -z "${SCRIPT_DIR:-}" ]]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    source "${SCRIPT_DIR}/common.sh"
fi
source "${SCRIPT_DIR}/parse_footer.sh"

# Show help for info command
info_help() {
    cat << EOF
Usage: duckdb-sign.sh info [OPTIONS] <extension_file>

Display DuckDB extension metadata and signature status.

Arguments:
  extension_file    Path to .duckdb_extension file

Options:
  --json            Output in JSON format
  -h, --help        Show this help message

Examples:
  duckdb-sign.sh info ./my_extension.duckdb_extension
  duckdb-sign.sh info --json ./my_extension.duckdb_extension

Exit Codes:
  0   Success
  1   Invalid arguments
  2   Extension file not found
  5   Invalid extension format
EOF
}

# Main function for info command
info_main() {
    local extfile=""
    local format="text"

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --json)
                format="json"
                shift
                ;;
            -h|--help)
                info_help
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
                error "Unknown option: $1" "" "Run 'duckdb-sign.sh info --help' for usage"
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
        error "Extension file not specified" "" "Usage: duckdb-sign.sh info <extension.duckdb_extension>"
        exit $EXIT_INVALID_ARGS
    fi

    # Display metadata
    parse_metadata "$extfile" "$format"
}

# Only run main if script is executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    info_main "$@"
fi
