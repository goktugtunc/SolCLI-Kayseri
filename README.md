# SolCLI

`SolCLI` is a simple Solana developer CLI written in C.

## Features

- `solcli download`: installs Rust, Solana CLI, AVM, and a compatible Anchor CLI when needed.
- `solcli agent`: opens an agentic Solana assistant for troubleshooting, guided workflows, and local scaffold actions.
- `solcli agent "task"`: runs a one-shot agentic task when the request is actionable.
- `solcli ask`: opens a question-focused Solana Q&A interface.
- `solcli ask "question"`: asks a one-shot Solana question.
- `solcli wallet`: manages local wallet profiles, active keypairs, network selection, balances, airdrops, and transfers.
- `solcli wallet assign`: assigns the active wallet to a selected project for future deploys.
- `solcli network`: lists and selects devnet/testnet/mainnet RPC networks.
- `solcli rpc`: sets or shows the active RPC URL.
- `solcli ping` and `solcli health`: checks current RPC latency, health, and version.
- `solcli build`, `solcli test`, `solcli deploy`, and `solcli clean`: select a Solana project in the current directory and run the right Anchor or native command.
- `solcli version`: shows SolCLI and installed tool versions.

The AI prompt is tuned around the Solana Skills ecosystem, including official guidance areas such as version compatibility, common errors, security checklists, testing strategy, IDL/client generation, and Solana tooling workflows. In `agent` mode, SolCLI can also execute certain local actions directly, such as scaffolding a basic Solana/Anchor project or writing a contract into an existing Anchor workspace, and it keeps session context across turns.

## Requirements

On Ubuntu/Debian-based systems, install:

```bash
sudo apt update
sudo apt install -y build-essential pkg-config libcurl4-openssl-dev curl
```

## Build And Install

```bash
make
make install
```

After installation, the binary is copied to `~/.local/bin/solcli`.

## API Key

For safety, provide the API key through an environment variable instead of hardcoding it:

```bash
export OPENAI_API_KEY="your-openai-api-key"
```

Optionally choose a different model:

```bash
export OPENAI_MODEL="gpt-4o-mini"
```

## Usage

```bash
solcli
solcli help
solcli download
solcli version
solcli wallet new
solcli wallet import
solcli wallet address
solcli wallet balance
solcli wallet airdrop
solcli wallet send <recipient> <amount>
solcli wallet list
solcli wallet active
solcli wallet assign
solcli wallet cluster devnet
solcli network list
solcli network use devnet
solcli network use mainnet
solcli rpc set https://api.devnet.solana.com
solcli rpc current
solcli ping
solcli health
solcli build --verbose
solcli test --watch
solcli deploy --devnet
solcli deploy --testnet
solcli deploy --mainnet
solcli clean
solcli agent
solcli agent "create a basic Solana project"
solcli agent "write a bank contract in the solana-starter project"
solcli ask
solcli ask "how does anchor account validation work?"
```

## Notes

- The `download` command requires an internet connection.
- You may need to open a new shell session after installing Solana tools.
- On some Linux distributions, the AVM-provided `anchor` binary may be incompatible; in that case `SolCLI` falls back to building `anchor-cli` from source.
- If you shared an API key publicly, revoke it and create a new one.
