# ProjectAgamemnon

Planning, coordination, and agentic orchestration for the HomericIntelligence distributed agent mesh.

Part of [Odysseus](https://github.com/HomericIntelligence/Odysseus) — the HomericIntelligence meta-repo.

## Role

Agamemnon sits at the center of the HomericIntelligence pipeline:

```
User <-> Odysseus <-> Nestor <-> Agamemnon <-> agentic pipeline loop -> completion
```

It receives researched briefs from ProjectNestor and coordinates the full planning and
execution pipeline using a HMAS 4-layer agentic hierarchy.

See [AGENTS.md](AGENTS.md) for multi-agent handoff protocols.

## Building

```bash
# Prerequisites: cmake >= 3.20, ninja, GCC or Clang with C++20 support
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

## License

MIT

## Data & Privacy

ProjectAgamemnon processes infrastructure metadata only (agent IDs, Tailscale host
identifiers, task state). It does not collect personal data. See
[SECURITY.md](SECURITY.md) for the full data and privacy policy.
