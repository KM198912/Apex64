# TitanBoot64 🚀

**TitanBoot64** is a small 64-bit hobby kernel stub with Limine-like boot mappings and Multiboot2 support. It demonstrates a higher-half kernel layout, a Higher-Half Direct Map (HHDM) for physical ↔ virtual addressing, basic framebuffer and serial I/O, and parsing of Multiboot2 tags (including the memory map).

---

## Highlights ✅

- 64-bit kernel (x86_64) — kernel executes in the **higher half** at `0xffffffff80000000`.
- Limine-like memory mappings implemented in the boot stub:
  - Identity mapping for physical memory 0..4 GiB (2 MiB huge pages)
  - HHDM mapping: `virt = HHDM_BASE + physical` for phys 0..4 GiB (2 MiB pages)
  - Kernel mapping window at `KERNEL_VIRT_BASE` → small kernel window (configurable)
- Multiboot2 header and tag handling (framebuffer, memory map, etc.).
- Basic drivers & users:
  - Framebuffer early init + flanterm backend (text rendering)
  - Serial (COM1) I/O for early debug
  - Console printing using `printf`-style library
  - ACPI RSDP discovery via Multiboot2 (exposed through a Limine-like boot structure)
- Small test harness: draws/fills the framebuffer and prints the Multiboot memory map on boot.

---

## Current memory layout (boot stub behavior)

- Physical -> Virtual mappings are created as follows (2 MiB pages):

| Base Physical Address | Base Virtual Address | Notes |
| --------------------- | -------------------- | ----- |
| 0x0000000000000000   | 0x0000000000000000  | Identity mapping (0..4 GiB, PD entries) |
| 0x0000000000000000   | HHDM start (e.g. `0xffff800000000000`) | HHDM mapping for phys < 4 GiB |
| Kernel physical load (e.g. 0x00200000) | Kernel VMA `0xffffffff80000000` + offset | Higher-half kernel mapping (PML4[511]) |

> NOTE: The project uses 2 MiB page mappings for identity and HHDM mappings so framebuffer and other low-physical MMIO regions are accessible both by physical identity and via the HHDM.

---

## Build & run 🛠️

Requirements:
- x86_64 cross toolchain (x86_64-elf-gcc / ld)
- `nasm`, `grub-mkrescue`, `xorriso`, `qemu-system-x86_64`

Build and run:

```bash
make           # build and create MyOS.iso
make run       # run in QEMU (uses -serial stdio)
```

---

## Header / project name

- Boot header name (Multiboot tag/metadata): **TitanBoot64**

---

## Third-party tools / libraries 📦

The following third-party components are vendored directly into the repository.
They are intentionally not included as git submodules to ensure build
reproducibility and offline availability.

- flanterm — framebuffer text backend (vendored)
  https://codeberg.org/Mintsuki/Flanterm

- printf — freestanding printf-style formatting library (vendored)
  https://github.com/mpaland/printf

---

## Design philosophy

TitanBoot64 does not replace Limine.  
Instead, it mirrors Limine’s *kernel-facing semantics* while remaining compatible
with GRUB and Multiboot2, lowering the barrier for testing kernels on real hardware.

---

## Status & TODO ✍️

- Implemented: Multiboot2 parsing (framebuffer, memory map), HHDM + identity map for 0..4 GiB, framebuffer write/debug, printing memory map.
- TODO:
  - Implement a physical memory allocator using the memory map
  - Finalize HHDM generic helpers and remove debug markers
  - Add more drivers (keyboard, PCI, ACPI parsing)

---

## Non-goals

- This is not a full bootloader
- This is not a production-ready kernel
- KASLR and relocation are intentionally out of scope (for now, contributions are always welcome!)

---

## License
- This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.
- Third-party components retain their original licenses; see their respective headers or upstream repositories, and may be removed at any time if licensing terms change.

---

## Author Notes

- This project is **not a general-purpose toolkit or framework**.  
  It exists to demonstrate and provide Limine-like kernel semantics while remaining compatible with GRUB and Multiboot2.

- The primary motivation is to make it easier for hobby OS developers to test kernels on real hardware using an existing GRUB setup, without repeatedly writing bootable USB images.

- TitanBoot64 is intended as a **reference implementation and starting point**, not as a drop-in dependency.

- Contributions are welcome! Feel free to open issues or pull requests for improvements, bug fixes, or new features.

- Happy coding!

---

## Acknowledgements

- Thanks to the developers of Limine and GRUB for their pioneering work in bootloading and kernel loading.
- Thanks to [Astrido](https://github.com/asterd-og) for the early framebuffer code.
- Thanks to the OSDev Wiki for its extensive documentation on Multiboot2 and x86_64 booting.

---