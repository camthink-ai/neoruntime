package mqttclient

import (
	"crypto/rand"
	"fmt"
	"sync"
	"time"

	pahomqtt "github.com/eclipse/paho.mqtt.golang"
)

type Config struct {
	Broker   string
	Username string
	Password string
}

type CommandResponse struct {
	ID      string
	Payload string
}

type Client struct {
	client   pahomqtt.Client
	config   Config
	respMu   sync.Mutex
	pending  map[string]chan CommandResponse
}

func NewClient(cfg Config) (*Client, error) {
	c := &Client{
		config:  cfg,
		pending: make(map[string]chan CommandResponse),
	}

	// generate random client ID
	b := make([]byte, 8)
	rand.Read(b)
	clientID := fmt.Sprintf("ct-disc-%x", b)

	opts := pahomqtt.NewClientOptions().
		AddBroker(cfg.Broker).
		SetClientID(clientID).
		SetAutoReconnect(true).
		SetConnectTimeout(10 * time.Second)

	if cfg.Username != "" {
		opts.SetUsername(cfg.Username)
		opts.SetPassword(cfg.Password)
	}

	opts.SetDefaultPublishHandler(func(client pahomqtt.Client, msg pahomqtt.Message) {
		c.handleResponse(msg.Topic(), string(msg.Payload()))
	})

	c.client = pahomqtt.NewClient(opts)
	if token := c.client.Connect(); token.Wait() && token.Error() != nil {
		return nil, fmt.Errorf("MQTT connect failed: %w", token.Error())
	}

	return c, nil
}

func (c *Client) SendCommand(sn, command, payload string, wait time.Duration) (*CommandResponse, error) {
	id := generateID()
	topic := fmt.Sprintf("ct/cmd/%s", sn)
	respTopic := fmt.Sprintf("ct/resp/%s", sn)

	// subscribe to response
	if token := c.client.Subscribe(respTopic, 1, nil); token.Wait() && token.Error() != nil {
		return nil, fmt.Errorf("subscribe failed: %w", token.Error())
	}

	// register pending response
	ch := make(chan CommandResponse, 1)
	c.respMu.Lock()
	c.pending[id] = ch
	c.respMu.Unlock()

	defer func() {
		c.respMu.Lock()
		delete(c.pending, id)
		c.respMu.Unlock()
		c.client.Unsubscribe(respTopic)
	}()

	// publish command
	msg := fmt.Sprintf(`{"cmd":"%s","payload":%s,"id":"%s"}`, command, payload, id)
	if token := c.client.Publish(topic, 1, false, msg); token.Wait() && token.Error() != nil {
		return nil, fmt.Errorf("publish failed: %w", token.Error())
	}

	// wait for response
	select {
	case resp := <-ch:
		return &resp, nil
	case <-time.After(wait):
		return nil, fmt.Errorf("timeout waiting for response from %s", sn)
	}
}

func (c *Client) handleResponse(topic, payload string) {
	c.respMu.Lock()
	defer c.respMu.Unlock()

	// broadcast to all pending (response doesn't include ID in simple version)
	for id, ch := range c.pending {
		select {
		case ch <- CommandResponse{ID: id, Payload: payload}:
		default:
		}
	}
}

func (c *Client) Close() {
	c.client.Disconnect(250)
}

func generateID() string {
	b := make([]byte, 4)
	rand.Read(b)
	return fmt.Sprintf("%x", b)
}
