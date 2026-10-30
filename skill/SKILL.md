---
name: explorer
description: The Rant explorer, a visual debugger that browses and shows everything on a Rant mesh, native or as a web page. Use when the user wants to see live Rant data, nodes and entities, or to serve the explorer in a browser.
---

# Rant explorer

The package `explorer` browses and shows anything on a Rant mesh: its nodes, their entities
and live values. It runs native, or as a web page through the bridge.

| goal | command |
|---|---|
| install | `rant install explorer` |
| open it | `rant explore`, `--domain N` for another domain |

## In a browser

The explorer package's group `WebExplorer` serves the explorer as a web page, so any browser
on the network opens it. The page reaches the mesh through the bridge on the same host, so run
both.

| goal | command |
|---|---|
| run it | `rant run WebExplorer`, `port=N` and `bind=IP` set where it listens, 6732 on every address by default |
| stop it | `rant stop WebExplorer` |
| at every boot | `rant enable WebExplorer`, with the same params, `rant disable WebExplorer` |
| output | `rant log WebExplorer`, `-f` to follow |
