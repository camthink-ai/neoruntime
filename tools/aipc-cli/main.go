/*
AIPC CLI - Command Line Interface for AIPC Edge AI Platform

Usage:
  aipc-cli [command] [flags]

Available Commands:
  app         Manage container applications
  model       Manage AI models
  stream      Manage video streams
  device      Control device peripherals
  event       Manage event bus
  system      System management
  completion  Generate shell completion scripts
  help        Help about any command

Flags:
  -a, --api string      API server address (default "http://localhost:8080")
      --config string   Config file (default ~/.aipc/config.yaml)
  -o, --output string   Output format: table, json, yaml (default "table")
  -v, --verbose         Enable verbose output
      --version         Show version

Use "aipc-cli [command] --help" for more information about a command.
*/
package main

import (
	"aipc/tools/aipc-cli/cmd"
)

func main() {
	cmd.Execute()
}
