# PCB/

- `PCB Design/` — all KiCad files.
- `Buck_images/` — oscilloscope photos from testing the buck-converter
  circuit on the PCB (directory names give the load condition).

## Which files are authoritative

- **Generic (sensor-module) board:**
  `PCB Design/KiCad files/Generic PCB/Generic PCB/Generic PCB.kicad_pcb`
  (+ matching `.kicad_sch`/`.kicad_pro`). Note the doubled `Generic PCB/`
  nesting is a KiCad project-folder artifact — descend twice.
- **Mirror board:**
  `PCB Design/KiCad files/Mirror PCB/Mrror PCB.kicad_pcb`
  (+ matching `.kicad_sch`/`.kicad_pro`).

Caveats for the next group:

- The `Mrror` spelling is a typo, but it is **load-bearing**: KiCad project
  files cross-reference each other by name — do not rename.
- Some `Mrror PCB*` artifacts (silkscreen PDFs, `Mrror_PCB_routed_v1.kicad_pcb`)
  also sit inside the *Generic* board's folder; the Mirror board's canonical
  files are the ones under `Mirror PCB/`.
- KiCad autosave/backup droppings (`_restore_backup_*`, `*.lck`, `*.bak`,
  `*_chatgpt*.zip`) were deleted at handover and are now gitignored.
- Known board erratum: the I²C bus lacks pull-ups — fit 4.7 kΩ from GP8/GP9
  to 3V3 (see `docs/handover.md`).
