# Changelog

### Core
- Added full state transition context (previous state, current state, cause)
- Developped task execution context 
- Clarified state transition semantics (event-driven, edge-based)

### Platform Wrappers
- Aligned ESP-IDF and Arduino behavior for state transitions
- Added an optional function to publish on state transition
- Simplified QoS configuration via overloaded APIs
- General internal cleanup of redundant code paths

### Examples

Added new examples and corrected the previous ones with more context awareness.

### Notes
- APIs are still evolving and not yet frozen
