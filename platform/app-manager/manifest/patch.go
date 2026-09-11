package manifest

import (
	"bytes"
	"fmt"
	"strings"

	"gopkg.in/yaml.v3"
)

// FieldPatch is one field-level edit: a dotted path into the document plus
// the JSON-encoded replacement value. A JSON null value deletes the target
// key (clearing a map like spec.models leaves no empty node behind).
type FieldPatch struct {
	Path  string // e.g. "spec.permissions.inference.models"
	Value []byte // JSON bytes; null = delete
}

// PatchDocument applies ops to src and returns the re-marshaled document.
// Comments, unknown fields, key order and untouched subtrees are preserved:
// the document is edited as a yaml.Node tree, never regenerated from a
// config object. Indentation normalizes to 2 spaces. Ops are applied in
// order against the same tree, so a later op may target a path an earlier
// op created; on any error the original document is left untouched (the
// tree is only marshaled after every op succeeded).
func PatchDocument(src []byte, ops []FieldPatch) ([]byte, error) {
	var doc yaml.Node
	if err := yaml.Unmarshal(src, &doc); err != nil {
		return nil, fmt.Errorf("parse yaml: %w", err)
	}
	if doc.Kind != yaml.DocumentNode || len(doc.Content) == 0 {
		return nil, fmt.Errorf("document has no content")
	}
	root := doc.Content[0]
	if root.Kind != yaml.MappingNode {
		return nil, fmt.Errorf("document root is not a mapping")
	}

	for _, op := range ops {
		if err := validatePatchPath(op.Path); err != nil {
			return nil, err
		}
		value, err := jsonToNode(op.Value)
		if err != nil {
			return nil, fmt.Errorf("patch %s: %w", op.Path, err)
		}
		if err := setPath(root, strings.Split(op.Path, "."), value); err != nil {
			return nil, fmt.Errorf("patch %s: %w", op.Path, err)
		}
	}

	var out bytes.Buffer
	enc := yaml.NewEncoder(&out)
	enc.SetIndent(2)
	if err := enc.Encode(&doc); err != nil {
		return nil, fmt.Errorf("encode yaml: %w", err)
	}
	if err := enc.Close(); err != nil {
		return nil, fmt.Errorf("encode yaml: %w", err)
	}
	return out.Bytes(), nil
}

// validatePatchPath rejects paths that would silently resolve to the wrong
// place: empty, or containing empty segments (leading/trailing/doubled dots).
func validatePatchPath(path string) error {
	if path == "" {
		return fmt.Errorf("empty patch path")
	}
	for _, seg := range strings.Split(path, ".") {
		if seg == "" {
			return fmt.Errorf("patch path %q contains an empty segment", path)
		}
	}
	return nil
}

// jsonToNode converts JSON bytes to a yaml.Node with types intact. JSON is a
// YAML 1.2 subset, so parsing it as YAML resolves tags exactly as JSON
// would: true → !!bool, 123 → !!int, while "true"/"50%" stay double-quoted
// !!str scalars. Strings keep their quotes in the output, which is what
// preserves the distinction between string "true" and boolean true — the
// whole point of routing patch values through JSON.
func jsonToNode(b []byte) (*yaml.Node, error) {
	var doc yaml.Node
	if err := yaml.Unmarshal(b, &doc); err != nil {
		return nil, fmt.Errorf("parse json value: %w", err)
	}
	if doc.Kind != yaml.DocumentNode || len(doc.Content) == 0 {
		return nil, fmt.Errorf("json value has no content")
	}
	n := doc.Content[0]
	// JSON collections arrive in flow style ({...} / [...]); switch them to
	// block style so patched values read like the hand-written YAML around
	// them. Scalar quoting is deliberately left untouched.
	clearFlowStyle(n)
	return n, nil
}

func clearFlowStyle(n *yaml.Node) {
	if n.Kind != yaml.MappingNode && n.Kind != yaml.SequenceNode {
		return
	}
	if n.Style == yaml.FlowStyle {
		n.Style = 0
	}
	for _, c := range n.Content {
		clearFlowStyle(c)
	}
}

// setPath walks (creating as needed) the mapping chain along segs and
// replaces or appends the value at the last segment. A null value deletes
// the target key instead (no-op when absent) — that is how a patch clears a
// map like spec.models without leaving `models:` empty nodes behind.
// Existing key nodes are preserved — only the value node is swapped — and
// comments attached to the replaced value carry over to the replacement
// when it brings none of its own.
func setPath(root *yaml.Node, segs []string, value *yaml.Node) error {
	cur := root
	for _, seg := range segs[:len(segs)-1] {
		next, err := childMapping(cur, seg)
		if err != nil {
			return err
		}
		if next == nil {
			key := &yaml.Node{Kind: yaml.ScalarNode, Tag: "!!str", Value: seg}
			next = &yaml.Node{Kind: yaml.MappingNode, Tag: "!!map"}
			cur.Content = append(cur.Content, key, next)
		}
		cur = next
	}
	last := segs[len(segs)-1]
	for i := 0; i+1 < len(cur.Content); i += 2 {
		if cur.Content[i].Value == last {
			if isNullNode(value) {
				cur.Content = append(cur.Content[:i], cur.Content[i+2:]...)
				return nil
			}
			old := cur.Content[i+1]
			if value.HeadComment == "" {
				value.HeadComment = old.HeadComment
			}
			if value.LineComment == "" {
				value.LineComment = old.LineComment
			}
			if value.FootComment == "" {
				value.FootComment = old.FootComment
			}
			cur.Content[i+1] = value
			return nil
		}
	}
	if isNullNode(value) {
		return nil // deleting a key that is not there is a no-op
	}
	key := &yaml.Node{Kind: yaml.ScalarNode, Tag: "!!str", Value: last}
	cur.Content = append(cur.Content, key, value)
	return nil
}

// isNullNode reports whether n is the null scalar JSON null parses to.
func isNullNode(n *yaml.Node) bool {
	return n.Kind == yaml.ScalarNode && n.Tag == "!!null"
}

// childMapping returns the mapping stored under key, or nil when the key is
// absent. A non-mapping value — or a non-mapping parent — is an error:
// silently replacing it would delete data the patch never mentioned.
func childMapping(n *yaml.Node, key string) (*yaml.Node, error) {
	if n.Kind != yaml.MappingNode {
		return nil, fmt.Errorf("cannot descend into %s at %q", kindName(n), key)
	}
	for i := 0; i+1 < len(n.Content); i += 2 {
		if n.Content[i].Value == key {
			v := n.Content[i+1]
			if v.Kind != yaml.MappingNode {
				return nil, fmt.Errorf("%q is %s, cannot descend into it", key, kindName(v))
			}
			return v, nil
		}
	}
	return nil, nil
}

func kindName(n *yaml.Node) string {
	switch n.Kind {
	case yaml.ScalarNode:
		return "a scalar"
	case yaml.SequenceNode:
		return "a sequence"
	case yaml.MappingNode:
		return "a mapping"
	default:
		return "an unknown node"
	}
}
