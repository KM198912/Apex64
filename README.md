# Apex64 🚀


**Apex64** is a 64-bit hobby kernel stub and boot stub that builds on ideas and
implementations explored in the TitanBoot64 project
(https://github.com/KM198912/TitanBoot64).

Apex64 preserves core concepts—higher-half execution, a Higher-Half Direct Map
(HHDM), and Multiboot2 compatibility—while refining mappings, improving the build
layout, and serving as a clean starting point for new experiments.


---

## Quick highlights

- Higher-half x86_64 kernel layout (higher-half VMA for the kernel).
- HHDM and identity mappings for low physical memory (2 MiB pages by default).
- Multiboot2 header & tag parsing (framebuffer, memory map, RSDP discovery).
- Early framebuffer + serial output and a small test harness.

---

## Relationship to TitanBoot64

Apex64 is explicitly derived from and compatible with concepts implemented in TitanBoot64. See the original project for reference and history: https://github.com/KM198912/TitanBoot64

Changes in Apex64 are intended to be iterative and compatible rather than a rewrite—use TitanBoot64 as the primary upstream reference.

---

## Build & run (basic)

Requirements: x86_64 cross toolchain, nasm, grub-mkrescue, xorriso, qemu-system-x86_64

Typical commands:

```bash
make
make run
```

---

## License

Apex64 is released under the BSD 2-Clause License with attribution requirements.
See LICENSE and NOTICE for details.

Earlier work and experiments can be found in the TitanBoot64 project:
https://github.com/KM198912/TitanBoot64


---

## Acknowledgements

- Based on TitanBoot64 (https://github.com/KM198912/TitanBoot64)
- Thanks to Limine/GRUB authors and OSDev community for guidance and documentation.

---

Happy hacking!