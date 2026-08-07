package discovery

import "errors"

var (
	ErrInvalidAnnounce = errors.New("invalid ct-disc announce packet")
	ErrInvalidPacket   = errors.New("invalid ct-disc packet")
)
