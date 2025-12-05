# DuckDB Extension Signing Tools

Command-line tools for signing DuckDB extensions with custom RSA-2048 keys.

## Requirements

- Linux operating system
- OpenSSL
- Bash 4+

## Quick Start

```bash
# 1. Generate a key pair
./duckdb-sign.sh keygen -o ~/my_key

# 2. Sign your extension
./duckdb-sign.sh sign -k ~/my_key.pem my_extension.duckdb_extension

# 3. Verify the signature
./duckdb-sign.sh verify -k ~/my_key.pub my_extension.duckdb_extension

# 4. Build DuckDB with your public key
cd /path/to/duckdb
cmake -DDUCKDB_CUSTOM_SIGNING_KEYS=~/my_key.pub ..
make
```

## Commands

### keygen - Generate Key Pair

```bash
./duckdb-sign.sh keygen [OPTIONS]

Options:
  -o, --output PATH   Base path for output files (default: ./signing_key)
  -f, --force         Overwrite existing files
  -h, --help          Show help message
```

### sign - Sign Extension

```bash
./duckdb-sign.sh sign [OPTIONS] <extension_file>

Options:
  -k, --key FILE      Path to private key file (required)
  -o, --output FILE   Output path (default: in-place modification)
  -h, --help          Show help message

Environment:
  DUCKDB_SIGN_KEY     Default private key path
```

### verify - Verify Signature

```bash
./duckdb-sign.sh verify [OPTIONS] <extension_file>

Options:
  -k, --key FILE      Path to public key file (required)
  -h, --help          Show help message
```

### info - Display Extension Info

```bash
./duckdb-sign.sh info [OPTIONS] <extension_file>

Options:
  --json              Output in JSON format
  -h, --help          Show help message
```

## Building DuckDB with Custom Keys

To trust extensions signed with your keys, rebuild DuckDB with the public keys embedded:

```bash
# Single key
cmake -DDUCKDB_CUSTOM_SIGNING_KEYS=/path/to/key.pub ..

# Multiple keys
cmake -DDUCKDB_CUSTOM_SIGNING_KEYS="/path/to/key1.pub;/path/to/key2.pub" ..
```

## Security Notes

- Keep private keys secure with restricted permissions (chmod 600)
- Never commit private keys to version control
- Use separate keys for different purposes
- Back up your keys securely

## Exit Codes

| Code | Description |
|------|-------------|
| 0    | Success |
| 1    | Invalid arguments |
| 2    | File not found |
| 3    | Invalid key format |
| 4    | Invalid private key |
| 5    | Invalid extension format |
| 6    | Signing failed |
| 10   | Signature invalid |
