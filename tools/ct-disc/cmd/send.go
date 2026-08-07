package cmd

import (
	"encoding/json"
	"fmt"
	"os"

	"github.com/camthink/ct-disc/pkg/mqttclient"
	"github.com/camthink/ct-disc/pkg/output"
	"github.com/spf13/cobra"
	"time"
)

var (
	sendBroker   string
	sendPayload  string
	sendWait     int
	sendUsername  string
	sendPassword  string
)

var sendCmd = &cobra.Command{
	Use:   "send <sn> <command>",
	Short: "Send command to device via MQTT",
	Long:  "Send a command to a specific device through the MQTT broker.\nUses ct/cmd/{sn} topic and waits for response on ct/resp/{sn}.",
	Args:  cobra.ExactArgs(2),
	RunE:  runSend,
}

func init() {
	sendCmd.Flags().StringVar(&sendBroker, "broker", "", "MQTT broker address (e.g. tcp://broker:1883)")
	sendCmd.Flags().StringVar(&sendPayload, "payload", "{}", "command payload (JSON)")
	sendCmd.Flags().IntVar(&sendWait, "wait", 10, "wait time for response in seconds")
	sendCmd.Flags().StringVar(&sendUsername, "username", "", "MQTT username")
	sendCmd.Flags().StringVar(&sendPassword, "password", "", "MQTT password")
	sendCmd.MarkFlagRequired("broker")
	rootCmd.AddCommand(sendCmd)
}

func runSend(cmd *cobra.Command, args []string) error {
	sn := args[0]
	command := args[1]
	printer := output.NewPrinter(outputFmt)

	// validate payload is valid JSON
	if !json.Valid([]byte(sendPayload)) {
		return fmt.Errorf("invalid JSON payload: %s", sendPayload)
	}

	client, err := mqttclient.NewClient(mqttclient.Config{
		Broker:   sendBroker,
		Username: sendUsername,
		Password: sendPassword,
	})
	if err != nil {
		return fmt.Errorf("MQTT connection failed: %w", err)
	}
	defer client.Close()

	fmt.Fprintf(os.Stderr, "Sending '%s' to %s via %s...\n", command, sn, sendBroker)

	resp, err := client.SendCommand(sn, command, sendPayload, time.Duration(sendWait)*time.Second)
	if err != nil {
		return err
	}

	output.Success(fmt.Sprintf("Response from %s:", sn))
	return printer.Print(map[string]string{
		"sn":      sn,
		"command": command,
		"response": resp.Payload,
	})
}
