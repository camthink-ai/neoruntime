package main

import "github.com/camthink/ct-disc/cmd"

var version = "0.1.0"

func main() {
	cmd.SetVersion(version)
	cmd.Execute()
}
