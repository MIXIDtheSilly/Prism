# Independent review handoff

For the generated Horizon bytecode/native reference and concrete reverse-engineering
findings, see [reverse-reference.md](reverse-reference.md). Search it with
`python tools/reverse.py search <term>`.

## Status (2026-10-10)

- **Layered image sharing:** addressed by design. A layered or non-AHardwareBuffer image is
  backed: each process keeps its own copy and they share a single-layer mirror as tall as the
  layers stacked, copied into after writes and out of before reads. On the emulator VrShell's
  1440x1584 two-layer eye buffers reach the compositor this way (stats lines: copies into
  mirrors in VrShell, out of them in the compositor).
- **fd import left in the chain:** fixed. `put_in_chain()` replaces the
  `VkImportMemoryFdInfoKHR` node with the AHardwareBuffer import.
- **Export budget:** fixed. The exporter keeps its end of the socket and a thread
  (`prism_exports`) sends the buffer on each import's request: unlimited imports through any
  dup, and a failed import costs nothing. When the last importer fd closes, the export ends.
- **Tracking tables:** fixed. Backed images live in blocks that never move, so `owner` pointers
  stay valid, and imports in a growable array. A backed image that can't be tracked is destroyed
  and `vkCreateImage` fails with `VK_ERROR_OUT_OF_HOST_MEMORY`. Room for an import is checked
  before anything is read from its fd. The tables still key on handles alone.
- **Verified:** a guest test that includes the driver's source (6 imports in the exporter, 5
  through dups in a forked process, a refused non-export followed by a good import, cleanup
  when fds close, 300 concurrent exports, 300 backed slots with the first slot unmoved): all
  pass. Deployed, Horizon's home at 90 Hz shared 89.8 frames a second, p99 13.7 ms, with no
  driver errors.
- **Tracking publication:** checked against the reader; no torn read. In
  libtrackingserviceclients the latest-state reader (0xdb568) and the mode reader (0xdac24) load
  completed (acquire), copy `copy[completed & 1]`, `dmb`, load started. They keep the copy if
  started equals completed, else they copy `copy[~started & 1]` and retry if started moved.
  `publish()` bumps started, fences, writes `copy[started & 1]` and releases completed, so the
  copy a reader keeps is never the one being written. The counters are compared only for
  equality and parity, so u32 wraparound is harmless. The history reader (0xdadc8) takes the
  newest slot as `(seq >> 1) & 31` and skips `(seq >> 1) + 1` while seq is odd, which is the
  slot `append_history()` writes.

Reviewed commit `a6b53ac` on 2026-10-03. This is a source review for the agent
implementing Prism; no emulator state or existing implementation files were changed.
The observations below are not device-tested fixes.

## First priority: prove layered image sharing

`docs/m1-progress.md` and `native/vulkan_prism/vulkan_prism.c` explicitly describe
the remaining problem: the two-layer eye image does not share its contents across
processes. `buffer_of_size()` makes a single-layer RGBA8 buffer with width 1024;
`buffer_image()` makes an image of that buffer's shape. Allocating through that
stand-in and binding the resulting memory to an eye image of a different shape
does not establish that gfxstream shares the eye image's pixels.

Before debugging VrShell rendering further, use a two-process probe: export a
1440x1584, two-layer image; render red into layer 0 and green into layer 1 in the
producer; synchronize completion; import in the consumer and read both layers
back. Run a single-layer UNORM case first as a control, then single-layer sRGB,
then two layers. Record pixel results, not just successful allocation/import.
Any proposed copy path must intercept the application's actual layered resource
and synchronize its contents into a resource the host really shares.

## Allocation chain still contains the fd import

In `prism_AllocateMemory()` (lines 532-587), `import_ahb` is inserted through
`replace_in_chain()`, which replaces a node only when its `sType` matches the new
node. An incoming `VkImportMemoryFdInfoKHR` has a different `sType`, so it remains
in the forwarded chain alongside `VkImportAndroidHardwareBufferInfoANDROID`.
This is confirmed by reading both functions; the downstream driver's response
has not been tested.

Suggested fix: rebuild the forwarded chain without the consumed fd import,
preserving unrelated nodes and leaving caller memory untouched. Validate by
mocking the downstream allocator and inspecting its complete chain. Include fd
imports with and without an export node and a dedicated-image node. Check that
success closes the caller's fd once, and failure leaves it open.

## Exported handles have a finite import budget

`prism_GetMemoryFdKHR()` queues `EXPORT_COPIES` (four) AHardwareBuffer messages,
then closes the sending socket. Duplicated receiving fds consume the same queue.
Consequently a fifth import cannot obtain another message. A failed allocation
after a successful receive also consumes one of these messages, even though the
caller retains its fd, making retries consume the budget.

This is an explicit current compromise, not evidence that today's compositor
exceeds the budget. Probe five imports through duplicated fds and an allocation
failure followed by a retry. A durable implementation needs repeatable access
to the buffer identity rather than a fixed number of queued messages.

## Tracking tables silently run out of slots

`backed_image()` and `own_buffer()` each have 256 slots. `prism_CreateImage()`
ignores failure to register a backed image; `prism_AllocateMemory()` ignores
failure to retain its buffer. Once full, successful Vulkan calls can therefore
return resources without the metadata later allocation/export paths require.

Suggested fix: use growable storage or propagate registration failure with
cleanup of the just-created resource. Validate the 257th live resource and reuse
after destruction. Include multiple devices if that use case is supported;
these tables currently key only on handles, without a device key.

## Tracking publication: verify against the real reader

`tracking_prism.c:publish()` copies the alternate pose before publishing
`begun` and `done`. Do not change the ordering based only on conventional seqlock
implementations: this is Meta's double-buffer protocol. Inspect the actual
reader's sampled sequence and final checks, then test a reader paused across two
writer updates and sequence wraparound. The source review alone does not prove
a torn-read bug.

## Validation performed

- Read the milestone notes and Vulkan/tracking implementations.
- Checked that AHB insertion matches by its own structure type and leaves the
  original fd-import node in place.
- Checked fixed table limits, ignored registration return values, and the
  four-message export loop.
- No native build, emulator launch, deployment, or runtime graphics test was run.
