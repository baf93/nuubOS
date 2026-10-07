# nuubOS
H700 handheld gaming OS. Dev target `root@192.168.5.36`. Before working, read `docs/dev/principles.md` + the area file in `docs/dev/` (`README.md` = index, also maps `§N` in code comments); new findings go there, not here. Epics: grep `docs/product/nuubOS-product-epics.md` by id.
Hard rules: build only via `./scripts/compile.sh` in Docker, targeted; deploy after changes; never commit/push unless asked; user commands in fish (no heredoc/`set -e`/`exit`); all visible strings localized; Alpha→RC = bug fixes only.
