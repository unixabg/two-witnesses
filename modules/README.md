# modules/ - what a board does with the mesh

Each module is one folder here: `modules/<name>/` with its `module.yml`, its
ESP-IDF component `tw_mod_<name>/` (with a `Kconfig`: it appears in
`make menuconfig`), and its Python plug-in `host/`. The design, the ids, and
what a module may and may not do: ../docs/MODULES.md.

    make modules          the modules found, and which this configuration has
    make check-modules    refuse clashes (ids, names, kinds, keys, pins, airtime)

None yet: CSI sensing becomes the first (docs/MODULES.md, stage 3).
