package plugin

import (
	"fmt"

	"aipc/platform/app-manager/manifest"
	"aipc/platform/app-manager/registry"
)

// DependencyResolver validates and resolves plugin dependencies
type DependencyResolver struct {
	pluginRegistry *registry.PluginRegistry
	appRegistry    *registry.Registry
}

// NewDependencyResolver creates a new DependencyResolver
func NewDependencyResolver(pr *registry.PluginRegistry, ar *registry.Registry) *DependencyResolver {
	return &DependencyResolver{
		pluginRegistry: pr,
		appRegistry:    ar,
	}
}

// Resolve validates that all required dependencies of a manifest are satisfied
// Returns (unsatisfied capabilities, error)
func (r *DependencyResolver) Resolve(m *manifest.AppManifest) ([]string, error) {
	if !m.HasPluginDependencies() {
		return nil, nil
	}

	var unsatisfied []string

	for _, dep := range m.Spec.PluginDependencies {
		providerID, found := r.pluginRegistry.FindByCapability(dep.Capability)
		if !found {
			if dep.Required {
				unsatisfied = append(unsatisfied, dep.Capability)
			}
			continue
		}

		// Check provider is installed and not failed
		provider, err := r.appRegistry.Get(providerID)
		if err != nil {
			if dep.Required {
				unsatisfied = append(unsatisfied, dep.Capability)
			}
			continue
		}

		if provider.State == registry.AppStateFailed {
			if dep.Required {
				unsatisfied = append(unsatisfied, dep.Capability)
			}
		}

		// TODO: semver version comparison with dep.MinVersion
	}

	if len(unsatisfied) > 0 {
		return unsatisfied, fmt.Errorf("unsatisfied required dependencies: %v", unsatisfied)
	}

	return nil, nil
}

// TopologicalSort returns apps in dependency order (plugins first, then consumers)
// Detects circular dependencies.
func (r *DependencyResolver) TopologicalSort(appIDs []string) ([]string, error) {
	// Build adjacency: appID -> depends on appIDs
	deps := make(map[string][]string)
	for _, id := range appIDs {
		app, err := r.appRegistry.Get(id)
		if err != nil {
			continue
		}
		for _, d := range app.Dependencies {
			providerID, found := r.pluginRegistry.FindByCapability(d.Capability)
			if found {
				deps[id] = append(deps[id], providerID)
			}
		}
	}

	// Kahn's algorithm
	inDegree := make(map[string]int)
	for _, id := range appIDs {
		if _, ok := inDegree[id]; !ok {
			inDegree[id] = 0
		}
	}
	for id, depList := range deps {
		_ = id
		for _, dep := range depList {
			inDegree[dep] += 0 // ensure exists
		}
		inDegree[id] = len(depList)
	}

	// Correct Kahn: inDegree[a] = number of appIDs that a depends on
	// We need reverse: inDegree[a] = number of things that depend on a
	// Actually for topological sort of "start order", we want:
	// If A depends on B, then B must start first.
	// So edge: A -> B means "A depends on B"
	// Topological sort gives B before A.

	// Rebuild with proper in-degree for dependents
	inDeg := make(map[string]int)
	adj := make(map[string][]string) // provider -> []consumers
	idSet := make(map[string]bool)

	for _, id := range appIDs {
		idSet[id] = true
		inDeg[id] = 0
	}

	for consumer, providers := range deps {
		for _, provider := range providers {
			if idSet[provider] {
				adj[provider] = append(adj[provider], consumer)
				inDeg[consumer]++
			}
		}
	}

	var queue []string
	for _, id := range appIDs {
		if inDeg[id] == 0 {
			queue = append(queue, id)
		}
	}

	var sorted []string
	for len(queue) > 0 {
		node := queue[0]
		queue = queue[1:]
		sorted = append(sorted, node)

		for _, neighbor := range adj[node] {
			inDeg[neighbor]--
			if inDeg[neighbor] == 0 {
				queue = append(queue, neighbor)
			}
		}
	}

	if len(sorted) != len(appIDs) {
		return nil, fmt.Errorf("circular dependency detected among apps")
	}

	return sorted, nil
}
