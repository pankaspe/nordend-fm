# Plugins

Features outside Nordend's core, one directory each. A plain build has none; packages build them
with `-DNORDEND_PLUGINS=all` (or a list), and a plugin built in can be switched off in
`config.toml`. **How they work, how to add one and what a plugin must bring (tests, docs,
dependencies): [docs/plugins.md](../docs/plugins.md).**

| Plugin | What it does | Needs |
|--------|--------------|-------|
| `remotes` | `smb://` and `sftp://` bookmarks in the sidebar, mounted through gvfs over D-Bus, with a password card | sdbus-c++ to build (the core has it already); gvfs (gvfsd, gvfsd-fuse) at run time, in its own processes |

- Build with all: `cmake -B build -DNORDEND_PLUGINS=all`
- Build with some: `-DNORDEND_PLUGINS="remotes"`, or `-DNORDEND_PLUGIN_REMOTES=ON`
- Switch one off at run time, in `~/.config/nordend/config.toml`: `[plugins]` `disabled = ["remotes"]`
