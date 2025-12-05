#!/bin/bash
# duckdb-sign.sh - DuckDB Extension Signing Tool
# Main entry point for extension signing operations
#
# Usage: duckdb-sign.sh <command> [options]
#
# Commands:
#   keygen    Generate an RSA-2048 key pair
#   sign      Sign an extension with a private key
#   verify    Verify an extension's signature
#   info      Display extension metadata

set -e

readonly VERSION="1.0.0"
readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Source common utilities
source "${SCRIPT_DIR}/common.sh"

# Show version
show_version() {
    echo "duckdb-sign.sh version $VERSION"
}

# Show help
show_help() {
    cat << EOF
DuckDB Extension Signing Tool v$VERSION

Usage: duckdb-sign.sh <command> [options]

Commands:
  keygen    Generate an RSA-2048 key pair for signing extensions
  sign      Sign an extension with a private key
  verify    Verify an extension's signature against a public key
  info      Display extension metadata and signature status

Global Options:
  -h, --help      Show this help message
  -V, --version   Show version information
  -q, --quiet     Suppress non-error output
  -v, --verbose   Show detailed output

Examples:
  # Generate a new key pair
  duckdb-sign.sh keygen -o ~/my_signing_key

  # Sign an extension
  duckdb-sign.sh sign -k ~/my_signing_key.pem my_extension.duckdb_extension

  # Verify a signature
  duckdb-sign.sh verify -k ~/my_signing_key.pub my_extension.duckdb_extension

  # Show extension info
  duckdb-sign.sh info my_extension.duckdb_extension

For command-specific help, run: duckdb-sign.sh <command> --help
EOF
}

# Main dispatch
main() {
    # Check for global options first
    case "${1:-}" in
        -h|--help)
            show_help
            exit 0
            ;;
        -V|--version)
            show_version
            exit 0
            ;;
    esac

    # Get command
    local command="${1:-}"

    if [[ -z "$command" ]]; then
        error "No command specified" "" "Run 'duckdb-sign.sh --help' for usage"
        exit $EXIT_INVALID_ARGS
    fi

    shift

    # Check dependencies
    if ! check_dependencies; then
        exit $EXIT_INVALID_ARGS
    fi

    # Dispatch to subcommand
    case "$command" in
        keygen)
            source "${SCRIPT_DIR}/keygen.sh"
            keygen_main "$@"
            ;;
        sign)
            source "${SCRIPT_DIR}/sign.sh"
            sign_main "$@"
            ;;
        verify)
            source "${SCRIPT_DIR}/verify.sh"
            verify_main "$@"
            ;;
        info)
            source "${SCRIPT_DIR}/info.sh"
            info_main "$@"
            ;;
        *)
            error "Unknown command: $command" "" "Run 'duckdb-sign.sh --help' for usage"
            exit $EXIT_INVALID_ARGS
            ;;
    esac
}

main "$@"
